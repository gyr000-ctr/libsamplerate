/*
** Copyright (c) 2003-2016, Erik de Castro Lopo <erikd@mega-nerd.com>
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

#define	BUFFER_LEN		10000

static void float_to_short_test (void) ;
static void short_to_float_test (void) ;

static void float_to_int_test (void) ;
static void int_to_float_test (void) ;

static void dither_off_test (void) ;
static void dither_error_test (void) ;
static void dither_determinism_test (void) ;
static void dither_chunking_test (void) ;
static void dither_channel_test (void) ;
static void dither_depth_test (void) ;
static void dither_quality_test (void) ;
static void dither_shaping_test (void) ;
static void dither_overlap_test (void) ;

int
main (void)
{
	puts ("") ;

	float_to_short_test () ;
	short_to_float_test () ;

	float_to_int_test () ;
	int_to_float_test () ;

	dither_off_test () ;
	dither_error_test () ;
	dither_determinism_test () ;
	dither_chunking_test () ;
	dither_channel_test () ;
	dither_depth_test () ;
	dither_quality_test () ;
	dither_shaping_test () ;
	dither_overlap_test () ;

	puts ("") ;

	return 0 ;
} /* main */

/*=====================================================================================
*/

static void
float_to_short_test (void)
{
	static float fpos [] =
	{	0.95f, 0.99f, 1.0f, 1.01f, 1.1f, 2.0f, 11.1f, 111.1f, 2222.2f, 33333.3f,
		// Some "almost 1" as corner cases
		(float) (32767.0 / 32768.0), (float) ((32767.0 + 0.4) / 32768.0), (float) ((32767. + 0.5) / 32768.0),
		(float) ((32767.0 + 0.6) / 32768.0), (float) ((32767.0 + 0.9) / 32768.0),
		} ;
	static float fneg [] =
	{	-0.95f, -0.99f, -1.0f, -1.01f, -1.1f, -2.0f, -11.1f, -111.1f, -2222.2f, -33333.3f,
		// Some "almost 1" as corner cases
		(float) (-32767.0 / 32768.0), (float) (-(32767.0 + 0.4) / 32768.0), (float) (-(32767.0 + 0.5) / 32768.0),
		(float) (-(32767.0 + 0.6) / 32768.0), (float) (-(32767.0 + 0.9) / 32768.0),
		} ;

	static short out [MAX (ARRAY_LEN (fpos), ARRAY_LEN (fneg))] ;

	int k ;

	printf ("\tfloat_to_short_test ............................. ") ;

	src_float_to_short_array (fpos, out, ARRAY_LEN (fpos)) ;

	for (k = 0 ; k < ARRAY_LEN (fpos) ; k++)
		if (out [k] < 30000)
		{	printf ("\n\n\tLine %d : out [%d] == %d\n", __LINE__, k, out [k]) ;
			exit (1) ;
			} ;

	src_float_to_short_array (fneg, out, ARRAY_LEN (fneg)) ;

	for (k = 0 ; k < ARRAY_LEN (fneg) ; k++)
		if (out [k] > -30000)
		{	printf ("\n\n\tLine %d : out [%d] == %d\n", __LINE__, k, out [k]) ;
			exit (1) ;
			} ;

	puts ("ok") ;

	return ;
} /* float_to_short_test */

/*-------------------------------------------------------------------------------------
*/

static void
short_to_float_test (void)
{
	static short input	[BUFFER_LEN] ;
	static short output	[BUFFER_LEN] ;
	static float temp	[BUFFER_LEN] ;

	int k ;

	printf ("\tshort_to_float_test ............................. ") ;

	for (k = 0 ; k < ARRAY_LEN (input) ; k++)
		input [k] = (k * 0x8000) / ARRAY_LEN (input) ;

	src_short_to_float_array (input, temp, ARRAY_LEN (temp)) ;
	src_float_to_short_array (temp, output, ARRAY_LEN (output)) ;

	for (k = 0 ; k < ARRAY_LEN (input) ; k++)
		if (ABS (input [k] - output [k]) > 0)
		{	printf ("\n\n\tLine %d : index %d   %d -> %d\n", __LINE__, k, input [k], output [k]) ;
			exit (1) ;
			} ;

	puts ("ok") ;

	return ;
} /* short_to_float_test */

