/* memory.c — VB physical memory routing.
 *
 * 27-bit decoded address window (see docs/HARDWARE_NOTES.md). Mirrors
 * are handled by folding through VB_PHYS_MASK; mirroring within a
 * region (e.g. WRAM repeating across its 16 MB window) is handled by
 * masking against the region size.
 *
 * Unmapped regions and unrecognised MMIO registers fatal-abort via
 * vb_stub_abort(). No silent zero-reads, no silent dropped writes.
 */
#include "memory.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "input.h"
#include "interrupts.h"
#include "stub_abort.h"
#include "timer.h"
#include "vip.h"
#include "vsu.h"
#include "wtrace.h"
#include "cpu_state.h"

static CPUState* s_bus_cpu;
static VbWriteObserver s_write_observer;
int vb_memory_register_write_observer(VbWriteObserver observer) {
    if (!observer || s_write_observer) return 0;
    s_write_observer = observer;
    return 1;
}
static void observe_write(uint32_t addr, uint32_t value, unsigned width) {
    uint32_t tag = s_write_observer && s_bus_cpu
        ? s_write_observer(s_bus_cpu, addr, value, width) : 0;
    if (((addr >> 24) & 7u) == 0)
        vb_vip_record_cpu_write(addr, value, width, tag);
}
static uint64_t s_timer_cycle,s_vip_cycle,s_vsu_cycle,s_input_cycle;
void vb_memory_set_cpu(CPUState* cpu) { s_bus_cpu=cpu; }
uint64_t vb_memory_access_cycle(void) {
    return s_bus_cpu ? s_bus_cpu->cycles-s_bus_cpu->bus_tail_cycles:s_vip_cycle;
}
int32_t vb_devices_cycles_to_next_event(uint64_t cycle) {
    uint64_t next=s_vip_cycle+(uint64_t)vb_vip_cycles_to_next_event();
    uint64_t timer=s_timer_cycle+(uint64_t)vb_timer_cycles_to_next_event();
    if(timer<next) next=timer;
    uint64_t input=s_input_cycle+(uint64_t)vb_input_cycles_to_next_event();
    if(input<next) next=input;
    return next>cycle ? (int32_t)(next-cycle):1;
}
void vb_devices_sync(uint64_t cycle) {
    if(cycle>s_timer_cycle) { vb_timer_tick((uint32_t)(cycle-s_timer_cycle));s_timer_cycle=cycle; }
    if(cycle>s_input_cycle) { vb_input_tick((uint32_t)(cycle-s_input_cycle));s_input_cycle=cycle; }
    if(cycle>=s_vip_cycle+(uint64_t)vb_vip_cycles_to_next_event()) {
        vb_vip_tick(cycle-s_vip_cycle);s_vip_cycle=cycle;
    }
}
void vb_devices_end_frame(uint64_t cycle) {
    vb_vsu_tick(cycle-s_vsu_cycle);s_vsu_cycle=cycle;
    vb_vsu_end_frame();
}
static void bus_begin(uint32_t addr, int writing) {
    if(!s_bus_cpu) return;
    uint64_t cycle=s_bus_cpu->cycles-s_bus_cpu->bus_tail_cycles;
    unsigned region=(addr>>24)&7;
    /* These devices update synchronously on register access. VIP events
     * remain instruction-boundary events, like the independent oracle. */
    if(region==2 && cycle>s_timer_cycle) { vb_timer_tick((uint32_t)(cycle-s_timer_cycle));s_timer_cycle=cycle; }
    if(region==2 && cycle>s_input_cycle) { vb_input_tick((uint32_t)(cycle-s_input_cycle));s_input_cycle=cycle; }
    if(region==1 && writing && !(addr&3) && cycle>=s_vsu_cycle) {
        vb_vsu_tick(cycle-s_vsu_cycle);s_vsu_cycle=cycle;
    }
}
static void bus_end(uint32_t addr) {
    if(!s_bus_cpu || ((addr>>24)&7)>=3) return;
    int32_t next=vb_vip_cycles_to_next_event(),timer=vb_timer_cycles_to_next_event();
    uint64_t deadline=s_vip_cycle+(uint64_t)(next>0 ? next:1);
    uint64_t timer_deadline=s_timer_cycle+(uint64_t)(timer>0 ? timer:1);
    if(timer_deadline<deadline) deadline=timer_deadline;
    uint64_t input_deadline=s_input_cycle+(uint64_t)vb_input_cycles_to_next_event();
    if(input_deadline<deadline) deadline=input_deadline;
    int level=vb_irq_highest_pending_level();
    if(level>=0 && !s_bus_cpu->psw_id && !s_bus_cpu->psw_ep && !s_bus_cpu->psw_np && level>=s_bus_cpu->psw_int_level)
        deadline=s_bus_cpu->cycles;
    if(deadline<s_bus_cpu->cycle_deadline) s_bus_cpu->cycle_deadline=deadline;
}

