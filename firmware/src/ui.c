/* SPDX-License-Identifier: GPL-3.0-only */
/* The kids' UI: four pages, one function button each (its LED is lit while the page shows):
 *   HOME WAVE (wave, scan, fat, detune) - EDIT FILTER (type, cutoff, reso) - ENV ADSR - FX SPACE (amount, size)
 * K1..K4 turn the bars at the bottom. OCT-/OCT+ shift the keys (both together: back to the middle).
 * An 80s CRT on every page: black, a faint graticule, glowing phosphor lines, one phosphor colour per page. No framebuffer: the screen is drawn in two 240 x 120
 * strips (cv_oy shifts screen coordinates into the strip) and only when something it shows has changed. */

#include "fcurve.c"                 /* the FILTER page's response curve */

#define PX 10                       /* the picture's panel */
#define PY 40
#define PW 220
#define PH 122

typedef struct { const char *name; int16_t *v; uint8_t max, step; } kn_t;
static const struct { uint8_t btn; const char *title; uint16_t bg; kn_t k[4]; } PG[4] = {
    {B_HOME, 0, RGB(120, 255, 140), {{"Wave", &trk[0].p[P_E0], 7, 1}, {"Scan", &trk[0].p[P_E1], 127, 4},
                                          {"Fat", &trk[0].p[P_E4], 4, 1}, {"Detune", &trk[0].p[P_DETUNE], 127, 4}}},
    {B_EDIT, "FILTER", RGB(255, 176, 0), {{0, &trk[0].p[P_FTYPE], 4, 1}, {"Cutoff", &trk[0].p[P_E2], 127, 4},
                                          {"Reso", &trk[0].p[P_E3], 127, 4}}},
    {B_ENV, "ENV", RGB(60, 230, 255), {{"Attack", &trk[0].p[P_ATK], 127, 4}, {"Decay", &trk[0].p[P_DEC], 127, 4},
                                        {"Sustain", &trk[0].p[P_SUS], 127, 4}, {"Release", &trk[0].p[P_REL], 127, 4}}},
    {B_FX, "SPACE", RGB(255, 80, 200), {{"Amount", &trk[0].p[P_REV], 127, 4}, {"Size", &song.g[G_RSIZE], 127, 4}}},
};
static const char *const FT_NAME[5] = {"LP", "BP", "HP", "NOTCH", "COMB"};
static uint8_t ui_page, ui_preset;          /* ui_preset: the one loaded last */
static uint32_t ui_banner;                  /* the preset banner shows until this fm1_ms (0: none) */
static int16_t ui_sig[7] = {-1};            /* what the screen shows now; -1: nothing yet (draw_splash resets it) */
static uint32_t ui_last, ui_ring;           /* last redraw (ms), ring animation step */
static int8_t ui_led = -1;

static int32_t isq(int32_t n)
{
    int32_t r = 0;
    while ((r + 1) * (r + 1) <= n)
        r++;
    return r;
}
/* a disc, or with t > 0 a ring t px thick, as horizontal spans */
static void disc(int32_t cx, int32_t cy, int32_t r, int32_t t, uint16_t c)
{
    int32_t dy, xo, xi, ri = r - t;
    for (dy = -r; dy <= r; dy++) {
        xo = isq(r * r + r - dy * dy);
        xi = t > 0 && dy * dy < ri * ri ? isq(ri * ri - dy * dy) : -1;
        if (xi < 0)
            cv_rect(cx - xo, cy + dy, 2 * xo + 1, 1, c);
        else {
            cv_rect(cx - xo, cy + dy, xo - xi, 1, c);
            cv_rect(cx + xi + 1, cy + dy, xo - xi, 1, c);
        }
    }
}

/* ------------------------------------------------------- pictures --- */
static int16_t pt[200][2];                  /* a glowing line's points (WAVE's are filled per layer) */

/* a phosphor line through n points: a wide dim halo, a narrower one, then the bright core (pass by pass, so cores stay on top) */
static void glow_pts(int32_t n, uint16_t c)
{
    static const uint8_t T[3] = {9, 6, 3}, M[3] = {22, 50, 100};
    int32_t i, j;
    for (j = 0; j < 3; j++)
        for (i = 0; i < n - 1; i++)
            cv_line_t(pt[i][0], pt[i][1], pt[i + 1][0], pt[i + 1][1], ux_mix(0, c, M[j]), T[j]);
}
static void glow_box(int32_t x, int32_t y, int32_t w, int32_t h, uint16_t c)
{
    static const uint8_t T[3] = {9, 6, 3}, M[3] = {22, 50, 100};
    int32_t j, t;
    for (j = 0; j < 3; j++) {
        t = T[j];
        cv_rect(x - t / 2, y - t / 2, w + t, t, ux_mix(0, c, M[j]));
        cv_rect(x - t / 2, y + h - t / 2, w + t, t, ux_mix(0, c, M[j]));
        cv_rect(x - t / 2, y - t / 2, t, h + t, ux_mix(0, c, M[j]));
        cv_rect(x + w - t / 2, y - t / 2, t, h + t, ux_mix(0, c, M[j]));
    }
}
static void graticule(int32_t x, int32_t y, int32_t w, int32_t h, uint16_t c)
{
    int32_t i, g = ux_mix(0, c, 20);
    cv_frame(x, y, w, h, g);
    cv_rect(x, y + h / 2, w, 1, g);
    for (i = 1; i < 10; i++)
        cv_rect(x + i * w / 10, y, 1, h, g);
}

