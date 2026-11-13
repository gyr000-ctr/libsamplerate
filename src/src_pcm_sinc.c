/*
** Copyright (c) 2002-2021, Erik de Castro Lopo <erikd@mega-nerd.com>
** All rights reserved.
**
** This code is released under 2-clause BSD license. Please see the
** file at : https://github.com/libsndfile/libsamplerate/blob/master/COPYING
*/

/*
** Fixed-point sinc converter for the integer PCM path.
**
** This mirrors src_sinc.c control flow exactly (same ratio, latency and
** stream semantics, same frame accounting) but the signal path is
** integer only :
**
**   - Samples are stored and processed as int32 Q31 fixed point.
**   - The coefficient tables are quantized once at state creation from
**     the const reference tables to int32 Q31. This is a one-time
**     initialisation of constant data, not a per-sample intermediate.
**   - Coefficient interpolation, scaling and accumulation use 64 bit
**     integer arithmetic only.
**   - The anti-alias gain scale (min (src_ratio, 1)) is folded into
**     the coefficients before the multiply-accumulate. This keeps the
**     accumulator bounded by ~1.6 * 2**62 (the worst-case strided L1
**     norm of the tables is ~1.59) even at the maximum 1/256 downsample
**     ratio, so a plain int64 accumulator can never overflow.
**
** The only floating point values are the conversion-ratio bookkeeping
** variables (input_index, src_ratio, ...), identical to the float path.
*/

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "common.h"

#define	PCM_SINC_MAGIC_MARKER	MAKE_MAGIC ('p', 's', 'i', 'n', 'c', ' ')

/*========================================================================================
*/

#define MAKE_INCREMENT_T(x) 	((increment_t) (x))

#define	SHIFT_BITS				12
#define	FP_ONE					((double) (((increment_t) 1) << SHIFT_BITS))
#define	INV_FP_ONE				(1.0 / FP_ONE)

/* Customixe max channls from Kconfig. */
#ifndef CONFIG_CHAN_NR
#define MAX_CHANNELS			128
#else
#define MAX_CHANNELS 			CONFIG_CHAN_NR
#endif

/*========================================================================================
*/

typedef int32_t increment_t ;
typedef float	coeff_t ;
typedef int _CHECK_SHIFT_BITS[2 * (SHIFT_BITS < sizeof (increment_t) * 8 - 1) - 1]; /* sanity check. */

#ifdef ENABLE_SINC_FAST_CONVERTER
  #include "fastest_coeffs.h"
#endif
#ifdef ENABLE_SINC_MEDIUM_CONVERTER
  #include "mid_qual_coeffs.h"
#endif
#ifdef ENABLE_SINC_BEST_CONVERTER
  #include "high_qual_coeffs.h"
#endif

typedef struct
{	int		sinc_magic_marker ;

	long	in_count, in_used ;		/* frames */
	long	out_count, out_gen ;	/* frames */

	int		coeff_half_len, index_inc ;

	double	src_ratio, input_index ;

	int32_t			*coeffs ;		/* quantized Q31 copy of the reference table */

	int		b_current, b_end, b_real_end, b_len ;

	/* Sure hope noone does more than 128 channels at once. */
	int64_t left_calc [MAX_CHANNELS], right_calc [MAX_CHANNELS] ;

	int32_t	*buffer ;				/* interleaved Q31 ring buffer */

	PCM_LAYOUT		layout ;
	pcm_read_fn		read ;
	pcm_write_fn	write ;
} PCM_SINC_FILTER ;

static SRC_ERROR pcm_sinc_process (SRC_STATE *state, SRC_PCM_DATA *data) ;

static SRC_ERROR pcm_sinc_prepare_data (PCM_SINC_FILTER *filter, int channels, SRC_PCM_DATA *data, int half_filter_chan_len) WARN_UNUSED ;

static void pcm_sinc_reset (SRC_STATE *state) ;
static SRC_STATE *pcm_sinc_copy (SRC_STATE *state) ;
static void pcm_sinc_close (SRC_STATE *state) ;

/* vt process stub : the float entry points must never be used on a PCM state. */
static SRC_ERROR
pcm_sinc_wrong_api_process (SRC_STATE *state, SRC_DATA *data)
{	(void) state ;
	(void) data ;
	return SRC_ERR_WRONG_API ;
} /* pcm_sinc_wrong_api_process */

