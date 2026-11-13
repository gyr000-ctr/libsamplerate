/*
** Copyright (c) 2002-2021, Erik de Castro Lopo <erikd@mega-nerd.com>
** All rights reserved.
**
** This code is released under 2-clause BSD license. Please see the
** file at : https://github.com/libsndfile/libsamplerate/blob/master/COPYING
*/

/*
** Throughput benchmark for the integer / fixed-point PCM path,
** compared against the classic float path. Not part of ctest ;
** run manually.
*/

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif
#include <math.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#include <samplerate.h>

#include "util.h"

#define BUFFER_LEN	(1<<16)

static float	finput [BUFFER_LEN] ;
static float	foutput [BUFFER_LEN] ;
static int16_t	sinput [BUFFER_LEN] ;
static int16_t	soutput [BUFFER_LEN] ;

static void
float_throughput (int converter)
{	SRC_DATA src_data ;
	clock_t start_time, clock_time ;
	double duration ;
	long total_frames = 0 ;
	int error ;

	printf ("    float  %-28s ", src_get_name (converter)) ;
	fflush (stdout) ;

	src_data.data_in = finput ;
	src_data.input_frames = ARRAY_LEN (finput) ;
	src_data.data_out = foutput ;
	src_data.output_frames = ARRAY_LEN (foutput) ;
	src_data.src_ratio = 0.99 ;

#ifdef _WIN32
	Sleep (1000) ;
#else
	sleep (1) ;
#endif

	start_time = clock () ;

	do
	{
		if ((error = src_simple (&src_data, converter, 1)) != 0)
		{	puts (src_strerror (error)) ;
			exit (1) ;
			} ;

		total_frames += src_data.output_frames_gen ;

		clock_time = clock () - start_time ;
		duration = (1.0 * clock_time) / CLOCKS_PER_SEC ;
	}
	while (duration < 2.0) ;

	printf (" %10.0f samples/sec\n", total_frames / duration) ;
} /* float_throughput */

static void
pcm_throughput (int converter)
{	SRC_STATE *state ;
	SRC_PCM_DATA data ;
	clock_t start_time, clock_time ;
	double duration ;
	long total_frames = 0 ;
	int error ;

	printf ("    pcm16  %-28s ", src_get_name (converter)) ;
	fflush (stdout) ;

	state = src_pcm_new (converter, 1, SRC_PCM_S16, SRC_PCM_S16, 1, &error) ;
	if (!state)
	{	puts (src_strerror (error)) ;
		exit (1) ;
		} ;

	memset (&data, 0, sizeof (data)) ;
	data.data_in = sinput ;
	data.input_frames = ARRAY_LEN (sinput) ;
	data.data_out = soutput ;
	data.output_frames = ARRAY_LEN (soutput) ;
	data.end_of_input = 1 ;
	data.src_ratio = 0.99 ;

#ifdef _WIN32
	Sleep (1000) ;
#else
	sleep (1) ;
#endif

	start_time = clock () ;

	do
	{
		if ((error = src_pcm_process (state, &data)) != 0)
		{	puts (src_strerror (error)) ;
			exit (1) ;
			} ;

		total_frames += data.output_frames_gen ;

		src_reset (state) ;

		clock_time = clock () - start_time ;
		duration = (1.0 * clock_time) / CLOCKS_PER_SEC ;
	}
	while (duration < 2.0) ;

	printf (" %10.0f samples/sec\n", total_frames / duration) ;

	src_delete (state) ;
} /* pcm_throughput */

int
main (void)
{	int converters [] = { SRC_SINC_BEST_QUALITY, SRC_SINC_MEDIUM_QUALITY,
			SRC_SINC_FASTEST, SRC_LINEAR, SRC_ZERO_ORDER_HOLD } ;

	puts ("\nPCM (integer/fixed-point) vs float throughput :\n") ;

	for (unsigned k = 0 ; k < ARRAY_LEN (converters) ; k++)
	{	float_throughput (converters [k]) ;
		pcm_throughput (converters [k]) ;
		}

	puts ("") ;

	return 0 ;
} /* main */
