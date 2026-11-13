/*
** Copyright (c) 2002-2021, Erik de Castro Lopo <erikd@mega-nerd.com>
** All rights reserved.
**
** This code is released under 2-clause BSD license. Please see the
** file at : https://github.com/libsndfile/libsamplerate/blob/master/COPYING
*/

/*
** Integer / fixed-point processing path.
**
** This file contains :
**   - PCM sample readers/writers for signed 16/24/32 bit PCM in both
**     interleaved and planar layouts. Samples are exchanged with the
**     converters as int32 Q31 fixed point values.
**   - Fixed-point variants of the ZOH and linear converters.
**
** The signal path here is integer only. No floating point intermediate
** sample data is used anywhere ; the only floating point values are the
** conversion-ratio bookkeeping variables, which are identical to the
** float path so that ratio, latency and stream semantics match.
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

/*========================================================================================
** PCM sample format conversion helpers (Q31 <-> 16/24/32 bit PCM).
*/

static inline int32_t
q31_from_s16 (int16_t s)
{	/* Shift left so the sign bit lands on bit 31. */
	return (int32_t) ((uint32_t) (uint16_t) s << 16) ;
} /* q31_from_s16 */

static inline int32_t
q31_from_s24 (const uint8_t *p)
{	/* Packed little-endian 24 bit. Shifting left by 8 moves the
	** 24 bit sign bit (bit 23) onto bit 31, sign-extending for free. */
	uint32_t v = (uint32_t) p [0] | ((uint32_t) p [1] << 8) | ((uint32_t) p [2] << 16) ;
	return (int32_t) (v << 8) ;
} /* q31_from_s24 */

static inline int16_t
s16_from_q31 (int32_t q)
{	int32_t v = (int32_t) (((int64_t) q + 0x8000) >> 16) ;
	if (v > 32767)
		v = 32767 ;
	else if (v < -32768)
		v = -32768 ;
	return (int16_t) v ;
} /* s16_from_q31 */

static inline int32_t
s24_from_q31 (int32_t q)
{	int32_t v = (int32_t) (((int64_t) q + 0x80) >> 8) ;
	if (v > 0x7FFFFF)
		v = 0x7FFFFF ;
	else if (v < -0x800000)
		v = -0x800000 ;
	return v ;
} /* s24_from_q31 */

static inline void
store_s24 (uint8_t *p, int32_t v)
{	p [0] = (uint8_t) (v) ;
	p [1] = (uint8_t) (v >> 8) ;
	p [2] = (uint8_t) (v >> 16) ;
} /* store_s24 */

/*========================================================================================
** PCM readers : (buffer, frame, channel) -> int32 Q31.
*/

static int32_t
rd_s16_interleaved (const void *in, long frame, int ch, int channels)
{	return q31_from_s16 (((const int16_t *) in) [frame * channels + ch]) ;
}

static int32_t
rd_s16_planar (const void *in, long frame, int ch, int channels)
{	const void *const *planes = (const void *const *) in ;
	(void) channels ;
	return q31_from_s16 (((const int16_t *) planes [ch]) [frame]) ;
}

static int32_t
rd_s24_interleaved (const void *in, long frame, int ch, int channels)
{	return q31_from_s24 (((const uint8_t *) in) + 3 * (frame * channels + ch)) ;
}

static int32_t
rd_s24_planar (const void *in, long frame, int ch, int channels)
{	const void *const *planes = (const void *const *) in ;
	(void) channels ;
	return q31_from_s24 (((const uint8_t *) planes [ch]) + 3 * frame) ;
}

static int32_t
rd_s32_interleaved (const void *in, long frame, int ch, int channels)
{	return ((const int32_t *) in) [frame * channels + ch] ;
}

static int32_t
rd_s32_planar (const void *in, long frame, int ch, int channels)
{	const void *const *planes = (const void *const *) in ;
	(void) channels ;
	return ((const int32_t *) planes [ch]) [frame] ;
}

/*========================================================================================
** PCM writers : int32 Q31 -> (buffer, frame, channel), with rounding
** and saturation (clipping is handled here, never by silent wraparound).
*/

static void
wr_s16_interleaved (void *out, long frame, int ch, int channels, int32_t q31)
{	((int16_t *) out) [frame * channels + ch] = s16_from_q31 (q31) ;
}