static SRC_STATE_VT pcm_sinc_state_vt =
{
	pcm_sinc_wrong_api_process,
	pcm_sinc_wrong_api_process,
	pcm_sinc_reset,
	pcm_sinc_copy,
	pcm_sinc_close
} ;

static inline increment_t
double_to_fp (double x)
{	return (increment_t) (psf_lrint ((x) * FP_ONE)) ;
} /* double_to_fp */

static inline increment_t
int_to_fp (int x)
{	return (((increment_t) (x)) << SHIFT_BITS) ;
} /* int_to_fp */

static inline int
fp_to_int (increment_t x)
{	return (((x) >> SHIFT_BITS)) ;
} /* fp_to_int */

static inline increment_t
fp_fraction_part (increment_t x)
{	return ((x) & ((((increment_t) 1) << SHIFT_BITS) - 1)) ;
} /* fp_fraction_part */

static inline int
int_div_ceil (int divident, int divisor) /* == (int) ceil ((float) divident / divisor) */
{	assert (divident >= 0 && divisor > 0) ; /* For positive numbers only */
	return (divident + (divisor - 1)) / divisor ;
}

/*----------------------------------------------------------------------------------------
*/

static PCM_SINC_FILTER *
pcm_sinc_filter_new (int converter_type, int channels, const PCM_LAYOUT *layout)
{
	assert (converter_type == SRC_SINC_FASTEST ||
		converter_type == SRC_SINC_MEDIUM_QUALITY ||
		converter_type == SRC_SINC_BEST_QUALITY) ;
	assert (channels > 0 && channels <= MAX_CHANNELS) ;
	assert (layout != NULL) ;

	PCM_SINC_FILTER *priv = (PCM_SINC_FILTER *) calloc (1, sizeof (PCM_SINC_FILTER)) ;
	if (!priv)
		return NULL ;

	priv->sinc_magic_marker = PCM_SINC_MAGIC_MARKER ;
	priv->layout = *layout ;
	priv->read = pcm_select_reader (layout->in_bits, layout->interleaved) ;
	priv->write = pcm_select_writer (layout->out_bits, layout->interleaved) ;

	coeff_t const *src_coeffs = NULL ;
	int coeff_count = 0 ;

	switch (converter_type)
	{
#ifdef ENABLE_SINC_FAST_CONVERTER
		case SRC_SINC_FASTEST :
			src_coeffs = fastest_coeffs.coeffs ;
			coeff_count = ARRAY_LEN (fastest_coeffs.coeffs) ;
			priv->index_inc = fastest_coeffs.increment ;
			break ;
#endif
#ifdef ENABLE_SINC_MEDIUM_CONVERTER
		case SRC_SINC_MEDIUM_QUALITY :
			src_coeffs = slow_mid_qual_coeffs.coeffs ;
			coeff_count = ARRAY_LEN (slow_mid_qual_coeffs.coeffs) ;
			priv->index_inc = slow_mid_qual_coeffs.increment ;
			break ;
#endif
#ifdef ENABLE_SINC_BEST_CONVERTER
		case SRC_SINC_BEST_QUALITY :
			src_coeffs = slow_high_qual_coeffs.coeffs ;
			coeff_count = ARRAY_LEN (slow_high_qual_coeffs.coeffs) ;
			priv->index_inc = slow_high_qual_coeffs.increment ;
			break ;
#endif
		}

	if (!src_coeffs || !priv->read || !priv->write)
	{
		free (priv) ;
		return NULL ;
	}

	priv->coeff_half_len = coeff_count - 2 ;

	/* One-time quantization of the constant reference table to Q31. */
	priv->coeffs = (int32_t *) malloc (sizeof (int32_t) * coeff_count) ;
	if (!priv->coeffs)
	{
		free (priv) ;
		return NULL ;
	}
	for (int k = 0 ; k < coeff_count ; k++)
	{
		int64_t c = (int64_t) psf_lrint ((double) src_coeffs [k] * 2147483648.0) ;
		if (c > INT32_MAX)
			c = INT32_MAX ;
		else if (c < INT32_MIN)
			c = INT32_MIN ;
		priv->coeffs [k] = (int32_t) c ;
	}

	priv->b_len = 3 * (int) psf_lrint ((priv->coeff_half_len + 2.0) / priv->index_inc * SRC_MAX_RATIO + 1) ;
	priv->b_len = MAX (priv->b_len, 4096) ;
	priv->b_len *= channels ;
	priv->b_len += 1 ; // There is a <= check against samples_in_hand requiring a buffer bigger than the calculation above

	priv->buffer = (int32_t *) calloc (priv->b_len + channels, sizeof (int32_t)) ;
	if (!priv->buffer)
	{
		free (priv->coeffs) ;
		free (priv) ;
		return NULL ;
	}

	return priv ;
} /* pcm_sinc_filter_new */

