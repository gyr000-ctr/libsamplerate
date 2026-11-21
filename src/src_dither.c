/*
** Copyright (c) 2002-2021, Erik de Castro Lopo <erikd@mega-nerd.com>
** All rights reserved.
**
** This code is released under 2-clause BSD license. Please see the
** file at : https://github.com/libsndfile/libsamplerate/blob/master/COPYING
*/

/*
** Optional dither for the float -> short conversion path.
**
** The dither amplitude is one LSB at the requested target bit depth. TPDF
** dither (the difference of two independent rectangular distributions)
** removes the correlation between the quantisation error and the signal;
** noise shaping additionally feeds the previous quantisation error back with
** a negative sign, so the error spectrum is high passed by (1 - z^-1).
**
** Each channel owns its own noise. The noise for a sample is a function of
** the channel, the target bit depth and the sample's index within the stream
** only, so:
**   - different channels never share noise;
**   - different target bit depths never share noise;
**   - splitting a stream into calls gives bit-for-bit the same result as one
**     call (there is no dependence on where a call boundary falls);
**   - repeated runs are reproducible.
** src_dither_reset() rewinds a stream to its start.
**
** Each channel runs eight independent PRNG streams, interleaved one per
** sample. Sample k of a channel uses stream (k modulo 8), so one vector unit
** of eight consecutive samples draws exactly one value from every stream.
** The caller-facing position is carried in the state, which lets the loops
** stay aligned to that grid.
**
** Internally everything is integer arithmetic in units of 1/256 of one target
** LSB. The target LSB is therefore 256 internal units at every bit depth and
** the noise shaping runs at a fixed 8 fractional bits. A dither state is not
** thread safe; one state must be used by one thread at a time.
*/

#ifdef HAVE_CONFIG_H
#include	"config.h"
#endif

#include	<stdint.h>
#include	<stdlib.h>
#include	<string.h>

#include	"samplerate.h"
#include	"common.h"

#define	DITHER_MAGIC_MARKER	MAKE_MAGIC ('s', 'r', 'c', 'd', 'i', 't')

#define	DITHER_MIN_BITS		1
#define	DITHER_MAX_BITS		16

/* One target LSB is 1 << DITHER_FRAC_BITS internal units. */
#define	DITHER_FRAC_BITS	8
#define	DITHER_FRAC_HALF	(1 << (DITHER_FRAC_BITS - 1))
#define	DITHER_FRAC_MASK	((1 << DITHER_FRAC_BITS) - 1)

/* Independent PRNG streams per channel, one per residue class of the sample
** index. Eight matches the width of the SSE2 vectors.
*/
#define	DITHER_STREAMS		8

/* Samples per inner block. Small enough to stay in L1. */
#define	DITHER_BLOCK		4096

/* The per stream start state is a pure function of the configuration, so a
** reset and a fresh state produce the same stream.
*/
#define	DITHER_SEED_BASE	0x9E3779B9u
#define	DITHER_SEED_CHANNEL	0x85EBCA6Bu
#define	DITHER_SEED_TYPE	0xC2B2AE35u
#define	DITHER_SEED_STREAM	0x27D4EB2Fu

typedef struct SRC_DITHER_STATE_tag
{	uint32_t	magic_marker ;
	int			type ;
	int			bit_depth ;
	int			channels ;

	/* One target LSB expressed in the 16 bit output domain. */
	int			step ;

	/* Per channel noise shaping feedback. */
	int32_t		*history ;

	/* Per channel, per stream PRNG state: channels * DITHER_STREAMS. */
	uint32_t	*rng ;

	/* Samples already processed in this stream. It is the same for every
	** channel because the API is frame based.
	*/
	int64_t		position ;

	/* Noise for the current run of samples of one channel, in the range
	** [-255, 255], i.e. +/- one target LSB peak to peak.
	*/
	int32_t		*noise ;

	/* Per channel staging buffers so multi channel data can use the
	** contiguous vector kernel.
	*/
	float		*gather_in ;
	short		*gather_out ;
} SRC_DITHER_STATE ;

