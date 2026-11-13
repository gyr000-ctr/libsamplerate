/*
** Copyright (c) 2002-2021, Erik de Castro Lopo <erikd@mega-nerd.com>
** All rights reserved.
**
** This code is released under 2-clause BSD license. Please see the
** file at : https://github.com/libsndfile/libsamplerate/blob/master/COPYING
*/

/*
** Precision selection layer shared by every converter (sinc, linear, zoh).
**
** The public API always exchanges float samples. Internally each converter
** keeps its delay line / working buffer and its filter coefficients in
** SRC_SAMPLE_T :
**   - default build : float  (historical single precision path),
**   - -DLIBSAMPLERATE_DOUBLE_PRECISION : double (full double precision
**     chain : coefficients, delay line and accumulation all in double).
**
** Control logic (ratio handling, fixed point index bookkeeping, quality
** tiers, state machine) is identical in both builds; only the numeric type
** of the data plane changes here.
*/

#ifndef SRC_PRECISION_H_INCLUDED
#define SRC_PRECISION_H_INCLUDED

#include <string.h>

#ifdef LIBSAMPLERATE_DOUBLE_PRECISION

typedef double SRC_SAMPLE_T ;

/* Promote an API sample into the internal working precision. */
static inline SRC_SAMPLE_T
src_from_api_sample (float x)
{
	return (SRC_SAMPLE_T) x ;
} /* src_from_api_sample */

/* Store an internal precision result into an API float slot. */
static inline void
src_store_api_sample (float *dst, SRC_SAMPLE_T x)
{
	*dst = (float) x ;
} /* src_store_api_sample */

/*
** Widen an API sample for arithmetic against an internal precision value.
** In the float build this is a plain double promotion (identical to the
** historical converter code, so generated code is unchanged); in the double
** build it keeps the operand in the internal double working precision.
*/
static inline SRC_SAMPLE_T
src_widen_api_sample (float x)
{
	return (SRC_SAMPLE_T) (double) x ;
} /* src_widen_api_sample */

/*
** Bulk copy of API (float) samples into an internal precision buffer.
** In double precision builds each element has to be widened, so this
** cannot be a plain memcpy.
*/
static inline void
src_copy_api_to_internal (SRC_SAMPLE_T *dst, const float *src, long count)
{
	for (long k = 0 ; k < count ; k++)
		dst [k] = (SRC_SAMPLE_T) src [k] ;

	return ;
} /* src_copy_api_to_internal */

#else /* !LIBSAMPLERATE_DOUBLE_PRECISION */

typedef float SRC_SAMPLE_T ;

static inline SRC_SAMPLE_T
src_from_api_sample (float x)
{
	return x ;
} /* src_from_api_sample */

static inline void
src_store_api_sample (float *dst, SRC_SAMPLE_T x)
{
	*dst = x ;
} /* src_store_api_sample */

/*
** Float build : return the double-promoted operand so the surrounding
** arithmetic runs in double exactly as the historical converter code did.
*/
static inline double
src_widen_api_sample (float x)
{
	return (double) x ;
} /* src_widen_api_sample */

static inline void
src_copy_api_to_internal (SRC_SAMPLE_T *dst, const float *src, long count)
{
	memcpy (dst, src, (size_t) count * sizeof (SRC_SAMPLE_T)) ;

	return ;
} /* src_copy_api_to_internal */

#endif /* LIBSAMPLERATE_DOUBLE_PRECISION */

#endif /* SRC_PRECISION_H_INCLUDED */
