/*
** Copyright (c) 2002-2016, Erik de Castro Lopo <erikd@mega-nerd.com>
** All rights reserved.
**
** This code is released under 2-clause BSD license. Please see the
** file at : https://github.com/libsndfile/libsamplerate/blob/master/COPYING
*/

/*
** API documentation is available here:
**     http://libsndfile.github.io/libsamplerate/api.html
*/

#ifndef SAMPLERATE_H
#define SAMPLERATE_H

#ifdef __cplusplus
extern "C" {
#endif	/* __cplusplus */


/* Opaque data type SRC_STATE. */
typedef struct SRC_STATE_tag SRC_STATE ;

/*
** Error numbers returned by the API functions. They are part of the public
** interface so that a caller can tell failure modes apart; src_strerror()
** converts any of them into a text string.
*/
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

	/* Dithering errors. */
	SRC_ERR_BAD_DITHER_TYPE,
	SRC_ERR_BAD_BIT_DEPTH,

	/* This must be the last error number. */
	SRC_ERR_MAX_ERROR
} SRC_ERROR ;

/* SRC_DATA is used to pass data to src_simple() and src_process(). */
typedef struct
{	const float	*data_in ;
	float	*data_out ;

	long	input_frames, output_frames ;
	long	input_frames_used, output_frames_gen ;

	int		end_of_input ;

	double	src_ratio ;
} SRC_DATA ;

/*
** User supplied callback function type for use with src_callback_new()
** and src_callback_read(). First parameter is the same pointer that was
** passed into src_callback_new(). Second parameter is pointer to a
** pointer. The user supplied callback function must modify *data to
** point to the start of the user supplied float array. The user supplied
** function must return the number of frames that **data points to.
*/

typedef long (*src_callback_t) (void *cb_data, float **data) ;

/*
**	Standard initialisation function : return an anonymous pointer to the
**	internal state of the converter. Choose a converter from the enums below.
**	Error returned in *error.
*/

SRC_STATE* src_new (int converter_type, int channels, int *error) ;

/*
** Clone a handle : return an anonymous pointer to a new converter
** containing the same internal state as orig. Error returned in *error.
*/
SRC_STATE* src_clone (SRC_STATE* orig, int *error) ;

/*
**	Initilisation for callback based API : return an anonymous pointer to the
**	internal state of the converter. Choose a converter from the enums below.
**	The cb_data pointer can point to any data or be set to NULL. Whatever the
**	value, when processing, user supplied function "func" gets called with
**	cb_data as first parameter.
*/

SRC_STATE* src_callback_new (src_callback_t func, int converter_type, int channels,
				int *error, void* cb_data) ;

/*
**	Cleanup all internal allocations.
**	Always returns NULL.
*/

SRC_STATE* src_delete (SRC_STATE *state) ;

/*
**	Standard processing function.
**	Returns non zero on error.
*/

int src_process (SRC_STATE *state, SRC_DATA *data) ;

/*
**	Callback based processing function. Read up to frames worth of data from
**	the converter int *data and return frames read or -1 on error.
*/
long src_callback_read (SRC_STATE *state, double src_ratio, long frames, float *data) ;

/*
**	Simple interface for performing a single conversion from input buffer to
**	output buffer at a fixed conversion ratio.
**	Simple interface does not require initialisation as it can only operate on
**	a single buffer worth of audio.
*/

int src_simple (SRC_DATA *data, int converter_type, int channels) ;

/*
** This library contains a number of different sample rate converters,
** numbered 0 through N.
**
** Return a string giving either a name or a more full description of each
** sample rate converter or NULL if no sample rate converter exists for
** the given value. The converters are sequentially numbered from 0 to N.
*/

const char *src_get_name (int converter_type) ;
const char *src_get_description (int converter_type) ;
const char *src_get_version (void) ;

/*
**	Set a new SRC ratio. This allows step responses
**	in the conversion ratio.
**	Returns non zero on error.
*/

int src_set_ratio (SRC_STATE *state, double new_ratio) ;

/*
**	Get the current channel count.
**	Returns negative on error, positive channel count otherwise
*/

int src_get_channels (SRC_STATE *state) ;

/*
**	Reset the internal SRC state.
**	Does not modify the quality settings.
**	Does not free any memory allocations.
**	Returns non zero on error.
*/

int src_reset (SRC_STATE *state) ;

