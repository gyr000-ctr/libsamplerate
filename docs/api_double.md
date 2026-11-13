# Double Precision API

In addition to the standard single precision (**float**) interfaces,
libsamplerate provides a double precision processing path intended for
scientific computing and other applications that need a full double
precision chain :

- the input and output buffers are **double** (no intermediate
  quantization to **float**),
- the sinc filter coefficient tables are evaluated in double precision
  (the table literals carry full double precision values),
- all accumulation inside the converters is performed in double
  precision.

The double precision path uses exactly the same converters, quality
settings (the `SRC_SINC_BEST_QUALITY`, `SRC_SINC_MEDIUM_QUALITY`,
`SRC_SINC_FASTEST`, `SRC_ZERO_ORDER_HOLD` and `SRC_LINEAR` enums) and
control logic as the single precision path ; only the sample type of the
processing chain differs.

## SRC_DATA_D

The double precision analogue of the [SRC_DATA](api_misc.md#src_data)
struct :

```c
typedef struct
{   const double  *data_in ;
    double        *data_out ;

    long  input_frames, output_frames ;
    long  input_frames_used, output_frames_gen ;

    int   end_of_input ;

    double  src_ratio ;
} SRC_DATA_D ;
```

Its fields have exactly the same meaning as the fields of **SRC_DATA**.

## Functions

```c
SRC_STATE* src_new_double (int converter_type, int channels, int *error) ;

int src_process_double (SRC_STATE *state, SRC_DATA_D *data) ;

int src_simple_double (SRC_DATA_D *data, int converter_type, int channels) ;
```

- **src_new_double** behaves like [src_new](api_full.md) but returns a
  converter state that processes double precision data.
- **src_process_double** is the double precision counterpart of
  **src_process** and may only be used on states created by
  **src_new_double** (mixing precisions is rejected with an error).
- **src_simple_double** is the double precision counterpart of
  [src_simple](api_simple.md) for single buffer conversions.

All other state handling functions (**src_delete**, **src_reset**,
**src_clone**, **src_set_ratio**, **src_error**, **src_strerror**, ...)
work unchanged on double precision states. The callback interface is
single precision only.

## Precision difference

For a given converter and ratio, the double precision output and the
single precision output of the same signal differ by the single
precision quantization of the input, coefficients and output buffers.
With the sinc converters this difference sits at the float quantization
floor (a difference SNR of roughly 148-152 dB for full scale signals) ;
the double precision output matches a direct double precision
convolution of the input with the filter coefficients. The
**tests/double_test.c** program measures and prints these differences.