static uint8_t* s_rom = NULL;
static uint32_t s_rom_size = 0;
static uint8_t s_cart_ram[65536];
uint8_t* vb_cart_ram_data(void) { return s_cart_ram; }
static uint8_t  s_wram[VB_WRAM_SIZE];

uint32_t vb_rom_size(void) { return s_rom_size; }
const uint8_t* vb_rom_data(void) { return s_rom; }
const uint8_t* vb_wram_data(void) { return s_wram; }

/* Per-region FNV-1a over WRAM (Axis-6 whole-session fidelity ring): 64
 * regions of 1 KiB over the 64 KiB WRAM. Regional (not one whole-RAM hash) so
 * a single volatile timing-derived cell can't avalanche the entire frame to
 * "divergent" — the comparator localizes which 1 KiB region differs and shows
 * the rest stays in lockstep. MUST match the oracle's hash
 * (beetle-vb/libretro.cpp, same region layout + FNV-1a constants). */
void vb_wram_region_fnv(uint32_t out[VB_WRAM_FNV_REGIONS]) {
    for (int r = 0; r < VB_WRAM_FNV_REGIONS; ++r) {
        const uint8_t* p = s_wram + (size_t)r * VB_WRAM_FNV_REGION_BYTES;
        uint32_t h = 2166136261u;
        for (int i = 0; i < VB_WRAM_FNV_REGION_BYTES; ++i) {
            h ^= p[i];
            h *= 16777619u;
        }
        out[r] = h;
    }
}

int vb_memory_init(const char* rom_path) {
    s_bus_cpu=NULL;s_timer_cycle=s_vip_cycle=s_vsu_cycle=s_input_cycle=0;
    if (!rom_path || !*rom_path) {
        return -1;
    }
    FILE* f = fopen(rom_path, "rb");
    if (!f) {
        return -2;
    }
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return -3; }
    long sz = ftell(f);
    if (sz <= 0) { fclose(f); return -4; }
    if (fseek(f, 0, SEEK_SET) != 0) { fclose(f); return -5; }

    s_rom = (uint8_t*)malloc((size_t)sz);
    if (!s_rom) { fclose(f); return -6; }
    if (fread(s_rom, 1, (size_t)sz, f) != (size_t)sz) {
        free(s_rom); s_rom = NULL;
        fclose(f);
        return -7;
    }
    fclose(f);
    s_rom_size = (uint32_t)sz;
    memset(s_wram, 0, sizeof(s_wram));
    memset(s_cart_ram, 0, sizeof(s_cart_ram));

    vb_vip_init();
    vb_vsu_init();
    vb_input_init();
    vb_irq_init();
    vb_timer_init();
    return 0;
}

void vb_memory_shutdown(void) {
    s_bus_cpu=NULL;
    free(s_rom);
    s_rom = NULL;
    s_rom_size = 0;
}

/* ---- Address classification ---- */
static uint32_t fold(uint32_t addr) { return addr & VB_PHYS_MASK; }
static int region_of(uint32_t phys) { return (phys >> 24) & 0x7; }