/*
** Return TRUE if ratio is a valid conversion ratio, FALSE
** otherwise.
*/

int src_is_valid_ratio (double ratio) ;

/*
**	Return an error number.
*/

int src_error (SRC_STATE *state) ;

/*
**	Convert the error number into a string.
*/
const char* src_strerror (int error) ;

/*
** The following enums can be used to set the interpolator type
** using the function src_set_converter().
*/

enum
{
	SRC_SINC_BEST_QUALITY		= 0,
	SRC_SINC_MEDIUM_QUALITY		= 1,
	SRC_SINC_FASTEST			= 2,
	SRC_ZERO_ORDER_HOLD			= 3,
	SRC_LINEAR					= 4,
} ;

/*
** Extra helper functions for converting from short to float and
** back again.
*/

void src_short_to_float_array (const short *in, float *out, int len) ;
void src_float_to_short_array (const float *in, short *out, int len) ;

void src_int_to_float_array (const int *in, float *out, int len) ;
void src_float_to_int_array (const float *in, int *out, int len) ;

/*
** Optional dithering for float -> short conversion.
**
** src_float_to_short_array() rounds to nearest without dither, which is
** audibly granular on quiet material. The functions below add an optional,
** explicitly enabled dither stage while leaving the output bit-for-bit
** unchanged when dithering is disabled.
**
** SRC_DITHER_NONE          : No dither. Output is sample-for-sample identical
**                            to src_float_to_short_array() when bit_depth is
**                            16.
** SRC_DITHER_TPDF          : Flat TPDF dither, 2 LSB peak-to-peak, added
**                            before rounding.
** SRC_DITHER_NOISE_SHAPED  : TPDF dither followed by first order high-pass
**                            error feedback (E(z) = (1 - z^-1) Q(z)), which
**                            moves the quantisation noise towards high
**                            frequencies where it is less audible.
**
** The noise is generated independently for every channel and every target
** bit depth, and the generator state persists across calls so that streaming
** in chunks gives the same result as one large call. Call src_dither_reset()
** to restart a stream from the beginning.
**
** A dither state is not thread safe: a single state must only be used by one
** thread at a time. Different states may be used concurrently.
*/

typedef struct SRC_DITHER_STATE_tag SRC_DITHER_STATE ;

enum
{
	SRC_DITHER_NONE			= 0,
	SRC_DITHER_TPDF			= 1,
	SRC_DITHER_NOISE_SHAPED	= 2
} ;

/*
** Create / destroy a dither state.
**
** dither_type must be one of the SRC_DITHER_* values above, bit_depth must
** be in the range [1, 16] and channels must be >= 1. On failure NULL is
** returned and *error (when not NULL) is set to a distinct SRC error value:
** SRC_ERR_BAD_DITHER_TYPE, SRC_ERR_BAD_BIT_DEPTH,
** SRC_ERR_BAD_CHANNEL_COUNT or SRC_ERR_MALLOC_FAILED.
*/
SRC_DITHER_STATE *src_dither_new (int dither_type, int bit_depth, int channels, int *error) ;

/* Free a dither state. Always returns NULL. NULL is accepted. */
SRC_DITHER_STATE *src_dither_delete (SRC_DITHER_STATE *state) ;

/* Restart the dither stream: RNGs and noise shaping history are reset to the
** values they had immediately after src_dither_new(). Returns an error
** number, SRC_ERR_BAD_STATE if state is NULL. */
int src_dither_reset (SRC_DITHER_STATE *state) ;

/*
** Convert frames frames (samples per channel) of interleaved float data to
** short data using the dither state.
**
** Returns an error number which can be decoded with src_strerror(). In
** particular SRC_ERR_BAD_STATE (state NULL or not initialised),
** SRC_ERR_BAD_PRIV_PTR (state memory corrupted) and SRC_ERR_DATA_OVERLAP
** (the input and output byte ranges overlap) are reported here, while
** SRC_ERR_BAD_DITHER_TYPE, SRC_ERR_BAD_BIT_DEPTH and
** SRC_ERR_BAD_CHANNEL_COUNT are reported by src_dither_new().
*/
int src_float_to_short_array_dithered (SRC_DITHER_STATE *state,
		const float *in, short *out, int frames) ;


#ifdef __cplusplus
}		/* extern "C" */
#endif	/* __cplusplus */

#endif	/* SAMPLERATE_H */

