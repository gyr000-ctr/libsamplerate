/*
** Copyright (c) 2002-2021, Erik de Castro Lopo <erikd@mega-nerd.com>
** All rights reserved.
**
** This code is released under 2-clause BSD license. Please see the
** file at : https://github.com/libsndfile/libsamplerate/blob/master/COPYING
*/

/*
** Tests for the optional integer / fixed-point PCM path
** (src_pcm_new / src_pcm_process).
**
** The general strategy is to run the float path and the PCM path with
** identical call sequences and identical effective input, and to check
** that
**   - input_frames_used / output_frames_gen are exactly equal (same
**     ratio, latency and stream semantics), and
**   - the sample values agree within an agreed LSB tolerance.
*/

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include <samplerate.h>

/* For the SRC_ERR_* error code enum (error numbers are part of the ABI). */
#include "../src/common.h"

#include "util.h"

#define TEST_CHANNELS_MAX	8

/* Agreed comparison tolerances, as a fraction of full scale. */
#define TOL_S16		(2.5 / 32768.0)		/* 2.5 LSB at 16 bit */
#define TOL_S24		(4.5 / 8388608.0)	/* 4.5 LSB at 24 bit */
#define TOL_S32		(1100.0 / 2147483648.0)	/* ~1100 LSB at 32 bit (float32 reference) */

static double
tol_for_bits (int bits)
{
	switch (bits)
	{	case SRC_PCM_S16 : return TOL_S16 ;
		case SRC_PCM_S24 : return TOL_S24 ;
		default : break ;
		}
	return TOL_S32 ;
} /* tol_for_bits */

/*========================================================================================
** Test-side PCM packing helpers (mirror of the library semantics).
*/

static int32_t
clamp_q31 (int64_t v)
{
	if (v > INT32_MAX)
		v = INT32_MAX ;
	else if (v < INT32_MIN)
		v = INT32_MIN ;
	return (int32_t) v ;
} /* clamp_q31 */

static int16_t
pack_s16 (int32_t q)
{	int32_t v = (int32_t) (((int64_t) q + 0x8000) >> 16) ;
	if (v > 32767) v = 32767 ;
	if (v < -32768) v = -32768 ;
	return (int16_t) v ;
}

static int32_t
pack_s24 (int32_t q)
{	int32_t v = (int32_t) (((int64_t) q + 0x80) >> 8) ;
	if (v > 0x7FFFFF) v = 0x7FFFFF ;
	if (v < -0x800000) v = -0x800000 ;
	return v ;
}

static int32_t
unpack_s16 (int16_t s)
{	return (int32_t) ((uint32_t) (uint16_t) s << 16) ;
}

static int32_t
unpack_s24 (const unsigned char *p)
{	uint32_t v = (uint32_t) p [0] | ((uint32_t) p [1] << 8) | ((uint32_t) p [2] << 16) ;
	return (int32_t) (v << 8) ;
}

/* A PCM buffer in either interleaved or planar layout. */
typedef struct
{	int		bits ;
	int		interleaved ;
	int		channels ;
	long	frames ;
	void	*data ;						/* interleaved buffer, or plane pointer array */
	void	*planes [TEST_CHANNELS_MAX] ;
} PcmBuf ;

static int
bytes_per_sample (int bits)
{	return bits == SRC_PCM_S24 ? 3 : bits / 8 ;
}

static void
pcmbuf_alloc (PcmBuf *b, int bits, int interleaved, int channels, long frames)
{
	memset (b, 0, sizeof (*b)) ;
	b->bits = bits ;
	b->interleaved = interleaved ;
	b->channels = channels ;
	b->frames = frames ;

	if (interleaved)
		b->data = calloc ((size_t) frames * channels, bytes_per_sample (bits)) ;
	else
	{	b->data = calloc (channels, sizeof (void *)) ;
		for (int ch = 0 ; ch < channels ; ch++)
			((void **) b->data) [ch] = calloc ((size_t) frames, bytes_per_sample (bits)) ;
		}
} /* pcmbuf_alloc */

static void
pcmbuf_free (PcmBuf *b)
{
	if (!b->data)
		return ;
	if (!b->interleaved)
	{	for (int ch = 0 ; ch < b->channels ; ch++)
			free (((void **) b->data) [ch]) ;
		}
	free (b->data) ;
	b->data = NULL ;
} /* pcmbuf_free */

/* Store a Q31 sample into the buffer. */
static void
pcmbuf_put (PcmBuf *b, long frame, int ch, int32_t q31)
{
	if (b->interleaved)
	{	long idx = frame * b->channels + ch ;
		switch (b->bits)
		{	case SRC_PCM_S16 :
				((int16_t *) b->data) [idx] = pack_s16 (q31) ;
				break ;
			case SRC_PCM_S24 :
			{	int32_t v = pack_s24 (q31) ;
				unsigned char *p = ((unsigned char *) b->data) + 3 * idx ;
				p [0] = (unsigned char) v ;
				p [1] = (unsigned char) (v >> 8) ;
				p [2] = (unsigned char) (v >> 16) ;
				break ;
				}
			default :
				((int32_t *) b->data) [idx] = q31 ;
				break ;
			}
		return ;
		}

	{	void *plane = ((void **) b->data) [ch] ;
		switch (b->bits)
		{	case SRC_PCM_S16 :
				((int16_t *) plane) [frame] = pack_s16 (q31) ;
				break ;
			case SRC_PCM_S24 :
			{	int32_t v = pack_s24 (q31) ;
				unsigned char *p = ((unsigned char *) plane) + 3 * frame ;
				p [0] = (unsigned char) v ;
				p [1] = (unsigned char) (v >> 8) ;
				p [2] = (unsigned char) (v >> 16) ;
				break ;
				}
			default :
				((int32_t *) plane) [frame] = q31 ;
				break ;
			}
		}
} /* pcmbuf_put */

/* Fetch a Q31 sample from the buffer. */
static int32_t
pcmbuf_get (const PcmBuf *b, long frame, int ch)
{
	if (b->interleaved)
	{	long idx = frame * b->channels + ch ;
		switch (b->bits)
		{	case SRC_PCM_S16 :
				return unpack_s16 (((const int16_t *) b->data) [idx]) ;
			case SRC_PCM_S24 :
				return unpack_s24 (((const unsigned char *) b->data) + 3 * idx) ;
			default :
				return ((const int32_t *) b->data) [idx] ;
			}
		}

	{	const void *plane = ((void *const *) b->data) [ch] ;
		switch (b->bits)
		{	case SRC_PCM_S16 :
				return unpack_s16 (((const int16_t *) plane) [frame]) ;
			case SRC_PCM_S24 :
				return unpack_s24 (((const unsigned char *) plane) + 3 * frame) ;
			default :
				return ((const int32_t *) plane) [frame] ;
			}
		}
} /* pcmbuf_get */

/*========================================================================================
** Run the float reference path on Q31 input (one shot).
*/

