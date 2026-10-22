/* SPDX-License-Identifier: GPL-3.0-only
 * SMK-37 Pro: TRS MIDI OUT on the 3.5 mm jack (UART1 TX, pin PC7).
 *
 * The out stream of the instrument is one funnel (usb.c midi_out_event): the notes
 * the keys and the sequencer play, the wheels' pitch bend and CC1. With the switch
 * on, this file mirrors that stream onto the jack. It also sends MIDI clock (24 ppqn)
 * and Start / Stop while the SMK is the clock master (CLOCK INT), so an external
 * device (e.g. an EP-133) can follow. USB is unchanged.
 *
 * SMK_TRS_TARGET (felucca.c, the real firmware) brings the UART; every host or
 * emulator build (hostsim.c, the browser) gets no-op stubs, so nothing there touches
 * a register. */
#ifdef SMK_TRS_TARGET
#include "../hal/smk_trs_tx.h"

#define TRS_Q 128u
static struct {
    uint32_t event[TRS_Q];
    volatile uint32_t read, write, recover;
    uint8_t bytes[3], length, position, panic;
    uint8_t active[16][16];
    uint16_t release;
} trs_q;
static uint8_t trs_dma[16] __attribute__((aligned(16)));
static uint8_t trs_running, trs_busy, trs_stopping;
static uint32_t trs_started;

/* a USB-MIDI event packet -> the bytes on the wire (0 = not a channel or realtime msg) */
static unsigned trs_decode(uint32_t pkt, uint8_t b[3])
{
    unsigned cin = pkt & 15u, s = (pkt >> 8) & 255u;
    b[0] = (uint8_t)s; b[1] = (uint8_t)(pkt >> 16); b[2] = (uint8_t)(pkt >> 24);
    if (cin >= 8u && cin <= 14u && (s >> 4) == cin && b[1] < 128u && ((cin == 12u || cin == 13u) || b[2] < 128u))
        return cin == 12u || cin == 13u ? 2u : 3u;
    if (cin == 15u && (s == 0xf8u || s == 0xfau || s == 0xfbu || s == 0xfcu || s == 0xfeu || s == 0xffu)) return 1u;
    if (cin == 3u && s == 0xf2u && b[1] < 128u && b[2] < 128u) return 3u;
    if (cin == 2u && (s == 0xf1u || s == 0xf3u) && b[1] < 128u) return 2u;
    if (cin == 5u && s == 0xf6u) return 1u;
    return 0u;                                   /* editor / update SysEx is never bridged */
}
static int trs_push(uint32_t pkt)
{
    uint8_t b[3];
    if (!trs_decode(pkt, b)) return 0;
    if (trs_q.recover || trs_q.write - trs_q.read >= TRS_Q) { trs_q.recover = 1u; return -1; }
    trs_q.event[trs_q.write % TRS_Q] = pkt;
    __asm__ volatile("" ::: "memory");
    trs_q.write++;
    return 1;
}
static int trs_next(uint8_t *byte)
{
    if (trs_q.position == trs_q.length) {
        trs_q.position = trs_q.length = 0;
        if (trs_q.recover) {
            trs_q.read = trs_q.write;
            if (trs_q.panic < 48u) {
                static const uint8_t cm[3] = {64u, 120u, 123u};
                trs_q.bytes[0] = (uint8_t)(0xb0u + trs_q.panic / 3u);
                trs_q.bytes[1] = cm[trs_q.panic % 3u];
                trs_q.bytes[2] = 0;
                trs_q.length = 3u; trs_q.panic++;
            } else {
                while (trs_q.release < 2048u) {  /* release each note actually transmitted */
                    unsigned key = trs_q.release++, ch = key / 128u, note = key % 128u;
                    if (!(trs_q.active[ch][note / 8u] & (1u << (note % 8u)))) continue;
                    trs_q.bytes[0] = (uint8_t)(0x80u + ch); trs_q.bytes[1] = (uint8_t)note; trs_q.bytes[2] = 0;
                    trs_q.length = 3u; break;
                }
                if (!trs_q.length) { trs_q.panic = 0; trs_q.release = 0; trs_q.recover = 0; return 0; }
            }
        } else {
            uint32_t p;
            if (trs_q.read == trs_q.write) return 0;
            p = trs_q.event[trs_q.read % TRS_Q];
            trs_q.read++;
            trs_q.length = (uint8_t)trs_decode(p, trs_q.bytes);
            if (!trs_q.length) return 0;
        }
    }
    *byte = trs_q.bytes[trs_q.position++];
    if (trs_q.position == trs_q.length && trs_q.length == 3u) {
        unsigned type = trs_q.bytes[0] >> 4, ch = trs_q.bytes[0] & 15u, note = trs_q.bytes[1];
        if (type == 8u || type == 9u) {
            uint8_t bit = (uint8_t)(1u << (note % 8u));
            if (type == 9u && trs_q.bytes[2]) trs_q.active[ch][note / 8u] |= bit;
            else trs_q.active[ch][note / 8u] &= (uint8_t)~bit;
        }
    }
    return 1;
}