LIBSAMPLERATE_DLL_PRIVATE SRC_STATE *
pcm_sinc_state_new (int converter_type, int channels, const PCM_LAYOUT *layout, SRC_ERROR *error)
{
	assert (converter_type == SRC_SINC_FASTEST ||
		converter_type == SRC_SINC_MEDIUM_QUALITY ||
		converter_type == SRC_SINC_BEST_QUALITY) ;
	assert (channels > 0) ;
	assert (layout != NULL) ;
	assert (error != NULL) ;

	if (channels > MAX_CHANNELS)
	{
		*error = SRC_ERR_BAD_CHANNEL_COUNT ;
		return NULL ;
	}

	SRC_STATE *state = (SRC_STATE *) calloc (1, sizeof (SRC_STATE)) ;
	if (!state)
	{
		*error = SRC_ERR_MALLOC_FAILED ;
		return NULL ;
	}

	state->channels = channels ;
	state->mode = SRC_MODE_PROCESS ;
	state->pcm_process = pcm_sinc_process ;
	state->vt = &pcm_sinc_state_vt ;

	state->private_data = pcm_sinc_filter_new (converter_type, state->channels, layout) ;
	if (!state->private_data)
	{
		free (state) ;
		*error = SRC_ERR_MALLOC_FAILED ;
		return NULL ;
	}

	pcm_sinc_reset (state) ;

	*error = SRC_ERR_NO_ERROR ;

	return state ;
} /* pcm_sinc_state_new */

static void
pcm_sinc_reset (SRC_STATE *state)
{	PCM_SINC_FILTER *filter ;

	filter = (PCM_SINC_FILTER *) state->private_data ;
	if (filter == NULL)
		return ;

	filter->b_current = filter->b_end = 0 ;
	filter->b_real_end = -1 ;

	filter->src_ratio = filter->input_index = 0.0 ;

	memset (filter->buffer, 0, filter->b_len * sizeof (filter->buffer [0])) ;

	/* Set this for a sanity check */
	memset (filter->buffer + filter->b_len, 0xAA, state->channels * sizeof (filter->buffer [0])) ;
} /* pcm_sinc_reset */

static SRC_STATE *
pcm_sinc_copy (SRC_STATE *state)
{
	assert (state != NULL) ;

	if (state->private_data == NULL)
		return NULL ;

	SRC_STATE *to = (SRC_STATE *) calloc (1, sizeof (SRC_STATE)) ;
	if (!to)
		return NULL ;
	memcpy (to, state, sizeof (SRC_STATE)) ;

	PCM_SINC_FILTER *from_filter = (PCM_SINC_FILTER *) state->private_data ;
	PCM_SINC_FILTER *to_filter = (PCM_SINC_FILTER *) calloc (1, sizeof (PCM_SINC_FILTER)) ;
	if (!to_filter)
	{
		free (to) ;
		return NULL ;
	}
	memcpy (to_filter, from_filter, sizeof (PCM_SINC_FILTER)) ;

	to_filter->buffer = (int32_t *) malloc (sizeof (int32_t) * (from_filter->b_len + state->channels)) ;
	if (!to_filter->buffer)
	{
		free (to) ;
		free (to_filter) ;
		return NULL ;
	}
	memcpy (to_filter->buffer, from_filter->buffer, sizeof (int32_t) * (from_filter->b_len + state->channels)) ;

	to_filter->coeffs = (int32_t *) malloc (sizeof (int32_t) * (from_filter->coeff_half_len + 2)) ;
	if (!to_filter->coeffs)
	{
		free (to_filter->buffer) ;
		free (to) ;
		free (to_filter) ;
		return NULL ;
	}
	memcpy (to_filter->coeffs, from_filter->coeffs, sizeof (int32_t) * (from_filter->coeff_half_len + 2)) ;

	to->private_data = to_filter ;

	return to ;
} /* pcm_sinc_copy */

