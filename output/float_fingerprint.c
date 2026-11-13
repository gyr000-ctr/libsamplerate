/*
** float_fingerprint.c : hashes the single precision (float) path output
** of every converter over a grid of ratios, channel counts and both
** processing modes (one shot and varispeed streaming). Used to prove
** zero regression of the float path : the hash must be identical before
** and after the double precision changes.
*/
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <samplerate.h>

static uint64_t
fnv1a (const void *data, size_t len, uint64_t h)
{   const unsigned char *p = (const unsigned char *) data ;
    for (size_t i = 0 ; i < len ; i++)
    {   h ^= p [i] ;
        h *= 1099511628211ULL ;
        } ;
    return h ;
}

static float
sig (long i, int ch)
{   double x = i * 0.001 + ch * 0.37 ;
    return (float) (0.6 * __builtin_sin (x * 7.0) + 0.3 * __builtin_sin (x * 23.0 + 1.0) + 0.1 * __builtin_sin (x * 61.0)) ;
}

int main (void)
{   static const int converters [] = { 0, 1, 2, 3, 4 } ;
    static const double ratios [] = { 0.099, 0.5, 0.789, 1.0, 1.9, 3.1 } ;
    static float in [8192 * 3], out [16384 * 3] ;
    uint64_t total = 1469598103934665603ULL ;

    for (long i = 0 ; i < 8192 ; i++)
        for (int ch = 0 ; ch < 3 ; ch++)
            in [i * 3 + ch] = sig (i, ch) ;

    for (unsigned ci = 0 ; ci < sizeof (converters) / sizeof (converters [0]) ; ci++)
    for (unsigned ri = 0 ; ri < sizeof (ratios) / sizeof (ratios [0]) ; ri++)
    for (int ch = 1 ; ch <= 3 ; ch++)
    {   SRC_DATA d ;
        uint64_t h = 1469598103934665603ULL ;
        int err ;

        /* One shot. */
        memset (&d, 0, sizeof d) ;
        memset (out, 0, sizeof out) ;
        d.data_in = in ; d.data_out = out ;
        d.input_frames = 4096 ; d.output_frames = 16384 ;
        d.src_ratio = ratios [ri] ;
        err = src_simple (&d, converters [ci], ch) ;
        if (err) { printf ("ERROR %s\n", src_strerror (err)) ; return 1 ; }
        h = fnv1a (out, d.output_frames_gen * ch * sizeof (float), h) ;
        h = fnv1a (&d.output_frames_gen, sizeof (long), h) ;
        h = fnv1a (&d.input_frames_used, sizeof (long), h) ;

        /* Varispeed streaming. */
        {   SRC_STATE *st = src_new (converters [ci], ch, &err) ;
            long ip = 0, op = 0 ;
            double ratio = ratios [ri] ;
            while (ip < 4096)
            {   long chunk = 1 + (ip * 5 + 11) % 613 ;
                if (ip + chunk > 4096) chunk = 4096 - ip ;
                memset (&d, 0, sizeof d) ;
                d.data_in = in + ip * ch ; d.data_out = out + op * ch ;
                d.input_frames = chunk ; d.output_frames = 16384 - op ;
                d.end_of_input = (ip + chunk == 4096) ;
                d.src_ratio = ratio ;
                err = src_process (st, &d) ;
                if (err) { printf ("ERROR %s\n", src_strerror (err)) ; return 1 ; }
                ip += d.input_frames_used ; op += d.output_frames_gen ;
                ratio *= 1.0001 ;
                }
            src_delete (st) ;
            h = fnv1a (out, op * ch * sizeof (float), h) ;
            h = fnv1a (&op, sizeof (long), h) ;
            }

        printf ("conv=%d ratio=%6.3f ch=%d hash=%016llx\n", converters [ci], ratios [ri], ch, (unsigned long long) h) ;
        total ^= h ; total *= 1099511628211ULL ;
        } ;

    printf ("TOTAL %016llx\n", (unsigned long long) total) ;
    return 0 ;
}
