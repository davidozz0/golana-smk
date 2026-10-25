#!/usr/bin/env python3
"""Genera web/emu/felucca_smk_web.c (SMK-37 Pro) da web/emu/felucca_web.c (FM-1).

L'emulatore FM-1 e' la base: stessa macchina (clock, audio, UI, storage), pannello diverso.
Le differenze: BOARD_SMK37, smk_panel_sim.h al posto del pannello FM-1, la flash della SMK
(JEDEC 0xEB6014) e gli export di input (pad, rotelle, fader, tastiera a 37 tasti).
"""
from pathlib import Path

repo = Path(__file__).resolve().parents[2]    # la radice del checkout (questo file sta in web/emu/)
src = repo / "web" / "emu" / "felucca_web.c"
dst = repo / "web" / "emu" / "golana_smk_web.c"
t = src.read_text(encoding="utf-8")

# 1) board
a = "#define FELUCCA_WEB 1\n"
assert t.count(a) == 1, "anchor FELUCCA_WEB"
t = t.replace(a, a + "#define BOARD_SMK37 1                    /* the SMK-37 Pro panel: smk_panel_sim.h below */\n"
                     '#include "fm1_flash_map.h"          /* FL_SMP_*, FL_STOCKTAG_*: storage.c reads them (1.6.1 st_stock_release) */\n')

# 1b) hostsim definisce un fm1_in minimo: il nostro sta in smk_panel_sim.h, incluso prima
#     (seq.c usa i campi della SMK: key_vel, SMK_KEY_F3, FM1_NKEY)
old = '#define main hostsim_main\n#include "../../tests/hostsim.c"\n#undef main\n'
assert t.count(old) == 1, "anchor hostsim"
t = t.replace(old,
              '#include "smk_panel_sim.h"\n'
              '#define FM1_IN_EXTERN 1                  /* the SMK state above replaces hostsim\'s tiny FM-1 one */\n'
              '#define main hostsim_main\n#include "../../tests/hostsim.c"\n#undef main\n')

# 2) pannello: dalla sezione "the panel" alla sezione "the screen"
p0 = "/* ------------------------------------------------------------ the panel --- */"
p1 = "/* ----------------------------------------------------------- the screen --- */"
i0, i1 = t.index(p0), t.index(p1)
t = t[:i0] + '''/* --------------------------------------------------------- the SMK panel --- */
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

''' + t[i1:]

# 3) flash: la parte della SMK
t = t.replace("static uint32_t fl_jedec_ram(void) { return 0x856014u; }             /* the FM-1's part (project.c) */",
              "static uint32_t fl_jedec_ram(void) { return 0xEB6014u; }             /* the SMK-37's part (project.c) */")
t = t.replace("#define FL_JEDEC_OK(id) ((id) == 0x856014u)                          /* as hal/fm1_flash_map.h (FM-1) */",
              "#define FL_JEDEC_OK(id) ((id) == 0xEB6014u || (id) == 0x856014u)     /* as hal/fm1_flash_map.h (SMK) */")
t = t.replace("#define FM1_OCT_BOTH 3u                                              /* OCT- + OCT+, as hal/fm1_input.h (FM-1) */\n", "")

# 4) exports di input e LED
q0 = "/* input, by the printed labels: buttons bit b = B_FX .. B_OCTUP (panel.c), keys bit k = key k (F3 .. G5),"
q1 = "EXPORT void web_test_heavy(void)"
i0, i1 = t.index(q0), t.index(q1)
t = t[:i0] + '''/* input: buttons by the SMK's own id (0..17, smk_input.h: 0 = KNOB BANK .. 17 = REC), keys by
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

''' + t[i1:]

# 4b) l'ADC e il MASTER sono quelli della SMK (smk_panel_sim.h): togli la versione FM-1
t = t.replace("""static uint32_t web_master_adc = 700;   /* MASTER, 0..1023 as the ADC reads it */
enum { FM1_ADC_BATT = 3, FM1_ADC_MASTER = 4 };
static int32_t fm1_adc_read(uint32_t ch) { return ch == FM1_ADC_MASTER ? (int32_t)web_master_adc : 620; }
""", "")

# 4c) il ciclo per-frame dell'emulatore e' quello dell'FM-1: ci mancano le chiamate della SMK-37
#     (main.c righe 370-378: il pad grid, i quattro fader, le rotelle, i LED dei pad)
old = "    ui_leds();\n    ui_draw();\n"
assert t.count(old) == 1, "anchor ui_leds/ui_draw in web_frame"
t = t.replace(old,
              "#if BOARD_SMK37\n"
              "    smk_grid();                                  /* (main.c's SMK-37 frame) */\n"
              "#endif\n"
              "    ui_leds();\n"
              "#if BOARD_SMK37\n"
              "    smk_faders();                                /* the four faders -> LEVEL/PAN/REV/DLY */\n"
              "    smk_wheels();                                /* pitch and mod: the bends follow the wheels */\n"
              "    padled_flush();                              /* the pads' colours */\n"
              "#endif\n"
              "    ui_draw();\n")

# 5) contare i frame (smk_input.h fm1_in.frames: main.c se ne serve)
t = t.replace("    master_poll();\n    felucca_dbg.ui_frames++;",
              "    master_poll();\n    fm1_in.frames++;\n    felucca_dbg.ui_frames++;")

dst.write_text(t, encoding="utf-8")
print(f"scritto {dst} ({len(t.splitlines())} righe)")
