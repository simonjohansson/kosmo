/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments */
/* Settings save flow against NOR flash: the record round trip, no erase when nothing changed, failed saves retried. */
#define main hostsim_main
#include "hostsim.c"
#undef main

static int32_t fm1_enc_take(uint32_t e) { (void)e; return 0; }
static void lcd_sync(void) {}
static void lcd_blit(uint32_t x, uint32_t y, uint32_t w, uint32_t h, const uint16_t *p)
{ (void)x; (void)y; (void)w; (void)h; (void)p; }
#define KOSMO_FLASH 1
#include "../firmware/src/gfx.c"
#include "../firmware/src/panel.c"

static uint8_t nor[0x100000], flash_ok = 1;
static int erase_error;
static uint32_t erases;
static int st_read(uint32_t off, void *dst, uint32_t n) { memcpy(dst, nor + off, n); return 0; }
static int st_erase(uint32_t off)
{
    erases++;
    if (erase_error) return -8;
    memset(nor + off, 0xFF, 4096);
    return 0;
}
static int st_prog(uint32_t off, const void *src, uint32_t n)
{
    const uint8_t *s = src;
    uint32_t i;
    for (i = 0; i < n; i++) nor[off + i] &= s[i];
    return 0;
}
static uint32_t irq_save(void) { return 0; }
static void irq_restore(uint32_t f) { (void)f; }
static uint32_t fl_jedec_ram(void) { return 0x856014u; }
static void fl_plain_window_init(void) {}
#define FL_FAR(fn) (fn)
#include "../firmware/src/storage.c"
#include "../firmware/src/settings_persist.c"

static int check(const char *what, int ok)
{
    printf("persist: %-66s %s\n", what, ok ? "ok" : "FAIL");
    return ok ? 0 : 1;
}

static void reset(void)
{
    memset(nor, 0xFF, sizeof nor);
    memset(&song, 0, sizeof song);
    memset(&persist_saved, 0, sizeof persist_saved);
    memset(&settings, 0, sizeof settings);
    panel = PANEL_DEFAULT;
    host_tracks_init();
    fm1_ms = 0;
    persist_pending = 0;
    persist_retry_ms = 0;
    flash_ok = 1;
    erase_error = 0;
    erases = 0;
}

int main(void)
{
    int bad = 0;
    uint32_t before;
    persist_t p;
    reset();
    settings_init();
    settings.palette = 3;
    settings.lowcut = 2;
    settings.zoom = 1;
    persist_saved.bold = 1;                       /* (an earlier firmware's font weight: kept as saved) */
    persist_saved.favorites.factory[8][0] = 1;    /* (Felucca's favorites block: kept as saved) */
    persist_saved.favorites.user = 1u << 31;
    settings_save();
    bad += check("a save is written at once, once, and the request is cleared", !persist_pending && erases == 1u &&
                  st_load(OBJ_SETTINGS, &p, sizeof p) == (int)sizeof p && p.palette == palette_to_stored(3) &&
                  p.lowcut == 2u && p.zoom == 1u && p.bold == 1u &&
                  p.favorites.factory[8][0] == 1 && p.favorites.user == (1u << 31) &&
                  !memcmp(&p.panel, &panel, sizeof panel));
    settings_save();
    bad += check("unchanged settings do not erase", !persist_pending && erases == 1u);
    settings.palette = 2;
    settings_save();
    bad += check("a changed palette is saved", !persist_pending && erases == 2u &&
                  st_load(OBJ_SETTINGS, &p, sizeof p) == (int)sizeof p && p.palette == palette_to_stored(2));
    settings.palette = 1;
    erase_error = 1;
    fm1_ms = 0xFFFFFF00u;
    settings_save();
    before = erases;
    settings_poll();
    fm1_ms += 999u;
    settings_poll();
    bad += check("failed settings retry waits through clock wrap", persist_pending && erases == before &&
                  persist_saved.palette == palette_to_stored(2) && settings.palette == 1u);
    erase_error = 0;
    fm1_ms++;
    settings_poll();
    bad += check("settings retry succeeds after 1000 ms", !persist_pending && erases == before + 1u &&
                  st_load(OBJ_SETTINGS, &p, sizeof p) == (int)sizeof p && p.palette == palette_to_stored(1));
    {   /* power-on: the flash copy comes back into the runtime */
        persist_saved.palette = 0;
        memset(&settings, 0, sizeof settings);
        persist_boot();
        bad += check("persist_boot reads the saved record back", persist_saved.palette == palette_to_stored(1) &&
                      settings.palette == 1u && persist_saved.panel.magic == PANEL_MAGIC);
    }
    {   /* ECHO STEREO (SETTINGS page): saved in the spare lowcut field, record size unchanged, back after power-on */
        settings.lowcut = ECHO_STEREO_TAG;
        settings_save();
        persist_saved.lowcut = 0;
        memset(&settings, 0, sizeof settings);
        persist_boot();
        bad += check("ECHO STEREO survives a save and a power-on load", st_load(OBJ_SETTINGS, &p, sizeof p) == (int)sizeof p &&
                      p.lowcut == ECHO_STEREO_TAG && settings.lowcut == ECHO_STEREO_TAG);
    }
    {   /* SEM UNISON (SETTINGS page): OFF at power-on, ON saved with ECHO STEREO in the same lowcut field, back after power-on */
        bad += check("SEM UNISON is OFF in a fresh record", (set_flags(0) >> 1) == 0 && (set_flags(ECHO_STEREO_TAG) >> 1) == 0);
        settings.lowcut = ECHO_STEREO_TAG | SEM_UNISON_TAG;
        settings_save();
        persist_saved.lowcut = 0;
        memset(&settings, 0, sizeof settings);
        persist_boot();
        bad += check("SEM UNISON ON (with ECHO STEREO) survives a save and a power-on load", st_load(OBJ_SETTINGS, &p, sizeof p) == (int)sizeof p &&
                      set_flags(p.lowcut) == 3u && set_flags(settings.lowcut) == 3u);
    }
    {   /* LFO RETRIG (SETTINGS page, bit 2): OFF in a fresh record and in the older tags, ON saved with the others, back after power-on */
        bad += check("LFO RETRIG is OFF in a fresh record and the older tags", (set_flags(0) >> 2) == 0 && (set_flags(ECHO_STEREO_TAG | SEM_UNISON_TAG) >> 2) == 0);
        settings.lowcut = ECHO_STEREO_TAG | SEM_UNISON_TAG | LFO_RETRIG_TAG;
        settings_save();
        persist_saved.lowcut = 0;
        memset(&settings, 0, sizeof settings);
        persist_boot();
        bad += check("LFO RETRIG ON (with the others) survives a save and a power-on load", st_load(OBJ_SETTINGS, &p, sizeof p) == (int)sizeof p &&
                      set_flags(p.lowcut) == 7u && set_flags(settings.lowcut) == 7u);
    }
    {   /* no flash part: RAM only, nothing written */
        reset();
        flash_ok = 0;
        settings_init();
        settings.palette = 4;
        settings_save();
        bad += check("a flash that is not the expected part stays RAM-only", !erases && persist_pending == 1);
    }
    printf("%s\n", bad ? "PERSISTENCE TEST FAILED" : "persistence test passed");
    return bad != 0;
}
