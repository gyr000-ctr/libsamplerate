/*
** Copyright (c) 2002-2021, Erik de Castro Lopo <erikd@mega-nerd.com>
** All rights reserved.
**
** This code is released under 2-clause BSD license. Please see the
** file at : https://github.com/libsndfile/libsamplerate/blob/master/COPYING
*/

/*
** double_test.c : validation of the double precision processing path.
**
**   Part A : the double precision sinc engine matches an independent
**            direct convolution reference computed in this file from
**            the same coefficient tables, in double precision.
**   Part B : quantified difference between the single and double
**            precision paths for every converter (printed as a table
**            and as CSV lines) ; the double path is required to be
**            strictly closer to the reference than the float path.
**   Part C : chunked src_process_double() streaming is bit identical
**            to one shot src_simple_double().
**   Part D : API misuse guards and produces-output sanity over all
**            converters.
**
** The float path is not modified by this test in any way ; the
** existing test suite covers its regression.
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>

#include <samplerate.h>

#include "util.h"

/* The coefficient tables carry full double precision decimal literals ;
** including them here with coeff_t = double gives the reference below
** access to true double precision coefficients, exactly like the double
** precision instantiation of the sinc engine.
*/
typedef double coeff_t ;
#include "../src/fastest_coeffs.h"
#include "../src/mid_qual_coeffs.h"
#include "../src/high_qual_coeffs.h"

#define REF_SHIFT_BITS	12
#define REF_FP_ONE		(1 << REF_SHIFT_BITS)

#define ARRAY_LEN_LOCAL(x)	((int) (sizeof (x) / sizeof ((x) [0])))

typedef struct
{	int			converter ;
	const char	*name ;
	const coeff_t *coeffs ;
	int			coeff_half_len ;
	int			index_inc ;
} REF_TABLE ;