static void
wr_s16_planar (void *out, long frame, int ch, int channels, int32_t q31)
{	void *const *planes = (void *const *) out ;
	(void) channels ;
	((int16_t *) planes [ch]) [frame] = s16_from_q31 (q31) ;
}

static void
wr_s24_interleaved (void *out, long frame, int ch, int channels, int32_t q31)
{	store_s24 (((uint8_t *) out) + 3 * (frame * channels + ch), s24_from_q31 (q31)) ;
}

static void
wr_s24_planar (void *out, long frame, int ch, int channels, int32_t q31)
{	void *const *planes = (void *const *) out ;
	(void) channels ;
	store_s24 (((uint8_t *) planes [ch]) + 3 * frame, s24_from_q31 (q31)) ;
}

static void
wr_s32_interleaved (void *out, long frame, int ch, int channels, int32_t q31)
{	((int32_t *) out) [frame * channels + ch] = q31 ;
}

static void
wr_s32_planar (void *out, long frame, int ch, int channels, int32_t q31)
{	void *const *planes = (void *const *) out ;
	(void) channels ;
	((int32_t *) planes [ch]) [frame] = q31 ;
}

/*========================================================================================
*/

LIBSAMPLERATE_DLL_PRIVATE int
pcm_is_valid_bits (int bits)
{	return bits == SRC_PCM_S16 || bits == SRC_PCM_S24 || bits == SRC_PCM_S32 ;
} /* pcm_is_valid_bits */

LIBSAMPLERATE_DLL_PRIVATE pcm_read_fn
pcm_select_reader (int bits, int interleaved)
{
	if (interleaved)
	{	switch (bits)
		{	case SRC_PCM_S16 : return rd_s16_interleaved ;
			case SRC_PCM_S24 : return rd_s24_interleaved ;
			case SRC_PCM_S32 : return rd_s32_interleaved ;
			} ;
		}
	else
	{	switch (bits)
		{	case SRC_PCM_S16 : return rd_s16_planar ;
			case SRC_PCM_S24 : return rd_s24_planar ;
			case SRC_PCM_S32 : return rd_s32_planar ;
			} ;
		} ;

	return NULL ;
} /* pcm_select_reader */

LIBSAMPLERATE_DLL_PRIVATE pcm_write_fn
pcm_select_writer (int bits, int interleaved)
{
	if (interleaved)
	{	switch (bits)
		{	case SRC_PCM_S16 : return wr_s16_interleaved ;
			case SRC_PCM_S24 : return wr_s24_interleaved ;
			case SRC_PCM_S32 : return wr_s32_interleaved ;
			} ;
		}
	else
	{	switch (bits)
		{	case SRC_PCM_S16 : return wr_s16_planar ;
			case SRC_PCM_S24 : return wr_s24_planar ;
			case SRC_PCM_S32 : return wr_s32_planar ;
			} ;
		} ;

	return NULL ;
} /* pcm_select_writer */

/*========================================================================================
** Common plumbing for the fixed-point converters.
*/

/* vt process stub : the float entry points must never be used on a PCM state. */
static SRC_ERROR
pcm_wrong_api_process (SRC_STATE *state, SRC_DATA *data)
{	(void) state ;
	(void) data ;
	return SRC_ERR_WRONG_API ;
} /* pcm_wrong_api_process */

/*========================================================================================
** Fixed-point ZOH converter. Mirrors src_zoh.c control flow exactly ;
** only the sample format differs (int32 Q31 instead of float).
*/

#define	PCM_ZOH_MAGIC_MARKER	MAKE_MAGIC ('p', 'c', 'm', 'z', 'o', 'h')

typedef struct
{	int		zoh_magic_marker ;
	bool	dirty ;
	long	in_count, in_used ;		/* frames */
	long	out_count, out_gen ;	/* frames */
	int32_t	*last_value ;
	PCM_LAYOUT		layout ;
	pcm_read_fn		read ;
	pcm_write_fn	write ;
} PCM_ZOH_DATA ;

static SRC_ERROR pcm_zoh_process (SRC_STATE *state, SRC_PCM_DATA *data) ;
static void pcm_zoh_reset (SRC_STATE *state) ;
static SRC_STATE *pcm_zoh_copy (SRC_STATE *state) ;
static void pcm_zoh_close (SRC_STATE *state) ;

