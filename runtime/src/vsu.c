/* vsu.c — Virtual Sound Unit.
 *
 * Port of beetle-vb/mednafen/vb/vsu.c (lines 26-470). The Beetle code
 * uses Blip_Synth for band-limited output; we use direct snapshot
 * sampling into a stereo ring buffer, which is plenty for chip audio
 * and avoids pulling in the Blip_Buffer dependency.
 *
 * Address map (from Beetle VSU_Write, line 135):
 *   0x01000000 + 0x000..0x27F : WaveData[0..4][0..0x1F]  (5 wave tables,
 *                               6 bits per sample, stride 4 between
 *                               samples — only every 4th byte stores)
 *   0x01000000 + 0x280..0x3FF : ModData[0..0x1F]         (mod table)
 *   0x01000000 + 0x400..0x5FF : per-channel control regs (6 ch * 0x40)
 *     ch_base + 0x00 : IntlControl  (start/dur/play-on-write)
 *     ch_base + 0x04 : Stereo level (LeftLevel<<4 | RightLevel)
 *     ch_base + 0x08 : Frequency lo (FREQ[7:0])
 *     ch_base + 0x0C : Frequency hi (FREQ[10:8] in low 3 bits)
 *     ch_base + 0x10 : EnvControl  lo (initial vol + grow/decay + speed)
 *     ch_base + 0x14 : EnvControl  hi (enable + reload + ch5 LFSR tap)
 *     ch_base + 0x18 : RAM-address (which WaveData[] to use)
 *     ch_base + 0x1C : SweepControl (ch4 only)
 *   0x01000000 + 0x580        : Sound-disable register (bit 0 -> mute all)
 */
#include "vsu.h"

#include <stdbool.h>
#include <string.h>

#include "stub_abort.h"
#include "vsu_shadow.h"

/* ------------------------- Register state ------------------------- */

static uint8_t  s_intl_control[6];
static uint8_t  s_left_level[6];
static uint8_t  s_right_level[6];
static uint16_t s_frequency[6];
static uint16_t s_env_control[6];   /* extra bits for ch4/ch5 fold in here */
static uint8_t  s_ram_address[6];
static uint8_t  s_sweep_control;

static uint8_t  s_wave_data[5][0x20];
static uint8_t  s_mod_data[0x20];

/* ------------------------ Step-machine state ---------------------- */

static int32_t  s_eff_freq[6];
static int32_t  s_envelope[6];

static int32_t  s_wave_pos[6];
static int32_t  s_mod_wave_pos;

static int32_t  s_latcher_clock_divider[6];
static int32_t  s_freq_counter[6];
static int32_t  s_interval_counter[6];
static int32_t  s_envelope_counter[6];
static int32_t  s_sweep_mod_counter;

static int32_t  s_effects_clock_divider[6];
static int32_t  s_interval_clock_divider[6];
static int32_t  s_envelope_clock_divider[6];
static int32_t  s_sweep_mod_clock_divider;

static int32_t  s_noise_latcher_clock_divider;
static uint32_t s_noise_latcher;
static uint32_t s_lfsr;

/* Channel 5 (noise) LFSR-tap selection table. Beetle vsu.c:75. */
static const unsigned s_tap_lut[8] = {
    15 - 1, 11 - 1, 14 - 1, 5 - 1, 9 - 1, 7 - 1, 10 - 1, 12 - 1
};

/* ------------------------ Sample-output ring --------------------- */
/* Power-of-two stereo output ring. Sized at ~95 s @ 44.1 kHz
 * (= 4 194 304 frames, 16 MiB). The SDL path only ever needs ~1.5 s,
 * but in a `--headless` accuracy run the dispatch loop free-runs at
 * ~30x realtime with no consumer, so the ring must be large enough that
 * a from-boot capture window [0, N) survives until the accuracy harness
 * drains it via vb_vsu_read_abs (see tools/audio_compare.py). */
#define VSU_RING_FRAMES (1u << 22)
#define VSU_RING_MASK   (VSU_RING_FRAMES - 1)
static int16_t  s_ring_l[VSU_RING_FRAMES];
static int16_t  s_ring_r[VSU_RING_FRAMES];
static size_t   s_ring_head;   /* next slot the producer will fill */
static size_t   s_ring_tail;   /* next slot the consumer will read */
/* Monotonic count of frames ever emitted. s_ring_head == s_ring_total &
 * VSU_RING_MASK always holds (both step by one per emit, neither resets
 * except in vb_vsu_init), so absolute frame index i lives in slot
 * (i & VSU_RING_MASK) until it is overwritten at write (i + capacity).
 * The accuracy capture tap reads by this absolute index, decoupled from
 * the SDL drain cursor s_ring_tail. */