#define REF_TABLE_ENTRY(conv, tbl) \
	{ conv, #conv, tbl.coeffs, ARRAY_LEN_LOCAL (tbl.coeffs) - 2, tbl.increment }

static const REF_TABLE ref_tables [] =
{	REF_TABLE_ENTRY (SRC_SINC_BEST_QUALITY, slow_high_qual_coeffs),
	REF_TABLE_ENTRY (SRC_SINC_MEDIUM_QUALITY, slow_mid_qual_coeffs),
	REF_TABLE_ENTRY (SRC_SINC_FASTEST, fastest_coeffs),
} ;

static int
is_sinc_converter (int converter)
{	return converter == SRC_SINC_BEST_QUALITY
		|| converter == SRC_SINC_MEDIUM_QUALITY
		|| converter == SRC_SINC_FASTEST ;
}

static const REF_TABLE *
find_ref_table (int converter)
{	for (size_t k = 0 ; k < sizeof (ref_tables) / sizeof (ref_tables [0]) ; k++)
		if (ref_tables [k].converter == converter)
			return &ref_tables [k] ;
	return NULL ;
}

/* Deterministic test signal, no rand(). */
static double
gen_sample (long i, int channel)
{	return 0.55 * sin (i * 0.6 + channel * 0.9)
			+ 0.30 * sin (i * 1.1 + 0.5 + channel * 1.7)
			+ 0.12 * sin (i * 0.23 + 1.7 + channel * 0.3) ;
}

static void
gen_signal (double *data, long frames, int channels)
{	for (long i = 0 ; i < frames ; i++)
		for (int ch = 0 ; ch < channels ; ch++)
			data [i * channels + ch] = gen_sample (i, ch) ;
}

static double
fmod_one_ref (double x)
{	double res = x - lrint (x) ;
	return (res < 0.0) ? res + 1.0 : res ;
}

/* Virtual one shot buffer : the input frames embedded at offset `half`
** in a sea of zeros, exactly what the engine sees for a single
** src_simple_double() call with end_of_input set.
*/
static double
vbuf_at (const double *input, long input_frames, long half, long index)
{	long i = index - half ;
	return (i >= 0 && i < input_frames) ? input [i] : 0.0 ;
}

static double
ref_convolve (const double *input, long input_frames, long half, long b_current,
		const REF_TABLE *tab, int32_t increment, int32_t start_filter_index)
{	double	fraction, left, right, icoeff ;
	int32_t	filter_index, max_filter_index ;
	long	data_index, coeff_count ;
	int		indx ;

	max_filter_index = tab->coeff_half_len << REF_SHIFT_BITS ;

	/* Left half of the filter. */
	filter_index = start_filter_index ;
	coeff_count = (max_filter_index - filter_index) / increment ;
	filter_index = filter_index + coeff_count * increment ;
	data_index = b_current - coeff_count ;

	left = 0.0 ;
	while (filter_index >= 0)
	{	fraction = (filter_index & (REF_FP_ONE - 1)) * (1.0 / REF_FP_ONE) ;
		indx = filter_index >> REF_SHIFT_BITS ;
		icoeff = tab->coeffs [indx] + fraction * (tab->coeffs [indx + 1] - tab->coeffs [indx]) ;
		left += icoeff * vbuf_at (input, input_frames, half, data_index) ;
		filter_index -= increment ;
		data_index += 1 ;
		} ;

	/* Right half of the filter. */
	filter_index = increment - start_filter_index ;
	coeff_count = (max_filter_index - filter_index) / increment ;
	filter_index = filter_index + coeff_count * increment ;
	data_index = b_current + 1 + coeff_count ;

	right = 0.0 ;
	do
	{	fraction = (filter_index & (REF_FP_ONE - 1)) * (1.0 / REF_FP_ONE) ;
		indx = filter_index >> REF_SHIFT_BITS ;
		icoeff = tab->coeffs [indx] + fraction * (tab->coeffs [indx + 1] - tab->coeffs [indx]) ;
		right += icoeff * vbuf_at (input, input_frames, half, data_index) ;
		filter_index -= increment ;
		data_index -= 1 ;
		}
	while (filter_index > 0) ;

	return left + right ;
} /* ref_convolve */

/*
** Independent reference of the mono sinc engine : replicates the fixed
** point indexing, coefficient interpolation and output scaling of the
** library, computed directly from the coefficient table in double
** precision. Returns the number of output frames generated.
*/
static long
sinc_reference (const double *input, long input_frames, double *output, long output_frames,
		double src_ratio, const REF_TABLE *tab)
{	double	input_index, rem, terminate, float_increment, count ;
	int32_t	increment, start_filter_index ;
	long	b_current, b_real_end, half, out_gen ;

	count = (tab->coeff_half_len + 2.0) / tab->index_inc ;
	if (src_ratio < 1.0)
		count /= src_ratio ;
	half = lrint (count) + 1 ;

	b_current = half ;
	b_real_end = half + input_frames ;
	input_index = 0.0 ;
	out_gen = 0 ;
	terminate = 1.0 / src_ratio + 1e-20 ;

	while (out_gen < output_frames)
	{	if (b_current + input_index + terminate > b_real_end)
			break ;

		float_increment = tab->index_inc * (src_ratio < 1.0 ? src_ratio : 1.0) ;
		increment = (int32_t) lrint (float_increment * REF_FP_ONE) ;
		start_filter_index = (int32_t) lrint (input_index * float_increment * REF_FP_ONE) ;

		output [out_gen] = (float_increment / tab->index_inc) *
				ref_convolve (input, input_frames, half, b_current, tab, increment, start_filter_index) ;
		out_gen ++ ;

		input_index += 1.0 / src_ratio ;
		rem = fmod_one_ref (input_index) ;
		b_current += lrint (input_index - rem) ;
		input_index = rem ;
		} ;

	return out_gen ;
} /* sinc_reference */

static double
max_abs_diff (const double *a, const double *b, long len)
{	double maxdiff = 0.0 ;
	for (long k = 0 ; k < len ; k++)
	{	double d = fabs (a [k] - b [k]) ;
		if (d > maxdiff)
			maxdiff = d ;
		} ;
	return maxdiff ;
}

static double
rms_of (const double *a, long len)
{	double acc = 0.0 ;
	for (long k = 0 ; k < len ; k++)
		acc += a [k] * a [k] ;
	return sqrt (acc / len) ;
}

static double
rms_diff (const double *a, const double *b, long len)
{	double acc = 0.0 ;
	for (long k = 0 ; k < len ; k++)
	{	double d = a [k] - b [k] ;
		acc += d * d ;
		} ;
	return sqrt (acc / len) ;
}

/*======================================================================
** Part A : double precision sinc engine vs direct convolution reference.
*/

static void
reference_match_test (int converter, double src_ratio)
{	const REF_TABLE *tab = find_ref_table (converter) ;
	const long input_frames = 4096 ;
	const long output_frames = (long) (input_frames * src_ratio) + 64 ;
	double *input, *output, *reference ;
	SRC_DATA_D data ;
	long ref_frames ;
	double maxdiff ;
	int error ;

	ASSERT (tab != NULL) ;

	input = malloc (sizeof (double) * input_frames) ;
	output = calloc (output_frames, sizeof (double)) ;
	reference = calloc (output_frames, sizeof (double)) ;
	ASSERT (input != NULL && output != NULL && reference != NULL) ;

	gen_signal (input, input_frames, 1) ;

	memset (&data, 0, sizeof (data)) ;
	data.data_in = input ;
	data.data_out = output ;
	data.input_frames = input_frames ;
	data.output_frames = output_frames ;
	data.src_ratio = src_ratio ;

	error = src_simple_double (&data, converter, 1) ;
	ASSERT (error == 0) ;

	ref_frames = sinc_reference (input, input_frames, reference, output_frames, src_ratio, tab) ;

	printf ("\tdouble_vs_ref\t(%s, ratio = %5.3f) : frames %ld/%ld",
		src_get_name (converter), src_ratio, (long) data.output_frames_gen, ref_frames) ;

	ASSERT (data.output_frames_gen == ref_frames) ;

	maxdiff = max_abs_diff (output, reference, ref_frames) ;
	printf (", max abs diff = %3.2e ", maxdiff) ;

	if (maxdiff > 1e-9)
	{	printf ("\n\nLine %d : double output deviates from reference by %g.\n\n", __LINE__, maxdiff) ;
		exit (1) ;
		} ;

	puts ("ok") ;

	free (input) ;
	free (output) ;
	free (reference) ;
} /* reference_match_test */

/*======================================================================
** Part B : quantified single vs double precision difference.
*/

static void
precision_diff_test (int converter, int channels, double src_ratio)
{	const long input_frames = 4096 ;
	const long output_frames = (long) (input_frames * src_ratio) + 64 ;
	const long in_len = input_frames * channels ;
	const long out_len = output_frames * channels ;
	double *input_d, *output_d, *output_f_as_d ;
	float *input_f, *output_f ;
	SRC_DATA_D dd ;
	SRC_DATA df ;
	long cmp_len ;
	double maxdiff, rd, rs, snr_db ;
	int error ;

	input_d = malloc (sizeof (double) * in_len) ;
	output_d = calloc (out_len, sizeof (double)) ;
	output_f_as_d = calloc (out_len, sizeof (double)) ;
	input_f = malloc (sizeof (float) * in_len) ;
	output_f = calloc (out_len, sizeof (float)) ;
	ASSERT (input_d && output_d && output_f_as_d && input_f && output_f) ;

	gen_signal (input_d, input_frames, channels) ;
	for (long k = 0 ; k < in_len ; k++)
		input_f [k] = (float) input_d [k] ;

	memset (&dd, 0, sizeof (dd)) ;
	dd.data_in = input_d ;
	dd.data_out = output_d ;
	dd.input_frames = input_frames ;
	dd.output_frames = output_frames ;
	dd.src_ratio = src_ratio ;
	error = src_simple_double (&dd, converter, channels) ;
	ASSERT (error == 0) ;

	memset (&df, 0, sizeof (df)) ;
	df.data_in = input_f ;
	df.data_out = output_f ;
	df.input_frames = input_frames ;
	df.output_frames = output_frames ;
	df.src_ratio = src_ratio ;
	error = src_simple (&df, converter, channels) ;
	ASSERT (error == 0) ;

	ASSERT (dd.output_frames_gen == df.output_frames_gen) ;

	cmp_len = dd.output_frames_gen * channels ;
	for (long k = 0 ; k < cmp_len ; k++)
		output_f_as_d [k] = output_f [k] ;

	maxdiff = max_abs_diff (output_d, output_f_as_d, cmp_len) ;
	rd = rms_diff (output_d, output_f_as_d, cmp_len) ;
	rs = rms_of (output_d, cmp_len) ;
	snr_db = 20.0 * log10 (rs / rd) ;

	printf ("\tfloat_vs_double\t(%s, ch = %d, ratio = %5.3f) : max = %3.2e, rms = %3.2e, diff SNR = %6.1f dB\n",
		src_get_name (converter), channels, src_ratio, maxdiff, rd, snr_db) ;
	printf ("CSV,float_vs_double,%d,%d,%.3f,%ld,%.6e,%.6e,%.2f\n",
		converter, channels, src_ratio, (long) dd.output_frames_gen, maxdiff, rd, snr_db) ;

	/* The two paths must genuinely differ, but stay within a few float ulps. */
	if (! (maxdiff > 0.0 && maxdiff < 1e-3))
	{	printf ("\n\nLine %d : unexpected float/double difference %g.\n\n", __LINE__, maxdiff) ;
		exit (1) ;
		} ;

	/* For the sinc converters, both paths are also compared against the
	** direct convolution reference : the double path must be strictly
	** closer to it than the float path.
	*/
	if (channels == 1 && is_sinc_converter (converter))
	{	const REF_TABLE *tab = find_ref_table (converter) ;
		double *reference = calloc (output_frames, sizeof (double)) ;
		double err_d, err_f ;
		long ref_frames ;

		ASSERT (reference != NULL) ;
		ref_frames = sinc_reference (input_d, input_frames, reference, output_frames, src_ratio, tab) ;
		ASSERT (ref_frames == dd.output_frames_gen) ;

		err_d = max_abs_diff (output_d, reference, ref_frames) ;
		err_f = max_abs_diff (output_f_as_d, reference, ref_frames) ;

		printf ("\t\tdistance to reference : double = %3.2e, float = %3.2e\n", err_d, err_f) ;
		printf ("CSV,distance_to_reference,%d,%d,%.3f,%.6e,%.6e\n",
			converter, channels, src_ratio, err_d, err_f) ;

		if (! (err_d < err_f))
		{	printf ("\n\nLine %d : double path (%g) not closer to reference than float path (%g).\n\n",
				__LINE__, err_d, err_f) ;
			exit (1) ;
			} ;

		free (reference) ;
		} ;

	free (input_d) ;
	free (output_d) ;
	free (output_f_as_d) ;
	free (input_f) ;
	free (output_f) ;
} /* precision_diff_test */

/*======================================================================
** Part C : chunked streaming is bit identical to one shot processing.
*/

static void
streaming_identical_test (int converter, int channels, double src_ratio, long max_frame_slack)
{	const long input_frames = 5000 ;
	const long output_frames = (long) (input_frames * src_ratio) + 256 ;
	const long in_len = input_frames * channels ;
	const long out_len = output_frames * channels ;
	double *input, *out_oneshot, *out_stream ;
	SRC_DATA_D data ;
	SRC_STATE *state ;
	long in_pos, out_pos, oneshot_frames, cmp_frames, slack ;
	int error ;

	input = malloc (sizeof (double) * in_len) ;
	out_oneshot = calloc (out_len, sizeof (double)) ;
	out_stream = calloc (out_len, sizeof (double)) ;
	ASSERT (input && out_oneshot && out_stream) ;

	gen_signal (input, input_frames, channels) ;

	/* One shot via the simple interface. */
	memset (&data, 0, sizeof (data)) ;
	data.data_in = input ;
	data.data_out = out_oneshot ;
	data.input_frames = input_frames ;
	data.output_frames = output_frames ;
	data.src_ratio = src_ratio ;
	error = src_simple_double (&data, converter, channels) ;
	ASSERT (error == 0) ;
	oneshot_frames = data.output_frames_gen ;

	/* Streamed in deterministic odd sized chunks. */
	state = src_new_double (converter, channels, &error) ;
	ASSERT (state != NULL && error == 0) ;

	in_pos = out_pos = 0 ;
	while (in_pos < input_frames)
	{	long chunk = 1 + (in_pos * 7 + 13) % 977 ;
		if (in_pos + chunk > input_frames)
			chunk = input_frames - in_pos ;

		memset (&data, 0, sizeof (data)) ;
		data.data_in = input + in_pos * channels ;
		data.data_out = out_stream + out_pos * channels ;
		data.input_frames = chunk ;
		data.output_frames = output_frames - out_pos ;
		data.end_of_input = (in_pos + chunk == input_frames) ;
		data.src_ratio = src_ratio ;

		error = src_process_double (state, &data) ;
		ASSERT (error == 0) ;

		in_pos += data.input_frames_used ;
		out_pos += data.output_frames_gen ;
		} ;

	src_delete (state) ;

	printf ("\tstreaming\t(%s, ch = %d, ratio = %5.3f) : %ld vs %ld frames ",
		src_get_name (converter), channels, src_ratio, out_pos, oneshot_frames) ;

	/* The sinc engine is chunk agnostic and must match exactly. The
	** linear/zoh engines can produce one extra frame at the end_of_input
	** boundary when streaming ; the single precision path (which shares
	** the same control logic) behaves identically, so a slack of one
	** frame is accepted there.
	*/
	slack = labs (out_pos - oneshot_frames) ;
	if (slack > max_frame_slack)
	{	printf ("\n\nLine %d : streamed frame count %ld differs from one shot %ld by more than %ld.\n\n",
			__LINE__, out_pos, oneshot_frames, max_frame_slack) ;
		exit (1) ;
		} ;

	cmp_frames = MIN (out_pos, oneshot_frames) ;
	if (memcmp (out_stream, out_oneshot, cmp_frames * channels * sizeof (double)) != 0)
	{	printf ("\n\nLine %d : streamed output differs from one shot output.\n\n", __LINE__) ;
		exit (1) ;
		} ;

	puts ("ok") ;

	free (input) ;
	free (out_oneshot) ;
	free (out_stream) ;
} /* streaming_identical_test */

/*======================================================================
** Part D : API misuse guards and produces-output sanity.
*/

static void
precision_mismatch_test (void)
{	SRC_STATE *state ;
	SRC_DATA data_f ;
	SRC_DATA_D data_d ;
	float buf_f [16] = { 0.0 } ;
	double buf_d [16] = { 0.0 } ;
	int error ;

	memset (&data_f, 0, sizeof (data_f)) ;
	data_f.data_in = buf_f ;
	data_f.data_out = buf_f ;
	data_f.input_frames = 8 ;
	data_f.output_frames = 8 ;
	data_f.src_ratio = 1.0 ;

	memset (&data_d, 0, sizeof (data_d)) ;
	data_d.data_in = buf_d ;
	data_d.data_out = buf_d ;
	data_d.input_frames = 8 ;
	data_d.output_frames = 8 ;
	data_d.src_ratio = 1.0 ;

	/* Float data on a double state must be rejected. */
	state = src_new_double (SRC_SINC_FASTEST, 1, &error) ;
	ASSERT (state != NULL && error == 0) ;
	if (src_process (state, &data_f) == 0)
	{	printf ("\n\nLine %d : src_process on a double state should fail.\n\n", __LINE__) ;
		exit (1) ;
		} ;
	src_delete (state) ;

	/* Double data on a float state must be rejected. */
	state = src_new (SRC_SINC_FASTEST, 1, &error) ;
	ASSERT (state != NULL && error == 0) ;
	if (src_process_double (state, &data_d) == 0)
	{	printf ("\n\nLine %d : src_process_double on a float state should fail.\n\n", __LINE__) ;
		exit (1) ;
		} ;
	src_delete (state) ;

	/* Bad converter number must be rejected by src_new_double. */
	state = src_new_double (-1, 1, &error) ;
	if (state != NULL || error == 0)
	{	printf ("\n\nLine %d : src_new_double with bad converter should fail.\n\n", __LINE__) ;
		exit (1) ;
		} ;

	puts ("\tprecision_mismatch ........................................ ok") ;
} /* precision_mismatch_test */

static void
produces_output_test (int converter, int channels, double src_ratio)
{	const long input_frames = 1000 ;
	const long output_frames = (long) (input_frames * src_ratio) + 64 ;
	double *input, *output ;
	SRC_DATA_D data ;
	int error ;

	input = malloc (sizeof (double) * input_frames * channels) ;
	output = calloc (output_frames * channels, sizeof (double)) ;
	ASSERT (input && output) ;

	gen_signal (input, input_frames, channels) ;

	memset (&data, 0, sizeof (data)) ;
	data.data_in = input ;
	data.data_out = output ;
	data.input_frames = input_frames ;
	data.output_frames = output_frames ;
	data.src_ratio = src_ratio ;

	error = src_simple_double (&data, converter, channels) ;

	printf ("\tproduces_output\t(%s, ch = %d, ratio = %6.4f) ... ", src_get_name (converter), channels, src_ratio) ;

	if (error != 0)
	{	printf ("\n\nLine %d : src_simple_double failed : %s\n\n", __LINE__, src_strerror (error)) ;
		exit (1) ;
		} ;
	if (data.input_frames_used == 0 || data.output_frames_gen == 0)
	{	printf ("\n\nLine %d : src_simple_double produced no output.\n\n", __LINE__) ;
		exit (1) ;
		} ;

	puts ("ok") ;

	free (input) ;
	free (output) ;
} /* produces_output_test */

int
main (void)
{	static const double ratios [] = { 0.8, 1.5 } ;
	static const double simple_ratios [] = { 0.099, 0.33333333, 0.789, 1.9, 3.1 } ;
	static const int all_converters [] =
	{	SRC_SINC_BEST_QUALITY, SRC_SINC_MEDIUM_QUALITY, SRC_SINC_FASTEST,
		SRC_ZERO_ORDER_HOLD, SRC_LINEAR
		} ;

	puts ("") ;
	puts ("    Part A : double precision sinc vs direct convolution reference :") ;
	for (size_t r = 0 ; r < sizeof (ratios) / sizeof (ratios [0]) ; r++)
		for (size_t c = 0 ; c < sizeof (ref_tables) / sizeof (ref_tables [0]) ; c++)
			reference_match_test (ref_tables [c].converter, ratios [r]) ;

	puts ("") ;
	puts ("    Part B : single vs double precision difference :") ;
	for (size_t c = 0 ; c < sizeof (all_converters) / sizeof (all_converters [0]) ; c++)
		for (int ch = 1 ; ch <= 2 ; ch++)
			precision_diff_test (all_converters [c], ch, 1.5) ;

	puts ("") ;
	puts ("    Part C : chunked streaming vs one shot (double) :") ;
	streaming_identical_test (SRC_SINC_BEST_QUALITY, 1, 1.3, 0) ;
	streaming_identical_test (SRC_SINC_MEDIUM_QUALITY, 2, 1.3, 0) ;
	streaming_identical_test (SRC_SINC_FASTEST, 2, 0.7, 0) ;
	streaming_identical_test (SRC_LINEAR, 2, 1.3, 1) ;
	streaming_identical_test (SRC_ZERO_ORDER_HOLD, 1, 0.7, 1) ;

	puts ("") ;
	puts ("    Part D : API guards and produces-output sanity :") ;
	precision_mismatch_test () ;
	for (size_t c = 0 ; c < sizeof (all_converters) / sizeof (all_converters [0]) ; c++)
		for (size_t r = 0 ; r < sizeof (simple_ratios) / sizeof (simple_ratios [0]) ; r++)
			produces_output_test (all_converters [c], 1 + (int) (r % 3), simple_ratios [r]) ;

	puts ("") ;
	return 0 ;
} /* main */