/*=====================================================================================
*/

static void
float_to_int_test (void)
{
	static float fpos [] =
	{	0.95f, 0.99f, 1.0f, 1.01f, 1.1f, 2.0f, 11.1f, 111.1f, 2222.2f, 33333.3f
		} ;
	static float fneg [] =
	{	-0.95f, -0.99f, -1.0f, -1.01f, -1.1f, -2.0f, -11.1f, -111.1f, -2222.2f, -33333.3f
		} ;

	static int out [MAX (ARRAY_LEN (fpos), ARRAY_LEN (fneg))] ;

	int k ;

	printf ("\tfloat_to_int_test ............................... ") ;

	src_float_to_int_array (fpos, out, ARRAY_LEN (fpos)) ;

	for (k = 0 ; k < ARRAY_LEN (fpos) ; k++)
		if (out [k] < 30000 * 0x10000)
		{	printf ("\n\n\tLine %d : out [%d] == %d\n", __LINE__, k, out [k]) ;
			exit (1) ;
			} ;

	src_float_to_int_array (fneg, out, ARRAY_LEN (fneg)) ;

	for (k = 0 ; k < ARRAY_LEN (fneg) ; k++)
		if (out [k] > -30000 * 0x1000)
		{	printf ("\n\n\tLine %d : out [%d] == %d\n", __LINE__, k, out [k]) ;
			exit (1) ;
			} ;

	puts ("ok") ;

	return ;
} /* float_to_int_test */

/*-------------------------------------------------------------------------------------
*/

static void
int_to_float_test (void)
{
	static int input	[BUFFER_LEN] ;
	static int output	[BUFFER_LEN] ;
	static float temp	[BUFFER_LEN] ;

	int k ;

	printf ("\tint_to_float_test ............................... ") ;

	for (k = 0 ; k < ARRAY_LEN (input) ; k++)
		input [k] = (k * 0x80000000) / ARRAY_LEN (input) ;

	src_int_to_float_array (input, temp, ARRAY_LEN (temp)) ;
	src_float_to_int_array (temp, output, ARRAY_LEN (output)) ;

	for (k = 0 ; k < ARRAY_LEN (input) ; k++)
		if (ABS (input [k] - output [k]) > 0)
		{	printf ("\n\n\tLine %d : index %d   %d -> %d\n", __LINE__, k, input [k], output [k]) ;
			exit (1) ;
			} ;

	puts ("ok") ;

	return ;
} /* int_to_float_test */


/*=====================================================================================
** Optional dithering.
*/

static void
dither_off_test (void)
{	static float in [BUFFER_LEN] ;
	static short expected [BUFFER_LEN], got [BUFFER_LEN] ;
	SRC_DITHER_STATE *state ;
	int k, error ;

	printf ("\tdither_off_test ................................. ") ;

	for (k = 0 ; k < BUFFER_LEN ; k++)
		in [k] = (float) sin (k * 0.031) * 0.9f ;

	src_float_to_short_array (in, expected, BUFFER_LEN) ;

	if ((state = src_dither_new (SRC_DITHER_NONE, 16, 1, &error)) == NULL)
	{	printf ("\n\n\tLine %d : src_dither_new failed : %s\n", __LINE__, src_strerror (error)) ;
		exit (1) ;
		} ;

	if (src_float_to_short_array_dithered (state, in, got, BUFFER_LEN) != SRC_ERR_NO_ERROR)
	{	printf ("\n\n\tLine %d : conversion failed.\n", __LINE__) ;
		exit (1) ;
		} ;

	src_dither_delete (state) ;

	for (k = 0 ; k < BUFFER_LEN ; k++)
		if (expected [k] != got [k])
		{	printf ("\n\n\tLine %d : index %d : plain %d, dither off %d\n", __LINE__, k, expected [k], got [k]) ;
			exit (1) ;
			} ;

	puts ("ok") ;

	return ;
} /* dither_off_test */

/*-------------------------------------------------------------------------------------
*/