static uint64_t s_ring_total;

/* Cycle accumulator -> sample-rate downsample. */
static int32_t  s_sample_cycle_residue;

/* The VB VSU is clocked at CPU/4 (5 MHz), NOT the full 20 MHz CPU clock.
 * Mednafen feeds its VSU `(v810_timestamp + CycleFix) >> 2` and runs the
 * Blip buffer at VB_MASTER_CLOCK/4 (beetle-vb/libretro.cpp:1944,2346).
 * Our vsu_step_channel is a verbatim port of Mednafen's 5 MHz-domain
 * update loop (FreqCounter reload = 2048-EffFreq, dividers 4800/4/4/120),
 * so it MUST be fed 5 MHz cycles. This accumulator carries the sub-4 CPU
 * cycle remainder across ticks so no VSU clocks are lost (the CycleFix
 * equivalent). Feeding 20 MHz here clocked every channel 4x too fast =
 * pitch ~2 octaves sharp. */
static int32_t  s_vsu_clock_residue;

void vb_vsu_init(void) {
    memset(s_intl_control, 0, sizeof(s_intl_control));
    memset(s_left_level, 0, sizeof(s_left_level));
    memset(s_right_level, 0, sizeof(s_right_level));
    memset(s_frequency, 0, sizeof(s_frequency));
    memset(s_env_control, 0, sizeof(s_env_control));
    memset(s_ram_address, 0, sizeof(s_ram_address));
    s_sweep_control = 0;

    memset(s_wave_data, 0, sizeof(s_wave_data));
    memset(s_mod_data, 0, sizeof(s_mod_data));

    for (int ch = 0; ch < 6; ++ch) {
        s_eff_freq[ch] = 0;
        s_envelope[ch] = 0;
        s_wave_pos[ch] = 0;
        s_latcher_clock_divider[ch] = 120;
        s_freq_counter[ch] = 0;
        s_interval_counter[ch] = 0;
        s_envelope_counter[ch] = 0;
        s_effects_clock_divider[ch] = 4800;
        s_interval_clock_divider[ch] = 4;
        s_envelope_clock_divider[ch] = 4;
    }
    s_mod_wave_pos = 0;
    s_sweep_mod_counter = 0;
    s_sweep_mod_clock_divider = 1;
    s_noise_latcher_clock_divider = 120;
    s_noise_latcher = 0;
    s_lfsr = 0;

    s_ring_head = s_ring_tail = 0;
    s_ring_total = 0;
    s_sample_cycle_residue = 0;
    s_vsu_clock_residue = 0;

    vb_vsu_shadow_reset();
}

void vb_vsu_shutdown(void) {}

/* ------------------------- Register writes ----------------------- */