static SRC_STATE_VT pcm_zoh_state_vt =
{
	pcm_wrong_api_process,
	pcm_wrong_api_process,
	pcm_zoh_reset,
	pcm_zoh_copy,
	pcm_zoh_close
} ;

static SRC_ERROR
pcm_zoh_process (SRC_STATE *state, SRC_PCM_DATA *data)
{	PCM_ZOH_DATA	*priv ;
	double		src_ratio, input_index, rem ;
	int			ch, channels ;

	if (data->input_frames <= 0)
		return SRC_ERR_NO_ERROR ;

	if (state->private_data == NULL)
		return SRC_ERR_NO_PRIVATE ;

	priv = (PCM_ZOH_DATA *) state->private_data ;
	channels = state->channels ;

	if (!priv->dirty)
	{	/* If we have just been reset, set the last_value data. */
		for (ch = 0 ; ch < channels ; ch++)
			priv->last_value [ch] = priv->read (data->data_in, 0, ch, channels) ;
		priv->dirty = true ;
		} ;

	priv->in_count = data->input_frames ;
	priv->out_count = data->output_frames ;
	priv->in_used = priv->out_gen = 0 ;

	src_ratio = state->last_ratio ;

	if (is_bad_src_ratio (src_ratio))
		return SRC_ERR_BAD_INTERNAL_STATE ;

	input_index = state->last_position ;

	/* Calculate samples before first sample in input array. */
	while (input_index < 1.0 && priv->out_gen < priv->out_count)
	{
		if (priv->in_used + input_index >= priv->in_count)
			break ;

		if (priv->out_count > 0 && fabs (state->last_ratio - data->src_ratio) > SRC_MIN_RATIO_DIFF)
			src_ratio = state->last_ratio + priv->out_gen * (data->src_ratio - state->last_ratio) / priv->out_count ;

		for (ch = 0 ; ch < channels ; ch++)
			priv->write (data->data_out, priv->out_gen, ch, channels, priv->last_value [ch]) ;
		priv->out_gen ++ ;

		/* Figure out the next index. */
		input_index += 1.0 / src_ratio ;
		} ;

	rem = fmod_one (input_index) ;
	priv->in_used += psf_lrint (input_index - rem) ;
	input_index = rem ;

	/* Main processing loop. */
	while (priv->out_gen < priv->out_count && priv->in_used + input_index <= priv->in_count)
	{
		if (priv->out_count > 0 && fabs (state->last_ratio - data->src_ratio) > SRC_MIN_RATIO_DIFF)
			src_ratio = state->last_ratio + priv->out_gen * (data->src_ratio - state->last_ratio) / priv->out_count ;

		for (ch = 0 ; ch < channels ; ch++)
			priv->write (data->data_out, priv->out_gen, ch, channels,
					priv->read (data->data_in, priv->in_used - 1, ch, channels)) ;
		priv->out_gen ++ ;

		/* Figure out the next index. */
		input_index += 1.0 / src_ratio ;
		rem = fmod_one (input_index) ;

		priv->in_used += psf_lrint (input_index - rem) ;
		input_index = rem ;
		} ;

	if (priv->in_used > priv->in_count)
	{	input_index += (priv->in_used - priv->in_count) ;
		priv->in_used = priv->in_count ;
		} ;

	state->last_position = input_index ;

	if (priv->in_used > 0)
		for (ch = 0 ; ch < channels ; ch++)
			priv->last_value [ch] = priv->read (data->data_in, priv->in_used - 1, ch, channels) ;

	/* Save current ratio rather then target ratio. */
	state->last_ratio = src_ratio ;

	data->input_frames_used = priv->in_used ;
	data->output_frames_gen = priv->out_gen ;

	return SRC_ERR_NO_ERROR ;
} /* pcm_zoh_process */

