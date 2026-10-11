/* SPDX-License-Identifier: GPL-3.0-only */
/* KOSMO_PERF only: read-only snapshot of the clock registers (JL_CLOCK 0x10000, PLLs from the SDK clock.c).
 *   fm1_perf_clk(r)   r[0..10] = SYS_DIV, CLK_CON0..3, 0x11930/34 (PLL1), 0x119A0..AC (PLL2) */
#pragma once
#include <stdint.h>

static void fm1_perf_clk(uint32_t *r)
{
    r[0] = *(volatile uint32_t *)0x10008u;
    r[1] = *(volatile uint32_t *)0x1000Cu;
    r[2] = *(volatile uint32_t *)0x10010u;
    r[3] = *(volatile uint32_t *)0x10014u;
    r[4] = *(volatile uint32_t *)0x10018u;
    r[5] = *(volatile uint32_t *)0x11930u;
    r[6] = *(volatile uint32_t *)0x11934u;
    r[7] = *(volatile uint32_t *)0x119A0u;
    r[8] = *(volatile uint32_t *)0x119A4u;
    r[9] = *(volatile uint32_t *)0x119A8u;
    r[10] = *(volatile uint32_t *)0x119ACu;
}

#include "fm1_time.h"
/* TIMER4 ticks (24 MHz) for 10000 x 32 nops + loop: the CPU clock in MHz is about 24 * 350000 / ticks if a nop takes a cycle */
static uint32_t fm1_perf_nops(void)
{
    uint32_t i, t0 = fm1_ticks();
    for (i = 0; i < 10000u; i++)
        __asm__ volatile(
            "nop\n nop\n nop\n nop\n nop\n nop\n nop\n nop\n nop\n nop\n nop\n nop\n nop\n nop\n nop\n nop\n"
            "nop\n nop\n nop\n nop\n nop\n nop\n nop\n nop\n nop\n nop\n nop\n nop\n nop\n nop\n nop\n nop\n");
    return fm1_ticks() - t0;
}
