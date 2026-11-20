/*
** Copyright (c) 2002-2016, Erik de Castro Lopo <erikd@mega-nerd.com>
** All rights reserved.
**
** This code is released under 2-clause BSD license. Please see the
** file at : https://github.com/libsndfile/libsamplerate/blob/master/COPYING
*/

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include <samplerate.h>

#include "util.h"

static void name_test (void) ;
static void error_test (void) ;
static void src_ratio_test (void) ;
static void zero_input_test (int converter) ;
static void get_channels_test (int converter);
static void get_latency_test (void) ;

int
main (void)
{
	puts ("") ;

	printf ("    version : %s\n\n", src_get_version ()) ;

	/* Current max converter is SRC_LINEAR. */
	name_test () ;

	error_test () ;

	src_ratio_test () ;

	zero_input_test (SRC_ZERO_ORDER_HOLD) ;
	zero_input_test (SRC_LINEAR) ;
#ifdef ENABLE_SINC_FAST_CONVERTER
	zero_input_test (SRC_SINC_FASTEST) ;
#endif
	get_channels_test (SRC_ZERO_ORDER_HOLD) ;
	get_channels_test (SRC_LINEAR) ;
#ifdef ENABLE_SINC_FAST_CONVERTER
	get_channels_test (SRC_SINC_FASTEST) ;
#endif
	get_latency_test () ;
	puts ("") ;
	return 0 ;
} /* main */

static void
name_test (void)
{	const char	*name ;
	int	k = 0 ;

	puts ("    name_test :") ;

	while (1)
	{	name = src_get_name (k) ;
		if (name == NULL)
			break ;
		printf ("\tName %d : %s\n", k, name) ;
		printf ("\tDesc %d : %s\n", k, src_get_description (k)) ;
		k ++ ;
		} ;

	puts ("") ;

	return ;
} /* name_test */

/*------------------------------------------------------------------------------
*/

typedef struct
{	double	ratio ;
	int		should_pass ;
} RATIO_TEST ;

static RATIO_TEST ratio_test [] =
{	{	1.0 / 256.1,	0 },
	{	1.0 / 256.0,	1 },
	{	1.0,			1 },
	{	256.0, 			1 },
	{	256.1,			0 },
	{	-1.0,			0 }
} ;

static void
src_ratio_test (void)
{	int k ;

	puts ("    src_ratio_test (SRC ratio must be in range [1/256, 256]):" ) ;


	for (k = 0 ; k < ARRAY_LEN (ratio_test) ; k++)
	{	if (ratio_test [k].should_pass && src_is_valid_ratio (ratio_test [k].ratio) == 0)
		{	printf ("\n\nLine %d : SRC ratio %f should have passed.\n\n", __LINE__, ratio_test [k].ratio) ;
			exit (1) ;
			} ;
		if (! ratio_test [k].should_pass && src_is_valid_ratio (ratio_test [k].ratio) != 0)
		{	printf ("\n\nLine %d : SRC ratio %f should not have passed.\n\n", __LINE__, ratio_test [k].ratio) ;
			exit (1) ;
			} ;
		printf ("\t SRC ratio (%9.5f) : %s ................... ok\n", ratio_test [k].ratio,
			(ratio_test [k].should_pass ? "pass" : "fail")) ;
		} ;

	puts ("") ;

	return ;
} /* src_ratio_test */

static void
error_test (void)
{	const char *errorstr ;
	int		k, errors = 0 ;

	puts ("    error_test :") ;

	for (k = 0 ; 1 ; k++)
	{	errorstr = src_strerror (k) ;
		printf ("\t%-2d : %s\n", k, errorstr) ;
		if (errorstr == NULL)
		{	errors ++ ;
			continue ;
			} ;
		if (strstr (errorstr, "Placeholder.") == errorstr)
			break ;
		} ;

	if (errors != 0)
	{	printf ("\n\nLine %d : Missing error numbers above.\n\n", __LINE__) ;
		exit (1) ;
		} ;

	puts ("") ;

	return ;
} /* error_test */

