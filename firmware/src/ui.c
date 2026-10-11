/* SPDX-License-Identifier: GPL-3.0-only */
/* The kids' UI: four pages, one function button each (its LED is lit while the page shows):
 *   HOME WAVE (wave, scan) - EDIT FILTER (type, cutoff, reso) - ENV ADSR - FX SPACE (amount, size)
 * K1..K4 turn the bars at the bottom. OCT-/OCT+ shift the keys (both together: back to the middle).
 * One bright colour per page, a big picture in the middle. No framebuffer: the screen is drawn in two 240 x 120
 * strips (cv_oy shifts screen coordinates into the strip) and only when something it shows has changed. */

/* stub until DSP branch lands */
static const char *const WAVE_NAMES[8] = {"SINE", "TRI", "SAW", "SQUARE", "PULSE", "SYNC", "SUB", "NOISE"};
static int16_t wave_peek(int table, int scan, int i)
{
    int32_t ph = (i * 2) & 127, t = ph < 64 ? ph * 512 - 16384 : 49152 - ph * 512;   /* a triangle, squarer with scan */
    (void)table;
    return (int16_t)(t * (128 - scan) / 128 + (i < 128 ? 12000 : -12000) * scan / 128);
}
/* end of stub */

#define INK RGB(40, 28, 80)
#define WHITE RGB(255, 255, 255)
#define PX 10                       /* the picture's panel */
#define PY 40
#define PW 220
#define PH 122

typedef struct { const char *name; int16_t *v; uint8_t max, step; } kn_t;
static const struct { uint8_t btn; const char *title; uint16_t bg; kn_t k[4]; } PG[4] = {
    {B_HOME, 0, RGB(255, 205, 50),  {{"Wave", &trk[0].p[P_E0], 7, 1}, {"Scan", &trk[0].p[P_E1], 127, 4}}},
    {B_EDIT, "FILTER", RGB(70, 220, 170), {{0, &trk[0].p[P_FTYPE], 3, 1}, {"Cutoff", &trk[0].p[P_E2], 127, 4},
                                          {"Reso", &trk[0].p[P_E3], 127, 4}}},
    {B_ENV, "ENV", RGB(255, 130, 160), {{"Attack", &trk[0].p[P_ATK], 127, 4}, {"Decay", &trk[0].p[P_DEC], 127, 4},
                                        {"Sustain", &trk[0].p[P_SUS], 127, 4}, {"Release", &trk[0].p[P_REL], 127, 4}}},
    {B_FX, "SPACE", RGB(130, 175, 255), {{"Amount", &trk[0].p[P_REV], 127, 4}, {"Size", &song.g[G_RSIZE], 127, 4}}},
};
static const char *const FT_NAME[4] = {"LP", "BP", "HP", "NOTCH"};
static uint8_t ui_page;
static int16_t ui_sig[8] = {-1};            /* what the screen shows now; -1: nothing yet (draw_splash resets it) */
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
static int16_t wy[128];

static void pic_wave(void)
{
    int32_t i, mid = PY + PH / 2;
    cv_rect(PX + 8, mid, PW - 16, 2, ux_mix(WHITE, INK, 15));
    for (i = 0; i < 127; i++) {
        int32_t x = PX + 10 + i * 200 / 127, x1 = PX + 10 + (i + 1) * 200 / 127;
        cv_line_t(x, wy[i] - 2, x1, wy[i + 1] - 2, RGB(235, 60, 130), 5);
        cv_line_t(x + 1, wy[i] - 2, x1 + 1, wy[i + 1] - 2, RGB(235, 60, 130), 5);
    }
}

static void pic_env(void)
{
    static const uint16_t C[4] = {RGB(255, 90, 90), RGB(255, 170, 40), RGB(70, 200, 100), RGB(80, 140, 255)};
    static const char L[4] = {'A', 'D', 'S', 'R'};
    int32_t base = PY + 106, H = 90, top = base - H, sy = base - trk[0].p[P_SUS] * H / 127;
    int32_t w[4] = {4 + trk[0].p[P_ATK] * 35 / 100, 4 + trk[0].p[P_DEC] * 35 / 100, 36, 4 + trk[0].p[P_REL] * 35 / 100};
    int32_t x = PX + (PW - w[0] - w[1] - w[2] - w[3]) / 2, py = base, i, j;
    for (i = 0; i < 4; i++) {
        for (j = 0; j < w[i]; j++, x++) {
            int32_t y = i == 0 ? base - j * H / w[0] : i == 1 ? top + j * (sy - top) / w[1] :
                        i == 2 ? sy : sy + j * (base - sy) / w[3];
            int32_t a = py < y ? py : y, b = py < y ? y : py;
            cv_rect(x, y, 1, base - y, C[i]);
            cv_rect(x, a - 1, 1, b - a + 3, INK);
            py = y;
        }
        {
            char s[2] = {L[i], 0};
            cv_text(x - w[i] / 2 - text_w(&AF_S, s) / 2, base + 3, &AF_S, s, INK);
        }
    }
}