static uint32_t rom_off(uint32_t phys) {
    uint32_t off = phys - VB_ROM_BASE;
    if (s_rom_size == 0) {
        vb_stub_abort("rom read with no ROM loaded", 0, phys);
    }
    /* Mirror the ROM image across the 16 MB cart-ROM window. */
    if ((s_rom_size & (s_rom_size - 1)) == 0) {
        return off & (s_rom_size - 1);
    }
    return off % s_rom_size;
}

static uint32_t wram_off(uint32_t phys) {
    return (phys - VB_WRAM_BASE) & (VB_WRAM_SIZE - 1);
}

/* ---- Reads ---- */
/* Misc-page (region 2) dispatch.
 *
 * Beetle's HWCTRL_Read / HWCTRL_Write (libretro.cpp:138-183) routes
 * by `A & 0xFF`:
 *     0x10 / 0x14 / 0x28  → input (SDR_LO / SDR_HI / SCR)
 *     0x18 / 0x1C / 0x20  → timer
 *     0x24                → WCR (waitstate control, 2 bits stored)
 *     everything else     → no-op (write drops, read returns 0)
 *
 * The "no-op" branch is real hardware behaviour, not a stub: the
 * misc-page bus decodes the low byte and silently ignores writes /
 * returns 0 on reads for addresses outside the documented register
 * set. Mednafen's behaviour has been verified against real hardware
 * for years.
 *
 * Misaligned writes (`A & 0x3 != 0`) drop; misaligned reads return 0
 * (Beetle: "HWCtrl Bogus Read?" / "Bogus Write?"). */
static uint8_t s_misc_wcr;

static int misc_is_input(uint32_t lo) {
    return (lo == 0x10) || (lo == 0x14) || (lo == 0x28);
}
static int misc_is_timer(uint32_t lo) {
    return (lo == 0x18) || (lo == 0x1C) || (lo == 0x20);
}

uint8_t raw_read8(uint32_t addr) {
    uint32_t p = fold(addr);
    switch (region_of(p)) {
        case 0: return vb_vip_read8(p);
        case 1: return vb_vsu_read8(p);
        case 2: {
            uint32_t lo = p & 0xFFu;
            if (lo & 0x3u) return 0;             /* misaligned → 0 */
            if (misc_is_timer(lo)) return vb_timer_read8(p);
            if (misc_is_input(lo)) return vb_input_read8(p);
            if (lo == 0x24)        return (uint8_t)(s_misc_wcr | 0xFCu);
            return 0;                            /* unmapped → 0 */
        }
        case 3: vb_stub_abort("read from reserved region 0x03000000", 0, addr);
        case 4: vb_stub_abort("cart-expansion read8 not modelled", 0, addr);
        case 5: return s_wram[wram_off(p)];
        case 6: return ((uint8_t)s_cart_ram[(p + 0) & 65535] << 0);
        case 7: return s_rom[rom_off(p)];
    }
    vb_stub_abort("vb_read8 fall-through", 0, addr);
}

uint16_t raw_read16(uint32_t addr) {
    addr &= ~1u;
    uint32_t p = fold(addr);
    switch (region_of(p)) {
        case 0: return vb_vip_read16(p);
        case 1: return vb_vsu_read16(p);
        case 2: {
            uint32_t lo = p & 0xFFu;
            if (lo & 0x3u) return 0;
            if (misc_is_timer(lo)) return vb_timer_read16(p);
            if (misc_is_input(lo)) return vb_input_read16(p);
            if (lo == 0x24)        return (uint16_t)(s_misc_wcr | 0xFCu);
            return 0;
        }
        case 3: vb_stub_abort("read from reserved region 0x03000000", 0, addr);
        case 4: vb_stub_abort("cart-expansion read16 not modelled", 0, addr);
        case 5: {
            uint32_t o = wram_off(p);
            return (uint16_t)s_wram[o] | ((uint16_t)s_wram[o + 1] << 8);
        }
        case 6: return ((uint16_t)s_cart_ram[(p + 0) & 65535] << 0) | ((uint16_t)s_cart_ram[(p + 1) & 65535] << 8);
        case 7: {
            uint32_t o = rom_off(p);
            return (uint16_t)s_rom[o] | ((uint16_t)s_rom[o + 1] << 8);
        }
    }
    vb_stub_abort("vb_read16 fall-through", 0, addr);
}