static void vsu_write_reg(uint32_t a, uint8_t v) {
    /* a is the sub-offset within the VSU page, masked to 0x7FF.
     * Beetle reads even-strided byte writes only (A & 3 == 0). */
    if (a & 3u) return;
    a &= 0x7FFu;

    if (a < 0x280u) {
        s_wave_data[a >> 7][(a >> 2) & 0x1Fu] = v & 0x3Fu;
        return;
    }
    if (a < 0x400u) {
        s_mod_data[(a >> 2) & 0x1Fu] = v;
        return;
    }
    if (a >= 0x600u) return;

    int ch = (a >> 6) & 0xFu;
    if (ch > 5) {
        if (a == 0x580u && (v & 1u)) {
            /* Sound-disable: clear "playing" bit on every channel. */
            for (int i = 0; i < 6; ++i)
                s_intl_control[i] &= (uint8_t)~0x80u;
        }
        return;
    }

    switch ((a >> 2) & 0xFu) {
        case 0x0: {
            s_intl_control[ch] = (uint8_t)(v & ~0x40u);
            if (v & 0x80u) {
                s_eff_freq[ch] = s_frequency[ch];
                s_freq_counter[ch] = (ch == 5)
                    ? 10 * (2048 - s_eff_freq[ch])
                    :       2048 - s_eff_freq[ch];
                s_interval_counter[ch] = (v & 0x1Fu) + 1;
                s_envelope_counter[ch] = (s_env_control[ch] & 0x7u) + 1;
                if (ch == 4) {
                    s_sweep_mod_counter = (s_sweep_control >> 4) & 7u;
                    s_sweep_mod_clock_divider =
                        (s_sweep_control & 0x80u) ? 8 : 1;
                    s_mod_wave_pos = 0;
                }
                s_wave_pos[ch] = 0;
                if (ch == 5) s_lfsr = 1;
                s_effects_clock_divider[ch] = 4800;
                s_interval_clock_divider[ch] = 4;
                s_envelope_clock_divider[ch] = 4;
            }
            break;
        }
        case 0x1:
            s_left_level[ch]  = (v >> 4) & 0xFu;
            s_right_level[ch] =  v       & 0xFu;
            break;
        case 0x2:
            s_frequency[ch] = (uint16_t)((s_frequency[ch] & 0xFF00u) | v);
            s_eff_freq[ch]  = (s_eff_freq[ch]  & 0xFF00) | v;
            break;
        case 0x3:
            s_frequency[ch] = (uint16_t)((s_frequency[ch] & 0x00FFu)
                                         | ((v & 0x7u) << 8));
            s_eff_freq[ch]  = (s_eff_freq[ch]  & 0x00FF)
                              | ((v & 0x7) << 8);
            break;
        case 0x4:
            s_env_control[ch] = (uint16_t)((s_env_control[ch] & 0xFF00u) | v);
            s_envelope[ch]    = (v >> 4) & 0xFu;
            break;
        case 0x5: {
            uint16_t hi;
            if (ch == 4)      hi = (uint16_t)((v & 0x73u) << 8);
            else if (ch == 5) { hi = (uint16_t)((v & 0x73u) << 8); s_lfsr = 1; }
            else              hi = (uint16_t)((v & 0x03u) << 8);
            s_env_control[ch] = (uint16_t)((s_env_control[ch] & 0x00FFu) | hi);
            break;
        }
        case 0x6:
            s_ram_address[ch] = v & 0xFu;
            break;
        case 0x7:
            if (ch == 4) s_sweep_control = v;
            break;
        default: break;
    }
}

/* ------------------------- Bus interface ------------------------- */

static int in_vsu_page(uint32_t phys) {
    /* VSU occupies the first 0x800 of region 1 (0x01000000-0x010007FF
     * canonically). Beetle ignores writes outside that range. */
    return phys >= 0x01000000u && phys < 0x01000800u;
}

uint8_t vb_vsu_read8(uint32_t addr) {
    /* VSU registers are write-only on real hardware; reads from the
     * register page return 0 (per Beetle, no read-back path). */
    if (in_vsu_page(addr)) return 0;
    vb_stub_abort("VSU read8 outside register window", 0, addr);
}

uint16_t vb_vsu_read16(uint32_t addr) {
    if (in_vsu_page(addr)) return 0;
    vb_stub_abort("VSU read16 outside register window", 0, addr);
}

uint32_t vb_vsu_read32(uint32_t addr) {
    if (in_vsu_page(addr)) return 0;
    vb_stub_abort("VSU read32 outside register window", 0, addr);
}

void vb_vsu_write8(uint32_t addr, uint8_t v) {
    if (in_vsu_page(addr)) {
        vsu_write_reg(addr - 0x01000000u, v);
        return;
    }
    vb_stub_abort("VSU write8 outside register window", 0, addr);
}

void vb_vsu_write16(uint32_t addr, uint16_t v) {
    /* Cart-side 16/32-bit writes hit even-byte register slots. The
     * Beetle reg map only stores low-byte content; we drop the high
     * byte (Beetle does the same — see VSU_Write checking A&3 == 0
     * and otherwise discarding). */
    vb_vsu_write8(addr, (uint8_t)(v & 0xFFu));
}

void vb_vsu_write32(uint32_t addr, uint32_t v) {
    vb_vsu_write8(addr, (uint8_t)(v & 0xFFu));
}

/* -------------------- Per-channel synthesis core ---------------- */

/* Compute current (left,right) sample contribution for one channel,
 * in the same 0..63 * (envelope*level/8 + 1) scale Beetle emits to
 * Blip_Synth. We bias the waveform by -0x20 so silence is 0 (Beetle
 * leaves DC and lets Blip_Synth high-pass it; for direct emission
 * we need a centered signal). */
