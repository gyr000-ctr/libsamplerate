/*
** Copyright (c) 2002-2021, Erik de Castro Lopo <erikd@mega-nerd.com>
** All rights reserved.
**
** This code is released under 2-clause BSD license. Please see the
** file at : https://github.com/libsndfile/libsamplerate/blob/master/COPYING
*/

/*
** Double precision instantiation of the sinc converter. The actual
** implementation lives in src_sinc_body.c which is shared with the
** single precision instantiation in src_sinc.c.
**
** Instantiating the body with SINC_COEFF_T = double also widens the
** coefficient tables (fastest_coeffs.h, mid_qual_coeffs.h and
** high_qual_coeffs.h) to double : the table literals carry full double
** precision decimal values, so the double path filters with true double
** precision coefficients and double precision accumulation.
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

#define SINC_SAMPLE_T		double
#define SINC_COEFF_T		double
#define SINC_STATE_NEW		sinc_state_new_double

#include "src_sinc_body.c"
