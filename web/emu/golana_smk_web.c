/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments
 * The design (the device clock driven by the audio the worklet asks for, the exports below, the saved
 * flash handed back before boot) follows X0X's browser build, web/emu/x0x_web.c of charlesvestal/fm1-x0x
 * (GPL-3.0), a Felucca fork. */
/* Felucca in the browser: the firmware's sources, as the host tests build them (tests/hostsim.c: libc,
 * engines, voices, FX, sequencer; then the UI, main.c, storage and projects, in src/felucca.c's order),
 * compiled to WebAssembly and run in an AudioWorklet against a simulated FM-1:
 *   - the device clock: TIMER4 ticks (24 MHz) and fm1_ms advance with the audio the worklet asks for; the
 *     audio ISR (audio.c fm1_alnk0_irq, the real one) renders each I2S half (HALF_FRAMES) as the DMA would
 *     ask for it, and the main loop (main.c's, below in web_frame) runs every 15 ms, with ui_input on every
 *     millisecond in between as main.c's wait loop calls it;
 *   - the panel: buttons and encoders by their printed labels (panel.c's table maps them to the matrix),
 *     the keys as fm1_in.notes, the LEDs read back from fm1_led / fm1_led_dim through the key matrix;
 *   - the screen: lcd_fill / lcd_blit into a 240 x 240 RGB565 frame (the panel's byte order);
 *   - the flash: a 1 MiB NOR image behind storage.c's hooks (settings, projects, user presets, the user
 *     sample slots read through it as XIP); the page keeps its storage sectors and gives them back at boot.
 * Not here: USB (MIDI, audio, the editor, updates), TRS MIDI, the serial console, the battery (full), the
 * crash / UBOOT paths. Nothing of the firmware changes for this file.
 *   web/emu/build.sh  ->  build/emu/{felucca.wasm, worklet.js, index.html} */
#include <stdint.h>

#define FELUCCA_WEB 1
#define BOARD_SMK37 1                    /* the SMK-37 Pro panel: smk_panel_sim.h below */
#include "fm1_flash_map.h"          /* FL_SMP_*, FL_STOCKTAG_*: storage.c reads them (1.6.1 st_stock_release) */
#ifndef FELUCCA_CDC
#define FELUCCA_CDC 1                    /* as src/felucca.c: MENU > USB SERIAL is there (nothing behind it) */
#endif
#define FELUCCA_FLASH 1
#define FELUCCA_OTA 0
#define FELUCCA_UAC 0
#define FELUCCA_UART 0
#ifndef FELUCCA_VERSION
#define FELUCCA_VERSION "WEB"
#endif

static uint8_t nor[0x100000];            /* the SPI NOR (erased: 0xFF) */
#define SMP_USER_XIP(k) ((const uint8_t *)nor + SMP_USER_BASE + (k) * SMP_USER_SIZE)   /* eng_sample.c */
#include "smk_panel_sim.h"
#define FM1_IN_EXTERN 1                  /* the SMK state above replaces hostsim's tiny FM-1 one */
#define main hostsim_main
#include "../../tests/hostsim.c"
#undef main

/* ------------------------------------------------------------ the clock --- */
#define FM1_TICKS_PER_US 24u             /* TIMER4: 24 MHz */
static uint32_t web_ticks;
static uint32_t fm1_ticks(void) { return web_ticks; }

/* ------------------------------------------------------ audio (ALNK0) --- */
static uint32_t web_half;                /* the half the next render fills */
#define FM1_AUDIO_HALF 1u
static uint8_t fm1_audio_pending(void) { return FM1_AUDIO_HALF; }
static void fm1_audio_ack_aux(uint8_t p) { (void)p; }
static uint32_t fm1_audio_free_half(void) { return web_half; }
static void fm1_audio_ack_half(void) {}
static void fm1_audio_init(int32_t *buf, uint32_t words, void (*isr)(void), uint32_t prio)
{ (void)buf; (void)words; (void)isr; (void)prio; }
static void fm1_audio_stop(void) {}
void isr_alnk0(void) {}
#include "../../firmware/src/audio.c"