static void
zero_input_test (int converter)
{	SRC_DATA data ;
	SRC_STATE *state ;
	float out [100] ;
	int error ;

	printf ("    %s (%-26s) ........ ", __func__, src_get_name (converter)) ;
	fflush (stdout) ;

	if ((state = src_new (converter, 1, &error)) == NULL)
	{	printf ("\n\nLine %d : src_new failed : %s.\n\n", __LINE__, src_strerror (error)) ;
		exit (1) ;
		} ;

	data.data_in = (float *) (size_t) 0xdeadbeef ;
	data.input_frames = 0 ;
	data.data_out = out ;
	data.output_frames = ARRAY_LEN (out) ;
	data.end_of_input = 0 ;
	data.src_ratio = 1.0 ;

	if ((error = src_process (state, &data)))
	{	printf ("\n\nLine %d : src_new failed : %s.\n\n", __LINE__, src_strerror (error)) ;
		exit (1) ;
		} ;

	state = src_delete (state) ;

	puts ("ok") ;
} /* zero_input_test */

static void get_channels_test(int converter)
{
	SRC_STATE *state;
	int channels_in, channels_out;
	const char *errorstr;

	state = NULL;
	if ((channels_out = src_get_channels(state)) >= 0)
	{
		printf ("\n\nLine %d : Return value should be negative, was : %d.\n\n", __LINE__, channels_out) ;
		exit (1) ;
	}
	errorstr = src_strerror(-channels_out);
	if (!strstr(errorstr, "NULL"))
	{
		printf ("\n\nLine %d : Inverted output should be valid error code mentioning a NULL pointer, was : %s.\n\n", __LINE__, errorstr) ;
		exit (1) ;
	}

	for (channels_in = 1; channels_in <= 8; channels_in++)
	{
		int error;
		state = src_new(converter, channels_in, &error);
		if ((channels_out = src_get_channels(state)) != channels_in)
		{
			printf ("\n\nLine %d : Return value should be %d, was : %d.\n\n", __LINE__, channels_in, channels_out) ;
			exit (1) ;
		}
		state = src_delete(state);
	}
}

/*------------------------------------------------------------------------------
** src_get_latency() tests.
**
** The expected values are the per-side lookahead of the interpolation kernel,
** i.e. the number of input frames the converter needs before it can emit an
** output frame. They are verified against the streaming behaviour seen when
** feeding one input frame at a time: the first output frame appears only after
** that many input frames have been supplied (per side, hence the +1 below for
** the source of the two-frame linear/ZOH window).
*/

static void
check_latency (int converter, double ratio, int expected)
{	int got, error ;
	SRC_STATE *state ;

	state = src_new (converter, 1, &error) ;
	if (state == NULL)
	{	printf ("\n\nLine %d : src_new failed : %s.\n\n", __LINE__, src_strerror (error)) ;
		exit (1) ;
		} ;

	got = src_get_latency (state, ratio) ;
	if (got != expected)
	{	printf ("\n\nLine %d : converter %d ratio %g : expected latency %d, got %d.\n\n",
				__LINE__, converter, ratio, expected, got) ;
		exit (1) ;
		} ;

	src_delete (state) ;
} /* check_latency */