/*========================================================================================
**	Beware all ye who dare pass this point. There be dragons here.
*/

static inline void
pcm_sinc_calc_output (PCM_SINC_FILTER *filter, increment_t increment, increment_t start_filter_index,
		int64_t scale_q31, int channels, int32_t *output)
{	int64_t		*left, *right ;
	increment_t	filter_index, max_filter_index ;
	int			data_index, coeff_count, indx ;

	left = filter->left_calc ;
	right = filter->right_calc ;

	/* Convert input parameters into fixed point. */
	max_filter_index = int_to_fp (filter->coeff_half_len) ;

	/* First apply the left half of the filter. */
	filter_index = start_filter_index ;
	coeff_count = (max_filter_index - filter_index) / increment ;
	filter_index = filter_index + coeff_count * increment ;
	data_index = filter->b_current - channels * coeff_count ;

	if (data_index < 0) /* Avoid underflow access to filter->buffer. */
	{	int steps = int_div_ceil (-data_index, channels) ;
		/* If the assert triggers we would have to take care not to underflow/overflow */
		assert (steps <= int_div_ceil (filter_index, increment)) ;
		filter_index -= increment * steps ;
		data_index += steps * channels ;
	}

	memset (left, 0, sizeof (left [0]) * channels) ;

	while (filter_index >= MAKE_INCREMENT_T (0))
	{	increment_t fraction = fp_fraction_part (filter_index) ;
		indx = fp_to_int (filter_index) ;
		assert (indx >= 0 && indx + 1 < filter->coeff_half_len + 2) ;
		assert (data_index >= 0 && data_index + channels - 1 < filter->b_len) ;
		assert (data_index + channels - 1 < filter->b_end) ;

		/* Interpolated Q31 coefficient, then fold in the anti-alias
		** gain scale (both operands < 2**31, products fit in int64). */
		int32_t c0 = filter->coeffs [indx] ;
		int32_t c1 = filter->coeffs [indx + 1] ;
		int32_t icoeff = c0 + (int32_t) ((((int64_t) c1 - (int64_t) c0) * fraction) >> SHIFT_BITS) ;
		int32_t scoeff = (int32_t) (((int64_t) icoeff * scale_q31 + (1LL << 30)) >> 31) ;

		for (int ch = 0 ; ch < channels ; ch++)
			left [ch] += (int64_t) scoeff * filter->buffer [data_index + ch] ;

		filter_index -= increment ;
		data_index = data_index + channels ;
		} ;

	/* Now apply the right half of the filter. */
	filter_index = increment - start_filter_index ;
	coeff_count = (max_filter_index - filter_index) / increment ;
	filter_index = filter_index + coeff_count * increment ;
	data_index = filter->b_current + channels * (1 + coeff_count) ;

	memset (right, 0, sizeof (right [0]) * channels) ;
	do
	{	increment_t fraction = fp_fraction_part (filter_index) ;
		indx = fp_to_int (filter_index) ;
		assert (indx >= 0 && indx + 1 < filter->coeff_half_len + 2) ;
		assert (data_index >= 0 && data_index + channels - 1 < filter->b_len) ;
		assert (data_index + channels - 1 < filter->b_end) ;

		int32_t c0 = filter->coeffs [indx] ;
		int32_t c1 = filter->coeffs [indx + 1] ;
		int32_t icoeff = c0 + (int32_t) ((((int64_t) c1 - (int64_t) c0) * fraction) >> SHIFT_BITS) ;
		int32_t scoeff = (int32_t) (((int64_t) icoeff * scale_q31 + (1LL << 30)) >> 31) ;

		for (int ch = 0 ; ch < channels ; ch++)
			right [ch] += (int64_t) scoeff * filter->buffer [data_index + ch] ;

		filter_index -= increment ;
		data_index = data_index - channels ;
		}
	while (filter_index > MAKE_INCREMENT_T (0)) ;

	for (int ch = 0 ; ch < channels ; ch++)
	{	/* Accumulator is Q62 (Q31 coeff times Q31 sample). Round to
		** Q31 and saturate ; never wrap. */
		int64_t acc = (left [ch] + right [ch] + (1LL << 30)) >> 31 ;
		if (acc > INT32_MAX)
			acc = INT32_MAX ;
		else if (acc < INT32_MIN)
			acc = INT32_MIN ;
		output [ch] = (int32_t) acc ;
		} ;

	return ;
} /* pcm_sinc_calc_output */