/* --------------------------------------------------------- the SMK panel --- */
/* smk_panel_sim.h is included above, before hostsim (seq.c needs its types) */
/* no power-on LED sweep in the simulator: the UI's own LEDs only */
static void fm1_led_anim_start(uint32_t glow)
{
    (void)glow;
    memset(fm1_led, 0, sizeof fm1_led);
    memset(fm1_led_dim, 0, sizeof fm1_led_dim);
    memset(fm1_led_breath, 0, sizeof fm1_led_breath);
    memset(fm1_led_mid, 0, sizeof fm1_led_mid);
}
static int fm1_led_anim_on(void) { return 0; }
static void fm1_led_idle_start(uint32_t glow) { (void)glow; }
static int fm1_led_idle_on(void) { return 0; }
static void fm1_led_anim_stop(void) {}

/* ----------------------------------------------------------- the screen --- */
static uint16_t web_fb[240 * 240];       /* RGB565 as the panel gets it (high byte first) */
static uint32_t web_draws;
static void lcd_fill(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint16_t c)
{
    uint32_t i, j;
    uint16_t sw = (uint16_t)((c >> 8) | (c << 8));
    for (j = 0; j < h && y + j < 240u; j++)
        for (i = 0; i < w && x + i < 240u; i++)
            web_fb[(y + j) * 240u + x + i] = sw;
    web_draws++;
}
static void lcd_blit(uint32_t x, uint32_t y, uint32_t w, uint32_t h, const uint16_t *p)
{
    uint32_t i, j;
    for (j = 0; j < h && y + j < 240u; j++)
        for (i = 0; i < w && x + i < 240u; i++)
            web_fb[(y + j) * 240u + x + i] = p[j * w + i];
    web_draws++;
}
static void lcd_sync(void) {}
static void lcd_power(uint32_t s)              /* MENU > SCREEN OFF (lcd.c): off, the page dark; on: redrawn before */
{
    if (!s) {
        memset(web_fb, 0, sizeof web_fb);
        web_draws++;
    }
}
static void lcd_wake_now(void) {}
static void lcd_init(void) {}

#include "../../firmware/src/gfx.c"
#include "../../firmware/src/panel.c"
#include "../../firmware/src/ui.c"
#include "../../firmware/src/menu_items.c"
#include "../../firmware/src/icons.c"
#include "../../firmware/src/ui_graph.c"
#include "../../firmware/src/ui_draw.c"
#include "../../firmware/src/ui_menu.c"
#include "../../firmware/src/ui_input.c"
#include "../../firmware/src/ui_layer.c"

/* ------------------------------------------------------------ the flash --- */
static uint32_t web_flash_writes;
static uint8_t flash_ok;
static int st_read(uint32_t off, void *dst, uint32_t n)
{
    if (off >= sizeof nor || n > sizeof nor - off)
        return -1;
    memcpy(dst, nor + off, n);
    return 0;
}
static int st_erase(uint32_t off)
{
    if (off >= sizeof nor || 4096u > sizeof nor - off)
        return -8;
    memset(nor + (off & ~4095u), 0xFF, 4096);
    web_flash_writes++;
    return 0;
}
static int st_prog(uint32_t off, const void *src, uint32_t n)       /* NOR: a write only clears bits */
{
    const uint8_t *s = src;
    uint32_t i;
    if (off >= sizeof nor || n > sizeof nor - off)
        return -8;
    for (i = 0; i < n; i++)
        nor[off + i] &= s[i];
    web_flash_writes++;
    return 0;
}
static uint32_t irq_save(void) { return 0; }
static void irq_restore(uint32_t f) { (void)f; }
static uint32_t fl_jedec_ram(void) { return 0xEB6014u; }             /* the SMK-37's part (project.c) */
#define FL_JEDEC_OK(id) ((id) == 0xEB6014u || (id) == 0x856014u)     /* as hal/fm1_flash_map.h (SMK) */
static void fl_plain_window_init(void) {}
#define FL_FAR(fn) (fn)
#include "../../firmware/src/storage.c"
#include "../../firmware/src/upreset.c"
#include "../../firmware/src/project.c"