static void
get_latency_test (void)
{	SRC_STATE *state ;
	int error, got, got_bad_ratio, got_null ;

	puts ("    get_latency_test :") ;

	/* An uninitialised (NULL) state must be distinguishable from a bad ratio. */
	got_null = src_get_latency (NULL, 1.0) ;
	if (got_null >= 0)
	{	printf ("\n\nLine %d : NULL state should return a negative error, got %d.\n\n",
				__LINE__, got_null) ;
		exit (1) ;
		} ;
	if (strstr (src_strerror (-got_null), "NULL") == NULL)
	{	printf ("\n\nLine %d : NULL state error should mention a NULL pointer, got : %s.\n\n",
				__LINE__, src_strerror (-got_null)) ;
		exit (1) ;
		} ;

	state = src_new (SRC_LINEAR, 1, &error) ;
	if (state == NULL)
	{	printf ("\n\nLine %d : src_new failed : %s.\n\n", __LINE__, src_strerror (error)) ;
		exit (1) ;
		} ;

	/* Out of range ratios, including zero, negative, NaN and out of bounds. */
	double bad_ratios [] = { 0.0, -1.0, 1.0 / 256.1, 256.1, 1.0 / 0.0, 0.0 / 0.0 } ;
	for (int k = 0 ; k < ARRAY_LEN (bad_ratios) ; k++)
	{	got_bad_ratio = src_get_latency (state, bad_ratios [k]) ;
		if (got_bad_ratio >= 0)
		{	printf ("\n\nLine %d : ratio %g should return a negative error, got %d.\n\n",
					__LINE__, bad_ratios [k], got_bad_ratio) ;
			exit (1) ;
			} ;
		if (got_bad_ratio == got_null)
		{	printf ("\n\nLine %d : bad ratio and NULL state must be distinguishable (both %d).\n\n",
					__LINE__, got_bad_ratio) ;
			exit (1) ;
			} ;
		} ;
	if (strstr (src_strerror (-got_bad_ratio), "ratio") == NULL)
	{	printf ("\n\nLine %d : bad ratio error should mention the ratio, got : %s.\n\n",
				__LINE__, src_strerror (-got_bad_ratio)) ;
		exit (1) ;
		} ;

	/* Boundary ratios are valid and can be queried before any processing. */
	if (src_get_latency (state, 1.0 / 256.0) <= 0 || src_get_latency (state, 256.0) <= 0)
	{	printf ("\n\nLine %d : boundary ratios must be accepted.\n\n", __LINE__) ;
		exit (1) ;
		} ;
	src_delete (state) ;

	/* ZOH and linear use the current and previous input frame at every ratio. */
	double ratios [] = { 1.0 / 256.0, 0.25, 0.5, 1.0, 2.0, 4.0, 256.0 } ;
	for (int k = 0 ; k < ARRAY_LEN (ratios) ; k++)
	{	check_latency (SRC_ZERO_ORDER_HOLD, ratios [k], 1) ;
		check_latency (SRC_LINEAR, ratios [k], 1) ;
		}

	/*
	** Sinc converters: constant for ratio >= 1.0 and growing as 1/ratio below
	** it. The constants below follow from (coeff_half_len + 2) / index_inc,
	** rounded, plus one.
	*/
#ifdef ENABLE_SINC_FAST_CONVERTER
	check_latency (SRC_SINC_FASTEST, 1.0, 20) ;
	check_latency (SRC_SINC_FASTEST, 2.0, 20) ;
	check_latency (SRC_SINC_FASTEST, 256.0, 20) ;
	check_latency (SRC_SINC_FASTEST, 0.5, 39) ;
	check_latency (SRC_SINC_FASTEST, 0.25, 78) ;
#endif
#ifdef ENABLE_SINC_MEDIUM_CONVERTER
	check_latency (SRC_SINC_MEDIUM_QUALITY, 1.0, 47) ;
	check_latency (SRC_SINC_MEDIUM_QUALITY, 2.0, 47) ;
	check_latency (SRC_SINC_MEDIUM_QUALITY, 0.5, 92) ;
	check_latency (SRC_SINC_MEDIUM_QUALITY, 0.25, 184) ;
#endif
#ifdef ENABLE_SINC_BEST_CONVERTER
	check_latency (SRC_SINC_BEST_QUALITY, 1.0, 144) ;
	check_latency (SRC_SINC_BEST_QUALITY, 4.0, 144) ;
	check_latency (SRC_SINC_BEST_QUALITY, 0.5, 287) ;
	check_latency (SRC_SINC_BEST_QUALITY, 0.25, 573) ;
#endif

	/*
	** The returned value follows the ratio passed to each query, on the same
	** unprocessed state, with no call to src_process() in between.
	*/
	state = src_new (SRC_LINEAR, 2, &error) ;
	if (state == NULL)
	{	printf ("\n\nLine %d : src_new failed : %s.\n\n", __LINE__, src_strerror (error)) ;
		exit (1) ;
		} ;
	got = src_get_latency (state, 1.0) ;
	if (got != 1)
	{	printf ("\n\nLine %d : fresh linear converter latency should be 1, got %d.\n\n", __LINE__, got) ;
		exit (1) ;
		} ;
	src_delete (state) ;

#ifdef ENABLE_SINC_FAST_CONVERTER
	state = src_new (SRC_SINC_FASTEST, 2, &error) ;
	if (state == NULL)
	{	printf ("\n\nLine %d : src_new failed : %s.\n\n", __LINE__, src_strerror (error)) ;
		exit (1) ;
		} ;
	int d1 = src_get_latency (state, 1.0) ;
	int d2 = src_get_latency (state, 0.5) ;
	int d3 = src_get_latency (state, 1.0) ;
	if (! (d1 == 20 && d2 == 39 && d3 == 20))
	{	printf ("\n\nLine %d : latency should track ratio changes: %d, %d, %d.\n\n",
				__LINE__, d1, d2, d3) ;
		exit (1) ;
		} ;

	/* Repeated queries must be stable (the function is read-only). */
	for (int k = 0 ; k < 10000 ; k++)
	{	if (src_get_latency (state, 0.25) != 78)
		{	printf ("\n\nLine %d : repeated query returned an unstable value at iteration %d.\n\n",
					__LINE__, k) ;
			exit (1) ;
			} ;
		} ;
	src_delete (state) ;
#endif

	/*
	** Querying the latency must not change the audio produced. Process the
	** same input twice, once with interleaved latency queries, and require the
	** output to match bit for bit.
	*/
	static float in [4096] ;
	static float out_a [8192], out_b [8192] ;
	static int converters [] = { SRC_ZERO_ORDER_HOLD, SRC_LINEAR
#ifdef ENABLE_SINC_FAST_CONVERTER
		, SRC_SINC_FASTEST
#endif
#ifdef ENABLE_SINC_MEDIUM_CONVERTER
		, SRC_SINC_MEDIUM_QUALITY
#endif
#ifdef ENABLE_SINC_BEST_CONVERTER
		, SRC_SINC_BEST_QUALITY
#endif
		} ;
	static double out_ratios [] = { 1.0, 2.0, 0.5, 0.25 } ;

	for (int n = 0 ; n < ARRAY_LEN (in) ; n++)
		in [n] = (float) sin (0.017 * n) + 0.25f * (float) sin (0.31 * n) ;

	for (int c = 0 ; c < ARRAY_LEN (converters) ; c++)
	{	for (int r = 0 ; r < ARRAY_LEN (out_ratios) ; r++)
		{	SRC_DATA data ;
			int frames [2] ;
			float *outs [2] = { out_a, out_b } ;

			for (int pass = 0 ; pass < 2 ; pass++)
			{	state = src_new (converters [c], 1, &error) ;
				if (state == NULL)
				{	printf ("\n\nLine %d : src_new failed : %s.\n\n", __LINE__, src_strerror (error)) ;
					exit (1) ;
					} ;

				memset (&data, 0, sizeof (data)) ;
				data.data_in = in ;
				data.input_frames = ARRAY_LEN (in) ;
				data.data_out = outs [pass] ;
				data.output_frames = ARRAY_LEN (out_a) ;
				data.end_of_input = 1 ;
				data.src_ratio = out_ratios [r] ;

				if (pass == 1 && src_get_latency (state, out_ratios [r]) < 0)
				{	printf ("\n\nLine %d : latency query failed.\n\n", __LINE__) ;
					exit (1) ;
					} ;

				if ((error = src_process (state, &data)))
				{	printf ("\n\nLine %d : src_process failed : %s.\n\n", __LINE__, src_strerror (error)) ;
					exit (1) ;
					} ;

				if (pass == 1 && src_get_latency (state, out_ratios [r]) < 0)
				{	printf ("\n\nLine %d : latency query after processing failed.\n\n", __LINE__) ;
					exit (1) ;
					} ;

				frames [pass] = (int) data.output_frames_gen ;
				src_delete (state) ;
				} ;

			if (frames [0] != frames [1] || memcmp (out_a, out_b, sizeof (float) * frames [0]) != 0)
			{	printf ("\n\nLine %d : converter %d ratio %g : latency queries changed the output.\n\n",
						__LINE__, converters [c], out_ratios [r]) ;
				exit (1) ;
				} ;
			} ;
		} ;

	puts ("\tok") ;

	return ;
} /* get_latency_test */
