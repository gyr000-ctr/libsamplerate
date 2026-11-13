/*
** Copyright (c) 2002-2021, Erik de Castro Lopo <erikd@mega-nerd.com>
** All rights reserved.
**
** This code is released under 2-clause BSD license. Please see the
** file at : https://github.com/libsndfile/libsamplerate/blob/master/COPYING
*/

/*
** Type-parametric zero order hold converter core. This file is not
** compiled directly ; it is included by src_zoh.c (single precision)
** and src_zoh_double.c (double precision) with the following macros
** defined :
**
**   ZOH_SAMPLE_T    sample type, float or double
**   ZOH_STATE_NEW   name of the exported state constructor
**   ZOH_EMIT_NAMES  if defined, emit zoh_get_name/description
*/

#if !defined (ZOH_SAMPLE_T) || !defined (ZOH_STATE_NEW)
#error "src_zoh_body.c must be included from src_zoh.c or src_zoh_double.c"
#endif

static SRC_ERROR zoh_vari_process (SRC_STATE *state, SRC_DATA_GEN *data) ;
static void zoh_reset (SRC_STATE *state) ;
static SRC_STATE *zoh_copy (SRC_STATE *state) ;
static void zoh_close (SRC_STATE *state) ;

/*========================================================================================
*/

#define	ZOH_MAGIC_MARKER	MAKE_MAGIC ('s', 'r', 'c', 'z', 'o', 'h')

typedef struct
{	int		zoh_magic_marker ;
	bool	dirty ;
	long	in_count, in_used ;
	long	out_count, out_gen ;
	ZOH_SAMPLE_T	*last_value ;
} ZOH_DATA ;

static SRC_STATE_VT zoh_state_vt =
{
	zoh_vari_process,
	zoh_vari_process,
	zoh_reset,
	zoh_copy,
	zoh_close
} ;

/*----------------------------------------------------------------------------------------
*/

static SRC_ERROR
zoh_vari_process (SRC_STATE *state, SRC_DATA_GEN *data)
{	ZOH_DATA 	*priv ;
	double		src_ratio, input_index, rem ;
	int			ch ;

	const ZOH_SAMPLE_T *data_in = (const ZOH_SAMPLE_T *) data->data_in ;
	ZOH_SAMPLE_T *data_out = (ZOH_SAMPLE_T *) data->data_out ;

	if (data->input_frames <= 0)
		return SRC_ERR_NO_ERROR ;

	if (state->private_data == NULL)
		return SRC_ERR_NO_PRIVATE ;

	priv = (ZOH_DATA*) state->private_data ;

	/* If there is not a problem, this will be optimised out. */
	if (data->elem_size != (int) sizeof (priv->last_value [0]))
		return SRC_ERR_SIZE_INCOMPATIBILITY ;

	if (!priv->dirty)
	{	/* If we have just been reset, set the last_value data. */
		for (ch = 0 ; ch < state->channels ; ch++)
			priv->last_value [ch] = data_in [ch] ;
		priv->dirty = true ;
		} ;

	priv->in_count = data->input_frames * state->channels ;
	priv->out_count = data->output_frames * state->channels ;
	priv->in_used = priv->out_gen = 0 ;

	src_ratio = state->last_ratio ;

	if (is_bad_src_ratio (src_ratio))
		return SRC_ERR_BAD_INTERNAL_STATE ;

	input_index = state->last_position ;

	/* Calculate samples before first sample in input array. */
	while (input_index < 1.0 && priv->out_gen < priv->out_count)
	{
		if (priv->in_used + state->channels * input_index >= priv->in_count)
			break ;

		if (priv->out_count > 0 && fabs (state->last_ratio - data->src_ratio) > SRC_MIN_RATIO_DIFF)
			src_ratio = state->last_ratio + priv->out_gen * (data->src_ratio - state->last_ratio) / priv->out_count ;

		for (ch = 0 ; ch < state->channels ; ch++)
		{	data_out [priv->out_gen] = priv->last_value [ch] ;
			priv->out_gen ++ ;
			} ;

		/* Figure out the next index. */
		input_index += 1.0 / src_ratio ;
		} ;

	rem = fmod_one (input_index) ;
	priv->in_used += state->channels * psf_lrint (input_index - rem) ;
	input_index = rem ;

	/* Main processing loop. */
	while (priv->out_gen < priv->out_count && priv->in_used + state->channels * input_index <= priv->in_count)
	{
		if (priv->out_count > 0 && fabs (state->last_ratio - data->src_ratio) > SRC_MIN_RATIO_DIFF)
			src_ratio = state->last_ratio + priv->out_gen * (data->src_ratio - state->last_ratio) / priv->out_count ;

		for (ch = 0 ; ch < state->channels ; ch++)
		{	data_out [priv->out_gen] = data_in [priv->in_used - state->channels + ch] ;
			priv->out_gen ++ ;
			} ;

		/* Figure out the next index. */
		input_index += 1.0 / src_ratio ;
		rem = fmod_one (input_index) ;

		priv->in_used += state->channels * psf_lrint (input_index - rem) ;
		input_index = rem ;
		} ;

	if (priv->in_used > priv->in_count)
	{	input_index += (priv->in_used - priv->in_count) / state->channels ;
		priv->in_used = priv->in_count ;
		} ;

	state->last_position = input_index ;

	if (priv->in_used > 0)
		for (ch = 0 ; ch < state->channels ; ch++)
			priv->last_value [ch] = data_in [priv->in_used - state->channels + ch] ;

	/* Save current ratio rather then target ratio. */
	state->last_ratio = src_ratio ;

	data->input_frames_used = priv->in_used / state->channels ;
	data->output_frames_gen = priv->out_gen / state->channels ;

	return SRC_ERR_NO_ERROR ;
} /* zoh_vari_process */

