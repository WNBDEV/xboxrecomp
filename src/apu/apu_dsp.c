/*
 * MCPX APU DSP (GP/EP) - Stub implementation
 *
 * The DSP Global Processor (GP) and Encode Processor (EP) handle effects
 * processing (reverb, chorus, etc.) and final output encoding. The full
 * DSP is ~3000 lines of DSP56300 emulation code.
 *
 * For initial audio, we bypass the DSP entirely:
 * - VP mixbins are passed directly to the EP output
 * - GP effects processing is skipped
 * - The EP just copies mixbin 0/1 (front L/R) to the monitor buffer
 *
 * This gives us basic voice playback without effects. The DSP can be
 * connected later for reverb, EQ, and other processing.
 *
 * Copyright (c) 2012 espes
 * Copyright (c) 2019-2025 Matt Borgerson
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 */

#include "apu_state.h"
#include "fpconv.h"

#include <stdlib.h>
#include <string.h>

/* ── DSP command doorbell acknowledgement ────────────────────────────────
 *
 * DirectSound does not stop at creating the device. It hands the audio DSP a
 * command block in guest RAM, writes a command word, and spins until the DSP
 * writes zero back. On real hardware the GP runs a DSP56300 program that does
 * that. Here the DSP is a passthrough stub, so the word never changes and the
 * title hangs inside DirectSound initialisation -- which on Wreckless gates the
 * entire engine, not just audio.
 *
 * RECOMP_APU_DSP_ACK=<addr>[,<addr>...] clears those guest dwords once per APU
 * frame, which is what "the command completed" looks like to the title.
 *
 * ponytail: this is a handshake acknowledgement, not a DSP. It says every
 * command succeeded instantly and computes nothing, so anything whose *result*
 * the title reads back will still be wrong. The real fix is DSP56300 emulation
 * in the GP/EP; this exists so audio init stops blocking everything behind it.
 *
 * The address is not derivable from the APU registers: GPSADDR/GPFADDR/
 * EPSADDR/EPFADDR point at the DSP's own scratch and frame memory, while the
 * command block is a DirectSound heap allocation. On Wreckless the registers
 * read 0x01504000 / 0x014EC000 / 0x0151C000 / 0x014F0000 and the doorbell is at
 * 0x014F8810 -- inside none of them. So it has to be observed: run with
 * RECOMP_WATCHDOG_SECS and the spin shows up as ebx plus the poll offset.
 */
#define APU_DSP_ACK_MAX 8
static uint32_t s_dsp_ack[APU_DSP_ACK_MAX];
static int s_dsp_ack_count = -1;

static void dsp_ack_init(void)
{
    const char *spec = getenv("RECOMP_APU_DSP_ACK");
    char buf[256], *p, *end;

    s_dsp_ack_count = 0;
    if (!spec || !*spec)
        return;
    strncpy(buf, spec, sizeof buf - 1);
    buf[sizeof buf - 1] = 0;
    for (p = buf; *p && s_dsp_ack_count < APU_DSP_ACK_MAX; ) {
        unsigned long v = strtoul(p, &end, 0);
        if (end == p)
            break;
        if (v)
            s_dsp_ack[s_dsp_ack_count++] = (uint32_t)v;
        p = (*end == ',') ? end + 1 : end;
    }
    if (s_dsp_ack_count)
        fprintf(stderr, "[APU] DSP doorbell ack: %d address(es), first 0x%08X\n",
                s_dsp_ack_count, s_dsp_ack[0]);
}

/* How the 32 mixbins become two channels; RECOMP_APU_MIXDOWN_ALL picks.
 *
 *   unset   the speaker bins, folded down to stereo (mixdown_speakers)
 *   1       every bin, even ones left and odd ones right -- the previous
 *           default, kept for titles that route audio somewhere unexpected
 *   0       bins 0 and 1 only, the original stub */
enum { MIXDOWN_FRONT, MIXDOWN_ALL, MIXDOWN_SPEAKERS };

static int mcpx_apu_mixdown_mode(void)
{
    static int mode = -1;
    if (mode < 0) {
        const char *e = getenv("RECOMP_APU_MIXDOWN_ALL");
        mode = (e && *e) ? (atoi(e) ? MIXDOWN_ALL : MIXDOWN_FRONT)
                         : MIXDOWN_SPEAKERS;
    }
    return mode;
}

