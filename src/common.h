/*
** Copyright (c) 2002-2021, Erik de Castro Lopo <erikd@mega-nerd.com>
** All rights reserved.
**
** This code is released under 2-clause BSD license. Please see the
** file at : https://github.com/libsndfile/libsamplerate/blob/master/COPYING
*/

#ifndef COMMON_H_INCLUDED
#define COMMON_H_INCLUDED

#include <stdint.h>
#ifdef HAVE_STDBOOL_H
#include <stdbool.h>
#endif

#if defined(__x86_64__) || defined(_M_X64)
#   define HAVE_SSE2_INTRINSICS
#elif defined(ENABLE_SSE2_LRINT) && (defined(_M_IX86) || defined(__i386__))
#   if defined(_MSC_VER)
#       define HAVE_SSE2_INTRINSICS
#   elif defined(__clang__)
#       ifdef __SSE2__
#           define HAVE_SSE2_INTRINSICS
#       elif (__has_attribute(target))
#           define HAVE_SSE2_INTRINSICS
#           define USE_TARGET_ATTRIBUTE
#       endif
#   elif defined(__GNUC__)
#       ifdef __SSE2__
#           define HAVE_SSE2_INTRINSICS
#       elif (__GNUC__ > 4 || (__GNUC__ == 4 && __GNUC_MINOR__ >= 9))
#           define HAVE_SSE2_INTRINSICS
#           define USE_TARGET_ATTRIBUTE
#       endif
#   endif
#endif

#ifdef HAVE_SSE2_INTRINSICS
#ifdef HAVE_IMMINTRIN_H
#include <immintrin.h>
#else
#include <emmintrin.h>
#endif
#endif /* HAVE_SSE2_INTRINSICS */

#include <math.h>

#ifdef HAVE_VISIBILITY
  #define LIBSAMPLERATE_DLL_PRIVATE __attribute__ ((visibility ("hidden")))
#elif defined (__APPLE__)
  #define LIBSAMPLERATE_DLL_PRIVATE __private_extern__
#else
  #define LIBSAMPLERATE_DLL_PRIVATE
#endif

#define	SRC_MAX_RATIO			256
#define	SRC_MAX_RATIO_STR		"256"

#define	SRC_MIN_RATIO_DIFF		(1e-20)

#ifndef MAX
#define	MAX(a,b)	(((a) > (b)) ? (a) : (b))
#endif

#ifndef MIN
#define	MIN(a,b)	(((a) < (b)) ? (a) : (b))
#endif

#define	ARRAY_LEN(x)			((int) (sizeof (x) / sizeof ((x) [0])))
#define OFFSETOF(type,member)	((int) (&((type*) 0)->member))

#define	MAKE_MAGIC(a,b,c,d,e,f)	((a) + ((b) << 4) + ((c) << 8) + ((d) << 12) + ((e) << 16) + ((f) << 20))

/*
** Inspiration : http://sourcefrog.net/weblog/software/languages/C/unused.html
*/
#ifdef UNUSED
#elif defined (__GNUC__)
#	define UNUSED(x) UNUSED_ ## x __attribute__ ((unused))
#elif defined (__LCLINT__)
#	define UNUSED(x) /*@unused@*/ x
#else
#	define UNUSED(x) x
#endif

#ifdef __GNUC__
#	define WARN_UNUSED	__attribute__ ((warn_unused_result))
#else
#	define WARN_UNUSED
#endif

#include "samplerate.h"

enum
{	SRC_FALSE	= 0,
	SRC_TRUE	= 1,
} ;

enum SRC_MODE
{
	SRC_MODE_PROCESS	= 0,
	SRC_MODE_CALLBACK	= 1
} ;

typedef enum SRC_ERROR
{
	SRC_ERR_NO_ERROR = 0,

	SRC_ERR_MALLOC_FAILED,
	SRC_ERR_BAD_STATE,
	SRC_ERR_BAD_DATA,
	SRC_ERR_BAD_DATA_PTR,
	SRC_ERR_NO_PRIVATE,
	SRC_ERR_BAD_SRC_RATIO,
	SRC_ERR_BAD_PROC_PTR,
	SRC_ERR_SHIFT_BITS,
	SRC_ERR_FILTER_LEN,
	SRC_ERR_BAD_CONVERTER,
	SRC_ERR_BAD_CHANNEL_COUNT,
	SRC_ERR_SINC_BAD_BUFFER_LEN,
	SRC_ERR_SIZE_INCOMPATIBILITY,
	SRC_ERR_BAD_PRIV_PTR,
	SRC_ERR_BAD_SINC_STATE,
	SRC_ERR_DATA_OVERLAP,
	SRC_ERR_BAD_CALLBACK,
	SRC_ERR_BAD_MODE,
	SRC_ERR_NULL_CALLBACK,
	SRC_ERR_NO_VARIABLE_RATIO,
	SRC_ERR_SINC_PREPARE_DATA_BAD_LEN,
	SRC_ERR_BAD_INTERNAL_STATE,
	SRC_ERR_BAD_BIT_DEPTH,
	SRC_ERR_WRONG_API,

	/* This must be the last error number. */
	SRC_ERR_MAX_ERROR
} SRC_ERROR ;