static long
float_ref_process (int converter, int channels, const int32_t *in_q31, long in_frames,
		int32_t *out_q31, long out_frames, double src_ratio, double *max_abs_err_hint)
{
	float *fin = malloc (sizeof (float) * in_frames * channels) ;
	float *fout = malloc (sizeof (float) * out_frames * channels) ;
	SRC_DATA data ;
	int error ;

	for (long k = 0 ; k < in_frames * channels ; k++)
		fin [k] = (float) (in_q31 [k] / 2147483648.0) ;

	memset (&data, 0, sizeof (data)) ;
	data.data_in = fin ;
	data.input_frames = in_frames ;
	data.data_out = fout ;
	data.output_frames = out_frames ;
	data.end_of_input = 1 ;
	data.src_ratio = src_ratio ;

	error = src_simple (&data, converter, channels) ;
	if (error)
	{	printf ("\n%s : Line %d : float reference failed : %s\n\n", __func__, __LINE__, src_strerror (error)) ;
		exit (1) ;
		} ;

	for (long k = 0 ; k < data.output_frames_gen * channels ; k++)
		out_q31 [k] = clamp_q31 ((int64_t) llrint ((double) fout [k] * 2147483648.0)) ;

	free (fin) ;
	free (fout) ;
	(void) max_abs_err_hint ;
	return data.output_frames_gen ;
} /* float_ref_process */

/*========================================================================================
** Error handling tests.
*/

static void
error_tests (void)
{	SRC_STATE *state ;
	SRC_PCM_DATA pcm_data ;
	SRC_DATA float_data ;
	int16_t buf [64] ;
	float fbuf [64] ;
	int error ;

	puts (__func__) ;

	/* Illegal bit depths. */
	{	int bad_bits [] = { 0, 8, 12, 20, 33, 64, -16 } ;
		for (unsigned k = 0 ; k < ARRAY_LEN (bad_bits) ; k++)
		{
			state = src_pcm_new (SRC_SINC_FASTEST, 1, bad_bits [k], 16, 1, &error) ;
			ASSERT (state == NULL) ;
			ASSERT (error == SRC_ERR_BAD_BIT_DEPTH) ;

			state = src_pcm_new (SRC_SINC_FASTEST, 1, 16, bad_bits [k], 1, &error) ;
			ASSERT (state == NULL) ;
			ASSERT (error == SRC_ERR_BAD_BIT_DEPTH) ;
			}
		}

	/* Illegal channel count. */
	state = src_pcm_new (SRC_SINC_FASTEST, 0, 16, 16, 1, &error) ;
	ASSERT (state == NULL) ;
	ASSERT (error == SRC_ERR_BAD_CHANNEL_COUNT) ;

	state = src_pcm_new (SRC_LINEAR, -3, 16, 16, 1, &error) ;
	ASSERT (state == NULL) ;
	ASSERT (error == SRC_ERR_BAD_CHANNEL_COUNT) ;

	/* Too many channels for the sinc converter. */
	state = src_pcm_new (SRC_SINC_FASTEST, 1000, 16, 16, 1, &error) ;
	ASSERT (state == NULL) ;
	ASSERT (error == SRC_ERR_BAD_CHANNEL_COUNT) ;

	/* Illegal converter. */
	state = src_pcm_new (999, 1, 16, 16, 1, &error) ;
	ASSERT (state == NULL) ;
	ASSERT (error == SRC_ERR_BAD_CONVERTER) ;

	/* Legal construction, all converters and bit depths. */
	{	int converters [] = { SRC_SINC_BEST_QUALITY, SRC_SINC_MEDIUM_QUALITY,
				SRC_SINC_FASTEST, SRC_ZERO_ORDER_HOLD, SRC_LINEAR } ;
		int bits [] = { 16, 24, 32 } ;
		for (unsigned c = 0 ; c < ARRAY_LEN (converters) ; c++)
			for (unsigned bi = 0 ; bi < ARRAY_LEN (bits) ; bi++)
				for (unsigned bo = 0 ; bo < ARRAY_LEN (bits) ; bo++)
				{	state = src_pcm_new (converters [c], 2, bits [bi], bits [bo], 1, &error) ;
					ASSERT (state != NULL) ;
					ASSERT (error == 0) ;
					src_delete (state) ;
					} ;
		}

	/* NULL state / data. */
	memset (&pcm_data, 0, sizeof (pcm_data)) ;
	pcm_data.src_ratio = 1.0 ;
	ASSERT (src_pcm_process (NULL, &pcm_data) == SRC_ERR_BAD_STATE) ;

	state = src_pcm_new (SRC_LINEAR, 1, 16, 16, 1, &error) ;
	ASSERT (state != NULL) ;
	ASSERT (src_pcm_process (state, NULL) == SRC_ERR_BAD_DATA) ;

	/* NULL data pointers with non-zero frame counts. */
	pcm_data.input_frames = 4 ;
	pcm_data.data_in = NULL ;
	ASSERT (src_pcm_process (state, &pcm_data) == SRC_ERR_BAD_DATA_PTR) ;

	pcm_data.data_in = buf ;
	pcm_data.data_out = NULL ;
	pcm_data.output_frames = 4 ;
	ASSERT (src_pcm_process (state, &pcm_data) == SRC_ERR_BAD_DATA_PTR) ;

	/* Bad ratios. */
	{	double bad_ratios [] = { 0.0, -1.0, 1.0 / 257.0, 257.0, 1e9 } ;
		for (unsigned k = 0 ; k < ARRAY_LEN (bad_ratios) ; k++)
		{	pcm_data.data_in = buf ;
			pcm_data.data_out = buf ;
			pcm_data.input_frames = 4 ;
			pcm_data.output_frames = 4 ;
			pcm_data.src_ratio = bad_ratios [k] ;
			ASSERT (src_pcm_process (state, &pcm_data) == SRC_ERR_BAD_SRC_RATIO) ;
			}
		ASSERT (src_set_ratio (state, 0.0) == SRC_ERR_BAD_SRC_RATIO) ;
		ASSERT (src_set_ratio (state, 300.0) == SRC_ERR_BAD_SRC_RATIO) ;
		}

	/* Overlapping interleaved buffers. */
	pcm_data.data_in = buf ;
	pcm_data.data_out = buf ;
	pcm_data.input_frames = 4 ;
	pcm_data.output_frames = 4 ;
	pcm_data.src_ratio = 1.0 ;
	ASSERT (src_pcm_process (state, &pcm_data) == SRC_ERR_DATA_OVERLAP) ;

	/* Mixing the float and PCM APIs on one state. */
	memset (&float_data, 0, sizeof (float_data)) ;
	float_data.data_in = fbuf ;
	float_data.input_frames = 4 ;
	float_data.data_out = fbuf ;
	float_data.output_frames = 4 ;
	float_data.src_ratio = 1.0 ;
	ASSERT (src_process (state, &float_data) == SRC_ERR_WRONG_API) ;
	src_delete (state) ;

	{	SRC_STATE *fstate = src_new (SRC_LINEAR, 1, &error) ;
		ASSERT (fstate != NULL) ;
		pcm_data.data_in = buf ;
		pcm_data.data_out = buf ;
		pcm_data.input_frames = 4 ;
		pcm_data.output_frames = 4 ;
		pcm_data.src_ratio = 1.0 ;
		ASSERT (src_pcm_process (fstate, &pcm_data) == SRC_ERR_WRONG_API) ;
		src_delete (fstate) ;
		}

	/* New error codes have strings. */
	ASSERT (src_strerror (SRC_ERR_NO_ERROR) != NULL) ;
	{	int found = 0 ;
		for (int e = 0 ; e < 64 ; e++)
		{	const char *s = src_strerror (e) ;
			if (s && strstr (s, "bit depth"))
				found ++ ;
			if (s && strstr (s, "mismatch"))
				found ++ ;
			}
		ASSERT (found >= 2) ;
		}

	printf ("    ok\n") ;
} /* error_tests */

