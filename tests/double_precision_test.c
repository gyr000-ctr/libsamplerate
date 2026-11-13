/*
** Copyright (c) 2002-2021, Erik de Castro Lopo <erikd@mega-nerd.com>
** All rights reserved.
**
** This code is released under 2-clause BSD license. Please see the
** file at : https://github.com/libsndfile/libsamplerate/blob/master/COPYING
*/

/*
** Precision path verification test.
**
** Drives the public API (src_new / src_process / src_reset / src_set_ratio)
** through every converter and quality tier with chunked streaming input,
** then dumps the generated samples with exact hex-float notation. The same
** binary is built and run against the float and double internal precision
** configurations; an external checker compares the dumps against each other
** and against an independent double precision reference implementation.
**
** The in-process checks below are precision agnostic and must pass in both
** configurations: finite output, exact frame accounting, reset behaviour.
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

#ifndef M_PI
#define M_PI 3.14159265358979323846264338
#endif

#define INPUT_FRAMES	1024
#define STREAM_CHUNK	257
#define MAX_CHANNELS_TEST	3

static const int converters [] =
{
	SRC_SINC_BEST_QUALITY,
	SRC_SINC_MEDIUM_QUALITY,
	SRC_SINC_FASTEST,
	SRC_ZERO_ORDER_HOLD,
	SRC_LINEAR,
} ;

static const double src_ratios [] =
{
	0.5, 0.7, 1.0, 1.5, 2.0
} ;

static const int channel_counts [] =
{
	1, 2, 3
} ;

static void
gen_input (float *input, long frames, int channels)
{
	static const double freqs [] = { 0.017, 0.049, 0.113, 0.227 } ;

	for (long frame = 0 ; frame < frames ; frame++)
		for (int ch = 0 ; ch < channels ; ch++)
		{
			double value = 0.0 ;
			for (unsigned k = 0 ; k < ARRAY_LEN (freqs) ; k++)
			{
				double phase = 2.0 * M_PI * freqs [k] * frame + 0.37 * ch * (k + 1) ;
				value += 0.22 * sin (phase + 0.11 * k) ;
			}
			/* A weak deterministic chirp-like term exercises varying amplitude. */
			value += 0.05 * sin (2.0 * M_PI * (0.05 + 0.0009 * frame) * frame) ;
			input [frame * channels + ch] = (float) value ;
		} ;

	return ;
} /* gen_input */

static long
run_streaming (int converter, int channels, double ratio, const float *input,
				long input_frames, long stream_chunk, float *output, long output_cap)
{
	SRC_STATE *state ;
	SRC_DATA data ;
	int error ;
	long in_offset = 0, out_total = 0, used_total = 0 ;

	if ((state = src_new (converter, channels, &error)) == NULL)
	{	printf ("\n\nLine %d : src_new failed : %s.\n\n", __LINE__, src_strerror (error)) ;
		exit (1) ;
		} ;

	while (in_offset < input_frames)
	{
		long this_chunk = input_frames - in_offset ;
		if (this_chunk > stream_chunk)
			this_chunk = stream_chunk ;

		memset (&data, 0, sizeof (data)) ;
		data.data_in = input + in_offset * channels ;
		data.data_out = output + out_total * channels ;
		data.input_frames = this_chunk ;
		data.output_frames = output_cap - out_total ;
		data.src_ratio = ratio ;
		data.end_of_input = (in_offset + this_chunk == input_frames) ;

		if ((error = src_process (state, &data)) != 0)
		{	printf ("\n\nLine %d : src_process failed : %s.\n\n", __LINE__, src_strerror (error)) ;
			exit (1) ;
			} ;

		in_offset += data.input_frames_used ;
		out_total += data.output_frames_gen ;
		used_total += data.input_frames_used ;

		if (out_total > output_cap)
		{	printf ("\n\nLine %d : output buffer overrun.\n\n", __LINE__) ;
			exit (1) ;
			} ;

		if (data.end_of_input && data.output_frames_gen == 0)
			break ;
		} ;

	if (used_total != input_frames)
	{	printf ("\n\nLine %d : consumed %ld of %ld input frames.\n\n",
					__LINE__, used_total, input_frames) ;
		exit (1) ;
		} ;

	src_delete (state) ;

	return out_total ;
} /* run_streaming */

static void
check_finite (const float *data, long count)
{
	for (long k = 0 ; k < count ; k++)
		if (!isfinite ((double) data [k]))
		{	printf ("\n\nLine %d : non-finite output at sample %ld.\n\n", __LINE__, k) ;
			exit (1) ;
			} ;

	return ;
} /* check_finite */