LIBSAMPLERATE_DLL_PRIVATE SRC_STATE *
pcm_zoh_state_new (int channels, const PCM_LAYOUT *layout, SRC_ERROR *error)
{
	assert (channels > 0) ;
	assert (layout != NULL) ;
	assert (error != NULL) ;

	SRC_STATE *state = (SRC_STATE *) calloc (1, sizeof (SRC_STATE)) ;
	if (!state)
	{
		*error = SRC_ERR_MALLOC_FAILED ;
		return NULL ;
	}

	state->channels = channels ;
	state->mode = SRC_MODE_PROCESS ;
	state->pcm_process = pcm_zoh_process ;

	PCM_ZOH_DATA *priv = (PCM_ZOH_DATA *) calloc (1, sizeof (PCM_ZOH_DATA)) ;
	if (priv)
	{
		priv->zoh_magic_marker = PCM_ZOH_MAGIC_MARKER ;
		priv->layout = *layout ;
		priv->read = pcm_select_reader (layout->in_bits, layout->interleaved) ;
		priv->write = pcm_select_writer (layout->out_bits, layout->interleaved) ;
		priv->last_value = (int32_t *) calloc (channels, sizeof (int32_t)) ;
		if (!priv->last_value || !priv->read || !priv->write)
		{
			if (priv->last_value)
				free (priv->last_value) ;
			free (priv) ;
			priv = NULL ;
		}
	}

	state->private_data = priv ;
	if (!state->private_data)
	{
		free (state) ;
		*error = SRC_ERR_MALLOC_FAILED ;
		return NULL ;
	}

	state->vt = &pcm_zoh_state_vt ;

	pcm_zoh_reset (state) ;

	*error = SRC_ERR_NO_ERROR ;

	return state ;
} /* pcm_zoh_state_new */

static void
pcm_zoh_reset (SRC_STATE *state)
{	PCM_ZOH_DATA *priv ;

	priv = (PCM_ZOH_DATA *) state->private_data ;
	if (priv == NULL)
		return ;

	priv->dirty = false ;
	memset (priv->last_value, 0, sizeof (int32_t) * state->channels) ;

	return ;
} /* pcm_zoh_reset */

static SRC_STATE *
pcm_zoh_copy (SRC_STATE *state)
{
	assert (state != NULL) ;

	if (state->private_data == NULL)
		return NULL ;

	SRC_STATE *to = (SRC_STATE *) calloc (1, sizeof (SRC_STATE)) ;
	if (!to)
		return NULL ;
	memcpy (to, state, sizeof (SRC_STATE)) ;

	PCM_ZOH_DATA *from_priv = (PCM_ZOH_DATA *) state->private_data ;
	PCM_ZOH_DATA *to_priv = (PCM_ZOH_DATA *) calloc (1, sizeof (PCM_ZOH_DATA)) ;
	if (!to_priv)
	{
		free (to) ;
		return NULL ;
	}

	memcpy (to_priv, from_priv, sizeof (PCM_ZOH_DATA)) ;
	to_priv->last_value = (int32_t *) malloc (sizeof (int32_t) * state->channels) ;
	if (!to_priv->last_value)
	{
		free (to) ;
		free (to_priv) ;
		return NULL ;
	}
	memcpy (to_priv->last_value, from_priv->last_value, sizeof (int32_t) * state->channels) ;

	to->private_data = to_priv ;

	return to ;
} /* pcm_zoh_copy */

static void
pcm_zoh_close (SRC_STATE *state)
{
	if (state)
	{
		PCM_ZOH_DATA *zoh = (PCM_ZOH_DATA *) state->private_data ;
		if (zoh)
		{
			if (zoh->last_value)
			{
				free (zoh->last_value) ;
				zoh->last_value = NULL ;
			}
			free (zoh) ;
			zoh = NULL ;
		}
		free (state) ;
		state = NULL ;
	}
} /* pcm_zoh_close */

/*========================================================================================
** Fixed-point linear converter. Mirrors src_linear.c control flow
** exactly ; interpolation is done in 64 bit fixed point.
*/

#define	PCM_LINEAR_MAGIC_MARKER	MAKE_MAGIC ('p', 'l', 'i', 'n', 'e', 'a')

typedef struct
{	int		linear_magic_marker ;
	bool	dirty ;
	long	in_count, in_used ;		/* frames */
	long	out_count, out_gen ;	/* frames */
	int32_t	*last_value ;
	PCM_LAYOUT		layout ;
	pcm_read_fn		read ;
	pcm_write_fn	write ;
} PCM_LINEAR_DATA ;

static SRC_ERROR pcm_linear_process (SRC_STATE *state, SRC_PCM_DATA *data) ;
static void pcm_linear_reset (SRC_STATE *state) ;
static SRC_STATE *pcm_linear_copy (SRC_STATE *state) ;
static void pcm_linear_close (SRC_STATE *state) ;

