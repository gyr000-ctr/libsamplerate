/*
** Copyright (c) 2002-2021, Erik de Castro Lopo <erikd@mega-nerd.com>
** All rights reserved.
**
** This code is released under 2-clause BSD license. Please see the
** file at : https://github.com/libsndfile/libsamplerate/blob/master/COPYING
*/

/*
** Single precision instantiation of the zero order hold converter. The
** actual implementation lives in src_zoh_body.c which is shared with
** the double precision instantiation in src_zoh_double.c.
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

#define ZOH_SAMPLE_T	float
#define ZOH_STATE_NEW	zoh_state_new
#define ZOH_EMIT_NAMES	1

#include "src_zoh_body.c"
