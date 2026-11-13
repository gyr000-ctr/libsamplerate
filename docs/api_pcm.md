# Integer / Fixed-Point PCM API

In addition to the classic float API, libsamplerate provides an optional
integer / fixed-point processing path for embedded audio, integer PCM and
fixed-point DSP use cases. It is strictly opt-in : the float API, the
default converters and the library ABI are unchanged, and a state created
with `src_new()` behaves exactly as before.

The integer path :

* accepts signed **16, 24 or 32 bit** PCM, in interleaved or planar
  (non-interleaved) layout, for any channel count supported by the
  converter,
* processes samples internally in fixed point (Q31) with 64 bit
  accumulators — **no floating point intermediate sample data** is used,
* keeps the same conversion-ratio, latency and stream (reset/clone)
  semantics as the float path ; per-call `input_frames_used` and
  `output_frames_gen` match the float path exactly,
* matches the float path output to within a small agreed tolerance
  (about 0.5 LSB at 16 bit ; at 32 bit the difference is dominated by
  the float path's own float32 precision),
* uses fixed-size internal buffers only — peak memory does not depend on
  the number of frames streamed,
* reports invalid configuration (bad bit depth, bad channel count, bad
  ratio, overlapping buffers, mixing the float and integer APIs on one
  state) as explicit errors instead of silently saturating, and
  saturates (never wraps) when the signal itself clips.

## Usage

```c
#include <samplerate.h>

int error ;
SRC_STATE *state = src_pcm_new (SRC_SINC_MEDIUM_QUALITY, 2,
                                SRC_PCM_S16, SRC_PCM_S16,
                                1 /* interleaved */, &error) ;

SRC_PCM_DATA data ;
memset (&data, 0, sizeof (data)) ;
data.data_in       = input ;    /* int16_t stereo interleaved */
data.input_frames  = in_frames ;
data.data_out      = output ;
data.output_frames = out_frames ;
data.end_of_input  = 1 ;
data.src_ratio     = 48000.0 / 44100.0 ;

error = src_pcm_process (state, &data) ;

src_delete (state) ;
```

`src_pcm_new()` takes the same converter types as `src_new()`
(`SRC_SINC_BEST_QUALITY`, `SRC_SINC_MEDIUM_QUALITY`, `SRC_SINC_FASTEST`,
`SRC_ZERO_ORDER_HOLD`, `SRC_LINEAR`). The bit depths must be one of
`SRC_PCM_S16`, `SRC_PCM_S24` or `SRC_PCM_S32` ; input and output bit
depths may differ. 24 bit samples are packed little-endian, 3 bytes per
sample.

When created with `interleaved == 0`, `data_in` must point to an array
of `channels` per-channel buffer pointers (a `const void * const *`),
and `data_out` likewise (a `void * const *`).

`SRC_PCM_DATA` mirrors `SRC_DATA` ; `src_ratio`, `end_of_input`,
`input_frames_used` and `output_frames_gen` behave identically.
`src_set_ratio()`, `src_reset()`, `src_clone()`, `src_error()`,
`src_strerror()` and `src_delete()` all work on PCM states.

Calling `src_process()` on a PCM state, or `src_pcm_process()` on a
float state, returns an error.
