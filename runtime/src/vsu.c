/* vsu.c — Virtual Sound Unit.
 *
 * Port of beetle-vb/mednafen/vb/vsu.c (lines 26-475), including the
 * Blip_Synth band-limited output stage. Each VSU channel feeds amplitude
 * deltas into a Blip_Buffer (clocked at VB_MASTER_CLOCK/4 = 5 MHz, the
 * same 5 MHz domain as the step machine), exactly as the oracle does
 * (VSU_Update vsu.c:289-469). The band-limited samples are drained at
 * 44.1 kHz into the same stereo ring the SDL/accuracy paths already read.
 * This replaces the earlier point-sampled (-0x20 / <<2 / 453-cadence)
 * snapshot output, which aliased and did not match the oracle waveshape
 * (Axis-5b; see docs/AXIS5B_BLIP_OUTPUT.md).
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

#include "blip/Blip_Buffer.h"
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

/* ------------------ Band-limited output (Blip_Synth) -------------- */
/* One Blip_Buffer per stereo side, clocked at the 5 MHz VSU domain, and
 * one shared Blip_Synth (the oracle declares a second NoiseSynth but never
 * uses it). `s_last_output` holds each channel's last-fed (L,R) amplitude
 * so we can delta-encode transitions, exactly mirroring the oracle's
 * last_output[6][2] (beetle-vb/mednafen/vb/vsu.c:67,301-304,461-464). */
static Blip_Buffer s_bb_l, s_bb_r;
static Blip_Synth  s_synth;
static int32_t     s_last_output[6][2];
static bool        s_blip_inited;

/* Relative VSU-cycle timestamp since the last Blip frame flush. Blip feeds
 * happen at times in [0, s_vsu_frame_ts]; a flush calls end_frame(ts),
 * drains the band-limited samples into the ring, and resets ts to 0 (the
 * oracle's VSU_EndFrame last_ts=0). Bounded by VSU_FLUSH_CLOCKS so the
 * Blip buffer never overruns and the per-flush memmove stays small. */
static int32_t  s_vsu_frame_ts;

/* Flush cadence in 5 MHz VSU cycles. ~3.3 ms / ~145 output samples per
 * flush — far under the 50 ms (~2205 sample) buffer the oracle sizes, so
 * Blip_Synth_offset never writes past the buffer, while keeping the
 * read_samples memmove cheap. Output rate is locked to 44.1 kHz by Blip's
 * clock-rate factor regardless of this value (it is NOT a sample cadence). */
#define VSU_FLUSH_CLOCKS  16384

/* The VB VSU is clocked at CPU/4 (5 MHz), NOT the full 20 MHz CPU clock.
 * Mednafen feeds its VSU `(v810_timestamp + CycleFix) >> 2` and runs the
 * Blip buffer at VB_MASTER_CLOCK/4 (beetle-vb/libretro.cpp:1944,2346).
 * Our vsu_update_channel is a verbatim port of Mednafen's 5 MHz-domain
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
    s_vsu_clock_residue = 0;

    /* Band-limited output stage. Mirror the oracle's setup exactly
     * (beetle-vb/libretro.cpp:2421-2423 + vsu.c:84): 44.1 kHz, 5 MHz clock,
     * bass-freq 20 (the high-pass that removes the waveform's DC bias — which
     * is why the channel output keeps the raw unsigned 0..63 sample with NO
     * -0x20 centering), Synth volume 1/6/2 over a 0x400 range. */
    if (!s_blip_inited) {
        Blip_Buffer_init(&s_bb_l);
        Blip_Buffer_init(&s_bb_r);
        Blip_Buffer_set_sample_rate(&s_bb_l, 44100, 50);
        Blip_Buffer_set_sample_rate(&s_bb_r, 44100, 50);
        Blip_Buffer_set_clock_rate(&s_bb_l, (long)(20000000 / 4));
        Blip_Buffer_set_clock_rate(&s_bb_r, (long)(20000000 / 4));
        Blip_Buffer_bass_freq(&s_bb_l, 20);
        Blip_Buffer_bass_freq(&s_bb_r, 20);
        Blip_Synth_set_volume(&s_synth, 1.0 / 6 / 2, 0x400);
        s_blip_inited = true;
    } else {
        Blip_Buffer_clear(&s_bb_l, 1);
        Blip_Buffer_clear(&s_bb_r, 1);
    }
    memset(s_last_output, 0, sizeof(s_last_output));
    s_vsu_frame_ts = 0;

    vb_vsu_shadow_reset();
}

void vb_vsu_shutdown(void) {
    if (s_blip_inited) {
        Blip_Buffer_deinit(&s_bb_l);
        Blip_Buffer_deinit(&s_bb_r);
        s_blip_inited = false;
    }
}

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

/* Compute current (left,right) amplitude for one channel, in the exact
 * 0..63 * (envelope*level/8 + 1) scale the oracle feeds to Blip_Synth
 * (VSU_CalcCurrentOutput, beetle-vb/mednafen/vb/vsu.c:251-287). NO -0x20
 * centering: the raw unsigned 0..63 waveform is fed and its DC component is
 * removed downstream by the Blip_Buffer bass-freq high-pass, matching the
 * oracle (the oracle's `- 0x20` is commented out there for the same reason).
 * The values are delta-encoded into the Synth, so absolute DC is irrelevant. */
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

    int l = s_envelope[ch] * s_left_level[ch];
    if (l) { l >>= 3; l += 1; }
    int r = s_envelope[ch] * s_right_level[ch];
    if (r) { r >>= 3; r += 1; }
    *left  = wd * l;
    *right = wd * r;
}