static inline void vsu_channel_output(int ch, int* left, int* right) {
    if (!(s_intl_control[ch] & 0x80u)) {
        *left = *right = 0;
        return;
    }
    int wd;
    if (ch == 5) {
        wd = (int)s_noise_latcher;     /* 0 or 63 */
    } else {
        if (s_ram_address[ch] > 4) wd = 0;
        else wd = s_wave_data[s_ram_address[ch]][s_wave_pos[ch]];
    }
    /* Center the 6-bit unsigned waveform around zero. */
    wd -= 0x20;

    int l = s_envelope[ch] * s_left_level[ch];
    if (l) { l >>= 3; l += 1; }
    int r = s_envelope[ch] * s_right_level[ch];
    if (r) { r >>= 3; r += 1; }
    *left  = wd * l;
    *right = wd * r;
}

/* Advance one channel by `clocks` CPU cycles. Mirrors the inner
 * while-loop of Beetle's VSU_Update (vsu.c:294-465) verbatim,
 * minus the Blip_Synth_offset calls. */
static void vsu_step_channel(int ch, int32_t clocks) {
    if (!(s_intl_control[ch] & 0x80u)) return;

    while (clocks > 0) {
        int32_t chunk = clocks;
        if (chunk > s_effects_clock_divider[ch])
            chunk = s_effects_clock_divider[ch];
        if (ch == 5) {
            if (chunk > s_noise_latcher_clock_divider)
                chunk = s_noise_latcher_clock_divider;
        } else if (s_eff_freq[ch] >= 2040) {
            if (chunk > s_latcher_clock_divider[ch])
                chunk = s_latcher_clock_divider[ch];
        } else {
            if (chunk > s_freq_counter[ch])
                chunk = s_freq_counter[ch];
        }
        if (chunk <= 0) chunk = 1;

        s_freq_counter[ch] -= chunk;
        while (s_freq_counter[ch] <= 0) {
            if (ch == 5) {
                int fb = ((s_lfsr >> 7) & 1)
                       ^ ((s_lfsr >> s_tap_lut[(s_env_control[5] >> 12) & 7]) & 1)
                       ^ 1;
                s_lfsr = ((s_lfsr << 1) & 0x7FFFu) | (uint32_t)fb;
                s_freq_counter[ch] += 10 * (2048 - s_eff_freq[ch]);
            } else {
                s_freq_counter[ch] += 2048 - s_eff_freq[ch];
                s_wave_pos[ch] = (s_wave_pos[ch] + 1) & 0x1F;
            }
        }
        s_latcher_clock_divider[ch] -= chunk;
        while (s_latcher_clock_divider[ch] <= 0)
            s_latcher_clock_divider[ch] += 120;

        if (ch == 5) {
            s_noise_latcher_clock_divider -= chunk;
            if (s_noise_latcher_clock_divider <= 0) {
                s_noise_latcher_clock_divider = 120;
                s_noise_latcher = ((s_lfsr & 1u) << 6) - (s_lfsr & 1u);
            }
        }

        s_effects_clock_divider[ch] -= chunk;
        while (s_effects_clock_divider[ch] <= 0) {
            s_effects_clock_divider[ch] += 4800;
            s_interval_clock_divider[ch]--;
            while (s_interval_clock_divider[ch] <= 0) {
                s_interval_clock_divider[ch] += 4;
                if (s_intl_control[ch] & 0x20u) {
                    s_interval_counter[ch]--;
                    if (!s_interval_counter[ch])
                        s_intl_control[ch] &= (uint8_t)~0x80u;
                }
                s_envelope_clock_divider[ch]--;
                while (s_envelope_clock_divider[ch] <= 0) {
                    s_envelope_clock_divider[ch] += 4;
                    if (s_env_control[ch] & 0x0100u) {
                        s_envelope_counter[ch]--;
                        if (!s_envelope_counter[ch]) {
                            s_envelope_counter[ch] =
                                (s_env_control[ch] & 0x7u) + 1;
                            if (s_env_control[ch] & 0x0008u) {
                                if (s_envelope[ch] < 0xF
                                    || (s_env_control[ch] & 0x200u))
                                    s_envelope[ch] = (s_envelope[ch] + 1) & 0xF;
                            } else {
                                if (s_envelope[ch] > 0
                                    || (s_env_control[ch] & 0x200u))
                                    s_envelope[ch] = (s_envelope[ch] - 1) & 0xF;
                            }
                        }
                    }
                }
            }
            if (ch == 4) {
                s_sweep_mod_clock_divider--;
                while (s_sweep_mod_clock_divider <= 0) {
                    s_sweep_mod_clock_divider +=
                        (s_sweep_control & 0x80u) ? 8 : 1;
                    if (((s_sweep_control >> 4) & 0x7u)
                        && (s_env_control[ch] & 0x4000u)) {
                        if (s_sweep_mod_counter) s_sweep_mod_counter--;
                        if (!s_sweep_mod_counter) {
                            s_sweep_mod_counter =
                                (s_sweep_control >> 4) & 0x7u;
                            if (s_env_control[ch] & 0x1000u) {
                                /* FM via mod table. */
                                if (s_mod_wave_pos < 32
                                    || (s_env_control[ch] & 0x2000u)) {
                                    s_mod_wave_pos &= 0x1F;
                                    s_eff_freq[ch] = (s_frequency[ch]
                                        + (int8_t)s_mod_data[s_mod_wave_pos])
                                        & 0x7FF;
                                    s_mod_wave_pos++;
                                }
                            } else {
                                /* Frequency sweep. */
                                int32_t d = s_eff_freq[ch]
                                          >> (s_sweep_control & 0x7u);
                                int32_t nf = s_eff_freq[ch]
                                    + ((s_sweep_control & 0x8u) ? d : -d);
                                if (nf < 0)        s_eff_freq[ch] = 0;
                                else if (nf > 0x7FF)
                                    s_intl_control[ch] &= (uint8_t)~0x80u;
                                else               s_eff_freq[ch] = nf;
                            }
                        }
                    }
                }
            }
        }
        clocks -= chunk;
    }
}