/* layer c of the FAT stack (0 = front): one step further back = up, right, smaller, phase-shifted by DETUNE (sides alternate) */
static void wave_layer(int32_t c)
{
    int32_t i, amp = 46 * (100 - 6 * c) / 100, sh = (c + 1) / 2 * trk[0].p[P_DETUNE] / 6 * (c & 1 ? 1 : -1);
    for (i = 0; i < 128; i++) {
        pt[i][0] = (int16_t)(PX + 10 + i * 200 / 127 + 3 * c);
        pt[i][1] = (int16_t)(PY + PH / 2 - 5 * c - ((wave_peek(trk[0].p[P_E0] & 7, trk[0].p[P_E1], i * 2 + sh) * amp) >> 15));
    }
}

static void pic_wave(void)
{
    static const uint8_t M[5] = {100, 62, 46, 34, 26};
    int32_t c, i;
    for (c = trk[0].p[P_E4]; c > 0; c--) {      /* back to front, no halo */
        wave_layer(c);
        for (i = 0; i < 127; i++)
            cv_line_t(pt[i][0], pt[i][1], pt[i + 1][0], pt[i + 1][1], ux_mix(0, PG[0].bg, M[c]), 3);
    }
    wave_layer(0);
    glow_pts(128, PG[0].bg);
}

static void pic_env(void)
{
    static const char L[4] = {'A', 'D', 'S', 'R'};
    int32_t base = PY + 106, H = 90, sy = base - trk[0].p[P_SUS] * H / 127, i;
    int32_t w[4] = {4 + trk[0].p[P_ATK] * 35 / 100, 4 + trk[0].p[P_DEC] * 35 / 100, 36, 4 + trk[0].p[P_REL] * 35 / 100};
    int32_t x = PX + (PW - w[0] - w[1] - w[2] - w[3]) / 2, y[5] = {base, base - H, sy, sy, base};
    for (i = 0; i < 5; i++) {
        pt[i][0] = (int16_t)x;
        pt[i][1] = (int16_t)y[i];
        x += i < 4 ? w[i] : 0;
    }
    glow_pts(5, PG[2].bg);
    for (i = 0; i < 4; i++) {
        char s[2] = {L[i], 0};
        cv_text((pt[i][0] + pt[i + 1][0] - text_w(&AF_S, s)) / 2, base + 8, &AF_S, s, PG[2].bg);
    }
}

/* the true response (fcurve.c) from +18 to -48 dB, 22 Hz .. 12.5 kHz on a log axis */
static void pic_filter(void)
{
    int32_t x, base = PY + PH - 12, y;
    for (x = 0; x < 200; x++) {
        y = (fcurve_db16(trk[0].p[P_FTYPE], trk[0].p[P_E2], trk[0].p[P_E3], x) + 48 * 16) * 100 / (66 * 16);
        pt[x][0] = (int16_t)(PX + 10 + x);
        pt[x][1] = (int16_t)(base - clamp(y, 0, 100));
    }
    glow_pts(200, PG[1].bg);
}

static void pic_space(void)
{
    int32_t sz = song.g[G_RSIZE], w = 60 + sz * 150 / 127, h = 40 + sz * 70 / 127, cx = PX + PW / 2, cy = PY + PH / 2;
    int32_t maxr = h / 2 - 4, i, r;
    uint16_t c = PG[3].bg, d = ux_mix(0, c, 40);
    glow_box(cx - w / 2, cy - h / 2, w, h, c);
    if (ui_ring)
        for (i = 0; i < 3; i++) {
            r = (int32_t)((ui_ring * 3 + (uint32_t)i * (uint32_t)maxr / 3u) % (uint32_t)maxr);
            if (r > 5) {
                disc(cx, cy, r + 2, 7, d);
                disc(cx, cy, r, 3, c);
            }
        }
    disc(cx, cy, 8, 0, d);
    disc(cx, cy, 5, 0, c);
}

static void banner(void)
{
    const char *n = PRESETS[ui_preset].name;
    char s[8] = "1 / 8";
    int32_t w = text_w(&AF_L, n) + 24, x = (240 - w) / 2;
    s[0] = (char)('1' + ui_preset);
    cv_rect(x, 78, w, 74, 0);
    graticule(x, 78, w, 74, PG[0].bg);
    glow_box(x + 4, 82, w - 8, 66, PG[0].bg);
    cv_text(x + 12, 90, &AF_L, n, PG[0].bg);
    cv_text(120 - text_w(&AF_M, s) / 2, 124, &AF_M, s, PG[0].bg);
}