/* Advance one channel by `clocks` VSU (5 MHz) cycles starting at relative
 * Blip-frame time `running_timestamp`, feeding band-limited amplitude deltas
 * into the Synth at each output boundary. A verbatim mirror of the oracle's
 * VSU_Update inner body for one channel (beetle-vb/mednafen/vb/vsu.c:294-465):
 * boundary feed at chunk start (even when the channel is off, so a stop edges
 * the output to 0), step machine, per-chunk feed at the advanced timestamp. */
static void vsu_update_channel(int ch, int32_t running_timestamp, int32_t clocks) {
    int left, right;

    /* Output sound here (oracle vsu.c:300-304). */
    vsu_channel_output(ch, &left, &right);
    Blip_Synth_offset(&s_synth, running_timestamp, left  - s_last_output[ch][0], &s_bb_l);
    Blip_Synth_offset(&s_synth, running_timestamp, right - s_last_output[ch][1], &s_bb_r);
    s_last_output[ch][0] = left;
    s_last_output[ch][1] = right;

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
        running_timestamp += chunk;

        /* Output sound here too (oracle vsu.c:459-464). */
        vsu_channel_output(ch, &left, &right);
        Blip_Synth_offset(&s_synth, running_timestamp, left  - s_last_output[ch][0], &s_bb_l);
        Blip_Synth_offset(&s_synth, running_timestamp, right - s_last_output[ch][1], &s_bb_r);
        s_last_output[ch][0] = left;
        s_last_output[ch][1] = right;
    }
}

/* The opt-in `vsu_shadow` differential verifier (vsu_shadow.h, default OFF)
 * is INTENTIONALLY not wired into the band-limited path. It compared a
 * per-sample full-precision float mix against the canon per-sample integer
 * mix — but band-limited output has no per-sample integer mix to feed, and
 * its full-precision gain deliberately DIVERGES from the oracle's
 * (envelope*level>>3)+1 quantization, which is exactly what this Axis-5b
 * output stage is matching. The shadow's source remains (and vb_vsu_init
 * still resets its status ring) so a future post-Blip rework can re-engage
 * it as an overrides-style opt-in; faithful default behavior is unaffected
 * because the shadow was default-OFF and byte-identical when disabled. */

/* ---------------------- Sample emission ------------------------- */

/* Push one band-limited stereo frame to the output ring, evicting the
 * oldest frame when the consumer (SDL, or none in --headless) is behind.
 * Same ring contract as before: s_ring_head == s_ring_total & MASK. */
static inline void ring_push(int16_t l, int16_t r) {
    size_t next_head = (s_ring_head + 1) & VSU_RING_MASK;
    if (next_head == s_ring_tail)
        s_ring_tail = (s_ring_tail + 1) & VSU_RING_MASK;
    s_ring_l[s_ring_head] = l;
    s_ring_r[s_ring_head] = r;
    s_ring_head = next_head;
    s_ring_total++;
}

/* End the current Blip frame and drain its band-limited 44.1 kHz samples
 * into the ring, then reset the relative frame timestamp (oracle pattern:
 * Blip_Buffer_end_frame + Blip_Buffer_read_samples in libretro.cpp:2027-2028,
 * VSU_EndFrame last_ts=0). Both sides advance identically, so L and R yield
 * the same count and stay frame-aligned. */
static void vsu_flush_frame(void) {
    if (s_vsu_frame_ts <= 0) return;
    Blip_Buffer_end_frame(&s_bb_l, s_vsu_frame_ts);
    Blip_Buffer_end_frame(&s_bb_r, s_vsu_frame_ts);
    s_vsu_frame_ts = 0;

    /* read_samples always strides by 2 (stereo-interleave), so read L into
     * slot 0 and R into slot 1 of an interleaved scratch, then deinterleave
     * into the ring. Drain fully in case more than one batch is available. */
    int16_t inter[256 * 2];
    for (;;) {
        long n = Blip_Buffer_read_samples(&s_bb_l, inter + 0, 256);
        long m = Blip_Buffer_read_samples(&s_bb_r, inter + 1, 256);
        (void)m;   /* n == m: both buffers share factor/offset */
        for (long i = 0; i < n; ++i)
            ring_push(inter[i * 2 + 0], inter[i * 2 + 1]);
        if (n < 256) break;
    }
}

void vb_vsu_tick(uint64_t cpu_cycles) {
    /* Channel synthesis + Blip feeds run in the 5 MHz VSU domain (CPU/4).
     * Convert this tick's CPU cycles to VSU cycles, carrying the sub-4
     * remainder so the channel clocks stay phase-exact across ticks (the
     * oracle's CycleFix). Output rate (44.1 kHz) is locked by Blip's
     * clock-rate factor, NOT by any cycle-per-sample cadence here. */
    s_vsu_clock_residue += (int32_t)cpu_cycles;
    int32_t vsu_cycles = s_vsu_clock_residue >> 2;
    s_vsu_clock_residue &= 3;

    while (vsu_cycles > 0) {
        /* Bound the relative frame timestamp so Blip_Synth_offset never
         * writes past the buffer; flush + reset when the window is full. */
        int32_t room = VSU_FLUSH_CLOCKS - s_vsu_frame_ts;
        if (room <= 0) { vsu_flush_frame(); continue; }

        int32_t chunk = (vsu_cycles < room) ? vsu_cycles : room;
        for (int ch = 0; ch < 6; ++ch)
            vsu_update_channel(ch, s_vsu_frame_ts, chunk);
        s_vsu_frame_ts += chunk;
        vsu_cycles     -= chunk;

        if (s_vsu_frame_ts >= VSU_FLUSH_CLOCKS) vsu_flush_frame();
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