typedef struct SRC_STATE_VT_tag
{
	/* Varispeed process function. */
	SRC_ERROR		(*vari_process) (SRC_STATE *state, SRC_DATA *data) ;

	/* Constant speed process function. */
	SRC_ERROR		(*const_process) (SRC_STATE *state, SRC_DATA *data) ;

	/* State reset. */
	void			(*reset) (SRC_STATE *state) ;

	/* State clone. */
	SRC_STATE		*(*copy) (SRC_STATE *state) ;

	/* State close. */
	void			(*close) (SRC_STATE *state) ;
} SRC_STATE_VT ;

/*----------------------------------------------------------
**	Integer / fixed-point PCM path (src_pcm.c, src_pcm_sinc.c).
*/

/* PCM sample layout descriptor for the fixed-point path. */
typedef struct
{	int		in_bits ;		/* 16, 24 or 32 */
	int		out_bits ;		/* 16, 24 or 32 */
	int		interleaved ;	/* non-zero -> interleaved, zero -> planar */
} PCM_LAYOUT ;

/*
** Reader/writer function types. For interleaved layouts, `in`/`out`
** point to the single interleaved buffer. For planar layouts they
** point to an array of `channels` per-channel buffer pointers.
** Samples are exchanged with the converters as int32 Q31 fixed point
** (full scale == 2**31).
*/
typedef int32_t	(*pcm_read_fn) (const void *in, long frame, int ch, int channels) ;
typedef void	(*pcm_write_fn) (void *out, long frame, int ch, int channels, int32_t q31) ;

struct SRC_STATE_tag
{
	SRC_STATE_VT *vt ;

	double	last_ratio, last_position ;

	SRC_ERROR	error ;
	int		channels ;

	/* SRC_MODE_PROCESS or SRC_MODE_CALLBACK */
	enum SRC_MODE	mode ;

	/* Data specific to SRC_MODE_CALLBACK. */
	src_callback_t	callback_func ;
	void			*user_callback_data ;
	long			saved_frames ;
	const float		*saved_data ;

	/*
	** Fixed-point PCM process function and sample layout. pcm_process
	** is non-NULL only for states created with src_pcm_new() ; it is
	** NULL for the classic float path.
	*/
	SRC_ERROR		(*pcm_process) (SRC_STATE *state, SRC_PCM_DATA *data) ;
	PCM_LAYOUT		pcm_layout ;

	/* Pointer to data to converter specific data. */
	void	*private_data ;
} ;

/* In src_sinc.c */
const char* sinc_get_name (int src_enum) ;
const char* sinc_get_description (int src_enum) ;

SRC_STATE *sinc_state_new (int converter_type, int channels, SRC_ERROR *error) ;

/* In src_linear.c */
const char* linear_get_name (int src_enum) ;
const char* linear_get_description (int src_enum) ;

SRC_STATE *linear_state_new (int channels, SRC_ERROR *error) ;

/* In src_zoh.c */
const char* zoh_get_name (int src_enum) ;
const char* zoh_get_description (int src_enum) ;

SRC_STATE *zoh_state_new (int channels, SRC_ERROR *error) ;

/* In src_pcm.c. Both return NULL for an invalid bit depth. */
pcm_read_fn		pcm_select_reader (int bits, int interleaved) ;
pcm_write_fn	pcm_select_writer (int bits, int interleaved) ;

int pcm_is_valid_bits (int bits) ;

SRC_STATE *pcm_zoh_state_new (int channels, const PCM_LAYOUT *layout, SRC_ERROR *error) ;
SRC_STATE *pcm_linear_state_new (int channels, const PCM_LAYOUT *layout, SRC_ERROR *error) ;

/* In src_pcm_sinc.c */
SRC_STATE *pcm_sinc_state_new (int converter_type, int channels, const PCM_LAYOUT *layout, SRC_ERROR *error) ;

/*----------------------------------------------------------
** SIMD optimized math functions.
*/

#ifdef HAVE_SSE2_INTRINSICS
static inline int
#ifdef USE_TARGET_ATTRIBUTE
__attribute__((target("sse2")))
#endif
psf_lrintf (float x)
{
	return _mm_cvtss_si32 (_mm_load_ss (&x)) ;
}
static inline int
#ifdef USE_TARGET_ATTRIBUTE
__attribute__((target("sse2")))
#endif
psf_lrint (double x)
{
	return _mm_cvtsd_si32 (_mm_load_sd (&x)) ;
}

#else

static inline int psf_lrintf (float x)
{
	return lrintf (x) ;
} /* psf_lrintf */

static inline int psf_lrint (double x)
{
	return lrint (x) ;
} /* psf_lrint */
#endif

/*----------------------------------------------------------
**	Common static inline functions.
*/

static inline double
fmod_one (double x)
{	double res ;

	res = x - psf_lrint (x) ;
	if (res < 0.0)
		return res + 1.0 ;

	return res ;
} /* fmod_one */

static inline int
is_bad_src_ratio (double ratio)
{	return (ratio < (1.0 / SRC_MAX_RATIO) || ratio > (1.0 * SRC_MAX_RATIO)) ;
} /* is_bad_src_ratio */


#endif	/* COMMON_H_INCLUDED */