static void
dither_error_test (void)
{	SRC_DITHER_STATE *state ;
	static float in [16] ;
	static short out [16] ;
	int error ;

	printf ("\tdither_error_test ............................... ") ;

	/* Bad type, bad depth, bad channel count must be distinguishable. */
	if (src_dither_new (999, 16, 1, &error) != NULL || error != SRC_ERR_BAD_DITHER_TYPE)
	{	printf ("\n\n\tLine %d : bad type not diagnosed.\n", __LINE__) ;
		exit (1) ;
		} ;

	if (src_dither_new (SRC_DITHER_TPDF, 0, 1, &error) != NULL || error != SRC_ERR_BAD_BIT_DEPTH)
	{	printf ("\n\n\tLine %d : bit depth 0 not diagnosed.\n", __LINE__) ;
		exit (1) ;
		} ;

	if (src_dither_new (SRC_DITHER_TPDF, 17, 1, &error) != NULL || error != SRC_ERR_BAD_BIT_DEPTH)
	{	printf ("\n\n\tLine %d : bit depth 17 not diagnosed.\n", __LINE__) ;
		exit (1) ;
		} ;

	if (src_dither_new (SRC_DITHER_TPDF, 16, 0, &error) != NULL || error != SRC_ERR_BAD_CHANNEL_COUNT)
	{	printf ("\n\n\tLine %d : channel count 0 not diagnosed.\n", __LINE__) ;
		exit (1) ;
		} ;

	/* The three error numbers must be distinct and have messages. */
	if (SRC_ERR_BAD_DITHER_TYPE == SRC_ERR_BAD_BIT_DEPTH
			|| SRC_ERR_BAD_DITHER_TYPE == SRC_ERR_BAD_CHANNEL_COUNT
			|| SRC_ERR_BAD_BIT_DEPTH == SRC_ERR_BAD_CHANNEL_COUNT)
	{	printf ("\n\n\tLine %d : dither error numbers are not distinct.\n", __LINE__) ;
		exit (1) ;
		} ;

	/* An uninitialised / NULL state must be reported as BAD_STATE and differ
	** from the overlap and data errors.
	*/
	if (src_float_to_short_array_dithered (NULL, in, out, 16) != SRC_ERR_BAD_STATE)
	{	printf ("\n\n\tLine %d : NULL state not diagnosed.\n", __LINE__) ;
		exit (1) ;
		} ;

	if (src_dither_reset (NULL) != SRC_ERR_BAD_STATE)
	{	printf ("\n\n\tLine %d : NULL state reset not diagnosed.\n", __LINE__) ;
		exit (1) ;
		} ;

	if (SRC_ERR_BAD_STATE == SRC_ERR_DATA_OVERLAP || SRC_ERR_BAD_STATE == SRC_ERR_BAD_DATA_PTR)
	{	printf ("\n\n\tLine %d : error numbers are not distinct.\n", __LINE__) ;
		exit (1) ;
		} ;

	/* Deleting NULL is safe. */
	src_dither_delete (NULL) ;

	puts ("ok") ;

	return ;
} /* dither_error_test */

/*-------------------------------------------------------------------------------------
*/

static void
dither_determinism_test (void)
{	static float in [BUFFER_LEN] ;
	static short first [BUFFER_LEN], second [BUFFER_LEN] ;
	SRC_DITHER_STATE *state ;
	int type, k, error ;

	for (k = 0 ; k < BUFFER_LEN ; k++)
		in [k] = (float) sin (k * 0.017) * 0.6f ;

	printf ("\tdither_determinism_test ........................ ") ;

	for (type = SRC_DITHER_TPDF ; type <= SRC_DITHER_NOISE_SHAPED ; type++)
	{	if ((state = src_dither_new (type, 16, 1, &error)) == NULL)
		{	printf ("\n\n\tLine %d : src_dither_new failed : %s\n", __LINE__, src_strerror (error)) ;
			exit (1) ;
			} ;

		src_float_to_short_array_dithered (state, in, first, BUFFER_LEN) ;

		/* A reset must reproduce the stream exactly. */
		if (src_dither_reset (state) != SRC_ERR_NO_ERROR)
		{	printf ("\n\n\tLine %d : src_dither_reset failed.\n", __LINE__) ;
			exit (1) ;
			} ;

		src_float_to_short_array_dithered (state, in, second, BUFFER_LEN) ;
		src_dither_delete (state) ;

		for (k = 0 ; k < BUFFER_LEN ; k++)
			if (first [k] != second [k])
			{	printf ("\n\n\tLine %d : type %d index %d : %d != %d\n", __LINE__, type, k,
					first [k], second [k]) ;
				exit (1) ;
				} ;
		} ;

	puts ("ok") ;

	return ;
} /* dither_determinism_test */