/* DirectSound's bin assignment (DSMIXBIN_*): 0-5 front L/R, centre, LFE,
 * back L/R; 6-9 the crosstalk-cancelled front and back pairs 3D voices
 * render into; 10 the I3DL2 reverb send; 11 and up the effect sends. Bins
 * 10 and up are inputs to effects the GP would run and output to the
 * speakers. With no GP they are dry copies of voices already in 0-9, and
 * summing them -- reverb send into the left only, every odd send into the
 * right -- doubled sounds, unbalanced them and comb-filtered the ones whose
 * copies differ by a filter.
 *
 * So: front, back and crosstalk pairs to their own side, centre at -3 dB to
 * both, and LFE low-passed (a subwoofer feed may be full range) into both. */
static void mixdown_speakers(float mixbins[NUM_MIXBINS][NUM_SAMPLES_PER_FRAME],
                             int i, float *left, float *right)
{
    static float lfe1, lfe2;
    const float k = 0.0156f;            /* one-pole at ~120 Hz, 48 kHz */
    float c = 0.7071f * mixbins[2][i];

    lfe1 += k * (mixbins[3][i] - lfe1);
    lfe2 += k * (lfe1 - lfe2);
    *left = mixbins[0][i] + mixbins[4][i] + mixbins[6][i] + mixbins[8][i]
          + c + 0.7071f * lfe2;
    *right = mixbins[1][i] + mixbins[5][i] + mixbins[7][i] + mixbins[9][i]
           + c + 0.7071f * lfe2;
}

/* The GP's command word, found the way the GP program finds it.
 *
 * DirectSound posts a command at byte 0x810 of the GP's scratch memory and
 * spins until it reads zero (the loop is in the RECOMP_DSP_ACK note in
 * xbox_memory_layout.c). The scratch memory is the pages listed in the
 * scatter-gather table GPSADDR points at -- the table, not the memory, which
 * is why comparing the doorbell with GPSADDR itself found nothing -- so the
 * word is the table's first page + 0x810, and no address has to be observed
 * and entered by hand. Completing every command instantly is still all this
 * stub does; no DSP program runs. RECOMP_APU_DSP_AUTOACK=0 turns it off. */
#define GP_COMMAND_WORD 0x810u

static void dsp_autoack(MCPXAPUState *d)
{
    static int on = -1;
    static uint32_t last_page;
    static unsigned shown;
    uint32_t table, page, *w;

    if (on < 0) {
        const char *e = getenv("RECOMP_APU_DSP_AUTOACK");
        on = !(e && *e == '0');
    }
    table = d->regs[NV_PAPU_GPSADDR];
    if (!on || !table)
        return;
    page = *(const uint32_t *)mcpx_apu_phys(table) & ~0xFFFu;   /* entry 0 */
    if (!page)
        return;
    if (page != last_page) {
        last_page = page;
        fprintf(stderr, "[APU] GP scratch page 0 at 0x%08X: command word 0x%08X\n",
                page, page + GP_COMMAND_WORD);
    }
    w = (uint32_t *)mcpx_apu_phys(page + GP_COMMAND_WORD);
    if (*w) {
        if (shown++ < 3)
            fprintf(stderr, "[APU] GP command 0x%08X completed\n", *w);
        *w = 0;
    }
}

void mcpx_apu_dsp_ack_poll(MCPXAPUState *d)
{
    int i;

    if (s_dsp_ack_count < 0)
        dsp_ack_init();
    if (!d->ram_ptr)
        return;
    dsp_autoack(d);
    for (i = 0; i < s_dsp_ack_count; i++) {
        uint32_t *slot = (uint32_t *)(d->ram_ptr + s_dsp_ack[i]);
        if (*slot) {
            static int shown[APU_DSP_ACK_MAX];
            if (shown[i]++ < 3)
                fprintf(stderr, "[APU] DSP doorbell 0x%08X: command 0x%08X"
                                " acknowledged\n", s_dsp_ack[i], *slot);
            *slot = 0;
        }
    }
}

void mcpx_apu_dsp_init(MCPXAPUState *d)
{
    /* Allocate minimal DSP state for GP and EP.
     * We need these to exist so reset doesn't crash,
     * but they won't actually run DSP programs. */
    d->gp.dsp = (DSPState *)calloc(1, sizeof(DSPState));
    d->ep.dsp = (DSPState *)calloc(1, sizeof(DSPState));

    if (d->gp.dsp) d->gp.dsp->is_gp = true;
    if (d->ep.dsp) d->ep.dsp->is_gp = false;

    d->gp.realtime = false;
    d->ep.realtime = false;

    fprintf(stderr, "[APU] DSP GP/EP initialized (STUBBED - passthrough mode)\n");
}

void mcpx_apu_update_dsp_preference(MCPXAPUState *d)
{
    /* In the real xemu, this reads settings to decide whether
     * GP/EP should run in realtime or cached mode. We ignore it. */
    (void)d;
}

/* The final mix before it is limited: its peak and how many samples went over
 * full scale, for the log. */
