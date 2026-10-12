/* SPDX-License-Identifier: GPL-3.0-only */
/* The kids' UI: five pages, one function button each (SETTINGS on GLO: K1 = ECHO PING-PONG / STEREO) (its LED is lit while the page shows):
 *   HOME WAVE (wave, scan, fat, detune) - EDIT FILTER (type, cutoff, reso) - ENV ADSR - FX ECHO (time, repeats, character,
 *   amount), pressed again SPACE (amount, size), again ECHO: the order of the signal chain; the header corner says 1/2 or 2/2
 * K1..K4 turn the bars at the bottom. OCT-/OCT+ shift the keys (both together: back to the middle).
 * An 80s CRT on every page: black, a faint graticule, glowing phosphor lines, one phosphor colour per page. No framebuffer: the screen is drawn in
 * strips of a rectangle (cv_ox / cv_oy shift screen coordinates into the strip) and only what changed: a knob step sends its bar and the picture
 * (and the WAVE title when the wave changed), a page or banner change the whole screen. */

#include "fcurve.c"                 /* the FILTER page's response curve */

#define PX 10                       /* the picture's panel */
#define PY 40
#define PW 220
#define PH 122

typedef struct { const char *name; int16_t *v; uint8_t max, step; } kn_t;
#define NPG 8                       /* pages 0..3 as before (3: SPACE), 4: ECHO; the FX button cycles ECHO, SPACE; 5: SETTINGS (GLO); 6, 7: SEM's 1/2 and 2/2 (HOME, the SEM engine) */
#define PG_SPACE 3
#define PG_ECHO 4
#define PG_SET 5
#define PG_SEM1 6
#define PG_SEM2 7
#define UI_SEM (trk[0].p[P_ENG] == 1)   /* the engine ALGORITHM chose */
static int16_t set_ec;                      /* SETTINGS K1: ECHO STEREO, 0 PING-PONG / 1 STEREO (kept in settings.lowcut as ECHO_STEREO_TAG) */
static const struct { uint8_t btn; const char *title; uint16_t bg; kn_t k[4]; } PG[NPG] = {
    {B_HOME, 0, RGB(120, 255, 140), {{"Wave", &trk[0].p[P_E0], NWAVES - 1, 1}, {"Scan", &trk[0].p[P_E1], 127, 4},
                                          {"Fat", &trk[0].p[P_E4], 4, 1}, {"Detune", &trk[0].p[P_DETUNE], 127, 4}}},
    {B_EDIT, "FILTER", RGB(255, 176, 0), {{0, &trk[0].p[P_FTYPE], 4, 1}, {"Cutoff", &trk[0].p[P_E2], 127, 4},
                                          {"Reso", &trk[0].p[P_E3], 127, 4}}},
    {B_ENV, "ENV", RGB(60, 230, 255), {{"Attack", &trk[0].p[P_ATK], 127, 4}, {"Decay", &trk[0].p[P_DEC], 127, 4},
                                        {"Sustain", &trk[0].p[P_SUS], 127, 4}, {"Release", &trk[0].p[P_REL], 127, 4}}},
    {B_FX, "SPACE", RGB(255, 80, 200), {{"Amount", &trk[0].p[P_REV], 127, 4}, {"Size", &song.g[G_RSIZE], 127, 4},
                                    {0, &song.g[G_RPROG], 5, 1}, {"Tone", &song.g[G_RTONE], 127, 4}}},
    {B_FX, "ECHO", RGB(255, 96, 40), {{"Time", &song.g[G_ETIME], 127, 4}, {"Repeats", &song.g[G_EFB], 127, 4},
                                      {0, &song.g[G_ECHAR], 4, 1}, {"Amount", &song.g[G_EAMT], 127, 4}}},
    {B_GLO, "SETTINGS", RGB(170, 120, 255), {{0, &set_ec, 1, 1}}},
    {B_HOME, "SEM", RGB(120, 255, 140), {{0, &trk[0].p[P_S0], 1, 1}, {"PW 1", &trk[0].p[P_S1], SEM_PW_MAX, 3},
                                         {0, &trk[0].p[P_S2], 1, 1}, {"PW 2", &trk[0].p[P_S3], SEM_PW_MAX, 3}}},
    {B_HOME, "SEM", RGB(120, 255, 140), {{"Tune", &trk[0].p[P_S4], 48, 1}, {"Fine", &trk[0].p[P_S5], 100, 2},
                                         {0, &trk[0].p[P_S6], 1, 1}, {"Mix", &trk[0].p[P_S7], 128, 4}}},
};
static const char *const SEM_W_NAME[2][2] = {{"1 Saw", "1 Pulse"}, {"2 Saw", "2 Pulse"}};
static const char *const SEM_SYNC_NAME[2] = {"Free", "Sync"};
static const char *const SET_EC_NAME[2] = {"Ping-pong", "Stereo"};
static const char *const FT_NAME[5] = {"LP", "BP", "HP", "NOTCH", "COMB"};
static const char *const SP_NAME[6] = {"Bedroom", "Castle", "Plate", "Cloud", "Gated", "Wobble"};   /* the bar is 52 px wide */
static const char *const SP_FULL[6] = {"BEDROOM", "CASTLE", "METAL PLATE", "CLOUD", "GATED", "WOBBLE"};
static const char *const EC_NAME[5] = {"Clean", "Crunch", "Toy", "Back", "Robot"};   /* BACKWARDS: the bar is 52 px wide */
static uint8_t ui_page, ui_preset;          /* ui_preset: the one loaded last */
static uint32_t ui_banner;                  /* the preset banner shows until this fm1_ms (0: none) */
static uint8_t ui_bkind;                    /* what it shows: 0 the preset, 1 the engine (ALGORITHM) */
static int16_t ui_sig[7] = {-1};            /* what the screen shows now; -1: nothing yet (draw_splash resets it) */
static uint32_t ui_last, ui_ring;           /* last redraw (ms), ring animation step */
static int8_t ui_led = -1;
typedef struct { int16_t x, y, w, h; } rc_t;
static rc_t ui_cr;                          /* the rectangle being drawn: scene() leaves out what does not touch it */
#define R_TITLE ((rc_t){0, 0, 240, 40})
#define R_PIC ((rc_t){PX, PY, PW + 4, ui_page == 2 ? 128 : 122})   /* what a picture draws lies inside (ENV's letters hang below the panel) */
static int ui_hit(rc_t r)
{
    return r.x < ui_cr.x + ui_cr.w && r.x + r.w > ui_cr.x && r.y < ui_cr.y + ui_cr.h && r.y + r.h > ui_cr.y;
}
static int32_t ui_nk(void)
{
    int32_t n = 0;
    while (n < 4 && PG[ui_page].k[n].v)
        n++;
    return n;
}
static rc_t bar_rc(int32_t i)               /* knob i's bar and name, 3 px of the gap on each side */
{
    int32_t n = ui_nk(), bw = (226 - 6 * (n - 1)) / n;
    return (rc_t){(int16_t)(4 + i * (bw + 6)), 170, (int16_t)(bw + 6), 60};
}

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
        pt[i][1] = (int16_t)(PY + PH / 2 - 5 * c - ((wave_peek(trk[0].p[P_E0], trk[0].p[P_E1], i * 2 + sh) * amp) >> 15));
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

