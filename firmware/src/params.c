/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments */
/* Parameter descriptors: range and power-on value of the part's and the global parameters. */
#define PD(l, f, mn, mx, df) {l, f, mn, mx, df, 0, 0}

static const param_desc_t TP[P_COUNT] = {
    [P_LEVEL] = PD("LVL", F_DB, 0, 127, 104),
    [P_ATK] = PD("ATK", F_TIME, 0, 127, 10),
    [P_DEC] = PD("DEC", F_TIME, 0, 127, 70),
    [P_SUS] = PD("SUS", F_PCT, 0, 127, 90),
    [P_REL] = PD("REL", F_TIME, 0, 127, 60),
    [P_ED_FLT] = PD("FLT", F_BIPCT, -64, 63, 0),
    [P_ED_PIT] = PD("PIT", F_BIPCT, -64, 63, 0),
    [P_ED_SHP] = PD("SHP", F_BIPCT, -64, 63, 0),
    [P_REV] = PD("REV", F_PCT, 0, 127, 30),
    [P_VOICE] = PD("VCE", F_ENUM, 0, 3, V_POLY),   /* POLY MONO LEGATO UNISON */
    [P_GLIDE] = PD("GLD", F_TIME, 0, 127, 0),
    [P_GLMODE] = PD("GLMOD", F_ENUM, 0, 1, 0),     /* RATE TIME */
    [P_PRIO] = PD("PRIO", F_ENUM, 0, 2, 0),        /* LAST LOW HIGH */
    [P_ALLOC] = PD("ALLOC", F_ENUM, 0, 1, 0),      /* ROTATE REUSE */
    [P_DETUNE] = PD("DTUNE", F_INT, 0, 127, 40),
    [P_PAN] = PD("PAN", F_BIPCT, -64, 63, 0),
    [P_MUTE] = PD("MUTE", F_ENUM, 0, 1, 0),
    [P_FTYPE] = PD("TYPE", F_ENUM, 0, 2, 0),       /* LP BP HP */
};

static const param_desc_t GP[G_COUNT] = {
    [G_TUNE] = PD("TUNE", F_INT, -50, 50, 0),
    [G_RSIZE] = PD("SIZE", F_PCT, 0, 127, 90),
    [G_RDAMP] = PD("DAMP", F_PCT, 0, 127, 60),
};