static uint32_t
dither_start_state (int type, int bit_depth, int channel, int stream)
{	uint32_t state ;

	state = DITHER_SEED_BASE
		^ ((uint32_t) channel * DITHER_SEED_CHANNEL)
		^ ((uint32_t) stream * DITHER_SEED_STREAM)
		^ ((uint32_t) bit_depth << 16) ;
	state ^= (uint32_t) type * DITHER_SEED_TYPE ;

	return state == 0 ? DITHER_SEED_BASE : state ;
} /* dither_start_state */

/*----------------------------------------------------------------------------
** Balanced remainder modulo 256, in [-128, 127].
*/

static inline int32_t
dither_balance (int32_t value)
{
	return ((value + DITHER_FRAC_HALF) & DITHER_FRAC_MASK) - DITHER_FRAC_HALF ;
} /* dither_balance */

/*----------------------------------------------------------------------------
** Noise generation. Each stream advances one xorshift32 step per sample that
** falls in its residue class.
*/

static inline uint32_t
dither_xorshift32 (uint32_t x)
{
	x ^= x << 13 ;
	x ^= x >> 17 ;
	x ^= x << 5 ;

	return x ;
} /* dither_xorshift32 */

/* Fill noise [0, count) for one channel, starting at stream position base. */
static void
dither_fill_noise (SRC_DITHER_STATE *state, int channel, int64_t base, int count)
{	uint32_t *rng = state->rng + (size_t) channel * DITHER_STREAMS ;
	int k ;

	for (k = 0 ; k < count ; k++)
	{	uint32_t *stream = rng + (size_t) ((base + k) % DITHER_STREAMS) ;

		*stream = dither_xorshift32 (*stream) ;
		state->noise [k] = (int32_t) ((*stream >> 16) & 0xff) - (int32_t) (*stream & 0xff) ;
		} ;
} /* dither_fill_noise */

/*----------------------------------------------------------------------------
** Portable kernels, one channel at a time, starting at the given position.
*/

/* Clamp an integer in the 16 bit output domain, keeping it an exact multiple
** of the target LSB (step) so a reduced bit depth stays on its grid.
*/
static inline int
dither_clamp (int32_t q, int step)
{	int max_q = (32767 / step) * step ;
	int min_q = -32768 ;

	if (q > max_q)
		return max_q ;
	if (q < min_q)
		return min_q ;

	return (int) q ;
} /* dither_clamp */

static void
dither_none_channel (const float *in, short *out, int frames, int stride, int step)
{	const float inv_step = 1.0f / (float) step ;
	int k ;

	for (k = 0 ; k < frames ; k++)
	{	float scaled = in [k * stride] * 32768.0f ;

		if (scaled != scaled)	/* NaN */
			scaled = 0.0f ;
		out [k * stride] = (short) dither_clamp (psf_lrintf (scaled * inv_step) * step, step) ;
		} ;
} /* dither_none_channel */

/* The fixed point value of one input sample: a 16 bit part A and the low byte
** t, so value = A * 256 + t. This is exactly how the SSE2 kernel derives it
** (and the expression must stay identical, or the two paths would round
** differently at the half way points).
*/
static inline int32_t
dither_fixed (float value)
{	int32_t a = psf_lrintf (value * 32768.0f) ;
	int32_t scaled = a * (1 << DITHER_FRAC_BITS) ;
	int32_t t = psf_lrintf (value * (32768.0f * (float) (1 << DITHER_FRAC_BITS))) - scaled ;

	return scaled + t ;
} /* dither_fixed */

static void
dither_tpdf_channel (const float *in, short *out, int frames, int stride,
		const int32_t *noise, int step)
{	int k ;

	for (k = 0 ; k < frames ; k++)
	{	float scaled = in [k * stride] ;
		int32_t q ;

		if (scaled != scaled)
			scaled = 0.0f ;
		/* Work in 1/256 target LSB units: the noise is already in them. */
		q = (dither_fixed (scaled) + noise [k] + DITHER_FRAC_HALF) >> DITHER_FRAC_BITS ;
		out [k * stride] = (short) dither_clamp (q * step, step) ;
		} ;
} /* dither_tpdf_channel */