/* ------------------------------------------------- main.c and its HAL --- */
typedef struct { uint32_t magic, count, vec, pc, emu, dbg, rets; } fm1_crash_t;
#define FM1_CRASH_MAGIC 0x43525348u
static fm1_crash_t fm1_crash;                     /* (none: main.c boot_leds runs the sweep) */
static struct { uint8_t p3_rst, wdt_con; uint32_t rst_src; } fm1_boot;
uint32_t _data_start[1], _data_end[1], _data_load[1], _bss_start[1], _bss_end[1];
uint32_t _pool_start[1], _pool_end[1], _rt_start[1], _rt_end[1], _rt_load[1];
static void fm1_adc_init(void) {}
static void fm1_timer5_ack(void) {}
static void fm1_timer5_start(void (*isr)(void), uint32_t prio) { (void)isr; (void)prio; }
void isr_timer5(void) {}
static void fm1_reboot(void) {}
static void fm1_enter_uboot(void) {}
static void fm1_guard_lock_top(void) {}
static void fm1_irq_enable_all(void) {}
static void fm1_irq_init(void) {}
static void fm1_time_init(void) {}
static void fm1_reset_reason(void) {}
static void fm1_wdt_arm(uint32_t v) { (void)v; }
static void fm1_mailbox_clear(void) {}
static void fm1_guard_enable(uint32_t m) { (void)m; }
enum { FM1_GUARD_STACK = 1, FM1_GUARD_WRITE = 2, FM1_GUARD_BUS = 4, FM1_GUARD_PC = 8 };
#if FELUCCA_CDC
static void cdc_task(void) {}
#endif
/* main.c's calls into the USB driver (registers): nothing here (usb.c's own functions stay as they are) */
#define usb_poll() ((void)0)
#define usb_start() ((void)0)
#define usb_retry(ms) ((void)(ms))
#define usb_detach() ((void)0)
#include "../../firmware/src/master.c"
#if GOLANA
#include "../../firmware/src/golana_ui.c"
#endif
#include "../../firmware/src/main.c"
#undef usb_poll
#undef usb_start
#undef usb_retry
#undef usb_detach

/* --------------------------------------------------------- the device --- */
static uint32_t web_booted, web_boot_ms, web_last_frame;
#if GOLANA
static uint32_t web_splash_t0;                 /* main.c: Golana's splash runs from here (web_ms) */
#endif

/* main.c fm1_main's boot, up to the main loop (no USB, no UART, no panel setup) */
static void web_power_on(void)
{
    persist_boot();
    settings_init();
    usb_serial_apply();
    lcd_init();
#if GOLANA
    gu_splash_frame(0);                   /* Golana's animated splash, its first frame (golana_gfx.c) */
#else
    draw_splash();                        /* main.c: the splash (ui_draw.c) */
#endif
    memset(&felucca_dbg, 0, sizeof felucca_dbg);
    felucca_dbg.magic = DBG_MAGIC;
    fm1_input_init();
    fm1_adc_init();
    panel_init();
    felucca_init();
#if GOLANA
    gu_boot();
#endif
    master_boot();                        /* main.c (#137): the pot's level from the first block */
    kb_boot_hold = 1;                     /* main.c (#137): the keys silent under the splash */
    audio_init();
    boot_leds();                          /* main.c: the power-on LED sweep, from the scan's start */
#if !GOLANA
    autosave_boot(boot_clean);            /* main.c: the last session's music (1.2, project.c) */
#endif
#if GOLANA
    gu_splash_start();                    /* main.c: Golana's animated splash, 30 ms on (web_ms runs it) */
    web_splash_t0 = fm1_ms + 30u;
    web_boot_ms = fm1_ms + 30u + GSP_MS;
#else
    web_boot_ms = fm1_ms + 430u;         /* main.c: 30 + 400 ms before the first frame */
#endif
    web_booted = 1;
}