/* the filter's gain at curve x (0..199), 0..60: a slope each side of the cutoff, a bump for the resonance */
static int32_t fgain(int32_t type, int32_t x, int32_t fc, int32_t reso)
{
    int32_t d = x - fc, ad = d < 0 ? -d : d, g;
    if (type == 3)
        return 44 - (ad < 28 ? (28 - ad) * 44 / 28 : 0);
    g = type == 0 ? (d <= 0 ? 44 : 44 - d * 44 / 70) : type == 2 ? (d >= 0 ? 44 : 44 + d * 44 / 70) :
        44 - ad * 44 / 55;
    if (g < 0)
        g = 0;
    return g + (ad < 14 ? reso * 16 / 127 * (14 - ad) / 14 : 0);
}

static void face(int32_t cx, int32_t cy, int32_t type)
{
    if (type == 0) {                                          /* LP: bear */
        disc(cx - 19, cy - 16, 9, 0, RGB(140, 85, 45));
        disc(cx + 19, cy - 16, 9, 0, RGB(140, 85, 45));
        disc(cx, cy, 22, 0, RGB(170, 105, 60));
        disc(cx, cy + 8, 10, 0, RGB(240, 205, 160));
        disc(cx, cy + 4, 3, 0, INK);
        disc(cx - 9, cy - 6, 3, 0, INK);
        disc(cx + 9, cy - 6, 3, 0, INK);
    } else if (type == 1) {                                   /* BP: bee */
        disc(cx - 14, cy - 18, 11, 0, RGB(200, 235, 255));
        disc(cx + 14, cy - 18, 11, 0, RGB(200, 235, 255));
        disc(cx, cy + 2, 22, 0, RGB(255, 215, 30));
        cv_rect(cx - 21, cy + 4, 42, 5, INK);
        cv_rect(cx - 17, cy + 14, 34, 5, INK);
        disc(cx - 8, cy - 8, 4, 0, WHITE);
        disc(cx + 8, cy - 8, 4, 0, WHITE);
        disc(cx - 8, cy - 8, 2, 0, INK);
        disc(cx + 8, cy - 8, 2, 0, INK);
    } else if (type == 2) {                                   /* HP: bird */
        cv_rect(cx - 4, cy - 30, 3, 10, RGB(60, 110, 230));
        cv_rect(cx + 2, cy - 28, 3, 8, RGB(60, 110, 230));
        disc(cx, cy, 22, 0, RGB(70, 130, 245));
        disc(cx - 14, cy + 6, 9, 0, RGB(40, 90, 200));
        disc(cx + 8, cy - 7, 6, 0, WHITE);
        disc(cx + 9, cy - 7, 3, 0, INK);
        cv_line_t(cx + 18, cy, cx + 34, cy + 5, RGB(255, 150, 30), 5);
    } else {                                                  /* NOTCH: ghost */
        disc(cx, cy - 4, 22, 0, WHITE);
        cv_rect(cx - 22, cy - 4, 45, 24, WHITE);
        disc(cx - 15, cy + 20, 7, 0, WHITE);
        disc(cx, cy + 20, 7, 0, WHITE);
        disc(cx + 15, cy + 20, 7, 0, WHITE);
        disc(cx - 8, cy - 8, 4, 0, INK);
        disc(cx + 8, cy - 8, 4, 0, INK);
        disc(cx, cy + 6, 4, 0, INK);
    }
}

static void pic_filter(void)
{
    int32_t x, base = PY + PH - 6, fc = 20 + trk[0].p[P_E2] * 160 / 127, py = 0, ty = trk[0].p[P_FTYPE] & 3;
    for (x = 0; x < 200; x++) {
        int32_t y = base - fgain(ty, x, fc, trk[0].p[P_E3]), a = x ? (py < y ? py : y) : y, b = x ? (py < y ? y : py) : y;
        cv_rect(PX + 10 + x, y, 1, base - y, RGB(190, 245, 225));
        cv_rect(PX + 10 + x, a - 1, 1, b - a + 3, RGB(0, 150, 110));
        py = y;
    }
    face(PX + PW / 2, PY + 36, ty);
}