/* Exact one channel noise shaper in 1/256 target LSB units. With u_k the
** quantiser input and q_k = round (u_k):
**
**     u_k = B_k - f_{k-1}
**     f_k = q_k * 256 - u_k        (the quantisation residue, so the output
**                                   error q_k*256 - B_k = f_k - f_{k-1})
**
** Storing f_k (rather than the remainder u_k - q_k*256, which is -f_k) puts
** the error transfer function at (1 - z^-1), i.e. a high pass.
*/

static void
dither_shaped_channel (const float *in, short *out, int frames, int stride,
		const int32_t *noise, int step, int32_t *history)
{	int32_t f = *history ;
	int k ;

	for (k = 0 ; k < frames ; k++)
	{	float scaled = in [k * stride] ;
		int32_t b, q ;

		if (scaled != scaled)
			scaled = 0.0f ;
		b = dither_fixed (scaled) + noise [k] ;

		q = (b - f + DITHER_FRAC_HALF) >> DITHER_FRAC_BITS ;
		/* dither_balance (f - (b & mask)) == -(remainder of u_k), i.e. f_k. */
		f = dither_balance (f - (b & DITHER_FRAC_MASK)) ;

		out [k * stride] = (short) dither_clamp (q * step, step) ;
		} ;

	*history = f ;
} /* dither_shaped_channel */

/*----------------------------------------------------------------------------
** SSE2 kernels.
**
** Both modes process eight consecutive samples of one channel per iteration
** and convert the input at full 16 bit resolution in one pass.
**
** Key identity used by the noise shaper: write B_k = 256 * A_k + t_k, where
** A_k = round (x_k * 32768) and t_k is the residual plus noise, |t_k| < 32768
** so it fits an int16. Then
**
**     (B_k - f_{k-1} + 128) >> 8  ==  A_k + ((t_k - f_{k-1} + 128) >> 8)
**
** exactly, because 256 * A_k is a multiple of 256. The low byte of B is the
** low byte of t, so the feedback recurrence only needs t, and its prefix sum
** can be computed with int16 lanes for the whole eight sample
** group. The feedback therefore costs one carry per group instead of a serial
** chain per sample.
*/

#ifdef HAVE_SSE2_INTRINSICS

static inline __m128i
dither_xorshift4 (__m128i x)
{
	x = _mm_xor_si128 (x, _mm_slli_epi32 (x, 13)) ;
	x = _mm_xor_si128 (x, _mm_srli_epi32 (x, 17)) ;

	return _mm_xor_si128 (x, _mm_slli_epi32 (x, 5)) ;
} /* dither_xorshift4 */

/* Inclusive prefix sum of eight int16 lanes, modulo 2^16: lane k receives
** sum_{j<=k} c_j.
*/
static inline __m128i
dither_scan_sum (__m128i c)
{
	c = _mm_add_epi16 (c, _mm_slli_si128 (c, 2)) ;
	c = _mm_add_epi16 (c, _mm_slli_si128 (c, 4)) ;

	return _mm_add_epi16 (c, _mm_slli_si128 (c, 8)) ;
} /* dither_scan_sum */