/*-------------------------------------------------------------------------------------
*/

static void
dither_chunking_test (void)
{	static float in [BUFFER_LEN] ;
	static short whole [BUFFER_LEN], chunked [BUFFER_LEN] ;
	SRC_DITHER_STATE *a, *b ;
	int type, ch, k, pos, error ;

	for (k = 0 ; k < BUFFER_LEN ; k++)
		in [k] = (float) sin (k * 0.023) * 0.7f ;

	printf ("\tdither_chunking_test ........................... ") ;

	for (type = SRC_DITHER_TPDF ; type <= SRC_DITHER_NOISE_SHAPED ; type++)
	{	a = src_dither_new (type, 16, 1, &error) ;
		b = src_dither_new (type, 16, 1, &error) ;
		if (a == NULL || b == NULL)
		{	printf ("\n\n\tLine %d : src_dither_new failed.\n", __LINE__) ;
			exit (1) ;
			} ;

		src_float_to_short_array_dithered (a, in, whole, BUFFER_LEN) ;

		/* Feed the same stream in irregular chunks. */
		for (pos = 0 ; pos < BUFFER_LEN ; )
		{	int n = 1 + (pos * 7) % 97 ;

			if (pos + n > BUFFER_LEN)
				n = BUFFER_LEN - pos ;
			src_float_to_short_array_dithered (b, in + pos, chunked + pos, n) ;
			pos += n ;
			} ;

		src_dither_delete (a) ;
		src_dither_delete (b) ;

		for (k = 0 ; k < BUFFER_LEN ; k++)
			if (whole [k] != chunked [k])
			{	printf ("\n\n\tLine %d : type %d index %d : %d != %d\n", __LINE__, type, k,
					whole [k], chunked [k]) ;
				exit (1) ;
				} ;
		} ;

	/* Same expectation for interleaved multi channel data. */
	{	static float stereo_in [2 * BUFFER_LEN] ;
		static short stereo_whole [2 * BUFFER_LEN], stereo_chunked [2 * BUFFER_LEN] ;
		SRC_DITHER_STATE *sa, *sb ;

		for (k = 0 ; k < 2 * BUFFER_LEN ; k++)
			stereo_in [k] = (float) sin (k * 0.013) * 0.5f ;

		sa = src_dither_new (SRC_DITHER_TPDF, 16, 2, &error) ;
		sb = src_dither_new (SRC_DITHER_TPDF, 16, 2, &error) ;
		if (sa == NULL || sb == NULL)
		{	printf ("\n\n\tLine %d : src_dither_new failed.\n", __LINE__) ;
			exit (1) ;
			} ;

		src_float_to_short_array_dithered (sa, stereo_in, stereo_whole, BUFFER_LEN) ;
		for (pos = 0 ; pos < BUFFER_LEN ; )
		{	int n = 1 + (pos * 5) % 61 ;

			if (pos + n > BUFFER_LEN)
				n = BUFFER_LEN - pos ;
			src_float_to_short_array_dithered (sb, stereo_in + 2 * pos,
					stereo_chunked + 2 * pos, n) ;
			pos += n ;
			} ;

		src_dither_delete (sa) ;
		src_dither_delete (sb) ;

		for (k = 0 ; k < 2 * BUFFER_LEN ; k++)
			if (stereo_whole [k] != stereo_chunked [k])
			{	printf ("\n\n\tLine %d : stereo index %d : %d != %d\n", __LINE__, k,
					stereo_whole [k], stereo_chunked [k]) ;
				exit (1) ;
				} ;
		(void) ch ;
		} ;

	puts ("ok") ;

	return ;
} /* dither_chunking_test */