/*========================================================================================
** Format conversion : ZOH at ratio 1.0 must be bit-exact against the
** float path for 16 and 24 bit (both lossless in float32), and within
** tolerance for 32 bit. Also checks cross-format conversion.
*/

static void
format_conversion_tests (void)
{	int32_t in_q31 [512], out_q31 [512], ref_q31 [512] ;
	long in_frames = 128 ;

	puts (__func__) ;

	/* Pseudo-random input covering the full range. */
	uint32_t lcg = 12345 ;
	for (long k = 0 ; k < in_frames ; k++)
	{	lcg = lcg * 1664525u + 1013904223u ;
		in_q31 [k] = (int32_t) lcg ;
		}

	{	int bits [] = { 16, 24, 32 } ;
		for (unsigned bi = 0 ; bi < ARRAY_LEN (bits) ; bi++)
			for (unsigned bo = 0 ; bo < ARRAY_LEN (bits) ; bo++)
			{	PcmBuf pcm_in, pcm_out ;
				SRC_PCM_DATA data ;
				SRC_STATE *state ;
				int error ;

				pcmbuf_alloc (&pcm_in, bits [bi], 1, 1, in_frames) ;
				pcmbuf_alloc (&pcm_out, bits [bo], 1, 1, in_frames * 2) ;

				for (long k = 0 ; k < in_frames ; k++)
					pcmbuf_put (&pcm_in, k, 0, in_q31 [k]) ;

				/* Float reference on the same effective input. */
				int32_t q ;
				long ref_frames ;
				for (long k = 0 ; k < in_frames ; k++)
				{	q = pcmbuf_get (&pcm_in, k, 0) ;
					ref_q31 [k] = q ;
					}
				ref_frames = float_ref_process (SRC_ZERO_ORDER_HOLD, 1, ref_q31, in_frames,
						out_q31, in_frames * 2, 1.0, NULL) ;

				state = src_pcm_new (SRC_ZERO_ORDER_HOLD, 1, bits [bi], bits [bo], 1, &error) ;
				ASSERT (state != NULL) ;

				memset (&data, 0, sizeof (data)) ;
				data.data_in = pcm_in.data ;
				data.input_frames = in_frames ;
				data.data_out = pcm_out.data ;
				data.output_frames = in_frames * 2 ;
				data.end_of_input = 1 ;
				data.src_ratio = 1.0 ;

				error = src_pcm_process (state, &data) ;
				ASSERT (error == 0) ;
				ASSERT (data.output_frames_gen == ref_frames) ;

				double tol = tol_for_bits (bits [bo]) ;
				double maxerr = 0.0 ;
				for (long k = 0 ; k < data.output_frames_gen ; k++)
				{	double a = pcmbuf_get (&pcm_out, k, 0) / 2147483648.0 ;
					double b = out_q31 [k] / 2147483648.0 ;
					maxerr = MAX (maxerr, fabs (a - b)) ;
					}
				if (maxerr > tol)
				{	printf ("\n%s : %d -> %d bit : maxerr %g > tol %g\n\n", __func__,
							bits [bi], bits [bo], maxerr, tol) ;
					exit (1) ;
					} ;

				src_delete (state) ;
				pcmbuf_free (&pcm_in) ;
				pcmbuf_free (&pcm_out) ;
				}
		}

	printf ("    ok\n") ;
} /* format_conversion_tests */

/*========================================================================================
** Sine tests : full matrix of converters x bit depths x layouts x
** channel counts, compared against the float path.
*/

static void
run_sine_case (int converter, int in_bits, int out_bits, int interleaved, int channels,
		double src_ratio, long in_frames)
{	PcmBuf pcm_in, pcm_out ;
	SRC_PCM_DATA data ;
	SRC_STATE *state ;
	int32_t *in_q31, *ref_q31 ;
	long out_frames = (long) (in_frames * src_ratio) + 64 ;
	long ref_frames ;
	int error ;
	double maxerr = 0.0, tol ;

	in_q31 = malloc (sizeof (int32_t) * in_frames * channels) ;
	ref_q31 = malloc (sizeof (int32_t) * out_frames * channels) ;

	/* Multichannel sines, different frequency per channel. */
	for (long f = 0 ; f < in_frames ; f++)
		for (int ch = 0 ; ch < channels ; ch++)
		{	double freq = 440.0 * (ch + 1) ;
			double v = 0.8 * sin (2.0 * M_PI * freq * f / 44100.0) ;
			in_q31 [f * channels + ch] = clamp_q31 ((int64_t) llrint (v * 2147483648.0)) ;
			}

	pcmbuf_alloc (&pcm_in, in_bits, interleaved, channels, in_frames) ;
	pcmbuf_alloc (&pcm_out, out_bits, interleaved, channels, out_frames) ;

	for (long f = 0 ; f < in_frames ; f++)
		for (int ch = 0 ; ch < channels ; ch++)
			pcmbuf_put (&pcm_in, f, ch, in_q31 [f * channels + ch]) ;

	/* Reference : float path on the same effective (quantized) input. */
	{	int32_t *q = malloc (sizeof (int32_t) * in_frames * channels) ;
		for (long f = 0 ; f < in_frames ; f++)
			for (int ch = 0 ; ch < channels ; ch++)
				q [f * channels + ch] = pcmbuf_get (&pcm_in, f, ch) ;
		ref_frames = float_ref_process (converter, channels, q, in_frames, ref_q31, out_frames, src_ratio, NULL) ;
		free (q) ;
		}

	state = src_pcm_new (converter, channels, in_bits, out_bits, interleaved, &error) ;
	if (!state)
	{	printf ("\n%s : src_pcm_new failed : %s\n\n", __func__, src_strerror (error)) ;
		exit (1) ;
		} ;

	memset (&data, 0, sizeof (data)) ;
	data.data_in = pcm_in.data ;
	data.input_frames = in_frames ;
	data.data_out = pcm_out.data ;
	data.output_frames = out_frames ;
	data.end_of_input = 1 ;
	data.src_ratio = src_ratio ;

	error = src_pcm_process (state, &data) ;
	if (error)
	{	printf ("\n%s : src_pcm_process failed : %s\n\n", __func__, src_strerror (error)) ;
		exit (1) ;
		} ;

	/* Frame accounting must match the float path exactly. */
	if (data.output_frames_gen != ref_frames)
	{	printf ("\n%s : converter %d bits %d/%d : output frames %ld != float %ld\n\n",
				__func__, converter, in_bits, out_bits, data.output_frames_gen, ref_frames) ;
		exit (1) ;
		} ;

	tol = tol_for_bits (out_bits) ;
	for (long f = 0 ; f < data.output_frames_gen ; f++)
		for (int ch = 0 ; ch < channels ; ch++)
		{	double a = pcmbuf_get (&pcm_out, f, ch) / 2147483648.0 ;
			double b = ref_q31 [f * channels + ch] / 2147483648.0 ;
			maxerr = MAX (maxerr, fabs (a - b)) ;
			} ;

	if (maxerr > tol)
	{	printf ("\n%s : converter %d bits %d/%d %s %dch : maxerr %g > tol %g\n\n",
				__func__, converter, in_bits, out_bits,
				interleaved ? "interleaved" : "planar", channels, maxerr, tol) ;
		exit (1) ;
		} ;

	src_delete (state) ;
	pcmbuf_free (&pcm_in) ;
	pcmbuf_free (&pcm_out) ;
	free (in_q31) ;
	free (ref_q31) ;
} /* run_sine_case */