/* One group of eight samples of one channel at full depth.
**
** rng points at the channel's eight stream states (two vectors); lane k holds
** stream k, so each vector advances by one draw. history is the noise shaping
** feedback at the group start and is ignored for TPDF.
*/
static void
dither_group_sse2 (const float *in, short *out, __m128i *rng, int shaped,
		int32_t *history)
{	const __m128 to_16 = _mm_set1_ps (32768.0f) ;
	const __m128 to_fixed = _mm_set1_ps (32768.0f * (float) (1 << DITHER_FRAC_BITS)) ;
	const __m128i byte_mask = _mm_set1_epi32 (0xff) ;
	const __m128i word_mask = _mm_set1_epi16 (0xff) ;
	const __m128i word_half = _mm_set1_epi16 (DITHER_FRAC_HALF) ;
	__m128 in0 = _mm_loadu_ps (in) ;
	__m128 in1 = _mm_loadu_ps (in + 4) ;
	__m128i a0 = _mm_cvtps_epi32 (_mm_mul_ps (in0, to_16)) ;
	__m128i a1 = _mm_cvtps_epi32 (_mm_mul_ps (in1, to_16)) ;
	__m128i t0 = _mm_sub_epi32 (_mm_cvtps_epi32 (_mm_mul_ps (in0, to_fixed)), _mm_slli_epi32 (a0, DITHER_FRAC_BITS)) ;
	__m128i t1 = _mm_sub_epi32 (_mm_cvtps_epi32 (_mm_mul_ps (in1, to_fixed)), _mm_slli_epi32 (a1, DITHER_FRAC_BITS)) ;
	__m128i noise0, noise1 ;
	__m128i t ;
	__m128i shift ;

	rng [0] = dither_xorshift4 (rng [0]) ;
	rng [1] = dither_xorshift4 (rng [1]) ;

	noise0 = _mm_sub_epi32 (_mm_and_si128 (_mm_srli_epi32 (rng [0], 16), byte_mask), _mm_and_si128 (rng [0], byte_mask)) ;
	noise1 = _mm_sub_epi32 (_mm_and_si128 (_mm_srli_epi32 (rng [1], 16), byte_mask), _mm_and_si128 (rng [1], byte_mask)) ;
	t = _mm_packs_epi32 (_mm_add_epi32 (t0, noise0), _mm_add_epi32 (t1, noise1)) ;

	if (shaped)
	{	int32_t f = *history ;
		__m128i c = _mm_and_si128 (t, word_mask) ;
		__m128i prefix = dither_scan_sum (c) ;
		__m128i f_prev = _mm_set1_epi16 ((int16_t) f) ;
		/* f_k = balance (f_{k-1} - c_k) unrolls to
		**     f_k = balance (f_{-1} - (c_0 + ... + c_k)),
		** so the group is one prefix sum. The value subtracted before
		** quantisation is the previous f, i.e. lane k uses f_{k-1}.
		*/
		__m128i all = _mm_sub_epi16 (f_prev, prefix) ;
		__m128i balanced = _mm_sub_epi16 (
			_mm_and_si128 (_mm_add_epi16 (all, word_half), word_mask), word_half) ;
		__m128i previous = _mm_or_si128 (_mm_slli_si128 (balanced, 2),
				_mm_cvtsi32_si128 ((int) (uint16_t) (int16_t) f)) ;

		shift = _mm_srai_epi16 (
			_mm_add_epi16 (_mm_sub_epi16 (t, previous), word_half), DITHER_FRAC_BITS) ;

		*history = (int16_t) _mm_extract_epi16 (balanced, 7) ;
		}
	else
	{	/* TPDF: the noise is +/- one target LSB, which is +/- 1 at the full
		** 16 bit depth, so this is the final rounded value.
		*/
		shift = _mm_srai_epi16 (_mm_add_epi16 (t, word_half), DITHER_FRAC_BITS) ;
		} ;
	_mm_storeu_si128 ((__m128i *) out,
			_mm_adds_epi16 (_mm_packs_epi32 (a0, a1), shift)) ;
} /* dither_group_sse2 */

#endif /* HAVE_SSE2_INTRINSICS */

/*----------------------------------------------------------------------------
*/