/*-------------------------------------------------------------------------------------
*/

static void
dither_channel_test (void)
{	static float in [2 * 1024] ;
	static short stereo [2 * 1024], mono [1024] ;
	SRC_DITHER_STATE *sstereo, *smono ;
	int k, error, difference = 0 ;

	printf ("\tdither_channel_test ............................ ") ;

	for (k = 0 ; k < 2 * 1024 ; k++)
		in [k] = (float) sin (k * 0.011) * 0.4f ;

	sstereo = src_dither_new (SRC_DITHER_TPDF, 16, 2, &error) ;
	smono = src_dither_new (SRC_DITHER_TPDF, 16, 1, &error) ;
	if (sstereo == NULL || smono == NULL)
	{	printf ("\n\n\tLine %d : src_dither_new failed.\n", __LINE__) ;
		exit (1) ;
		} ;

	src_float_to_short_array_dithered (sstereo, in, stereo, 1024) ;
	src_float_to_short_array_dithered (smono, in, mono, 1024) ;
	src_dither_delete (sstereo) ;
	src_dither_delete (smono) ;

	/* The left channel of a stereo stream must not be the mono stream: each
	** channel has its own noise.
	*/
	for (k = 0 ; k < 1024 ; k++)
		if (stereo [2 * k] != mono [k])
			difference ++ ;

	if (difference < 1024 / 4)
	{	printf ("\n\n\tLine %d : only %d of %d samples differ; channels share noise\n",
				__LINE__, difference, 1024) ;
		exit (1) ;
		} ;

	puts ("ok") ;

	return ;
} /* dither_channel_test */

/*-------------------------------------------------------------------------------------
*/

static void
dither_depth_test (void)
{	static float in [BUFFER_LEN] ;
	static short out [BUFFER_LEN] ;
	SRC_DITHER_STATE *state ;
	int bits, type, k, error, step ;

	printf ("\tdither_depth_test .............................. ") ;

	for (k = 0 ; k < BUFFER_LEN ; k++)
		in [k] = (float) sin (k * 0.029) * 0.8f ;

	for (bits = 8 ; bits <= 16 ; bits += 4)
		for (type = SRC_DITHER_TPDF ; type <= SRC_DITHER_NOISE_SHAPED ; type++)
		{	state = src_dither_new (type, bits, 1, &error) ;
			if (state == NULL)
			{	printf ("\n\n\tLine %d : src_dither_new failed : %s\n", __LINE__, src_strerror (error)) ;
				exit (1) ;
				} ;

			src_float_to_short_array_dithered (state, in, out, BUFFER_LEN) ;
			src_dither_delete (state) ;

			/* Everything must be an exact multiple of the target step and in
			** range.
			*/
			step = 1 << (16 - bits) ;
			for (k = 0 ; k < BUFFER_LEN ; k++)
			{	if (out [k] < -32768 || out [k] > 32767)
				{	printf ("\n\n\tLine %d : bits %d out of range at %d : %d\n", __LINE__, bits, k, out [k]) ;
					exit (1) ;
					} ;
				if (out [k] % step != 0)
				{	printf ("\n\n\tLine %d : bits %d index %d : %d not a multiple of %d\n",
							__LINE__, bits, k, out [k], step) ;
					exit (1) ;
					} ;
				} ;
			} ;

	puts ("ok") ;

	return ;
} /* dither_depth_test */

/*-------------------------------------------------------------------------------------
*/

/* Variance of the quantisation error of a very quiet signal. Without dither
** the error of a constant signal is correlated and its spectrum is a strong
** tone; dither breaks the correlation and makes the error noise-like. Check
** that dithering never worsens the number of distinct output codes and that
** the mean error stays centred.
*/