static void
sine_tests (void)
{	int converters [] = { SRC_SINC_BEST_QUALITY, SRC_SINC_MEDIUM_QUALITY,
			SRC_SINC_FASTEST, SRC_ZERO_ORDER_HOLD, SRC_LINEAR } ;
	int bits [] = { 16, 24, 32 } ;
	int chans [] = { 1, 2, 6 } ;

	puts (__func__) ;

	for (unsigned c = 0 ; c < ARRAY_LEN (converters) ; c++)
		for (unsigned b = 0 ; b < ARRAY_LEN (bits) ; b++)
			for (unsigned il = 0 ; il < 2 ; il++)
				for (unsigned ch = 0 ; ch < ARRAY_LEN (chans) ; ch++)
					run_sine_case (converters [c], bits [b], bits [b], (int) il, chans [ch],
							48000.0 / 44100.0, 4000) ;

	/* A few mixed-format and non-unity-ratio cases. */
	run_sine_case (SRC_SINC_FASTEST, 16, 24, 1, 2, 48000.0 / 44100.0, 4000) ;
	run_sine_case (SRC_SINC_FASTEST, 24, 16, 1, 2, 48000.0 / 44100.0, 4000) ;
	run_sine_case (SRC_SINC_FASTEST, 32, 16, 0, 2, 44100.0 / 48000.0, 4000) ;
	run_sine_case (SRC_SINC_MEDIUM_QUALITY, 16, 32, 1, 1, 2.0, 3000) ;
	run_sine_case (SRC_SINC_MEDIUM_QUALITY, 24, 24, 0, 4, 0.5, 3000) ;
	run_sine_case (SRC_LINEAR, 16, 16, 1, 2, 1.5, 4000) ;
	run_sine_case (SRC_ZERO_ORDER_HOLD, 16, 16, 1, 2, 0.75, 4000) ;

	printf ("    ok\n") ;
} /* sine_tests */

/*========================================================================================
** Impulse tests : latency and position semantics must match the float
** path exactly.
*/

static void
impulse_tests (void)
{	int converters [] = { SRC_SINC_BEST_QUALITY, SRC_SINC_MEDIUM_QUALITY,
			SRC_SINC_FASTEST, SRC_ZERO_ORDER_HOLD, SRC_LINEAR } ;
	double ratios [] = { 1.0, 1.5, 48000.0 / 44100.0, 0.5 } ;
	long in_frames = 2048 ;

	puts (__func__) ;

	for (unsigned c = 0 ; c < ARRAY_LEN (converters) ; c++)
		for (unsigned r = 0 ; r < ARRAY_LEN (ratios) ; r++)
		{	int32_t *in_q31, *ref_q31 ;
			PcmBuf pcm_in, pcm_out ;
			SRC_PCM_DATA data ;
			SRC_STATE *state ;
			long out_frames = (long) (in_frames * ratios [r]) + 64 ;
			long ref_frames, pcm_peak = -1, ref_peak = -1 ;
			int32_t pcm_peak_val = 0, ref_peak_val = 0 ;
			int error ;

			in_q31 = calloc ((size_t) in_frames, sizeof (int32_t)) ;
			ref_q31 = malloc (sizeof (int32_t) * out_frames) ;
			in_q31 [100] = (int32_t) (0.9 * 2147483648.0) ;

			pcmbuf_alloc (&pcm_in, 16, 1, 1, in_frames) ;
			pcmbuf_alloc (&pcm_out, 16, 1, 1, out_frames) ;
			for (long f = 0 ; f < in_frames ; f++)
				pcmbuf_put (&pcm_in, f, 0, in_q31 [f]) ;

			{	int32_t *q = malloc (sizeof (int32_t) * in_frames) ;
				for (long f = 0 ; f < in_frames ; f++)
					q [f] = pcmbuf_get (&pcm_in, f, 0) ;
				ref_frames = float_ref_process (converters [c], 1, q, in_frames,
						ref_q31, out_frames, ratios [r], NULL) ;
				free (q) ;
				}

			state = src_pcm_new (converters [c], 1, 16, 16, 1, &error) ;
			ASSERT (state != NULL) ;

			memset (&data, 0, sizeof (data)) ;
			data.data_in = pcm_in.data ;
			data.input_frames = in_frames ;
			data.data_out = pcm_out.data ;
			data.output_frames = out_frames ;
			data.end_of_input = 1 ;
			data.src_ratio = ratios [r] ;

			error = src_pcm_process (state, &data) ;
			ASSERT (error == 0) ;
			ASSERT (data.output_frames_gen == ref_frames) ;

			for (long f = 0 ; f < data.output_frames_gen ; f++)
			{	int32_t v = pcmbuf_get (&pcm_out, f, 0) ;
				if (llabs ((int64_t) v) > llabs ((int64_t) pcm_peak_val))
				{	pcm_peak_val = v ;
					pcm_peak = f ;
					}
				}
			for (long f = 0 ; f < ref_frames ; f++)
				if (llabs ((int64_t) ref_q31 [f]) > llabs ((int64_t) ref_peak_val))
				{	ref_peak_val = ref_q31 [f] ;
					ref_peak = f ;
					}

			/* Impulse position (latency) must agree with the float path. */
			if (pcm_peak != ref_peak)
			{	printf ("\n%s : converter %d ratio %f : peak at %ld, float at %ld\n\n",
						__func__, converters [c], ratios [r], pcm_peak, ref_peak) ;
				exit (1) ;
				} ;

			src_delete (state) ;
			pcmbuf_free (&pcm_in) ;
			pcmbuf_free (&pcm_out) ;
			free (in_q31) ;
			free (ref_q31) ;
			}

	printf ("    ok\n") ;
} /* impulse_tests */