/* Verified-enhancement shadow: float re-render of one channel. Mirrors
 * vsu_channel_output EXACTLY in waveform/envelope/level selection, but keeps
 * the per-channel gain in full precision (no `(envelope*level)>>3 + 1`
 * 4-bit requantize) and accumulates in float. Same notes, same envelopes,
 * same timing — only the gain quantization differs. Present-time only; this
 * never feeds the canon path nor the verify oracle (see vsu_shadow.h). */
static inline void vsu_channel_output_shadow(int ch, float* left, float* right) {
    if (!(s_intl_control[ch] & 0x80u)) {
        *left = *right = 0.0f;
        return;
    }
    int wd;
    if (ch == 5) {
        wd = (int)s_noise_latcher;
    } else {
        if (s_ram_address[ch] > 4) wd = 0;
        else wd = s_wave_data[s_ram_address[ch]][s_wave_pos[ch]];
    }
    wd -= 0x20;
    /* Full-precision gain: envelope*level/8 (the canon truncates to int and
     * adds 1; the shadow keeps the fractional gain and the silence floor). */
    float fl = (float)(s_envelope[ch] * s_left_level[ch]) / 8.0f;
    float fr = (float)(s_envelope[ch] * s_right_level[ch]) / 8.0f;
    *left  = (float)wd * fl;
    *right = (float)wd * fr;
}

/* ---------------------- Sample emission ------------------------- */

static inline void vsu_emit_one_sample(void) {
    int32_t mix_l = 0, mix_r = 0;
    float   sh_l = 0.0f, sh_r = 0.0f;
    const bool shadow_on = vb_vsu_shadow_enabled();
    for (int ch = 0; ch < 6; ++ch) {
        int l, r;
        vsu_channel_output(ch, &l, &r);
        mix_l += l;
        mix_r += r;
        if (shadow_on) {
            float fl, fr;
            vsu_channel_output_shadow(ch, &fl, &fr);
            sh_l += fl;
            sh_r += fr;
        }
    }
    /* Per-channel peak ~= +/- 31 * 30 = +/- 930. Six channels max
     * out near +/- 5580. Scale by 4 maps the loudest plausible mix
     * to +/- 22 320, leaving ~30 % headroom before the S16 clip. */
    mix_l <<= 2;
    mix_r <<= 2;
    if (mix_l >  32767) mix_l =  32767;
    if (mix_l < -32768) mix_l = -32768;
    if (mix_r >  32767) mix_r =  32767;
    if (mix_r < -32768) mix_r = -32768;

    int16_t out_l = (int16_t)mix_l;
    int16_t out_r = (int16_t)mix_r;

    /* Verified-enhancement shadow (default OFF). Feed the canon mix
     * (pre-clip numeric value) and the float re-render (same *4 scale) to the
     * differential verifier; it substitutes the shadow only after a proven
     * window and reverts loudly otherwise. When OFF, this is a no-op and the
     * stored bytes are byte-identical to the canon path above. */
    if (shadow_on) {
        int16_t s_out_l, s_out_r;
        if (vb_vsu_shadow_substitute((float)(mix_l), (float)(mix_r),
                                     sh_l * 4.0f, sh_r * 4.0f,
                                     &s_out_l, &s_out_r)) {
            out_l = s_out_l;
            out_r = s_out_r;
        }
    }

    /* Drop the oldest frame when the consumer hasn't drained
     * (happens during `--headless` runs with no SDL audio device). */
    size_t next_head = (s_ring_head + 1) & VSU_RING_MASK;
    if (next_head == s_ring_tail) {
        s_ring_tail = (s_ring_tail + 1) & VSU_RING_MASK;
    }
    s_ring_l[s_ring_head] = out_l;
    s_ring_r[s_ring_head] = out_r;
    s_ring_head = next_head;
    s_ring_total++;
}