uint32_t raw_read32(uint32_t addr) {
    addr &= ~3u;
    uint32_t p = fold(addr);
    switch (region_of(p)) {
        case 0: return vb_vip_read32(p);
        case 1: return vb_vsu_read32(p);
        case 2: {
            uint32_t lo = p & 0xFFu;
            if (lo & 0x3u) return 0;
            if (misc_is_timer(lo)) return vb_timer_read32(p);
            if (misc_is_input(lo)) return vb_input_read32(p);
            if (lo == 0x24)        return (uint32_t)(s_misc_wcr | 0xFCu);
            return 0;
        }
        case 3: vb_stub_abort("read from reserved region 0x03000000", 0, addr);
        case 4: vb_stub_abort("cart-expansion read32 not modelled", 0, addr);
        case 5: {
            uint32_t o = wram_off(p);
            return  (uint32_t)s_wram[o]
                  | ((uint32_t)s_wram[o + 1] << 8)
                  | ((uint32_t)s_wram[o + 2] << 16)
                  | ((uint32_t)s_wram[o + 3] << 24);
        }
        case 6: return ((uint32_t)s_cart_ram[(p + 0) & 65535] << 0) | ((uint32_t)s_cart_ram[(p + 1) & 65535] << 8) | ((uint32_t)s_cart_ram[(p + 2) & 65535] << 16) | ((uint32_t)s_cart_ram[(p + 3) & 65535] << 24);
        case 7: {
            uint32_t o = rom_off(p);
            return  (uint32_t)s_rom[o]
                  | ((uint32_t)s_rom[o + 1] << 8)
                  | ((uint32_t)s_rom[o + 2] << 16)
                  | ((uint32_t)s_rom[o + 3] << 24);
        }
    }
    vb_stub_abort("vb_read32 fall-through", 0, addr);
}

/* ---- Writes ----
 *
 * Each writer records into the always-on wtrace ring at entry — before
 * the regional dispatch, before any stub_abort. This ensures every
 * store the cart issues is visible to the debug server, including
 * doomed writes (e.g. into cart-ROM region 7) that abort. Source PC
 * is sampled by wtrace from the active CPU pointer.
 */
void raw_write8(uint32_t addr, uint8_t v) {
    vb_wtrace_record(addr, (uint32_t)v, 1u);
    observe_write(addr, v, 1);
    uint32_t p = fold(addr);
    switch (region_of(p)) {
        case 0: vb_vip_write8(p, v); return;
        case 1: vb_vsu_write8(p, v); return;
        case 2: {
            uint32_t lo = p & 0xFFu;
            if (lo & 0x3u) return;                  /* misaligned → drop */
            if (misc_is_timer(lo)) { vb_timer_write8(p, v); return; }
            if (misc_is_input(lo)) { vb_input_write8(p, v); return; }
            if (lo == 0x24)        { s_misc_wcr = v & 0x3u; return; }
            return;                                 /* unmapped → drop */
        }
        case 3: vb_stub_abort("write to reserved region 0x03000000", 0, addr);
        case 4: vb_stub_abort("cart-expansion write8 not modelled", 0, addr);
        case 5: s_wram[wram_off(p)] = v; return;
        case 6: s_cart_ram[(p + 0) & 65535] = (uint8_t)(v >> 0); return;
        case 7: vb_stub_abort("write to cart ROM (read-only)", 0, addr);
    }
    vb_stub_abort("vb_write8 fall-through", 0, addr);
}