SRC_DITHER_STATE *
src_dither_new (int dither_type, int bit_depth, int channels, int *error)
{	SRC_DITHER_STATE *state ;
	int ch, stream ;

	if (error)
		*error = SRC_ERR_NO_ERROR ;

	if (dither_type < SRC_DITHER_NONE || dither_type > SRC_DITHER_NOISE_SHAPED)
	{	if (error)
			*error = SRC_ERR_BAD_DITHER_TYPE ;
		return NULL ;
		} ;

	if (bit_depth < DITHER_MIN_BITS || bit_depth > DITHER_MAX_BITS)
	{	if (error)
			*error = SRC_ERR_BAD_BIT_DEPTH ;
		return NULL ;
		} ;

	if (channels < 1)
	{	if (error)
			*error = SRC_ERR_BAD_CHANNEL_COUNT ;
		return NULL ;
		} ;

	if ((state = calloc (1, sizeof (*state))) == NULL)
	{	if (error)
			*error = SRC_ERR_MALLOC_FAILED ;
		return NULL ;
		} ;

	state->history = calloc ((size_t) channels, sizeof (state->history [0])) ;
	state->rng = calloc ((size_t) channels * DITHER_STREAMS, sizeof (state->rng [0])) ;
	state->noise = calloc (DITHER_BLOCK, sizeof (state->noise [0])) ;
	state->gather_in = calloc (DITHER_BLOCK, sizeof (state->gather_in [0])) ;
	state->gather_out = calloc (DITHER_BLOCK, sizeof (state->gather_out [0])) ;

	if (state->history == NULL || state->rng == NULL || state->noise == NULL
			|| state->gather_in == NULL || state->gather_out == NULL)
	{	free (state->history) ;
		free (state->rng) ;
		free (state->noise) ;
		free (state->gather_in) ;
		free (state->gather_out) ;
		free (state) ;
		if (error)
			*error = SRC_ERR_MALLOC_FAILED ;
		return NULL ;
		} ;

	state->magic_marker = DITHER_MAGIC_MARKER ;
	state->type = dither_type ;
	state->bit_depth = bit_depth ;
	state->channels = channels ;
	state->step = 1 << (16 - bit_depth) ;
	state->position = 0 ;

	for (ch = 0 ; ch < channels ; ch++)
		for (stream = 0 ; stream < DITHER_STREAMS ; stream++)
			state->rng [ch * DITHER_STREAMS + stream] =
				dither_start_state (dither_type, bit_depth, ch, stream) ;

	return state ;
} /* src_dither_new */

SRC_DITHER_STATE *
src_dither_delete (SRC_DITHER_STATE *state)
{
	if (state)
	{	state->magic_marker = 0 ;
		free (state->history) ;
		free (state->rng) ;
		free (state->noise) ;
		free (state->gather_in) ;
		free (state->gather_out) ;
		free (state) ;
		} ;

	return NULL ;
} /* src_dither_delete */

int
src_dither_reset (SRC_DITHER_STATE *state)
{	int ch, stream ;

	if (state == NULL)
		return SRC_ERR_BAD_STATE ;

	if (state->magic_marker != DITHER_MAGIC_MARKER)
		return SRC_ERR_BAD_PRIV_PTR ;

	memset (state->history, 0, (size_t) state->channels * sizeof (state->history [0])) ;
	state->position = 0 ;

	for (ch = 0 ; ch < state->channels ; ch++)
		for (stream = 0 ; stream < DITHER_STREAMS ; stream++)
			state->rng [ch * DITHER_STREAMS + stream] =
				dither_start_state (state->type, state->bit_depth, ch, stream) ;

	return SRC_ERR_NO_ERROR ;
} /* src_dither_reset */

/*----------------------------------------------------------------------------
*/