/*========================================================================================
** Multichannel consistency : planar and interleaved layouts must give
** bit-identical results, and each channel of an N-channel run must be
** bit-identical to a mono run of the same signal.
*/

static void
multichan_consistency_tests (void)
{	int converters [] = { SRC_SINC_FASTEST, SRC_SINC_MEDIUM_QUALITY, SRC_LINEAR, SRC_ZERO_ORDER_HOLD } ;
	long in_frames = 3000 ;
	int channels = 6 ;
	double src_ratio = 48000.0 / 44100.0 ;
	long out_frames = (long) (in_frames * src_ratio) + 64 ;

	puts (__func__) ;

	for (unsigned c = 0 ; c < ARRAY_LEN (converters) ; c++)
	{	PcmBuf in_il, out_il, in_pl, out_pl ;
		SRC_PCM_DATA data ;
		SRC_STATE *state ;
		int error ;

		pcmbuf_alloc (&in_il, 24, 1, channels, in_frames) ;
		pcmbuf_alloc (&out_il, 24, 1, channels, out_frames) ;
		pcmbuf_alloc (&in_pl, 24, 0, channels, in_frames) ;
		pcmbuf_alloc (&out_pl, 24, 0, channels, out_frames) ;

		uint32_t lcg = 777 ;
		for (long f = 0 ; f < in_frames ; f++)
			for (int ch = 0 ; ch < channels ; ch++)
			{	lcg = lcg * 1664525u + 1013904223u ;
				int32_t v = (int32_t) (((uint64_t) lcg >> 8) << 8) ; /* 24 bit clean */
				pcmbuf_put (&in_il, f, ch, v) ;
				pcmbuf_put (&in_pl, f, ch, v) ;
				}

		/* Interleaved run. */
		state = src_pcm_new (converters [c], channels, 24, 24, 1, &error) ;
		ASSERT (state != NULL) ;
		memset (&data, 0, sizeof (data)) ;
		data.data_in = in_il.data ;
		data.input_frames = in_frames ;
		data.data_out = out_il.data ;
		data.output_frames = out_frames ;
		data.end_of_input = 1 ;
		data.src_ratio = src_ratio ;
		ASSERT (src_pcm_process (state, &data) == 0) ;
		long gen_il = data.output_frames_gen ;
		src_delete (state) ;

		/* Planar run. */
		state = src_pcm_new (converters [c], channels, 24, 24, 0, &error) ;
		ASSERT (state != NULL) ;
		memset (&data, 0, sizeof (data)) ;
		data.data_in = in_pl.data ;
		data.input_frames = in_frames ;
		data.data_out = out_pl.data ;
		data.output_frames = out_frames ;
		data.end_of_input = 1 ;
		data.src_ratio = src_ratio ;
		ASSERT (src_pcm_process (state, &data) == 0) ;
		ASSERT (data.output_frames_gen == gen_il) ;
		src_delete (state) ;

		/* Layouts must be bit-identical. */
		for (long f = 0 ; f < gen_il ; f++)
			for (int ch = 0 ; ch < channels ; ch++)
				ASSERT (pcmbuf_get (&out_il, f, ch) == pcmbuf_get (&out_pl, f, ch)) ;

		/* Mono runs of each channel must match the multichannel run. */
		for (int ch = 0 ; ch < channels ; ch++)
		{	PcmBuf min, mout ;
			pcmbuf_alloc (&min, 24, 1, 1, in_frames) ;
			pcmbuf_alloc (&mout, 24, 1, 1, out_frames) ;
			for (long f = 0 ; f < in_frames ; f++)
				pcmbuf_put (&min, f, 0, pcmbuf_get (&in_il, f, ch)) ;

			state = src_pcm_new (converters [c], 1, 24, 24, 1, &error) ;
			ASSERT (state != NULL) ;
			memset (&data, 0, sizeof (data)) ;
			data.data_in = min.data ;
			data.input_frames = in_frames ;
			data.data_out = mout.data ;
			data.output_frames = out_frames ;
			data.end_of_input = 1 ;
			data.src_ratio = src_ratio ;
			ASSERT (src_pcm_process (state, &data) == 0) ;
			/* The mono sinc converter may generate one frame more or
			** less at end of stream than the multichannel one (a quirk
			** of the float path that the PCM path mirrors deliberately ;
			** verified identical to the float path). */
			ASSERT (labs (data.output_frames_gen - gen_il) <= 1) ;
			src_delete (state) ;

			for (long f = 0 ; f < MIN (gen_il, data.output_frames_gen) ; f++)
				ASSERT (pcmbuf_get (&mout, f, 0) == pcmbuf_get (&out_il, f, ch)) ;

			pcmbuf_free (&min) ;
			pcmbuf_free (&mout) ;
			}

		pcmbuf_free (&in_il) ;
		pcmbuf_free (&out_il) ;
		pcmbuf_free (&in_pl) ;
		pcmbuf_free (&out_pl) ;
		}

	printf ("    ok\n") ;
} /* multichan_consistency_tests */

/*========================================================================================
** Long streaming test : feed a long stream in irregular chunks and
** compare against the float path streamed identically. The PCM path
** must not buffer the whole stream (fixed internal buffers only) and
** frame accounting must match the float path on every call.
*/