static SRC_ERROR
pcm_sinc_process (SRC_STATE *state, SRC_PCM_DATA *data)
{	PCM_SINC_FILTER *filter ;
	double		input_index, src_ratio, count, float_increment, terminate, rem ;
	increment_t	increment, start_filter_index ;
	int			half_filter_chan_len, samples_in_hand ;
	int64_t		scale_q31 ;
	int32_t		output [MAX_CHANNELS] ;

	if (state->private_data == NULL)
		return SRC_ERR_NO_PRIVATE ;

	filter = (PCM_SINC_FILTER *) state->private_data ;

	filter->in_count = data->input_frames ;
	filter->out_count = data->output_frames ;
	filter->in_used = filter->out_gen = 0 ;

	src_ratio = state->last_ratio ;

	if (is_bad_src_ratio (src_ratio))
		return SRC_ERR_BAD_INTERNAL_STATE ;

	/* Check the sample rate ratio wrt the buffer len. */
	count = (filter->coeff_half_len + 2.0) / filter->index_inc ;
	if (MIN (state->last_ratio, data->src_ratio) < 1.0)
		count /= MIN (state->last_ratio, data->src_ratio) ;

	/* Maximum coefficientson either side of center point. */
	half_filter_chan_len = state->channels * (int) (psf_lrint (count) + 1) ;

	input_index = state->last_position ;

	rem = fmod_one (input_index) ;
	filter->b_current = (filter->b_current + state->channels * psf_lrint (input_index - rem)) % filter->b_len ;
	input_index = rem ;

	terminate = 1.0 / src_ratio + 1e-20 ;

	/* Main processing loop. */
	while (filter->out_gen < filter->out_count)
	{
		/* Need to reload buffer? */
		samples_in_hand = (filter->b_end - filter->b_current + filter->b_len) % filter->b_len ;

		if (samples_in_hand <= half_filter_chan_len)
		{	if ((state->error = pcm_sinc_prepare_data (filter, state->channels, data, half_filter_chan_len)) != 0)
				return state->error ;

			samples_in_hand = (filter->b_end - filter->b_current + filter->b_len) % filter->b_len ;
			if (samples_in_hand <= half_filter_chan_len)
				break ;
			} ;

		/* This is the termination condition. The float path uses '>'
		** for mono and '>=' for multichannel ; mirror that quirk so
		** that end-of-stream frame counts match the float path
		** exactly for every channel count. */
		if (filter->b_real_end >= 0)
		{	double t = filter->b_current + input_index + terminate ;
			if ((state->channels == 1 && t > filter->b_real_end) ||
					(state->channels != 1 && t >= filter->b_real_end))
				break ;
			} ;

		if (filter->out_count > 0 && fabs (state->last_ratio - data->src_ratio) > 1e-10)
			src_ratio = state->last_ratio + filter->out_gen * (data->src_ratio - state->last_ratio) / filter->out_count ;

		float_increment = filter->index_inc * (src_ratio < 1.0 ? src_ratio : 1.0) ;
		increment = double_to_fp (float_increment) ;

		start_filter_index = double_to_fp (input_index * float_increment) ;

		/* Anti-alias gain scale, min (src_ratio, 1) as Q31. */
		scale_q31 = (int64_t) ((src_ratio < 1.0 ? src_ratio : 1.0) * 2147483648.0 + 0.5) ;

		pcm_sinc_calc_output (filter, increment, start_filter_index, scale_q31, state->channels, output) ;

		for (int ch = 0 ; ch < state->channels ; ch++)
			filter->write (data->data_out, filter->out_gen, ch, state->channels, output [ch]) ;
		filter->out_gen ++ ;

		/* Figure out the next index. */
		input_index += 1.0 / src_ratio ;
		rem = fmod_one (input_index) ;

		filter->b_current = (filter->b_current + state->channels * psf_lrint (input_index - rem)) % filter->b_len ;
		input_index = rem ;
		} ;

	state->last_position = input_index ;

	/* Save current ratio rather then target ratio. */
	state->last_ratio = src_ratio ;

	data->input_frames_used = filter->in_used ;
	data->output_frames_gen = filter->out_gen ;

	return SRC_ERR_NO_ERROR ;
} /* pcm_sinc_process */