/* ---------------------------------------------------------- page --- */
static void scene(void)
{
    uint16_t bg = PG[ui_page].bg, track = ux_mix(0, bg, 20);
    int32_t n = 0, i, gap = 6, bw, ty = clamp(trk[0].p[P_FTYPE], 0, 4);
    const char *title = ui_page ? PG[ui_page].title : WAVE_NAMES[trk[0].p[P_E0] & 7];
    while (n < 4 && PG[ui_page].k[n].v)
        n++;
    cv_text(12, 4, &AF_L, title, bg);
    graticule(PX, PY, PW, PH, bg);
    (ui_page == 0 ? pic_wave : ui_page == 1 ? pic_filter : ui_page == 2 ? pic_env : pic_space)();
    bw = (226 - gap * (n - 1)) / n;
    for (i = 0; i < n; i++) {
        const kn_t *k = &PG[ui_page].k[i];
        int32_t x = 7 + i * (bw + gap), v = *k->v, fill = k->max < 16 ? (v + 1) * 38 / (k->max + 1) : v * 38 / k->max;
        const char *name = k->name ? k->name : FT_NAME[ty];
        cv_rrect(x, 170, bw, 38, 8, track, 0);
        if (fill > 0)
            cv_rrect(x, 208 - (fill < 8 ? 8 : fill), bw, fill < 8 ? 8 : fill, 8, bg, track);
        cv_text(x + (bw - text_w(&AF_M, name)) / 2, 212, &AF_M, name, bg);
    }
    if (ui_banner)
        banner();
}

static void ui_draw(void)
{
    uint32_t y0;
    int32_t i;
    if (ux.mono) {                             /* the GREY palette mixes to grey: these pages are in colour */
        ux.mono = 0;
        ux.gen++;
    }
    for (y0 = 0; y0 < 240u; y0 += 120u) {
        cv_begin(240, 120, 0);
        cv_oy = -(int32_t)y0;
        scene();
        cv_oy = 0;
        cv_blit(0, y0);
    }
}

static void draw_splash(void)
{
    lcd_fill(0, 0, 240, 240, T_BG);
    draw_text_box(0, 100, 240, &AF_L, "KOSMO", T_THEME, 1);
    ui_sig[0] = -1;
}

static void ui_input(void)
{
    uint32_t pressed = fm1_input_edges(0), i, active = 0;
    uint32_t dn = 1u << panel.btn[B_OCTDN], up = 1u << panel.btn[B_OCTUP];
    int16_t sig[7] = {0};
    int32_t pst;
    if ((pressed & (dn | up)) && (fm1_in.buttons & (dn | up)) == (dn | up))
        song.octave = 0;
    else if ((pressed & dn) && song.octave > -3)
        song.octave--;
    else if ((pressed & up) && song.octave < 3)
        song.octave++;
    for (i = 0; i < 4u; i++)
        if (pressed & (1u << panel.btn[PG[i].btn]))
            ui_page = (uint8_t)i;
    pst = panel_enc(EN_PRESET);
    if (pst) {                                  /* PRESETS: step with wrap-around, show the name for 1.5 s */
        ui_preset = (uint8_t)((ui_preset + pst % (int32_t)NPRESETS + (int32_t)NPRESETS) % (int32_t)NPRESETS);
        preset_load(ui_preset);
        ui_banner = fm1_ms + 1500u;
    }
    if (ui_banner && (int32_t)(fm1_ms - ui_banner) >= 0)
        ui_banner = 0;
    for (i = 0; i < 4u; i++) {                  /* knobs K1..K4 (a knob the page does not use is only drained) */
        int32_t s = panel_enc(EN_K1 + i), v;
        const kn_t *k = &PG[ui_page].k[i];
        if (!s || !k->v)
            continue;
        v = *k->v + s * k->step;
        *k->v = (int16_t)(v < 0 ? 0 : v > k->max ? k->max : v);
    }
    if (!fm1_led_anim_on() && ui_led != (int8_t)ui_page) {     /* the page's button glows, the others do not */
        for (i = 0; i < 4u; i++)
            fm1_led_key(panel.btn[PG[i].btn], i == ui_page);
        ui_led = (int8_t)ui_page;
    }
    for (i = 0; i < NVOICE; i++)
        active |= trk[0].v[i].active;
    if (active && ui_page == 3 && fm1_ms - ui_last >= 40u)
        ui_ring = ui_ring % 48u + 1u;           /* the rings creep outward while notes play (never 0: that is "quiet") */
    else if (!active)
        ui_ring = 0;
    sig[0] = (int16_t)ui_page;
    for (i = 0; i < 4u; i++)
        sig[1 + i] = PG[ui_page].k[i].v ? *PG[ui_page].k[i].v : 0;
    sig[5] = (int16_t)ui_ring;
    sig[6] = (int16_t)(ui_banner ? 1 + ui_preset : 0);
    if (!memcmp(sig, ui_sig, sizeof sig) || fm1_ms - ui_last < 40u)
        return;
    memcpy(ui_sig, sig, sizeof sig);
    ui_last = fm1_ms;
    ui_draw();
}