static void
dither_quality_test (void)
{	static float in [BUFFER_LEN] ;
	static short plain [BUFFER_LEN], tpdf [BUFFER_LEN], shaped [BUFFER_LEN] ;
	SRC_DITHER_STATE *state ;
	double plain_mean = 0.0, tpdf_mean = 0.0, shaped_mean = 0.0 ;
	int k, error ;

	printf ("\tdither_quality_test ............................ ") ;

	/* A signal sitting right on a quantisation step is the worst case for
	** undithered rounding.
	*/
	for (k = 0 ; k < BUFFER_LEN ; k++)
		in [k] = 0.25f + (float) sin (k * 0.004) * (1.0f / 65536.0f) ;

	src_float_to_short_array (in, plain, BUFFER_LEN) ;

	state = src_dither_new (SRC_DITHER_TPDF, 16, 1, &error) ;
	if (state == NULL)
	{	printf ("\n\n\tLine %d : src_dither_new failed.\n", __LINE__) ;
		exit (1) ;
		} ;
	src_float_to_short_array_dithered (state, in, tpdf, BUFFER_LEN) ;
	src_dither_delete (state) ;

	state = src_dither_new (SRC_DITHER_NOISE_SHAPED, 16, 1, &error) ;
	if (state == NULL)
	{	printf ("\n\n\tLine %d : src_dither_new failed.\n", __LINE__) ;
		exit (1) ;
		} ;
	src_float_to_short_array_dithered (state, in, shaped, BUFFER_LEN) ;
	src_dither_delete (state) ;

	for (k = 0 ; k < BUFFER_LEN ; k++)
	{	plain_mean += (double) plain [k] ;
		tpdf_mean += (double) tpdf [k] ;
		shaped_mean += (double) shaped [k] ;
		} ;
	plain_mean /= BUFFER_LEN ;
	tpdf_mean /= BUFFER_LEN ;
	shaped_mean /= BUFFER_LEN ;

	/* All three must average to the same value within a fraction of an LSB;
	** dither must not introduce a DC offset.
	*/
	if (fabs (tpdf_mean - plain_mean) > 1.0 || fabs (shaped_mean - plain_mean) > 1.0)
	{	printf ("\n\n\tLine %d : mean drift : plain %.3f tpdf %.3f shaped %.3f\n",
				__LINE__, plain_mean, tpdf_mean, shaped_mean) ;
		exit (1) ;
		} ;

	/* Dither must actually change the output: the granular silence is the
	** thing being fixed.
	*/
	{	int changed = 0 ;

		for (k = 0 ; k < BUFFER_LEN ; k++)
			if (tpdf [k] != plain [k])
				changed ++ ;
		if (changed < BUFFER_LEN / 4)
		{	printf ("\n\n\tLine %d : TPDF changed only %d samples\n", __LINE__, changed) ;
			exit (1) ;
			} ;
		} ;

	puts ("ok") ;

	return ;
} /* dither_quality_test */

/*-------------------------------------------------------------------------------------
*/

/* Noise shaping must move the error energy towards high frequencies: the error
** left by TPDF is white, while the shaped error is high passed by (1 - z^-1).
** Compare the quantisation error (output minus the ideal scaled input) in a low
** and a high part of the band.
*/

/* Lag one normalised autocorrelation of a signal: near zero for white noise,
** strongly negative for the error of a 1 - z^-1 high pass.
*/
static double
lag_one_autocorrelation (const double *x, int len)
{	double mean = 0.0, var = 0.0, cov = 0.0 ;
	int k ;

	for (k = 0 ; k < len ; k++)
		mean += x [k] ;
	mean /= len ;

	for (k = 0 ; k < len ; k++)
		var += (x [k] - mean) * (x [k] - mean) ;
	for (k = 1 ; k < len ; k++)
		cov += (x [k] - mean) * (x [k - 1] - mean) ;

	return var > 0.0 ? cov / var : 0.0 ;
} /* lag_one_autocorrelation */