static SRC_STATE_VT pcm_linear_state_vt =
{
	pcm_wrong_api_process,
	pcm_wrong_api_process,
	pcm_linear_reset,
	pcm_linear_copy,
	pcm_linear_close
} ;

static inline int32_t
pcm_lerp (int32_t a, int32_t b, uint32_t t_q31)
{	/* a + t * (b - a), all Q31. The product fits in int64 because
	** |b - a| < 2**32 and t < 2**31. */
	int64_t diff = (int64_t) b - (int64_t) a ;
	int64_t prod = (diff * (int64_t) t_q31 + (1LL << 30)) >> 31 ;
	return (int32_t) ((int64_t) a + prod) ;
} /* pcm_lerp */

static SRC_ERROR
pcm_linear_process (SRC_STATE *state, SRC_PCM_DATA *data)
{	PCM_LINEAR_DATA	*priv ;
	double		src_ratio, input_index, rem ;
	int			ch, channels ;

	if (data->input_frames <= 0)
		return SRC_ERR_NO_ERROR ;

	if (state->private_data == NULL)
		return SRC_ERR_NO_PRIVATE ;

	priv = (PCM_LINEAR_DATA *) state->private_data ;
	channels = state->channels ;

	if (!priv->dirty)
	{	/* If we have just been reset, set the last_value data. */
		for (ch = 0 ; ch < channels ; ch++)
			priv->last_value [ch] = priv->read (data->data_in, 0, ch, channels) ;
		priv->dirty = true ;
		} ;

	priv->in_count = data->input_frames ;
	priv->out_count = data->output_frames ;
	priv->in_used = priv->out_gen = 0 ;

	src_ratio = state->last_ratio ;

	if (is_bad_src_ratio (src_ratio))
		return SRC_ERR_BAD_INTERNAL_STATE ;

	input_index = state->last_position ;

	/* Calculate samples before first sample in input array. */
	while (input_index < 1.0 && priv->out_gen < priv->out_count)
	{
		if (priv->in_used + (1.0 + input_index) >= priv->in_count)
			break ;

		if (priv->out_count > 0 && fabs (state->last_ratio - data->src_ratio) > SRC_MIN_RATIO_DIFF)
			src_ratio = state->last_ratio + priv->out_gen * (data->src_ratio - state->last_ratio) / priv->out_count ;

		{
			uint32_t t_q31 = (uint32_t) (input_index * 2147483648.0) ;
			for (ch = 0 ; ch < channels ; ch++)
			{
				int32_t cur = priv->read (data->data_in, priv->in_used, ch, channels) ;
				priv->write (data->data_out, priv->out_gen, ch, channels,
						pcm_lerp (priv->last_value [ch], cur, t_q31)) ;
			}
		}
		priv->out_gen ++ ;

		/* Figure out the next index. */
		input_index += 1.0 / src_ratio ;
		} ;

	rem = fmod_one (input_index) ;
	priv->in_used += psf_lrint (input_index - rem) ;
	input_index = rem ;

	/* Main processing loop. */
	while (priv->out_gen < priv->out_count && priv->in_used + input_index < priv->in_count)
	{
		if (priv->out_count > 0 && fabs (state->last_ratio - data->src_ratio) > SRC_MIN_RATIO_DIFF)
			src_ratio = state->last_ratio + priv->out_gen * (data->src_ratio - state->last_ratio) / priv->out_count ;

		{
			uint32_t t_q31 = (uint32_t) (input_index * 2147483648.0) ;
			for (ch = 0 ; ch < channels ; ch++)
			{
				int32_t prev = priv->read (data->data_in, priv->in_used - 1, ch, channels) ;
				int32_t cur = priv->read (data->data_in, priv->in_used, ch, channels) ;
				priv->write (data->data_out, priv->out_gen, ch, channels,
						pcm_lerp (prev, cur, t_q31)) ;
			}
		}
		priv->out_gen ++ ;

		/* Figure out the next index. */
		input_index += 1.0 / src_ratio ;
		rem = fmod_one (input_index) ;

		priv->in_used += psf_lrint (input_index - rem) ;
		input_index = rem ;
		} ;

	if (priv->in_used > priv->in_count)
	{	input_index += (priv->in_used - priv->in_count) ;
		priv->in_used = priv->in_count ;
		} ;

	state->last_position = input_index ;

	if (priv->in_used > 0)
		for (ch = 0 ; ch < channels ; ch++)
			priv->last_value [ch] = priv->read (data->data_in, priv->in_used - 1, ch, channels) ;

	/* Save current ratio rather then target ratio. */
	state->last_ratio = src_ratio ;

	data->input_frames_used = priv->in_used ;
	data->output_frames_gen = priv->out_gen ;

	return SRC_ERR_NO_ERROR ;
} /* pcm_linear_process */