/*----------------------------------------------------------------------------------------
*/

static SRC_ERROR
pcm_sinc_prepare_data (PCM_SINC_FILTER *filter, int channels, SRC_PCM_DATA *data, int half_filter_chan_len)
{	int len = 0 ;

	if (filter->b_real_end >= 0)
		return SRC_ERR_NO_ERROR ;	/* Should be terminating. Just return. */

	if (data->data_in == NULL)
		return SRC_ERR_NO_ERROR ;

	if (filter->b_current == 0)
	{	/* Initial state. Set up zeros at the start of the buffer and
		** then load new data after that.
		*/
		len = filter->b_len - 2 * half_filter_chan_len ;

		filter->b_current = filter->b_end = half_filter_chan_len ;
		}
	else if (filter->b_end + half_filter_chan_len + channels < filter->b_len)
	{	/*  Load data at current end position. */
		len = MAX (filter->b_len - filter->b_current - half_filter_chan_len, 0) ;
		}
	else
	{	/* Move data at end of buffer back to the start of the buffer. */
		len = filter->b_end - filter->b_current ;
		memmove (filter->buffer, filter->buffer + filter->b_current - half_filter_chan_len,
						(half_filter_chan_len + len) * sizeof (filter->buffer [0])) ;

		filter->b_current = half_filter_chan_len ;
		filter->b_end = filter->b_current + len ;

		/* Now load data at current end of buffer. */
		len = MAX (filter->b_len - filter->b_current - half_filter_chan_len, 0) ;
		} ;

	/* len is in interleaved samples ; convert the frame count. */
	len = MIN ((int) ((filter->in_count - filter->in_used) * channels), len) ;
	len -= (len % channels) ;

	if (len < 0 || filter->b_end + len > filter->b_len)
		return SRC_ERR_SINC_PREPARE_DATA_BAD_LEN ;

	/* Unpack PCM input straight into the Q31 ring buffer. */
	{
		int			frames = len / channels ;
		int32_t		*dst = filter->buffer + filter->b_end ;
		long		first_frame = filter->in_used ;

		for (int f = 0 ; f < frames ; f++)
			for (int ch = 0 ; ch < channels ; ch++)
				*dst ++ = filter->read (data->data_in, first_frame + f, ch, channels) ;
		} ;

	filter->b_end += len ;
	filter->in_used += len / channels ;

	if (filter->in_used == filter->in_count &&
			filter->b_end - filter->b_current < 2 * half_filter_chan_len && data->end_of_input)
	{	/* Handle the case where all data in the current buffer has been
		** consumed and this is the last buffer.
		*/

		if (filter->b_len - filter->b_end < half_filter_chan_len + 5)
		{	/* If necessary, move data down to the start of the buffer. */
			len = filter->b_end - filter->b_current ;
			memmove (filter->buffer, filter->buffer + filter->b_current - half_filter_chan_len,
							(half_filter_chan_len + len) * sizeof (filter->buffer [0])) ;

			filter->b_current = half_filter_chan_len ;
			filter->b_end = filter->b_current + len ;
			} ;

		filter->b_real_end = filter->b_end ;
		len = half_filter_chan_len + 5 ;

		if (len < 0 || filter->b_end + len > filter->b_len)
			len = filter->b_len - filter->b_end ;

		memset (filter->buffer + filter->b_end, 0, len * sizeof (filter->buffer [0])) ;
		filter->b_end += len ;
		} ;

	return SRC_ERR_NO_ERROR ;
} /* pcm_sinc_prepare_data */

static void
pcm_sinc_close (SRC_STATE *state)
{
	if (state)
	{
		PCM_SINC_FILTER *sinc = (PCM_SINC_FILTER *) state->private_data ;
		if (sinc)
		{
			if (sinc->buffer)
			{
				free (sinc->buffer) ;
				sinc->buffer = NULL ;
			}
			if (sinc->coeffs)
			{
				free (sinc->coeffs) ;
				sinc->coeffs = NULL ;
			}
			free (sinc) ;
			sinc = NULL ;
		}
		free (state) ;
		state = NULL ;
	}
} /* pcm_sinc_close */