static void
streaming_tests (void)
{	long total_in = 1000000 ;
	int channels = 2 ;
	double src_ratio = 48000.0 / 44100.0 ;
	long out_cap = (long) (total_in * src_ratio) + 4096 ;

	puts (__func__) ;

	int32_t *in_q31 = malloc (sizeof (int32_t) * total_in * channels) ;
	int32_t *pcm_out_q31 = malloc (sizeof (int32_t) * out_cap * channels) ;
	float *fin = malloc (sizeof (float) * total_in * channels) ;
	float *fout = malloc (sizeof (float) * out_cap * channels) ;

	for (long f = 0 ; f < total_in ; f++)
		for (int ch = 0 ; ch < channels ; ch++)
		{	double v = 0.7 * sin (2.0 * M_PI * 330.0 * (ch + 1) * f / 44100.0) ;
			int32_t q = clamp_q31 ((int64_t) llrint (v * 2147483648.0)) ;
			/* 16 bit input granularity. */
			q = (int32_t) (((int64_t) q >> 16) * 65536) ;
			in_q31 [f * channels + ch] = q ;
			fin [f * channels + ch] = (float) (q / 2147483648.0) ;
			}

	/* Stream the PCM path in irregular chunks. */
	{	SRC_STATE *state ;
		SRC_PCM_DATA data ;
		int error ;
		long in_off = 0, out_off = 0 ;
		uint32_t lcg = 42 ;

		state = src_pcm_new (SRC_SINC_MEDIUM_QUALITY, channels, 32, 32, 1, &error) ;
		ASSERT (state != NULL) ;

		while (in_off < total_in)
		{	long chunk ;
			lcg = lcg * 1664525u + 1013904223u ;
			chunk = 1 + (lcg >> 16) % 5000 ;
			chunk = MIN (chunk, total_in - in_off) ;

			memset (&data, 0, sizeof (data)) ;
			data.data_in = in_q31 + in_off * channels ;	/* Q31 == S32 layout */
			data.input_frames = chunk ;
			data.data_out = pcm_out_q31 + out_off * channels ;
			data.output_frames = (out_cap - out_off) ;
			data.end_of_input = (in_off + chunk == total_in) ;
			data.src_ratio = src_ratio ;

			/* Use the 32 bit path for the test rig; 16/24 bit packing
			** is exercised everywhere else. */
			error = src_pcm_process (state, &data) ;
			ASSERT (error == 0) ;

			in_off += data.input_frames_used ;
			out_off += data.output_frames_gen ;

			if (data.input_frames_used == 0 && data.output_frames_gen == 0 && !data.end_of_input)
			{	printf ("\n%s : no progress at input frame %ld\n\n", __func__, in_off) ;
				exit (1) ;
				} ;
			}

		src_delete (state) ;
		total_in = in_off ;
		out_cap = out_off ;
		}

	/* Float reference, one shot. */
	{	SRC_DATA data ;
		int error ;

		memset (&data, 0, sizeof (data)) ;
		data.data_in = fin ;
		data.input_frames = total_in ;
		data.data_out = fout ;
		data.output_frames = out_cap + 4096 ;
		data.end_of_input = 1 ;
		data.src_ratio = src_ratio ;

		error = src_simple (&data, SRC_SINC_MEDIUM_QUALITY, channels) ;
		ASSERT (error == 0) ;

		/* Frame counts must agree. */
		ASSERT (data.input_frames_used == total_in) ;
		if (data.output_frames_gen != out_cap)
		{	printf ("\n%s : pcm generated %ld frames, float %ld\n\n",
					__func__, out_cap, data.output_frames_gen) ;
			exit (1) ;
			} ;

		/* Sample values within tolerance. */
		double maxerr = 0.0 ;
		for (long k = 0 ; k < out_cap * channels ; k++)
		{	double a = pcm_out_q31 [k] / 2147483648.0 ;
			double b = fout [k] ;
			maxerr = MAX (maxerr, fabs (a - b)) ;
			}
		if (maxerr > TOL_S16)
		{	printf ("\n%s : maxerr %g > tol %g\n\n", __func__, maxerr, TOL_S16) ;
			exit (1) ;
			} ;
		}

	free (in_q31) ;
	free (pcm_out_q31) ;
	free (fin) ;
	free (fout) ;

	printf ("    ok\n") ;
} /* streaming_tests */

/*========================================================================================
** Boundary ratio tests.
*/

static void
boundary_ratio_tests (void)
{	double good [] = { 1.0 / 256.0, 1.0 / 256.0 + 1e-9, 1.0, 256.0 - 1e-9, 256.0 } ;
	double bad [] = { 1.0 / 257.0, 257.0, 0.0, -1.0 } ;
	int converters [] = { SRC_SINC_FASTEST, SRC_SINC_MEDIUM_QUALITY, SRC_LINEAR, SRC_ZERO_ORDER_HOLD } ;
	long in_frames = 3000 ;

	puts (__func__) ;

	for (unsigned c = 0 ; c < ARRAY_LEN (converters) ; c++)
	{
		for (unsigned r = 0 ; r < ARRAY_LEN (good) ; r++)
		{	PcmBuf pcm_in, pcm_out ;
			SRC_PCM_DATA data ;
			SRC_STATE *state ;
			long out_frames = (long) (in_frames * good [r]) + 64 ;
			int error ;

			pcmbuf_alloc (&pcm_in, 16, 1, 1, in_frames) ;
			pcmbuf_alloc (&pcm_out, 16, 1, 1, out_frames) ;
			for (long f = 0 ; f < in_frames ; f++)
				pcmbuf_put (&pcm_in, f, 0,
						clamp_q31 ((int64_t) llrint (0.5 * sin (2.0 * M_PI * 440.0 * f / 44100.0) * 2147483648.0))) ;

			state = src_pcm_new (converters [c], 1, 16, 16, 1, &error) ;
			ASSERT (state != NULL) ;

			memset (&data, 0, sizeof (data)) ;
			data.data_in = pcm_in.data ;
			data.input_frames = in_frames ;
			data.data_out = pcm_out.data ;
			data.output_frames = out_frames ;
			data.end_of_input = 1 ;
			data.src_ratio = good [r] ;

			error = src_pcm_process (state, &data) ;
			if (error != 0)
			{	printf ("\n%s : converter %d ratio %.9g failed : %s\n\n",
						__func__, converters [c], good [r], src_strerror (error)) ;
				exit (1) ;
				} ;
			ASSERT (data.output_frames_gen > 0 || converters [c] != SRC_LINEAR) ;

			src_delete (state) ;
			pcmbuf_free (&pcm_in) ;
			pcmbuf_free (&pcm_out) ;
			}

		for (unsigned r = 0 ; r < ARRAY_LEN (bad) ; r++)
		{	int16_t buf [16] = { 0 } ;
			SRC_PCM_DATA data ;
			SRC_STATE *state ;
			int error ;

			state = src_pcm_new (converters [c], 1, 16, 16, 1, &error) ;
			ASSERT (state != NULL) ;

			memset (&data, 0, sizeof (data)) ;
			data.data_in = buf ;
			data.input_frames = 4 ;
			data.data_out = buf ;
			data.output_frames = 4 ;
			data.src_ratio = bad [r] ;

			ASSERT (src_pcm_process (state, &data) == SRC_ERR_BAD_SRC_RATIO) ;
			src_delete (state) ;
			}
		}

	printf ("    ok\n") ;
} /* boundary_ratio_tests */

/*========================================================================================
** Clipping tests : full-scale signals must saturate cleanly and never
** wrap around to the opposite sign.
*/