/* SEM: VCO1, VCO2 and their mix as three glowing traces on one time axis (VCO1 three cycles wide). PW shows in the pulse, TUNE and FINE in
 * VCO2's speed, SYNC restarts VCO2 at every VCO1 cycle; MIX lights the source it favours and the mix itself. */
static void pic_sem(void)
{
    const int16_t *p = trk[0].p;
    int32_t a[100], b[100], i, j, mx = clamp(p[P_S7], 0, 128), ratio;
    uint32_t pw1 = sem_pw(p[P_S1]), pw2 = sem_pw(p[P_S3]), st1 = 128849019u, st2, ph1 = 0, ph2 = 0;
    static const char *const L[3] = {"1", "2", "MIX"};
    ratio = (int32_t)(pitch_inc((uint32_t)(960 + (p[P_S4] - 24) * 16 + (p[P_S5] - 50) * 16 / 100)) / (pitch_inc(960) >> 8));
    st2 = (st1 >> 8) * (uint32_t)ratio;
    for (i = 0; i < 100; i++) {
        a[i] = sem_naive(p[P_S0], ph1, pw1);
        b[i] = sem_naive(p[P_S2], ph2, pw2);
        ph1 += st1;
        ph2 = p[P_S6] && ph1 < st1 ? 0u : ph2 + st2;
    }
    for (j = 0; j < 3; j++) {
        int32_t cy = PY + 21 + 40 * j;
        uint16_t c = PG[PG_SEM1].bg;
        for (i = 0; i < 100; i++) {
            int32_t y = j == 0 ? a[i] : j == 1 ? b[i] : (a[i] * (128 - mx) + b[i] * mx) >> 7;
            pt[i][0] = (int16_t)(PX + 28 + i * 184 / 99);
            pt[i][1] = (int16_t)(cy - ((y * 15) >> 15));
        }
        glow_pts(100, j == 2 ? c : ux_mix(0, c, 25 + (j ? mx : 128 - mx) * 45 / 128));
        cv_text(PX + 4, cy - 8, &AF_S, L[j], ux_mix(0, c, 60));
    }
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

/* SPACE: the room as a glowing box (SIZE), sound rays leaving its centre and ricocheting off the walls; their number and
 * how far they get follow the decay (the program's feedback and the line length), the brightness fades along the path.
 * GATED: the rays stop at the first wall. While notes play the rays breathe (ui_ring). */
static void pic_space(void)
{
    static const int8_t DIR[16][2] = {{62, 16}, {55, 33}, {42, 48}, {16, 62}, {-16, 62}, {-42, 48}, {-55, 33}, {-62, 16},
                                      {-62, -16}, {-55, -33}, {-42, -48}, {-16, -62}, {16, -62}, {42, -48}, {55, -33}, {62, -16}};
    const sp_prog_t *p = &SP_PROG[clamp(song.g[G_RPROG], 0, 5)];
    int32_t sz = song.g[G_RSIZE], w = 60 + sz * 150 / 127, h = 40 + sz * 52 / 127, cx = PX + PW / 2, cy = PY + PH / 2 + 6;
    int32_t dec = 3276800 / (32768 - p->g) * (p->lo + (p->hi - p->lo) * sz / 127) >> 8;     /* the loops until it is gone (1 / (1 - g)) x SIZE's length, x 100 */
    int32_t nr = clamp(2 + dec / 250, 3, 6), len = 40 + dec / 4, hw = w / 2, hh = h / 2, j, k;
    uint16_t c = PG[PG_SPACE].bg;
    const char *nm = SP_FULL[clamp(song.g[G_RPROG], 0, 5)];
    if (ui_ring)
        len = len * (70 + (int32_t)ui_ring) / 118;
    for (j = 0; j < nr; j++) {
        int32_t d = (j * 16 / nr + (j & 1)) & 15, dx = DIR[d][0], dy = DIR[d][1], x = 0, y = 0, rem = len;
        for (k = 0; k < 10 && rem > 0; k++) {
            int32_t tx = ((dx > 0 ? hw : -hw) - x) * 64 / dx, ty = ((dy > 0 ? hh : -hh) - y) * 64 / dy, t = tx < ty ? tx : ty, nx, ny, m;
            int32_t step = t < rem ? t : rem;
            nx = x + dx * step / 64;
            ny = y + dy * step / 64;
            m = 12 + 88 * rem / len;
            cv_line_t(cx + x, cy + y, cx + nx, cy + ny, ux_mix(0, c, m), 2);
            x = nx;
            y = ny;
            rem -= step;
            if (t > step)
                break;
            if (tx < ty)
                dx = -dx;
            else
                dy = -dy;
            if (p->hold) {                        /* GATED: cut off at the wall */
                cv_rect(cx + x - 2, cy + y - 2, 5, 5, c);
                break;
            }
        }
    }
    glow_box(cx - hw, cy - hh, w, h, c);
    disc(cx, cy, 6, 0, ux_mix(0, c, 40));
    disc(cx, cy, 3, 0, c);
    cv_text(PX + 6, PY + 4, &AF_S, nm, c);
}

/* ECHO: a row of copies of the waveform, each one smaller and dimmer: the spacing is TIME, the fade REPEATS, the whole
 * row dims with AMOUNT; the character shapes the copies (CRUNCH clipped, TOY stepped, BACKWARDS reversed, ROBOT held) */
static void pic_echo(void)
{
    int32_t sp = 24 + song.g[G_ETIME] * 38 / 127, gain = 28 + song.g[G_EFB] * 68 / 127, lum = 30 + song.g[G_EAMT] * 70 / 127;
    int32_t ch = song.g[G_ECHAR], a[9] = {100}, n = 0, k, i, x0;
    while (n < 8 && PX + 10 + n * sp + 32 <= PX + PW - 4 && a[n] >= 6) {     /* as many as fit and still show */
        a[n + 1] = a[n] * gain / 100;
        n++;
    }
    for (k = n - 1; k >= 0; k--) {
        int32_t amp = 44 * a[k] / 100, h = 0;
        x0 = PX + 10 + k * sp;
        for (i = 0; i < 32; i++) {
            int32_t y;
            if (!(ch == 4 && i % 6))                             /* ROBOT holds every 6th point */
                y = (wave_peek(trk[0].p[P_E0], trk[0].p[P_E1], (ch == 3 ? 31 - i : i) * 8) * amp) >> 15;
            else
                y = h;
            y = ch == 1 ? clamp(y * 8 / 5, -amp * 6 / 10, amp * 6 / 10) : ch == 2 ? y / 6 * 6 : y;
            h = y;
            pt[i][0] = (int16_t)(x0 + i);
            pt[i][1] = (int16_t)(PY + PH / 2 - y);
        }
        if (k == 0)
            glow_pts(32, ux_mix(0, PG[PG_ECHO].bg, lum));
        else
            for (i = 0; i < 31; i++)
                cv_line_t(pt[i][0], pt[i][1], pt[i + 1][0], pt[i + 1][1], ux_mix(0, PG[PG_ECHO].bg, a[k] * lum / 100 + 8), 3);
    }
}

/* SETTINGS: ECHO STEREO. PING-PONG: dots bouncing between the L and R walls; STEREO: dots staying on their own side */
static void pic_settings(void)
{
    int32_t i, xl = PX + 36, xr = PX + PW - 36, y0 = PY + 28, dy = 12, st = set_ec;
    uint16_t c = PG[PG_SET].bg;
    cv_text(PX + 6, PY + 4, &AF_S, "ECHO", c);
    cv_text(xl - text_w(&AF_M, "L") / 2, PY + PH - 20, &AF_M, "L", c);
    cv_text(xr - text_w(&AF_M, "R") / 2, PY + PH - 20, &AF_M, "R", c);
    for (i = 0; i < 6; i++) {
        int32_t m = 100 - i * 14, y = y0 + i * dy, x = i & 1 ? xr : xl;
        if (st) {                                   /* two columns, each dot keeps its side */
            disc(xl, y, 5, 0, ux_mix(0, c, m));
            disc(xr, y, 5, 0, ux_mix(0, c, m));
            continue;
        }
        if (i < 5)
            cv_line_t(x, y, i & 1 ? xl : xr, y + dy, ux_mix(0, c, 25), 2);
        disc(x, y, 5, 0, ux_mix(0, c, m));
    }
}

static void banner(void)
{
    const char *n = ui_bkind ? ENGINES[clamp(trk[0].p[P_ENG], 0, NENG - 1)]->name : PRESETS[ui_preset].name;
    char s[12];
    uint32_t k = 0, a = (uint32_t)ui_preset + 1u;
    int32_t w = text_w(&AF_L, n) + 24, x = (240 - w) / 2;
    if (a > 9u)
        s[k++] = (char)('0' + a / 10u);
    s[k++] = (char)('0' + a % 10u);
    memcpy(s + k, " / ", 3);
    k += 3;
    if (NPRESETS > 9u)
        s[k++] = (char)('0' + NPRESETS / 10u);
    s[k++] = (char)('0' + NPRESETS % 10u);
    s[k] = 0;
    if (ui_bkind) {                             /* the engine: its name over ENGINE */
        w = text_w(&AF_L, n) > text_w(&AF_M, "ENGINE") ? w : text_w(&AF_M, "ENGINE") + 24;
        x = (240 - w) / 2;
        memcpy(s, "ENGINE", 7);
    }
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
    int32_t n = ui_nk(), i, gap = 6, bw, ty = clamp(trk[0].p[P_FTYPE], 0, 4);
    const char *title = ui_page ? PG[ui_page].title : WAVE_NAMES[clamp(trk[0].p[P_E0], 0, NWAVES - 1)];
    if (ui_hit(R_TITLE)) {
        cv_text(12, 4, &AF_L, title, bg);
        if (PG[ui_page].btn == B_FX || ui_page >= PG_SEM1) {   /* which FX page: 1/2 ECHO, 2/2 SPACE; SEM's 1/2, 2/2 */
            const char *t = ui_page == PG_ECHO || ui_page == PG_SEM1 ? "1/2" : "2/2";
            cv_text(228 - text_w(&AF_M, t), 12, &AF_M, t, bg);
        }
    }
    if (ui_hit(R_PIC)) {
        graticule(PX, PY, PW, PH, bg);
        (ui_page >= PG_SEM1 ? pic_sem : ui_page == 0 ? pic_wave : ui_page == 1 ? pic_filter : ui_page == 2 ? pic_env : ui_page == PG_ECHO ? pic_echo : ui_page == PG_SET ? pic_settings : pic_space)();
    }
    bw = (226 - gap * (n - 1)) / n;
    for (i = 0; i < n; i++) {
        const kn_t *k = &PG[ui_page].k[i];
        if (!ui_hit(bar_rc(i)))
            continue;
        int32_t x = 7 + i * (bw + gap), v = *k->v, fill = k->max < 16 ? (v + 1) * 38 / (k->max + 1) : v * 38 / k->max;
        const char *name = k->name ? k->name : ui_page == PG_SET ? SET_EC_NAME[clamp(*k->v, 0, 1)] : ui_page == PG_SEM1 ? SEM_W_NAME[i >> 1][clamp(*k->v, 0, 1)] : ui_page == PG_SEM2 ? SEM_SYNC_NAME[clamp(*k->v, 0, 1)] : ui_page == PG_ECHO ? EC_NAME[clamp(*k->v, 0, 4)] : ui_page == PG_SPACE ? SP_NAME[clamp(*k->v, 0, 5)] : FT_NAME[ty];
        cv_rrect(x, 170, bw, 38, 8, track, 0);
        if (fill > 0)
            cv_rrect(x, 208 - (fill < 8 ? 8 : fill), bw, fill < 8 ? 8 : fill, 8, bg, track);
        cv_text(x + (bw - text_w(&AF_M, name)) / 2, 212, &AF_M, name, bg);
    }
    if (ui_banner && ui_hit((rc_t){0, 78, 240, 74}))
        banner();
}

static void ui_rect(rc_t r)                     /* draw the screen's rectangle r, in strips that fit the canvas */
{
    int32_t rows = 64, n = (r.h + rows - 1) / rows, h = (r.h + n - 1) / n, y0;
    if (ux.mono) {                             /* the GREY palette mixes to grey: these pages are in colour */
        ux.mono = 0;
        ux.gen++;
    }
    for (y0 = r.y; y0 < r.y + r.h; y0 += h) {
        if (y0 + h > r.y + r.h)
            h = r.y + r.h - y0;
        ui_cr = (rc_t){r.x, (int16_t)y0, r.w, (int16_t)h};
        cv_begin((uint32_t)r.w, (uint32_t)h, 0);
        cv_ox = -r.x;
        cv_oy = -y0;
        scene();
        cv_ox = cv_oy = 0;
        cv_blit((uint32_t)r.x, (uint32_t)y0);
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
    int16_t sig[7] = {0}, old[7];
    int32_t pst;
    if ((pressed & (dn | up)) && (fm1_in.buttons & (dn | up)) == (dn | up))
        song.octave = 0;
    else if ((pressed & dn) && song.octave > -3)
        song.octave--;
    else if ((pressed & up) && song.octave < 3)
        song.octave++;
    for (i = 0; i < 4u; i++)
        if (pressed & (1u << panel.btn[PG[i].btn]))
            ui_page = (uint8_t)(i == PG_SPACE ? (ui_page == PG_ECHO ? PG_SPACE : PG_ECHO) : i == 0 && UI_SEM ? (ui_page == PG_SEM1 ? PG_SEM2 : PG_SEM1) : i);   /* FX: ECHO, SPACE, ECHO ...; SEM: 1/2, 2/2 */
    if (pressed & (1u << panel.btn[B_GLO]))
        ui_page = PG_SET;
    pst = panel_enc(EN_PRESET);
    if (pst) {                                  /* PRESETS: step with wrap-around, show the name for 1.5 s */
        ui_preset = (uint8_t)((ui_preset + pst % (int32_t)NPRESETS + (int32_t)NPRESETS) % (int32_t)NPRESETS);
        preset_load(ui_preset);
        ui_banner = fm1_ms + 1500u;
        ui_bkind = 0;
    }
    pst = panel_enc(EN_ALGO);
    if (pst) {                                  /* ALGORITHM: the engine, with its name shown for 1.5 s */
        trk[0].p[P_ENG] = (int16_t)((trk[0].p[P_ENG] + pst % NENG + NENG) % NENG);
        ui_banner = fm1_ms + 1500u;
        ui_bkind = 1;
    }
    if (PG[ui_page].btn == B_HOME)              /* HOME is the engine's: WAVE, or SEM's 1/2 and 2/2 */
        ui_page = (uint8_t)(UI_SEM ? (ui_page == PG_SEM2 ? PG_SEM2 : PG_SEM1) : 0);
    if (ui_banner && (int32_t)(fm1_ms - ui_banner) >= 0)
        ui_banner = 0;
    set_ec = settings.lowcut == ECHO_STEREO_TAG;
    for (i = 0; i < 4u; i++) {                  /* knobs K1..K4 (a knob the page does not use is only drained) */
        int32_t s = panel_enc(EN_K1 + i), v;
        const kn_t *k = &PG[ui_page].k[i];
        if (!s || !k->v)
            continue;
        if (k->v == &trk[0].p[P_E1])            /* SCAN: n steps in one pass sweep n * n (a fast turn crosses the table) */
            s *= s < 0 ? -s : s;
        v = *k->v + s * k->step;
        *k->v = (int16_t)(v < 0 ? 0 : v > k->max ? k->max : v);
    }
    if ((settings.lowcut == ECHO_STEREO_TAG) != (set_ec != 0)) {   /* SETTINGS K1 turned: keep it and save */
        settings.lowcut = set_ec ? ECHO_STEREO_TAG : 0u;
        settings_save();
    }
    ec_stereo = set_ec != 0;
    if (!fm1_led_anim_on() && ui_led != (int8_t)ui_page) {     /* the page's button glows, the others do not */
        for (i = 0; i < 4u; i++)
            fm1_led_key(panel.btn[PG[i].btn], PG[i].btn == PG[ui_page].btn);
        fm1_led_key(panel.btn[B_GLO], ui_page == PG_SET);
        ui_led = (int8_t)ui_page;
    }
    for (i = 0; i < NVOICE; i++)
        active |= trk[0].v[i].active;
    if (active && ui_page == PG_SPACE && fm1_ms - ui_last >= 40u)
        ui_ring = ui_ring % 48u + 1u;           /* the rings creep outward while notes play (never 0: that is "quiet") */
    else if (!active)
        ui_ring = 0;
    sig[0] = (int16_t)ui_page;
    for (i = 0; i < 4u; i++)
        sig[1 + i] = PG[ui_page].k[i].v ? *PG[ui_page].k[i].v : 0;
    sig[5] = (int16_t)ui_ring;
    sig[6] = (int16_t)(ui_banner ? (ui_bkind ? 100 + trk[0].p[P_ENG] : 1 + ui_preset) : 0);
    if (!memcmp(sig, ui_sig, sizeof sig))
        return;
    ui_last = fm1_ms;
#if KOSMO_PERF
    uint32_t pt0 = fm1_ticks(), pt1;
#endif
    if (ui_sig[0] != sig[0] || ui_sig[6] != sig[6]) {          /* a new page, or the banner: all of it */
        memcpy(ui_sig, sig, sizeof sig);
        ui_rect((rc_t){0, 0, 240, 240});
#if KOSMO_PERF
        pt1 = fm1_ticks() - pt0;
        lcd_sync();
        pf_acc(&pf_page, pt1);
        pf_acc(&pf_pagef, fm1_ticks() - pt0);
#endif
        return;
    }
    /* a knob step: its bar first (the quick feedback), then the picture, which always shows the latest state, so steps
     * that came in meanwhile are drawn once. The WAVE title changes with the wave only */
    memcpy(old, ui_sig, sizeof old);
    memcpy(ui_sig, sig, sizeof sig);
    for (i = 0; i < 4u; i++)
        if (old[1 + i] != sig[1 + i] && PG[ui_page].k[i].v)
            ui_rect(bar_rc((int32_t)i));
    if (ui_page == 0 && old[1] != sig[1])
        ui_rect(R_TITLE);
#if KOSMO_PERF
    pt1 = fm1_ticks();
#endif
    ui_rect(R_PIC);
#if KOSMO_PERF
    pf_acc(&pf_pic, fm1_ticks() - pt1);
    pf_acc(&pf_step, fm1_ticks() - pt0);
    lcd_sync();
    pf_acc(&pf_stepf, fm1_ticks() - pt0);
#endif
}

#if KOSMO_PERF
static void perf_cc(uint32_t cc, uint32_t v)
{
    if (cc == 102u)
        ui_page = (uint8_t)(v & 3u);
    else if (cc == 103u)
        trk[0].p[P_E0] = (int16_t)(v % NWAVES);
    else if (cc == 104u)
        trk[0].p[P_E1] = (int16_t)v;
    else if (cc == 105u)
        trk[0].p[P_E4] = (int16_t)(v > 4u ? 4u : v);
    else if (cc == 106u)
        trk[0].p[P_DETUNE] = (int16_t)v;
    else if (cc == 107u)
        trk[0].p[P_E2] = (int16_t)v;                 /* cutoff */
    else if (cc == 108u)
        preset_load(v);
    else if (cc == 109u)
        song.g[G_ECHAR] = (int16_t)(v % 5u);
    else if (cc == 110u)
        song.g[G_EAMT] = (int16_t)v;
    else if (cc == 111u)
        song.g[G_RPROG] = (int16_t)(v % 6u);
    else if (cc == 113u) {                           /* the engine: 0 WAVE, 1 SEM, 2 SEM with PULSE + PULSE + SYNC (its dearest) */
        trk[0].p[P_ENG] = (int16_t)(v ? 1 : 0);
        trk[0].p[P_S0] = trk[0].p[P_S2] = trk[0].p[P_S6] = (int16_t)(v == 2u);
        trk[0].p[P_S4] = (int16_t)(v == 2u ? 31 : 24);
    }
    else
        trk[0].p[P_REV] = (int16_t)v;                /* 112: SPACE amount */
}
#endif
