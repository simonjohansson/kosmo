/* SPDX-License-Identifier: GPL-3.0-only */
/* KOSMO_PERF=1: once a second the main loop writes one "P ..." line (k=v pairs, TIMER4 ticks = 1/24 us) into the CDC
 * out ring. Window counters reset after each line. The audio ISR and ui_input feed it (PERF_* hooks). */
typedef struct { uint32_t n, sum, max; } perf_acc_t;
static perf_acc_t pf_render, pf_all;                /* audio ISR: render alone, and with TIMER5 nested */
static perf_acc_t pf_in, pf_usbp, pf_uartp;       /* TIMER5 parts: fm1_input_tick, usb_poll, uart_midi_poll */
static perf_acc_t pf_t5, pf_t5n;                  /* TIMER5 ISR: outside the audio ISR / nested in it */
static perf_acc_t pf_step, pf_stepf, pf_page, pf_pagef, pf_pic;   /* ui_input: cpu time / until the panel has it */
static char pf_b[640];
static uint32_t pf_l;

static void pf_acc(perf_acc_t *a, uint32_t t) { a->n++; a->sum += t; if (t > a->max) a->max = t; }
static void pf_half(uint32_t r, uint32_t a) { pf_acc(&pf_render, r); pf_acc(&pf_all, a); }
extern uint32_t _bss_end[], _pool_start[], _pool_end[];

static void pf_s(const char *s) { while (*s && pf_l < sizeof pf_b - 1u) pf_b[pf_l++] = *s++; }
static void pf_kv(const char *k, uint32_t v)
{
    char t[12];
    uint32_t i = 0;
    pf_s(" "); pf_s(k); pf_s("=");
    do { t[i++] = (char)('0' + v % 10u); v /= 10u; } while (v);
    while (i && pf_l < sizeof pf_b - 1u) pf_b[pf_l++] = t[--i];
}
static void pf_acc_kv(const char *k, perf_acc_t *a)
{
    char kk[8];
    uint32_t i = 0;
    while (k[i]) { kk[i] = k[i]; i++; }
    kk[i + 1u] = 0;
    kk[i] = 'n'; pf_kv(kk, a->n);
    kk[i] = 's'; pf_kv(kk, a->sum);
    kk[i] = 'x'; pf_kv(kk, a->max);
    a->n = a->sum = a->max = 0;
}
static void pf_flush(void)
{
    uint32_t i;
    pf_s("\n");
    for (i = 0; i < pf_l; i++)
        if (co_w - co_r < CO_N)
            cdc_out[co_w++ % CO_N] = (uint8_t)pf_b[i];
    pf_l = 0;
}


static void perf_service(void)                 /* main loop */
{
    static uint32_t next, seq;
    uint32_t r[11], i, t0;
    static volatile uint32_t x = 1;
    if ((int32_t)(fm1_ms - next) < 0)
        return;
    next = fm1_ms + 1000u;
    pf_s("P");
    pf_kv("seq", seq++);
    pf_acc_kv("r", &pf_render); pf_acc_kv("a", &pf_all);
    pf_acc_kv("st", &pf_step); pf_acc_kv("sf", &pf_stepf);
    pf_acc_kv("pg", &pf_page); pf_acc_kv("pf", &pf_pagef); pf_acc_kv("pc", &pf_pic);
    pf_acc_kv("i", &pf_in); pf_acc_kv("ub", &pf_usbp); pf_acc_kv("ua", &pf_uartp);
    pf_acc_kv("t", &pf_t5); pf_acc_kv("u", &pf_t5n);
    { uint32_t k; char nm[4] = "xN";
      for (k = 0; k < 7u; k++) { nm[1] = (char)('0' + k); pf_kv(nm, pfx_s[k]); nm[2] = 'n'; pf_kv(nm, pfx_n[k]); nm[2] = 0; pfx_s[k] = pfx_n[k] = 0; } }
    pf_kv("spi", lcd_bytes);
    pf_kv("late", felucca_dbg.late);
    pf_kv("shed", shed_count);
    pf_kv("bss", (uint32_t)(_bss_end) - 0x01C08000u);
    pf_kv("pool", (uint32_t)(_pool_end) - (uint32_t)(_pool_start));
    pf_kv("nest", felucca_dbg.nested);
    pf_flush();
    if (KOSMO_PERF == 1 && seq % 5u == 1u) {                       /* clock registers and a CPU speed probe, every 5 s */
        fm1_perf_clk(r);
        pf_s("C");
        pf_kv("sysdiv", r[0]); pf_kv("c0", r[1]); pf_kv("c1", r[2]); pf_kv("c2", r[3]); pf_kv("c3", r[4]);
        pf_kv("p0", r[5]); pf_kv("p1", r[6]); pf_kv("q0", r[7]); pf_kv("q1", r[8]); pf_kv("q2", r[9]); pf_kv("q3", r[10]);
        t0 = fm1_ticks();
        for (i = 0; i < 20000u; i++)            /* 4 dependent ops per pass; IRQs may steal time: the host takes the min */
            x = (x * 3u + i) ^ (x >> 3);
        pf_kv("ips", fm1_ticks() - t0);
        { uint32_t m = fm1_perf_nops(), k = fm1_perf_nops(); pf_kv("nops", m < k ? m : k); }
        pf_flush();
    }
}