float g_apu_mix_peak;
unsigned g_apu_mix_clipped, g_apu_mix_samples;

/* A sample over full scale used to be clamped, which squares off the wave --
 * the harsh crackle of a clipped mix. Now the whole mix is turned down at
 * once, just enough to fit, and comes back up over ~50 ms. Below full scale
 * the gain is exactly 1 and the output is what it was. */
static float s_limit_gain = 1.0f;
#define LIMIT_RELEASE (1.0f / (0.05f * 48000.0f))

void mcpx_apu_dsp_frame(MCPXAPUState *d,
                         float mixbins[NUM_MIXBINS][NUM_SAMPLES_PER_FRAME])
{
    /* Bypass DSP: take mixbin 0 (front-left) and mixbin 1 (front-right)
     * and write them directly to the monitor frame buffer as the final
     * EP output.
     *
     * The Xbox DirectSound typically routes:
     *   Mixbin 0 = Front Left
     *   Mixbin 1 = Front Right
     *   Mixbin 2 = Center (often unused in stereo)
     *   Mixbin 3 = LFE
     *   Mixbin 4-5 = Rear L/R
     *
     * For stereo output, bins 0 and 1 are what we want.
     */

    int off = (d->ep_frame_div % 8) * NUM_SAMPLES_PER_FRAME;

    if (d->monitor.point != MCPX_APU_DEBUG_MON_VP) {
        for (int i = 0; i < NUM_SAMPLES_PER_FRAME; i++) {
            /* Bins 2..31 used to be computed and then dropped on the floor.
             * On hardware the GP and EP mix the submixes down; here they are
             * stubs, so thirty of thirty-two bins were discarded every frame
             * with no counter anywhere to say so.
             *
             * Measured on Jet Set Radio Future, one 200 s gameplay run, with
             * a positive control moving beside it:
             *
             *     [APU-BIN] 2D heard=557466 lost=0
             *               3D heard=0      lost=377768
             *               lost by bin: 6,7,8,9,10
             *
             * 557,466 music voice-frames heard and none lost; 377,768 effect
             * voice-frames produced correctly and thrown away. The title's 3D
             * positional voices -- its sound effects -- are routed to bins 6
             * to 10 by the guest's own V0BIN..V3BIN, and music on 2D voices
             * lands in bins 0 and 1, which is why the music was always
             * audible and no effect ever was. Every instrument upstream of
             * this line read healthy.
             *
             * Gating the HRTF submix override was tried first and did not fix
             * it, so the defect is the width of this mixdown and nothing else.
             *
             * The first fix summed every bin, even left and odd right. That
             * kept JSRF's effects but also mixed in the reverb and effect
             * sends raw; the default is now a fold-down of the speaker bins
             * (mixdown_speakers), which still includes 6 to 9. */
            float left, right;
            int mode = mcpx_apu_mixdown_mode();
            if (mode == MIXDOWN_SPEAKERS) {
                mixdown_speakers(mixbins, i, &left, &right);
            } else if (mode == MIXDOWN_ALL) {
                left = 0.0f;
                right = 0.0f;
                for (int b = 0; b < NUM_MIXBINS; ++b) {
                    if (b & 1) right += mixbins[b][i];
                    else       left  += mixbins[b][i];
                }
            } else {
                left = mixbins[0][i];
                right = mixbins[1][i];
            }
            {
                float m = fmaxf(fabsf(left), fabsf(right));
                float fit = m > 1.0f ? 1.0f / m : 1.0f;
                if (m > g_apu_mix_peak)
                    g_apu_mix_peak = m;
                g_apu_mix_clipped += m > 1.0f;
                g_apu_mix_samples++;
                if (fit < s_limit_gain)
                    s_limit_gain = fit;
                else if (s_limit_gain < 1.0f)
                    s_limit_gain = fminf(1.0f, s_limit_gain + LIMIT_RELEASE);
                if (s_limit_gain < 1.0f) {
                    left *= s_limit_gain;
                    right *= s_limit_gain;
                }
            }
            /* Clamp to [-1, 1] range */
            if (left > 1.0f) left = 1.0f;
            if (left < -1.0f) left = -1.0f;
            if (right > 1.0f) right = 1.0f;
            if (right < -1.0f) right = -1.0f;

            /* Convert to 16-bit and write (not accumulate) into frame buffer.
             * Each of the 8 sub-frames writes its own 32-sample slice. */
            d->monitor.frame_buf[off + i][0] = (int16_t)(left * 32767.0f);
            d->monitor.frame_buf[off + i][1] = (int16_t)(right * 32767.0f);
        }
    }

    g_dbg.gp.cycles = 0;
    g_dbg.ep.cycles = 0;
}