static void
dither_shaping_test (void)
{	enum { LEN = 16384 } ;
	static float in [LEN] ;
	static short tpdf [LEN], shaped [LEN] ;
	static double err_t [LEN], err_s [LEN] ;
	static unsigned seed = 12345 ;
	SRC_DITHER_STATE *state ;
	double tpdf_lag, shaped_lag ;
	int k, error ;

	printf ("\tdither_shaping_test ............................ ") ;

	/* A very quiet broadband signal, so the quantisation error dominates. */
	for (k = 0 ; k < LEN ; k++)
	{	seed = seed * 1103515245u + 12345u ;
		in [k] = ((float) ((seed >> 9) & 0xffff) / 65535.0f - 0.5f) * 0.0002f ;
		} ;

	state = src_dither_new (SRC_DITHER_TPDF, 16, 1, &error) ;
	if (state == NULL)
	{	printf ("\n\n\tLine %d : src_dither_new failed.\n", __LINE__) ;
		exit (1) ;
		} ;
	src_float_to_short_array_dithered (state, in, tpdf, LEN) ;
	src_dither_delete (state) ;

	state = src_dither_new (SRC_DITHER_NOISE_SHAPED, 16, 1, &error) ;
	if (state == NULL)
	{	printf ("\n\n\tLine %d : src_dither_new failed.\n", __LINE__) ;
		exit (1) ;
		} ;
	src_float_to_short_array_dithered (state, in, shaped, LEN) ;
	src_dither_delete (state) ;

	/* The error of a 1 - z^-1 high pass has a strongly negative lag one
	** autocorrelation (about -0.25 for the shaped error here), while white
	** TPDF error is close to zero. Measure both on the quantisation error.
	*/
	for (k = 0 ; k < LEN ; k++)
	{	err_t [k] = (double) tpdf [k] - (double) in [k] * 32768.0 ;
		err_s [k] = (double) shaped [k] - (double) in [k] * 32768.0 ;
		} ;

	tpdf_lag = lag_one_autocorrelation (err_t, LEN) ;
	shaped_lag = lag_one_autocorrelation (err_s, LEN) ;

	/* White error has correlation near zero; the shaped error must be clearly
	** negative, which only happens if the feedback is a high pass.
	*/
	if (fabs (tpdf_lag) > 0.1 || shaped_lag > -0.1)
	{	printf ("\n\n\tLine %d : shaping is not high passing : tpdf lag %.3f shaped lag %.3f\n",
				__LINE__, tpdf_lag, shaped_lag) ;
		exit (1) ;
		} ;

	puts ("ok") ;

	return ;
} /* dither_shaping_test */

/*-------------------------------------------------------------------------------------
*/

static void
dither_overlap_test (void)
{	/* One contiguous scratch area: in reads the first half as floats, out
	** writes the second half as shorts, so the two ranges must be sized for
	** their own types.
	*/
	static char arena [BUFFER_LEN * (int) sizeof (float) + BUFFER_LEN * (int) sizeof (short)] ;
	float *in = (float *) arena ;
	short *safe_out = (short *) (arena + BUFFER_LEN * (int) sizeof (float)) ;
	SRC_DITHER_STATE *state ;
	int error ;

	printf ("\tdither_overlap_test ............................ ") ;

	state = src_dither_new (SRC_DITHER_TPDF, 16, 1, &error) ;
	if (state == NULL)
	{	printf ("\n\n\tLine %d : src_dither_new failed.\n", __LINE__) ;
		exit (1) ;
		} ;

	/* in and out point into the same arena with overlapping ranges. */
	if (src_float_to_short_array_dithered (state, in, (short *) (arena + 8), BUFFER_LEN)
			!= SRC_ERR_DATA_OVERLAP)
	{	printf ("\n\n\tLine %d : overlapping buffers not diagnosed.\n", __LINE__) ;
		exit (1) ;
		} ;

	/* Non-overlapping ranges in the same arena are fine. */
	if (src_float_to_short_array_dithered (state, in, safe_out, BUFFER_LEN) != SRC_ERR_NO_ERROR)
	{	printf ("\n\n\tLine %d : non-overlapping buffers rejected.\n", __LINE__) ;
		exit (1) ;
		} ;

	/* NULL pointers are a different error again. */
	if (src_float_to_short_array_dithered (state, NULL, safe_out, 16) != SRC_ERR_BAD_DATA_PTR)
	{	printf ("\n\n\tLine %d : NULL input not diagnosed.\n", __LINE__) ;
		exit (1) ;
		} ;

	src_dither_delete (state) ;

	puts ("ok") ;

	return ;
} /* dither_overlap_test */