/* one pass of main.c fm1_main's loop (what the browser has: no USB, editor, UBOOT, console) */
static void web_frame(void)
{
    int32_t b = fm1_adc_read(FM1_ADC_BATT);
    if (b > 0)
        song.batt_raw = song.batt_raw ? song.batt_raw + (b - song.batt_raw) / 32 : b;
    master_poll();
    fm1_in.frames++;
    felucca_dbg.ui_frames++;
#if GOLANA
    gu_input();
    settings_poll();
#if BOARD_SMK37
    smk_grid();                                  /* the pads' step grid (ui_input.c smk_leds reads it) */
#endif
    gu_leds();
#if BOARD_SMK37
    /* the SMK's hardware, as in main.c's loop: wheels, faders, TRS OUT switch, pad LED chain */
    smk_faders();
    smk_wheels();
    smk_trs_configure((ui_padprefs & PAD_TRS_OUT) != 0u);
    padled_flush();
#endif
    gu_draw();
#else
    ui_input();
    settings_poll();
    autosave_poll();
#if BOARD_SMK37
    smk_grid();                                  /* (main.c's SMK-37 frame) */
#endif
    ui_leds();
#if BOARD_SMK37
    smk_faders();                                /* the four faders -> LEVEL/PAN/REV/DLY */
    smk_wheels();                                /* pitch and mod: the bends follow the wheels */
    padled_flush();                              /* the pads' colours */
#endif
    ui_draw();
#endif
}

/* output ring, stereo float, filled by web_ms, drained by web_render */
#define RING 4096u
static float ring[RING * 2];
static uint32_t ring_w, ring_r, web_samples_due;

/* one millisecond of the device: the clock, the audio halves due by now, the main loop */
static void web_ms(void)
{
    fm1_ms++;
    web_ticks += 1000u * FM1_TICKS_PER_US;
    web_samples_due += FS;
    while (web_samples_due >= 1000u * HALF_FRAMES) {      /* an I2S half: the audio ISR renders it */
        uint32_t k;
        const int32_t *o = &abuf[web_half * HALF_WORDS];
        web_samples_due -= 1000u * HALF_FRAMES;
        fm1_alnk0_irq();
        for (k = 0; k < HALF_FRAMES; k++) {
            uint32_t i = (ring_w++ % RING) * 2u;
            ring[i] = (float)o[2u * k] / 8388608.0f;
            ring[i + 1] = (float)o[2u * k + 1u] / 8388608.0f;
        }
        web_half ^= 1u;
    }
#if GOLANA
    if (web_booted && web_boot_ms && (int32_t)(fm1_ms - web_splash_t0) >= 0 && gu_splash_step(fm1_ms - web_splash_t0))
        web_boot_ms = fm1_ms;                             /* main.c: its 3 s over, or skipped */
#endif
    if (!web_booted || (int32_t)(fm1_ms - web_boot_ms) < 0)
        return;
    if (web_boot_ms) {                                    /* main.c: the splash goes, the loop starts */
        web_boot_ms = 0;
        kb_boot_hold = 0;
        lcd_fill(0, 0, 240, 240, T_BG);
        web_last_frame = fm1_ms - 15u;
    }
    if (fm1_ms - web_last_frame >= 15u) {                /* main.c: a frame, then ui_input until 15 ms are over */
        web_last_frame = fm1_ms;
        web_frame();
    } else {
#if GOLANA
        gu_input();                                       /* main.c's wait loop */
#else
        ui_input();                                       /* main.c's wait loop */
#endif
    }
}

#define EXPORT __attribute__((used, visibility("default")))
#undef __attribute__