/* producer: the audio ISR (usb.c midi_out_event) */
static void smk_trs_output(uint32_t pkt)
{
    if (trs_running && !trs_stopping) (void)trs_push(pkt);
}
/* main loop: follow the MENU > MIDI > TRS OUT switch */
static void smk_trs_configure(int enabled)
{
    if (enabled && !trs_running && !trs_stopping) {
        memset(&trs_q, 0, sizeof trs_q);
        trs_busy = 0;
        fm1_irq_off();
        smk_trs_uart_start();
        fm1_irq_on();
        trs_running = 1;
    } else if (!enabled && trs_running && !trs_stopping) {
        trs_stopping = 1;
        trs_q.recover = 1u;                      /* drain a channel panic before the UART stops */
    }
}
/* 2 kHz (main.c TIMER5, beside usb_poll): drain the queue to the UART */
static void smk_trs_poll(void)
{
    uint32_t n = 0;
    if (!trs_running) return;
    if (trs_busy) {
        if (!smk_trs_uart_done()) {
            if (fm1_ticks() - trs_started > 24000000u) { smk_trs_uart_stop(); trs_running = trs_stopping = 0; }
            return;
        }
        trs_busy = 0;
    }
    while (n < sizeof trs_dma && trs_next(&trs_dma[n])) n++;
    if (n) { trs_started = fm1_ticks(); trs_busy = 1; smk_trs_uart_send(trs_dma, n); }
    else if (trs_stopping && !trs_q.recover) { smk_trs_uart_stop(); trs_running = 0; trs_stopping = 0; }
}
/* clock (24 ppqn) and Start / Stop, internal clock master only (seq.c) */
static uint32_t trs_clk_acc;
static void smk_trs_transport(uint32_t status)
{
    if (!trs_running || song.g[G_CLOCK]) return;
    trs_clk_acc = 0;
    smk_trs_output(0x0Fu | (status << 8));       /* CIN 0x0F, single realtime byte */
}
static void smk_trs_clock_block(uint32_t n)
{
    uint32_t per;
    if (!trs_running || song.g[G_CLOCK] || !song.playing) { trs_clk_acc = 0; return; }
    per = beat_samples() / 24u;
    if (!per) return;
    trs_clk_acc += n;
    while (trs_clk_acc >= per) { trs_clk_acc -= per; smk_trs_output(0x0Fu | (0xF8u << 8)); }
}
#else
/* host / browser: the jack is not emulated; the out stream and the switch remain */
static void smk_trs_output(uint32_t pkt) { (void)pkt; }
static void smk_trs_configure(int enabled) { (void)enabled; }
static void smk_trs_poll(void) {}
static void smk_trs_transport(uint32_t status) { (void)status; }
static void smk_trs_clock_block(uint32_t n) { (void)n; }
#endif