static void pic_space(void)
{
    int32_t sz = song.g[G_RSIZE], w = 60 + sz * 150 / 127, h = 40 + sz * 70 / 127, cx = PX + PW / 2, cy = PY + PH / 2;
    int32_t maxr = h / 2 - 4, i, r;
    cv_rrect(cx - w / 2, cy - h / 2, w, h, 8, INK, WHITE);
    cv_rrect(cx - w / 2 + 4, cy - h / 2 + 4, w - 8, h - 8, 6, RGB(255, 245, 200), INK);
    if (ui_ring)
        for (i = 0; i < 3; i++) {
            r = (int32_t)((ui_ring * 3 + (uint32_t)i * (uint32_t)maxr / 3u) % (uint32_t)maxr);
            if (r > 3)
                disc(cx, cy, r, 3, RGB(80 + 50 * i, 140, 255));
        }
    disc(cx, cy, 6, 0, RGB(235, 60, 130));
}

/* ---------------------------------------------------------- page --- */
static void scene(void)
{
    uint16_t bg = PG[ui_page].bg, track = ux_mix(bg, INK, 25);
    int32_t n = 0, i, gap = 6, bw, oct = song.octave, ty = trk[0].p[P_FTYPE] & 3;
    char s[8] = "OCT +0";
    const char *title = ui_page ? PG[ui_page].title : WAVE_NAMES[trk[0].p[P_E0] & 7];
    while (n < 4 && PG[ui_page].k[n].v)
        n++;
    cv_text(12, 4, &AF_L, title, INK);
    s[4] = oct < 0 ? '-' : '+';
    s[5] = (char)('0' + (oct < 0 ? -oct : oct));
    cv_text_r(232, 12, &AF_M, s, INK, bg);
    cv_rrect(PX, PY, PW, PH, 10, ux_mix(bg, WHITE, 75), bg);
    (ui_page == 0 ? pic_wave : ui_page == 1 ? pic_filter : ui_page == 2 ? pic_env : pic_space)();
    bw = (226 - gap * (n - 1)) / n;
    for (i = 0; i < n; i++) {
        const kn_t *k = &PG[ui_page].k[i];
        int32_t x = 7 + i * (bw + gap), v = *k->v, fill = k->max < 16 ? (v + 1) * 38 / (k->max + 1) : v * 38 / k->max;
        const char *name = k->name ? k->name : FT_NAME[ty];
        cv_rrect(x, 170, bw, 38, 8, track, bg);
        if (fill > 0)
            cv_rrect(x, 208 - (fill < 8 ? 8 : fill), bw, fill < 8 ? 8 : fill, 8, WHITE, track);
        cv_text(x + (bw - text_w(&AF_M, name)) / 2, 212, &AF_M, name, INK);
    }
}

static void ui_draw(void)
{
    uint32_t y0;
    int32_t i;
    if (ux.mono) {                             /* the GREY palette mixes to grey: these pages are in colour */
        ux.mono = 0;
        ux.gen++;
    }
    for (i = 0; i < 128; i++)                  /* the scanned frame, once for both strips */
        wy[i] = (int16_t)(PY + PH / 2 - ((wave_peek(trk[0].p[P_E0] & 7, trk[0].p[P_E1], i * 2) * 46) >> 15));
    for (y0 = 0; y0 < 240u; y0 += 120u) {
        cv_begin(240, 120, PG[ui_page].bg);
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
    int16_t sig[8] = {0};
    if ((pressed & (dn | up)) && (fm1_in.buttons & (dn | up)) == (dn | up))
        song.octave = 0;
    else if ((pressed & dn) && song.octave > -3)
        song.octave--;
    else if ((pressed & up) && song.octave < 3)
        song.octave++;
    for (i = 0; i < 4u; i++)
        if (pressed & (1u << panel.btn[PG[i].btn]))
            ui_page = (uint8_t)i;
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
    sig[5] = song.octave;
    sig[6] = (int16_t)ui_ring;
    if (!memcmp(sig, ui_sig, sizeof sig) || fm1_ms - ui_last < 40u)
        return;
    memcpy(ui_sig, sig, sizeof sig);
    ui_last = fm1_ms;
    ui_draw();
}