int
src_float_to_short_array_dithered (SRC_DITHER_STATE *state,
		const float *in, short *out, int frames)
{	uintptr_t in_addr, out_addr, in_end, out_end ;
	size_t samples ;
	int ch ;

	if (state == NULL)
		return SRC_ERR_BAD_STATE ;

	if (state->magic_marker != DITHER_MAGIC_MARKER)
		return SRC_ERR_BAD_PRIV_PTR ;

	if (frames <= 0)
		return SRC_ERR_NO_ERROR ;

	if (in == NULL || out == NULL)
		return SRC_ERR_BAD_DATA_PTR ;

	samples = (size_t) frames * (size_t) state->channels ;

	/* Reject any overlap of the input and output byte ranges: writing the
	** output while still reading from an overlapping input would corrupt
	** later samples.
	*/
	in_addr = (uintptr_t) in ;
	out_addr = (uintptr_t) out ;
	in_end = in_addr + samples * sizeof (*in) ;
	out_end = out_addr + samples * sizeof (*out) ;

	if (in_addr < out_end && out_addr < in_end)
		return SRC_ERR_DATA_OVERLAP ;

	if (state->type == SRC_DITHER_NONE)
	{	/* Bit for bit identical to the plain conversion at full depth. */
		if (state->bit_depth == 16)
		{	src_float_to_short_array (in, out, (int) samples) ;
			state->position += frames ;
			return SRC_ERR_NO_ERROR ;
			} ;

		for (ch = 0 ; ch < state->channels ; ch++)
		{	int block ;

			for (block = 0 ; block < frames ; block += DITHER_BLOCK)
			{	int count = MIN (DITHER_BLOCK, frames - block) ;

				dither_none_channel (in + (size_t) block * state->channels + ch,
						out + (size_t) block * state->channels + ch,
						count, state->channels, state->step) ;
				} ;
			} ;

		state->position += frames ;

		return SRC_ERR_NO_ERROR ;
		} ;

	for (ch = 0 ; ch < state->channels ; ch++)
	{	const int stride = state->channels ;
		int64_t position = state->position ;
		int64_t target = state->position + frames ;
		int done = 0 ;

		/* The noise for a sample depends only on its stream position, and the
		** vector kernel needs the sample index aligned to the eight stream
		** grid. A short portable prefix brings the position up to alignment,
		** whole groups of eight run in the vector kernel, and the portable
		** kernels finish the tail. All paths are bit-exact, so the caller may
		** split the stream in any way.
		*/
		while (position < target)
		{	int align = (int) ((DITHER_STREAMS - (position & (DITHER_STREAMS - 1)))
					& (DITHER_STREAMS - 1)) ;
			const float *block_in = in + (size_t) done * stride + ch ;
			short *block_out = out + (size_t) done * stride + ch ;
			int count ;

#ifdef HAVE_SSE2_INTRINSICS
			if (align == 0 && state->bit_depth == 16)
			{	__m128i rng [2] ;
				uint32_t *channel_rng = state->rng + (size_t) ch * DITHER_STREAMS ;
				int32_t feedback = state->history [ch] ;
				int64_t limit = MIN (target - position, DITHER_BLOCK) ;
				const float *vector_in = block_in ;
				short *vector_out = block_out ;
				int k = 0, f ;

				/* The vector kernel reads consecutive samples. Interleaved
				** multi channel data is gathered per channel first; mono data
				** (stride 1) needs no copy.
				*/
				if (stride > 1)
				{	for (f = 0 ; f < (int) limit ; f++)
					{	state->gather_in [f] = block_in [(size_t) f * stride] ;
						} ;
					vector_in = state->gather_in ;
					vector_out = state->gather_out ;
					} ;

				rng [0] = _mm_setr_epi32 ((int) channel_rng [0], (int) channel_rng [1],
						(int) channel_rng [2], (int) channel_rng [3]) ;
				rng [1] = _mm_setr_epi32 ((int) channel_rng [4], (int) channel_rng [5],
						(int) channel_rng [6], (int) channel_rng [7]) ;

				for ( ; k + DITHER_STREAMS <= limit ; k += DITHER_STREAMS)
					dither_group_sse2 (vector_in + k, vector_out + k, rng,
							state->type == SRC_DITHER_NOISE_SHAPED, &feedback) ;

				{	uint32_t tmp [4] ;

					_mm_storeu_si128 ((__m128i *) tmp, rng [0]) ;
					channel_rng [0] = tmp [0] ; channel_rng [1] = tmp [1] ;
					channel_rng [2] = tmp [2] ; channel_rng [3] = tmp [3] ;
					_mm_storeu_si128 ((__m128i *) tmp, rng [1]) ;
					channel_rng [4] = tmp [0] ; channel_rng [5] = tmp [1] ;
					channel_rng [6] = tmp [2] ; channel_rng [7] = tmp [3] ;
					} ;

				state->history [ch] = feedback ;

				if (stride > 1)
				{	for (f = 0 ; f < k ; f++)
						block_out [(size_t) f * stride] = state->gather_out [f] ;
					} ;

				if (k > 0)
				{	done += k ;
					position += k ;
					continue ;
					} ;
				} ;
#endif

			count = MIN ((int) (target - position), align > 0 ? align : DITHER_BLOCK) ;

			dither_fill_noise (state, ch, position, count) ;
			if (state->type == SRC_DITHER_TPDF)
				dither_tpdf_channel (block_in, block_out, count, stride,
						state->noise, state->step) ;
			else
				dither_shaped_channel (block_in, block_out, count, stride,
						state->noise, state->step, &state->history [ch]) ;

			done += count ;
			position += count ;
			} ;
		} ;

	state->position += frames ;

	return SRC_ERR_NO_ERROR ;
} /* src_float_to_short_array_dithered */