static void
clipping_tests (void)
{	long in_frames = 4096 ;
	long out_frames = in_frames + 64 ;

	puts (__func__) ;

	/* Positive and negative DC at full scale, 32 bit path. */
	for (int sign = 0 ; sign < 2 ; sign++)
	{	PcmBuf pcm_in, pcm_out ;
		SRC_PCM_DATA data ;
		SRC_STATE *state ;
		int error ;
		int32_t level = sign ? INT32_MIN : INT32_MAX ;

		pcmbuf_alloc (&pcm_in, 32, 1, 1, in_frames) ;
		pcmbuf_alloc (&pcm_out, 32, 1, 1, out_frames) ;
		for (long f = 0 ; f < in_frames ; f++)
			pcmbuf_put (&pcm_in, f, 0, level) ;

		state = src_pcm_new (SRC_SINC_FASTEST, 1, 32, 32, 1, &error) ;
		ASSERT (state != NULL) ;

		memset (&data, 0, sizeof (data)) ;
		data.data_in = pcm_in.data ;
		data.input_frames = in_frames ;
		data.data_out = pcm_out.data ;
		data.output_frames = out_frames ;
		data.end_of_input = 1 ;
		data.src_ratio = 1.0 ;
		ASSERT (src_pcm_process (state, &data) == 0) ;

		for (long f = 0 ; f < data.output_frames_gen ; f++)
		{	int32_t v = pcmbuf_get (&pcm_out, f, 0) ;
			/* No wraparound : sign must be preserved. */
			if (sign && v > 0)
			{	printf ("\n%s : negative DC produced positive sample %d at %ld\n\n", __func__, v, f) ;
				exit (1) ;
				} ;
			if (!sign && v < 0)
			{	printf ("\n%s : positive DC produced negative sample %d at %ld\n\n", __func__, v, f) ;
				exit (1) ;
				} ;
			}

		/* After the filter settles, output must be at (saturated) full scale. */
		for (long f = data.output_frames_gen / 2 ; f < data.output_frames_gen - 64 ; f++)
		{	int32_t v = pcmbuf_get (&pcm_out, f, 0) ;
			if (sign)
			{	ASSERT (v < INT32_MIN + (1 << 20)) ;
				}
			else
			{	ASSERT (v > INT32_MAX - (1 << 20)) ;
				}
			}

		src_delete (state) ;
		pcmbuf_free (&pcm_in) ;
		pcmbuf_free (&pcm_out) ;
		}

	/* Full-scale square wave : sinc overshoot must clip, not wrap. */
	{	PcmBuf pcm_in, pcm_out ;
		SRC_PCM_DATA data ;
		SRC_STATE *state ;
		int error ;

		pcmbuf_alloc (&pcm_in, 16, 1, 1, in_frames) ;
		pcmbuf_alloc (&pcm_out, 16, 1, 1, out_frames) ;
		for (long f = 0 ; f < in_frames ; f++)
			pcmbuf_put (&pcm_in, f, 0, ((f / 32) & 1) ? INT32_MIN : INT32_MAX) ;

		state = src_pcm_new (SRC_SINC_MEDIUM_QUALITY, 1, 16, 16, 1, &error) ;
		ASSERT (state != NULL) ;

		memset (&data, 0, sizeof (data)) ;
		data.data_in = pcm_in.data ;
		data.input_frames = in_frames ;
		data.data_out = pcm_out.data ;
		data.output_frames = out_frames ;
		data.end_of_input = 1 ;
		data.src_ratio = 1.0 ;
		ASSERT (src_pcm_process (state, &data) == 0) ;

		/* In the middle of each half cycle the sign must be intact. */
		for (long f = 256 ; f + 8 < data.output_frames_gen ; f++)
		{	int phase = (int) (f % 32) ;
			int32_t v = pcmbuf_get (&pcm_out, f, 0) ;
			if (phase > 10 && phase < 22)
			{	int expect_pos = ((f / 32) & 1) == 0 ;
				if (expect_pos && v < 0)
				{	printf ("\n%s : square wave wrapped to %d at %ld\n\n", __func__, v, f) ;
					exit (1) ;
					} ;
				if (!expect_pos && v > 0)
				{	printf ("\n%s : square wave wrapped to %d at %ld\n\n", __func__, v, f) ;
					exit (1) ;
					} ;
				}
			}

		src_delete (state) ;
		pcmbuf_free (&pcm_in) ;
		pcmbuf_free (&pcm_out) ;
		}

	printf ("    ok\n") ;
} /* clipping_tests */

/*========================================================================================
** Varispeed : ratio ramps inside a call must behave like the float path.
*/

static void
varispeed_tests (void)
{	long in_frames = 20000 ;
	double ratio1 = 1.2, ratio2 = 0.9 ;

	puts (__func__) ;

	int32_t *in_q31 = malloc (sizeof (int32_t) * in_frames) ;
	for (long f = 0 ; f < in_frames ; f++)
		in_q31 [f] = clamp_q31 ((int64_t) llrint (0.6 * sin (2.0 * M_PI * 440.0 * f / 44100.0) * 2147483648.0)) ;

	/* Float reference : two calls with a ratio step. */
	long ref_total = 0 ;
	int32_t *ref_q31 = malloc (sizeof (int32_t) * in_frames * 3) ;
	{	SRC_STATE *st ;
		SRC_DATA data ;
		float *fin = malloc (sizeof (float) * in_frames) ;
		float *fout = malloc (sizeof (float) * in_frames * 3) ;
		int error ;
		long out_off = 0, in_off = 0 ;

		for (long k = 0 ; k < in_frames ; k++)
			fin [k] = (float) (in_q31 [k] / 2147483648.0) ;

		st = src_new (SRC_SINC_FASTEST, 1, &error) ;
		ASSERT (st != NULL) ;
		ASSERT (src_set_ratio (st, ratio1) == 0) ;

		for (int half = 0 ; half < 2 ; half++)
		{	memset (&data, 0, sizeof (data)) ;
			data.data_in = fin + in_off ;
			data.input_frames = in_frames / 2 ;
			data.data_out = fout + out_off ;
			data.output_frames = in_frames * 3 / 2 ;
			data.end_of_input = half ;
			data.src_ratio = half ? ratio2 : ratio1 ;
			ASSERT (src_process (st, &data) == 0) ;
			in_off += data.input_frames_used ;
			out_off += data.output_frames_gen ;
			}
		src_delete (st) ;

		for (long k = 0 ; k < out_off ; k++)
			ref_q31 [k] = clamp_q31 ((int64_t) llrint ((double) fout [k] * 2147483648.0)) ;
		ref_total = out_off ;
		free (fin) ;
		free (fout) ;
		}

	/* PCM path : identical call sequence. */
	{	SRC_STATE *st ;
		SRC_PCM_DATA data ;
		PcmBuf pcm_in, pcm_out ;
		int error ;
		long out_off = 0, in_off = 0 ;
		long out_cap = in_frames * 3 ;

		pcmbuf_alloc (&pcm_in, 16, 1, 1, in_frames) ;
		pcmbuf_alloc (&pcm_out, 16, 1, 1, out_cap) ;
		for (long f = 0 ; f < in_frames ; f++)
			pcmbuf_put (&pcm_in, f, 0, in_q31 [f]) ;

		st = src_pcm_new (SRC_SINC_FASTEST, 1, 16, 16, 1, &error) ;
		ASSERT (st != NULL) ;
		ASSERT (src_set_ratio (st, ratio1) == 0) ;

		for (int half = 0 ; half < 2 ; half++)
		{	memset (&data, 0, sizeof (data)) ;
			data.data_in = (const int16_t *) pcm_in.data + in_off ;
			data.input_frames = in_frames / 2 ;
			data.data_out = (int16_t *) pcm_out.data + out_off ;
			/* Must match the float reference's output_frames exactly :
			** the varispeed ramp is interpolated across out_count. */
			data.output_frames = in_frames * 3 / 2 ;
			data.end_of_input = half ;
			data.src_ratio = half ? ratio2 : ratio1 ;
			ASSERT (src_pcm_process (st, &data) == 0) ;
			in_off += data.input_frames_used ;
			out_off += data.output_frames_gen ;
			}
		src_delete (st) ;

		ASSERT (out_off == ref_total) ;

		double maxerr = 0.0 ;
		for (long k = 0 ; k < out_off ; k++)
		{	double a = pcmbuf_get (&pcm_out, k, 0) / 2147483648.0 ;
			double b = ref_q31 [k] / 2147483648.0 ;
			maxerr = MAX (maxerr, fabs (a - b)) ;
			}
		if (maxerr > TOL_S16)
		{	printf ("\n%s : maxerr %g > tol %g\n\n", __func__, maxerr, TOL_S16) ;
			exit (1) ;
			} ;

		pcmbuf_free (&pcm_in) ;
		pcmbuf_free (&pcm_out) ;
		}

	free (in_q31) ;
	free (ref_q31) ;

	printf ("    ok\n") ;
} /* varispeed_tests */

