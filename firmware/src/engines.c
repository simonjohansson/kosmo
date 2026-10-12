/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments */
/* The engines (WAVE, SEM), chosen with ALGORITHM (P_ENG), and the part's sound at power-on. */
#include "dsp.c"
#include "lfo.c"
#include "eng_wave.c"
#include "eng_sem.c"

static const engine_t *const ENGINES[] = {&ENG_WAVE, &ENG_SEM};
#define NENG ((int16_t)NELEM(ENGINES))
#define ENGINE_OF(t) (ENGINES[(t)->eng])

/* the engines' parameters at their defaults (each keeps its own, so switching back keeps both sounds), WAVE selected */
static void engines_defaults(track_t *t)
{
    uint32_t e, i;
    for (e = 0; e < (uint32_t)NENG; e++)
        for (i = 0; i < 8u; i++)
            t->p[ENGINES[e]->pbase + i] = ENGINES[e]->edit[i].def;
    t->p[P_ENG] = 0;
    t->eng = 0;
}