static void
reset_check (int converter, int channels)
{
	SRC_STATE *state ;
	SRC_DATA data ;
	float input [64], out_a [128], out_b [128] ;
	int error ;

	for (int k = 0 ; k < ARRAY_LEN (input) ; k++)
		input [k] = (float) (0.25 * sin (0.3 * k)) ;

	if ((state = src_new (converter, channels, &error)) == NULL)
	{	printf ("\n\nLine %d : src_new failed : %s.\n\n", __LINE__, src_strerror (error)) ;
		exit (1) ;
		} ;

	memset (&data, 0, sizeof (data)) ;
	data.data_in = input ;
	data.data_out = out_a ;
	data.input_frames = ARRAY_LEN (input) / channels ;
	data.output_frames = ARRAY_LEN (out_a) / channels ;
	data.src_ratio = 1.25 ;
	data.end_of_input = 1 ;

	if ((error = src_process (state, &data)) != 0)
	{	printf ("\n\nLine %d : src_process failed : %s.\n\n", __LINE__, src_strerror (error)) ;
		exit (1) ;
		} ;

	if (src_reset (state) != 0)
	{	printf ("\n\nLine %d : src_reset failed.\n\n", __LINE__) ;
		exit (1) ;
		} ;

	memset (out_b, 0, sizeof (out_b)) ;
	data.data_out = out_b ;
	data.input_frames_used = 0 ;
	data.output_frames_gen = 0 ;

	if ((error = src_process (state, &data)) != 0)
	{	printf ("\n\nLine %d : src_process failed : %s.\n\n", __LINE__, src_strerror (error)) ;
		exit (1) ;
		} ;

	if (data.output_frames_gen == 0)
	{	printf ("\n\nLine %d : no output after reset.\n\n", __LINE__) ;
		exit (1) ;
		} ;

	for (long k = 0 ; k < data.output_frames_gen * channels ; k++)
		if (out_a [k] != out_b [k])
		{	printf ("\n\nLine %d : reset changed output at sample %ld.\n\n", __LINE__, k) ;
			exit (1) ;
			} ;

	src_delete (state) ;

	return ;
} /* reset_check */

static void
varispeed_check (int converter, int channels)
{
	SRC_STATE *state ;
	SRC_DATA data ;
	float input [512], output [1024] ;
	int error ;
	long in_offset = 0, out_total = 0 ;

	for (int k = 0 ; k < ARRAY_LEN (input) ; k++)
		input [k] = (float) (0.3 * sin (0.07 * k)) ;

	if ((state = src_new (converter, channels, &error)) == NULL)
	{	printf ("\n\nLine %d : src_new failed : %s.\n\n", __LINE__, src_strerror (error)) ;
		exit (1) ;
		} ;

	int call = 0 ;
	while (in_offset < ARRAY_LEN (input) / channels)
	{
		memset (&data, 0, sizeof (data)) ;
		data.data_in = input + in_offset * channels ;
		data.data_out = output + out_total * channels ;
		data.input_frames = 64 ;
		data.output_frames = (ARRAY_LEN (output) / channels) - out_total ;
		/* Ramped ratio across calls to exercise the vari-speed path. */
		data.src_ratio = 0.6 + 0.15 * (call % 7) ;
		data.end_of_input = (in_offset + 64 >= ARRAY_LEN (input) / channels) ;

		if ((error = src_process (state, &data)) != 0)
		{	printf ("\n\nLine %d : src_process failed : %s.\n\n", __LINE__, src_strerror (error)) ;
			exit (1) ;
			} ;

		in_offset += data.input_frames_used ;
		out_total += data.output_frames_gen ;
		call ++ ;

		if (data.end_of_input && data.output_frames_gen == 0)
			break ;
		} ;

	check_finite (output, out_total * channels) ;
	src_delete (state) ;

	return ;
} /* varispeed_check */

int
main (int argc, char **argv)
{
	const char *dump_path = (argc > 1) ? argv [1] : "double_precision_dump.txt" ;
	FILE *dump = fopen (dump_path, "w") ;

	if (dump == NULL)
	{	printf ("\n\nLine %d : cannot open dump file '%s'.\n\n", __LINE__, dump_path) ;
		return 1 ;
		} ;

	puts ("") ;
#ifdef LIBSAMPLERATE_DOUBLE_PRECISION
	puts ("    double_precision_test : internal precision = double") ;
	fprintf (dump, "# precision: double\n") ;
#else
	puts ("    double_precision_test : internal precision = float") ;
	fprintf (dump, "# precision: float\n") ;
#endif

	long input_cap = INPUT_FRAMES * MAX_CHANNELS_TEST ;
	float *input = calloc ((size_t) input_cap, sizeof (float)) ;
	/* Largest ratio is 2.0; allow headroom for filter delay / termination.
	** Capacities below are frame counts, so the sample allocation must be
	** scaled by the largest channel count to avoid interleaved overrun. */
	long output_cap_frames = INPUT_FRAMES * 3 ;
	float *output = calloc ((size_t) output_cap_frames * MAX_CHANNELS_TEST, sizeof (float)) ;

	if (input == NULL || output == NULL)
	{	printf ("\n\nLine %d : allocation failed.\n\n", __LINE__) ;
		return 1 ;
		} ;

	for (unsigned ci = 0 ; ci < ARRAY_LEN (converters) ; ci++)
	{
		int converter = converters [ci] ;
		reset_check (converter, 2) ;
		varispeed_check (converter, 1) ;

		for (unsigned ri = 0 ; ri < ARRAY_LEN (src_ratios) ; ri++)
			for (unsigned hi = 0 ; hi < ARRAY_LEN (channel_counts) ; hi++)
			{
				int channels = channel_counts [hi] ;
				double ratio = src_ratios [ri] ;

				gen_input (input, INPUT_FRAMES, channels) ;

				long out_frames = run_streaming (converter, channels, ratio,
						input, INPUT_FRAMES, STREAM_CHUNK, output, output_cap_frames) ;

				check_finite (output, out_frames * channels) ;

				printf ("\tconv=%d ratio=%4.2f ch=%d -> %ld frames\n",
						converter, ratio, channels, out_frames) ;

				fprintf (dump, "CASE %d %.17g %d %ld %ld\n",
						converter, ratio, channels, (long) INPUT_FRAMES, out_frames) ;
				for (long k = 0 ; k < out_frames * channels ; k++)
					fprintf (dump, "%a\n", output [k]) ;
			} ;
		} ;

	free (input) ;
	free (output) ;
	fclose (dump) ;

	printf ("\n    dump written to %s\n\n", dump_path) ;

	return 0 ;
} /* main */