void raw_write16(uint32_t addr, uint16_t v) {
    addr &= ~1u;
    vb_wtrace_record(addr, (uint32_t)v, 2u);
    observe_write(addr, v, 2);
    uint32_t p = fold(addr);
    switch (region_of(p)) {
        case 0: vb_vip_write16(p, v); return;
        case 1: vb_vsu_write16(p, v); return;
        case 2: {
            uint32_t lo = p & 0xFFu;
            if (lo & 0x3u) return;
            if (misc_is_timer(lo)) { vb_timer_write16(p, v); return; }
            if (misc_is_input(lo)) { vb_input_write16(p, v); return; }
            if (lo == 0x24)        { s_misc_wcr = (uint8_t)(v & 0x3u); return; }
            return;
        }
        case 3: vb_stub_abort("write to reserved region 0x03000000", 0, addr);
        case 4: vb_stub_abort("cart-expansion write16 not modelled", 0, addr);
        case 5: {
            uint32_t o = wram_off(p);
            s_wram[o]     = (uint8_t)(v & 0xFF);
            s_wram[o + 1] = (uint8_t)((v >> 8) & 0xFF);
            return;
        }
        case 6: s_cart_ram[(p + 0) & 65535] = (uint8_t)(v >> 0); s_cart_ram[(p + 1) & 65535] = (uint8_t)(v >> 8); return;
        case 7: vb_stub_abort("write to cart ROM (read-only)", 0, addr);
    }
    vb_stub_abort("vb_write16 fall-through", 0, addr);
}

void raw_write32(uint32_t addr, uint32_t v) {
    addr &= ~3u;
    vb_wtrace_record(addr, v, 4u);
    observe_write(addr, v, 4);
    uint32_t p = fold(addr);
    switch (region_of(p)) {
        case 0: vb_vip_write32(p, v); return;
        case 1: vb_vsu_write32(p, v); return;
        case 2: {
            uint32_t lo = p & 0xFFu;
            if (lo & 0x3u) return;
            if (misc_is_timer(lo)) { vb_timer_write32(p, v); return; }
            if (misc_is_input(lo)) { vb_input_write32(p, v); return; }
            if (lo == 0x24)        { s_misc_wcr = (uint8_t)(v & 0x3u); return; }
            return;
        }
        case 3: vb_stub_abort("write to reserved region 0x03000000", 0, addr);
        case 4: vb_stub_abort("cart-expansion write32 not modelled", 0, addr);
        case 5: {
            uint32_t o = wram_off(p);
            s_wram[o]     = (uint8_t)(v & 0xFF);
            s_wram[o + 1] = (uint8_t)((v >> 8) & 0xFF);
            s_wram[o + 2] = (uint8_t)((v >> 16) & 0xFF);
            s_wram[o + 3] = (uint8_t)((v >> 24) & 0xFF);
            return;
        }
        case 6: s_cart_ram[(p + 0) & 65535] = (uint8_t)(v >> 0); s_cart_ram[(p + 1) & 65535] = (uint8_t)(v >> 8); s_cart_ram[(p + 2) & 65535] = (uint8_t)(v >> 16); s_cart_ram[(p + 3) & 65535] = (uint8_t)(v >> 24); return;
        case 7: vb_stub_abort("write to cart ROM (read-only)", 0, addr);
    }
    vb_stub_abort("vb_write32 fall-through", 0, addr);
}

/* Bulk read for the debug server. Reads byte-by-byte through vb_read8
 * so unmapped accesses surface the same way executed accesses would. */
size_t vb_memory_dump(uint32_t addr, uint8_t* out, size_t len) {
    for (size_t i = 0; i < len; ++i) {
        out[i] = vb_read8((uint32_t)(addr + i));
    }
    return len;
}

uint8_t vb_read8(uint32_t addr) { bus_begin(addr,0);uint8_t result=raw_read8(addr);bus_end(addr);return result; }
void vb_write8(uint32_t addr,uint8_t value) { bus_begin(addr,1);raw_write8(addr,value);bus_end(addr); }

uint16_t vb_read16(uint32_t addr) { bus_begin(addr,0);uint16_t result=raw_read16(addr);bus_end(addr);return result; }
void vb_write16(uint32_t addr,uint16_t value) { bus_begin(addr,1);raw_write16(addr,value);bus_end(addr); }

uint32_t vb_read32(uint32_t addr) { bus_begin(addr,0);uint32_t result=raw_read32(addr);bus_end(addr);return result; }
void vb_write32(uint32_t addr,uint32_t value) { bus_begin(addr,1);raw_write32(addr,value);bus_end(addr); }