static float out_l[1024], out_r[1024];
EXPORT float *web_out_l(void) { return out_l; }
EXPORT float *web_out_r(void) { return out_r; }
/* n frames (<= 1024) into out_l / out_r: the device runs until they exist */
EXPORT void web_render(uint32_t n)
{
    uint32_t k;
    if (n > 1024u)
        n = 1024u;
    while (ring_w - ring_r < n)
        web_ms();
    for (k = 0; k < n; k++) {
        uint32_t i = (ring_r++ % RING) * 2u;
        out_l[k] = ring[i];
        out_r[k] = ring[i + 1];
    }
}

/* the flash: erased, then the page's saved sectors (web_nor), then boot */
EXPORT uint8_t *web_nor(void) { return nor; }
EXPORT uint32_t web_nor_size(void) { return sizeof nor; }
EXPORT void web_nor_erase(void) { memset(nor, 0xFF, sizeof nor); }
EXPORT uint32_t web_flash_writes_count(void) { return web_flash_writes; }
EXPORT void web_boot(void)
{
    if (!web_booted)
        web_power_on();
}

/* input: buttons by the SMK's own id (0..17, smk_input.h: 0 = KNOB BANK .. 17 = REC), keys by
 * keybed key 0..36 (C3..C6), encoders 0..7 (K1..K8) by role or directly, pads, faders 0..127,
 * wheels raw (as smk_input.h reads them), sustain pedal */
EXPORT void web_buttons(uint32_t mask)                   /* bit i = button i (0..17) */
{
    uint32_t old = fm1_in.buttons;
    mask &= (1u << FM1_NBTN) - 1u;
    fm1_in.pressed |= mask & ~old;
    fm1_in.released |= old & ~mask;
    fm1_in.buttons = mask;
}
EXPORT void web_pedal(uint32_t v) { fm1_in.pedal = (uint16_t)(v > 127u ? 127u : v); }
EXPORT void web_keys(uint32_t mask)                      /* bit k = keybed key k (0 = C3 .. 36 = C6) */
{
    uint32_t k;
    for (k = 0; k < FM1_NKEY; k++)
        if (((mask >> k) & 1u) != (uint32_t)((fm1_in.keys >> k) & 1u))
            smk_key_set(k, (mask >> k) & 1u ? 100u : 0u);   /* a medium strike */
}
EXPORT void web_key(uint32_t k, uint32_t vel)            /* one key, by velocity (0 = up): the browser ABI */
{
    smk_key_set(k, vel);
}
EXPORT void web_pad(uint32_t pad, uint32_t vel)           /* a pad hit (vel 1..127) or a release (0) */
{
    int32_t n0;
    uint32_t n, bit;
    if (pad >= SMK_NPAD)
        return;
    n0 = (int32_t)SMK_WHITE_NOTE[SMK_WHITE_OF_PAD(pad)];
    if (vel) {
        smk_pad_note[pad] = (uint8_t)(n0 + 1);
    } else {
        if (!smk_pad_note[pad])
            return;
        n0 = smk_pad_note[pad] - 1;                      /* the key it pressed, whatever the layout is now */
        smk_pad_note[pad] = 0;
    }
    n = (uint32_t)n0;
    bit = 1u << n;
    if (vel) {
        fm1_in.key_vel[SMK_KEY_F3 + n] = (uint8_t)(vel > 127u ? 127u : vel);
        fm1_in.pnotes |= bit;
        fm1_in.notes_pressed |= bit;
    } else
        fm1_in.pnotes &= ~bit;
    fm1_in.notes = fm1_in.knotes | fm1_in.pnotes;
}
EXPORT void web_enc(uint32_t role, int32_t n)
{
    if (role < NE)
        web_enc_steps[panel.enc[role] & 7u] += n * panel.dir[role];
}
EXPORT void web_enc_raw(uint32_t e, int32_t n) { if (e < 8u) web_enc_steps[e] += n; }   /* K1..K8 */
EXPORT void web_fader(uint32_t i, uint32_t v) { if (i < 4u) fm1_in.fader[i] = (uint8_t)(v > 127u ? 127u : v); }
EXPORT void web_wheel(uint32_t i, uint32_t v) { if (i < 2u) fm1_in.wheel[i] = (uint16_t)(v > 1023u ? 1023u : v); }
EXPORT void web_master(uint32_t v) { smk_master = v > 1023u ? 1023 : (int32_t)v; }
EXPORT uint32_t web_master_pot(void) { return (uint32_t)smk_master; }
/* a channel message from Web MIDI (usb.c midi_enqueue): 0 = the ring was full */
EXPORT uint32_t web_midi(uint32_t status, uint32_t d1, uint32_t d2)
{
    if (status < 0x80u || status >= 0xF0u)
        return 1;
    return (uint32_t)midi_enqueue((status >> 4) | status << 8 | (d1 & 127u) << 16 | (d2 & 127u) << 24, 1);
}
/* the LEDs: bit i = button i lit (smk_input.h: LED i = button i); the pads' colours below */
static uint32_t led_mask(const uint8_t *l)
{
    uint32_t i, m = 0;
    for (i = 0; i < SMK_NLED; i++)
        m |= (l[i] ? 1u : 0u) << i;
    return m;
}
EXPORT uint32_t web_lit_buttons(void) { return led_mask(fm1_led); }
EXPORT uint32_t web_dim_buttons(void) { return led_mask(fm1_led_dim); }
EXPORT uint32_t web_breath_buttons(void) { return led_mask(fm1_led_breath); }
EXPORT uint32_t web_mid_buttons(void) { return led_mask(fm1_led_mid); }
EXPORT uint32_t web_lit_keys(void) { return 0; }         /* the SMK has no per-key LEDs (worklet.js asks) */
EXPORT uint32_t web_dim_keys(void) { return 0; }
EXPORT uint32_t web_breath_keys(void) { return 0; }
EXPORT uint32_t web_dim_level(void) { return web_dim_lo; }
EXPORT uint8_t *web_pad_leds(void) { return (uint8_t *)padled_rgb; }
EXPORT uint32_t web_frames(void) { return fm1_in.frames; }
EXPORT uint32_t web_faders_disabled(void) { return (uint32_t)SMK_FADER_DISABLE; }

