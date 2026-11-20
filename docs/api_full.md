---
layout: default
---

# Full API

The full API consists of the following functions :

```c
SRC_STATE* src_new (int converter_type, int channels, int *error) ;
SRC_STATE* src_delete (SRC_STATE *state) ;

int src_process (SRC_STATE *state, SRC_DATA *data) ;
int src_reset (SRC_STATE *state) ;
int src_set_ratio (SRC_STATE *state, double new_ratio) ;
```

## Initialisation

```c
SRC_STATE* src_new (int converter_type, int channels, int *error) ;
```

The **src\_new** function returns an anonymous pointer to a sample rate
converter object, src\_state. If an error occurs the function returns a
NULL pointer and fills in the error value pointed to by the **error**
pointer supplied by the caller. The converter must be one of the
supplied converter types documented [here](api_misc.md#converters).

## Cleanup

```c
SRC_STATE* src_delete (SRC_STATE *state) ;
```

The **src\_delete** function frees up all memory allocated for the given
sample rate converter object and returns a NULL pointer. The caller is
responsible for freeing any memory passed to the sample rate converter
via the pointer to the **SRC\_DATA** struct.

## Process

```c
int src_process (SRC_STATE *state, SRC_DATA *data) ;
```

The **src\_process** function processes the data provided by the caller
in an **SRC\_DATA** struct using the sample rate converter object
specified by the **SRC\_STATE** pointer. When operating on streaming
data, this function can be called over and over again, with each new
call providing new input data and returning new output data.

The **SRC\_DATA** struct passed as the second parameter to the
**src\_process** function has the following fields:

```c
typedef struct
{   const float  *data_in;
    float *data_out;

    long   input_frames, output_frames ;
    long   input_frames_used, output_frames_gen ;

    int    end_of_input ;

    double src_ratio ;
} SRC_DATA ;
```

The fields of this struct which must be filled in by the caller are:

data_in
: A pointer to the input data samples.

input_frames
: The number of frames of data pointed to by data_in.

data_out
: A pointer to the output data samples.

output_frames
: Maximum number of frames pointer to by data_out.

src_ratio
: Equal to output_sample_rate / input_sample_rate.

end_of_input
: Equal to 0 if more input data is available and 1 otherwise.

Note that the data\_in and data\_out arrays may not overlap. If they do,
the library will return an error code.

When the **src\_process** function returns **output\_frames\_gen** will
be set to the number of output frames generated and
**input\_frames\_used** will be set to the number of input frames
consumed to generate the provided number of output frames.

The **src\_process** function returns non-zero if an error occurs. The
non-zero error return value can be decoded into a text string using the
function documented [here](api_misc.md#error-reporting).

## Reset

```c
int src_reset (SRC_STATE *state) ;
```

The **src\_reset** function resets the internal state of the sample rate
converter object to the same state it had immediately after its creation
using **src\_new**. This should be called whenever a sample rate
converter is to be used on two separate, unrelated pieces of audio.

## Clone

```c
SRC_STATE* src_clone (SRC_STATE *state, int *error) ;
```

The **src_clone** function creates a copy of the internal state of the sample
rate converter object. The output of the next call to **src\_process** will be
identical for both the original and cloned state (given the same **SRC_DATA**
input). This could be used to later resume sample rate conversion at a specific
location in a stream with the same state, which may be useful in real-time
applications.

If an error occurs the function returns a NULL pointer and fills in the
error value pointed to by the **error** pointer supplied by the caller.

## Set Ratio

```c
int src_set_ratio (SRC_STATE *state, double new_ratio) ;
```

When using the **src_process** or **src_callback_process** APIs and updating the
**src_ratio** field of the **SRC_DATA** struct, the library will try to
smoothly transition between the conversion ratio of the last call and the
conversion ratio of the current call.

If the user wants to bypass this smooth transition and achieve a step response in
the conversion ratio, the **src_set_ratio** function can be used to set the
starting conversion ratio of the next call to **src_process** or
**src_callback_process**.

This function returns non-zero on error and the error return value can be
decoded into a text string using the function documented [here](api_misc.md#error-reporting).

## Get Latency

```c
int src_get_latency (SRC_STATE *state, double src_ratio) ;
```

The **src\_get\_latency** function returns the converter's latency for the
given conversion ratio, expressed as a whole number of input frames (samples
per channel). The latency is the number of input frames by which the output
stream appears to trail the input stream: the converter must read that far
ahead of the output it is producing. A freshly initialised converter needs one
more input frame than this before it can emit its first output frame, and
thereafter output frame *n* corresponds to input position *n / src\_ratio*.

The value depends only on the converter type and on **src\_ratio**, so
querying it for a new ratio immediately reflects that ratio; a call to
**src\_process** is not required first. The query consumes no samples and does
not modify the converter state, so the audio produced by subsequent processing
is unchanged.

For the **SRC\_SINC\_\*** converters the latency is the one sided lookahead of
the interpolation kernel: constant for ratios greater than or equal to 1.0 and
growing in inverse proportion to the ratio when down-sampling. The
**SRC\_ZERO\_ORDER\_HOLD** and **SRC\_LINEAR** converters report a latency of
one frame at every ratio. To express the latency in output frames, multiply by
**src\_ratio**; to express it in seconds, divide by the input sample rate.

On success the return value is zero or positive. On failure a negative value is
returned, which is the negated error number and can be decoded with
[**src\_strerror**](api_misc.md#error-reporting). A NULL **state** (converter
not initialised) and a **src\_ratio** outside the range [1/256, 256] return
different, distinguishable error values.

The function only reads the converter state and takes no lock, so it is safe
to call concurrently with itself on the same state. As with every function in
this library, the state must not be processed, reset, cloned or deleted by
another thread while a query is in progress.