void vb_vsu_tick(uint64_t cpu_cycles) {
    int64_t remaining = (int64_t)cpu_cycles;          /* 20 MHz CPU domain */
    while (remaining > 0) {
        int32_t need = VSU_CYCLES_PER_SAMPLE - s_sample_cycle_residue;
        if (need <= 0) need = 1;
        int32_t step = (remaining < need) ? (int32_t)remaining : need;

        /* Channel synthesis runs in the 5 MHz VSU domain (CPU/4). Convert
         * this chunk of CPU cycles to VSU cycles, carrying the sub-4
         * remainder so the channel clocks stay phase-exact across calls.
         * Sample emission below stays in the CPU domain (453 cyc/sample
         * = 20 MHz/44.1 kHz), so the output rate is unchanged. */
        s_vsu_clock_residue += step;
        int32_t vsu_step = s_vsu_clock_residue >> 2;
        s_vsu_clock_residue &= 3;
        if (vsu_step > 0)
            for (int ch = 0; ch < 6; ++ch)
                vsu_step_channel(ch, vsu_step);

        s_sample_cycle_residue += step;
        remaining -= step;

        if (s_sample_cycle_residue >= VSU_CYCLES_PER_SAMPLE) {
            s_sample_cycle_residue -= VSU_CYCLES_PER_SAMPLE;
            vsu_emit_one_sample();
        }
    }
}

size_t vb_vsu_pull_samples(int16_t* dst, size_t n_frames) {
    if (!dst || !n_frames) return 0;
    size_t produced = 0;
    while (produced < n_frames && s_ring_tail != s_ring_head) {
        dst[produced * 2 + 0] = s_ring_l[s_ring_tail];
        dst[produced * 2 + 1] = s_ring_r[s_ring_tail];
        s_ring_tail = (s_ring_tail + 1) & VSU_RING_MASK;
        produced++;
    }
    for (size_t i = produced; i < n_frames; ++i) {
        dst[i * 2 + 0] = 0;
        dst[i * 2 + 1] = 0;
    }
    return produced;
}

size_t vb_vsu_pending_frames(void) {
    return (s_ring_head - s_ring_tail) & VSU_RING_MASK;
}

uint64_t vb_vsu_total_frames(void) {
    return s_ring_total;
}

unsigned vb_vsu_output_hz(void) {
    return VSU_OUTPUT_HZ;
}

size_t vb_vsu_read_abs(uint64_t start_abs, int16_t* dst, size_t max_frames,
                       uint64_t* out_head_abs, uint64_t* out_resident_lo) {
    const uint64_t head = s_ring_total;
    /* Oldest still-readable absolute index: frame i is overwritten at
     * write (i + VSU_RING_FRAMES), so anything >= head - capacity is
     * still in its slot. (head - 1 is the newest written frame.) */
    const uint64_t resident_lo =
        (head > (uint64_t)VSU_RING_FRAMES) ? head - (uint64_t)VSU_RING_FRAMES : 0;
    if (out_head_abs)    *out_head_abs    = head;
    if (out_resident_lo) *out_resident_lo = resident_lo;
    if (!dst || !max_frames) return 0;

    uint64_t cur = start_abs < resident_lo ? resident_lo : start_abs;
    size_t produced = 0;
    while (produced < max_frames && cur < head) {
        size_t slot = (size_t)(cur & VSU_RING_MASK);
        dst[produced * 2 + 0] = s_ring_l[slot];
        dst[produced * 2 + 1] = s_ring_r[slot];
        produced++;
        cur++;
    }
    return produced;
}