EXPORT void web_test_heavy(void)
{
    static const uint8_t ENG[3][2] = {{12, 4}, {ENGI_PHYS, 7}, {8, 0}};
    static const uint8_t CH[4][4] = {{48, 55, 60, 64}, {45, 52, 57, 60}, {41, 48, 53, 57}, {43, 50, 55, 59}};
    uint32_t p, i;
    for (p = 0; p < 3u; p++) {
        track_t *t = &trk[p];
        host_preset(t, ENG[p][0], ENG[p][1]);
        t->p[P_VOICE] = V_POLY;
        t->p[P_SUS] = 127;
        for (i = 0; i < 16u; i++) {
            uint8_t n[4], k;
            for (k = 0; k < 4u; k++)
                n[k] = (uint8_t)(CH[i / 4u][k] + 12u * p);
            put_step(t, i, 4, n, ST_NOTE, 0);
        }
    }
    host_drums(&trk[3]);
    for (i = 0; i < 16u; i++) {
        uint8_t n[4];
        uint32_t k = 0;
        if (i % 4u == 0u)
            n[k++] = 36;
        if (i == 4u || i == 12u)
            n[k++] = 38;
        n[k++] = i % 2u ? 42 : 46;
        put_step(&trk[3], i, k, n, ST_NOTE, 0);
    }
    ui.force = 1;
}

EXPORT uint32_t web_test_user_preset(uint32_t i) { return (uint32_t)up_used(i); }

EXPORT uint16_t *web_screen(void) { return web_fb; }
EXPORT uint32_t web_screen_draws(void) { return web_draws; }
EXPORT uint32_t web_now_ms(void) { return fm1_ms; }
EXPORT uint32_t web_playing(void) { return song.playing; }
EXPORT uint32_t web_sample_rate(void) { return FS; }