LIBSAMPLERATE_DLL_PRIVATE SRC_STATE *
pcm_linear_state_new (int channels, const PCM_LAYOUT *layout, SRC_ERROR *error)
{
	assert (channels > 0) ;
	assert (layout != NULL) ;
	assert (error != NULL) ;

	SRC_STATE *state = (SRC_STATE *) calloc (1, sizeof (SRC_STATE)) ;
	if (!state)
	{
		*error = SRC_ERR_MALLOC_FAILED ;
		return NULL ;
	}

	state->channels = channels ;
	state->mode = SRC_MODE_PROCESS ;
	state->pcm_process = pcm_linear_process ;

	PCM_LINEAR_DATA *priv = (PCM_LINEAR_DATA *) calloc (1, sizeof (PCM_LINEAR_DATA)) ;
	if (priv)
	{
		priv->linear_magic_marker = PCM_LINEAR_MAGIC_MARKER ;
		priv->layout = *layout ;
		priv->read = pcm_select_reader (layout->in_bits, layout->interleaved) ;
		priv->write = pcm_select_writer (layout->out_bits, layout->interleaved) ;
		priv->last_value = (int32_t *) calloc (channels, sizeof (int32_t)) ;
		if (!priv->last_value || !priv->read || !priv->write)
		{
			if (priv->last_value)
				free (priv->last_value) ;
			free (priv) ;
			priv = NULL ;
		}
	}

	state->private_data = priv ;
	if (!state->private_data)
	{
		free (state) ;
		*error = SRC_ERR_MALLOC_FAILED ;
		return NULL ;
	}

	state->vt = &pcm_linear_state_vt ;

	pcm_linear_reset (state) ;

	*error = SRC_ERR_NO_ERROR ;

	return state ;
} /* pcm_linear_state_new */

static void
pcm_linear_reset (SRC_STATE *state)
{	PCM_LINEAR_DATA *priv = NULL ;

	priv = (PCM_LINEAR_DATA *) state->private_data ;
	if (priv == NULL)
		return ;

	priv->dirty = false ;
	memset (priv->last_value, 0, sizeof (priv->last_value [0]) * state->channels) ;

	return ;
} /* pcm_linear_reset */

static SRC_STATE *
pcm_linear_copy (SRC_STATE *state)
{
	assert (state != NULL) ;

	if (state->private_data == NULL)
		return NULL ;

	SRC_STATE *to = (SRC_STATE *) calloc (1, sizeof (SRC_STATE)) ;
	if (!to)
		return NULL ;
	memcpy (to, state, sizeof (SRC_STATE)) ;

	PCM_LINEAR_DATA *from_priv = (PCM_LINEAR_DATA *) state->private_data ;
	PCM_LINEAR_DATA *to_priv = (PCM_LINEAR_DATA *) calloc (1, sizeof (PCM_LINEAR_DATA)) ;
	if (!to_priv)
	{
		free (to) ;
		return NULL ;
	}

	memcpy (to_priv, from_priv, sizeof (PCM_LINEAR_DATA)) ;
	to_priv->last_value = (int32_t *) malloc (sizeof (int32_t) * state->channels) ;
	if (!to_priv->last_value)
	{
		free (to) ;
		free (to_priv) ;
		return NULL ;
	}
	memcpy (to_priv->last_value, from_priv->last_value, sizeof (int32_t) * state->channels) ;

	to->private_data = to_priv ;

	return to ;
} /* pcm_linear_copy */

static void
pcm_linear_close (SRC_STATE *state)
{
	if (state)
	{
		PCM_LINEAR_DATA *linear = (PCM_LINEAR_DATA *) state->private_data ;
		if (linear)
		{
			if (linear->last_value)
			{
				free (linear->last_value) ;
				linear->last_value = NULL ;
			}
			free (linear) ;
			linear = NULL ;
		}
		free (state) ;
		state = NULL ;
	}
} /* pcm_linear_close */