/*------------------------------------------------------------------------------
*/

#ifdef ZOH_EMIT_NAMES

LIBSAMPLERATE_DLL_PRIVATE const char*
zoh_get_name (int src_enum)
{
	if (src_enum == SRC_ZERO_ORDER_HOLD)
		return "ZOH Interpolator" ;

	return NULL ;
} /* zoh_get_name */

LIBSAMPLERATE_DLL_PRIVATE const char*
zoh_get_description (int src_enum)
{
	if (src_enum == SRC_ZERO_ORDER_HOLD)
		return "Zero order hold interpolator, very fast, poor quality." ;

	return NULL ;
} /* zoh_get_descrition */

#endif /* ZOH_EMIT_NAMES */

static ZOH_DATA *
zoh_data_new (int channels)
{
	assert (channels > 0) ;

	ZOH_DATA *priv = (ZOH_DATA *) calloc (1, sizeof (ZOH_DATA)) ;
	if (priv)
	{
		priv->zoh_magic_marker = ZOH_MAGIC_MARKER ;
		priv->last_value = (ZOH_SAMPLE_T *) calloc (channels, sizeof (ZOH_SAMPLE_T)) ;
		if (!priv->last_value)
		{
			free (priv) ;
			priv = NULL ;
		}
	}

	return priv ;
}

LIBSAMPLERATE_DLL_PRIVATE SRC_STATE *
ZOH_STATE_NEW (int channels, SRC_ERROR *error)
{
	assert (channels > 0) ;
	assert (error != NULL) ;

	SRC_STATE *state = (SRC_STATE *) calloc (1, sizeof (SRC_STATE)) ;
	if (!state)
	{
		*error = SRC_ERR_MALLOC_FAILED ;
		return NULL ;
	}

	state->channels = channels ;
	state->mode = SRC_MODE_PROCESS ;

	state->private_data = zoh_data_new (state->channels) ;
	if (!state->private_data)
	{
		free (state) ;
		*error = SRC_ERR_MALLOC_FAILED ;
		return NULL ;
	}

	state->vt = &zoh_state_vt ;

	zoh_reset (state) ;

	*error = SRC_ERR_NO_ERROR ;

	return state ;
}

/*===================================================================================
*/

static void
zoh_reset (SRC_STATE *state)
{	ZOH_DATA *priv ;

	priv = (ZOH_DATA*) state->private_data ;
	if (priv == NULL)
		return ;

	priv->dirty = false ;
	memset (priv->last_value, 0, sizeof (ZOH_SAMPLE_T) * state->channels) ;

	return ;
} /* zoh_reset */

static SRC_STATE *
zoh_copy (SRC_STATE *state)
{
	assert (state != NULL) ;

	if (state->private_data == NULL)
		return NULL ;

	SRC_STATE *to = (SRC_STATE *) calloc (1, sizeof (SRC_STATE)) ;
	if (!to)
		return NULL ;
	memcpy (to, state, sizeof (SRC_STATE)) ;

	ZOH_DATA* from_priv = (ZOH_DATA*) state->private_data ;
	ZOH_DATA *to_priv = (ZOH_DATA *) calloc (1, sizeof (ZOH_DATA)) ;
	if (!to_priv)
	{
		free (to) ;
		return NULL ;
	}

	memcpy (to_priv, from_priv, sizeof (ZOH_DATA)) ;
	to_priv->last_value = (ZOH_SAMPLE_T *) malloc (sizeof (ZOH_SAMPLE_T) * state->channels) ;
	if (!to_priv->last_value)
	{
		free (to) ;
		free (to_priv) ;
		return NULL ;
	}
	memcpy (to_priv->last_value, from_priv->last_value, sizeof (ZOH_SAMPLE_T) * state->channels) ;

	to->private_data = to_priv ;

	return to ;
} /* zoh_copy */

static void
zoh_close (SRC_STATE *state)
{
	if (state)
	{
		ZOH_DATA *zoh = (ZOH_DATA *) state->private_data ;
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
} /* zoh_close */