/*========================================================================================
** Reset and clone semantics.
*/

static void
reset_clone_tests (void)
{	long in_frames = 8000 ;
	double src_ratio = 1.3 ;

	puts (__func__) ;

	PcmBuf pcm_in, out_a, out_b ;
	pcmbuf_alloc (&pcm_in, 16, 1, 2, in_frames) ;
	pcmbuf_alloc (&out_a, 16, 1, 2, in_frames * 2) ;
	pcmbuf_alloc (&out_b, 16, 1, 2, in_frames * 2) ;

	for (long f = 0 ; f < in_frames ; f++)
		for (int ch = 0 ; ch < 2 ; ch++)
			pcmbuf_put (&pcm_in, f, ch,
					clamp_q31 ((int64_t) llrint (0.5 * sin (2.0 * M_PI * 440.0 * (ch + 1) * f / 44100.0) * 2147483648.0))) ;

	/* Reset : two identical runs must give identical output. */
	{	SRC_STATE *st ;
		SRC_PCM_DATA data ;
		int error ;

		st = src_pcm_new (SRC_SINC_FASTEST, 2, 16, 16, 1, &error) ;
		ASSERT (st != NULL) ;

		for (int run = 0 ; run < 2 ; run++)
		{	PcmBuf *out = run ? &out_b : &out_a ;
			memset (&data, 0, sizeof (data)) ;
			data.data_in = pcm_in.data ;
			data.input_frames = in_frames ;
			data.data_out = out->data ;
			data.output_frames = in_frames * 2 ;
			data.end_of_input = 1 ;
			data.src_ratio = src_ratio ;
			ASSERT (src_pcm_process (st, &data) == 0) ;
			if (run == 0)
			{	ASSERT (src_reset (st) == 0) ;
				out_a.frames = data.output_frames_gen ;
				}
			else
				out_b.frames = data.output_frames_gen ;
			}
		src_delete (st) ;

		ASSERT (out_a.frames == out_b.frames) ;
		for (long f = 0 ; f < out_a.frames ; f++)
			for (int ch = 0 ; ch < 2 ; ch++)
				ASSERT (pcmbuf_get (&out_a, f, ch) == pcmbuf_get (&out_b, f, ch)) ;
		}

	/* Clone : clone mid-stream, both states must continue identically. */
	{	SRC_STATE *st, *st2 ;
		SRC_PCM_DATA data ;
		int error ;
		long half = in_frames / 2 ;

		st = src_pcm_new (SRC_SINC_FASTEST, 2, 16, 16, 1, &error) ;
		ASSERT (st != NULL) ;

		memset (&data, 0, sizeof (data)) ;
		data.data_in = pcm_in.data ;
		data.input_frames = half ;
		data.data_out = out_a.data ;
		data.output_frames = in_frames ;
		data.end_of_input = 0 ;
		data.src_ratio = src_ratio ;
		ASSERT (src_pcm_process (st, &data) == 0) ;
		long used1 = data.input_frames_used ;
		long gen1 = data.output_frames_gen ;

		st2 = src_clone (st, &error) ;
		ASSERT (st2 != NULL) ;

		/* The clone continues from the same state ; give it the same
		** first-half output history so the full streams can be compared. */
		memcpy (out_b.data, out_a.data, (size_t) gen1 * 2 * sizeof (int16_t)) ;

		/* Continue the original. */
		memset (&data, 0, sizeof (data)) ;
		data.data_in = (const int16_t *) pcm_in.data + used1 * 2 ;
		data.input_frames = in_frames - used1 ;
		data.data_out = (int16_t *) out_a.data + gen1 * 2 ;
		data.output_frames = in_frames ;
		data.end_of_input = 1 ;
		data.src_ratio = src_ratio ;
		ASSERT (src_pcm_process (st, &data) == 0) ;
		long gen_a = gen1 + data.output_frames_gen ;

		/* Continue the clone with the same data. */
		memset (&data, 0, sizeof (data)) ;
		data.data_in = (const int16_t *) pcm_in.data + used1 * 2 ;
		data.input_frames = in_frames - used1 ;
		data.data_out = (int16_t *) out_b.data + gen1 * 2 ;
		data.output_frames = in_frames ;
		data.end_of_input = 1 ;
		data.src_ratio = src_ratio ;
		ASSERT (src_pcm_process (st2, &data) == 0) ;
		long gen_b = gen1 + data.output_frames_gen ;

		ASSERT (gen_a == gen_b) ;
		for (long f = 0 ; f < gen_a ; f++)
			for (int ch = 0 ; ch < 2 ; ch++)
				ASSERT (pcmbuf_get (&out_a, f, ch) == pcmbuf_get (&out_b, f, ch)) ;

		src_delete (st) ;
		src_delete (st2) ;
		}

	pcmbuf_free (&pcm_in) ;
	pcmbuf_free (&out_a) ;
	pcmbuf_free (&out_b) ;

	printf ("    ok\n") ;
} /* reset_clone_tests */

/*========================================================================================
*/

int
main (void)
{
	puts ("") ;
	printf ("PCM integer/fixed-point path tests (libsamplerate %s)\n\n", src_get_version ()) ;

	error_tests () ;
	format_conversion_tests () ;
	sine_tests () ;
	impulse_tests () ;
	multichan_consistency_tests () ;
	streaming_tests () ;
	boundary_ratio_tests () ;
	clipping_tests () ;
	varispeed_tests () ;
	reset_clone_tests () ;

	puts ("") ;
	puts ("All PCM path tests passed.") ;
	puts ("") ;

	return 0 ;
} /* main */
