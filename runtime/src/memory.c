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

static uint8_t* s_rom = NULL;
static uint32_t s_rom_size = 0;
static uint8_t  s_wram[VB_WRAM_SIZE];

uint32_t vb_rom_size(void) { return s_rom_size; }
const uint8_t* vb_rom_data(void) { return s_rom; }
const uint8_t* vb_wram_data(void) { return s_wram; }

int vb_memory_init(const char* rom_path) {
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

    vb_vip_init();
    vb_vsu_init();
    vb_input_init();
    vb_irq_init();
    vb_timer_init();
    return 0;
}

void vb_memory_shutdown(void) {
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

uint8_t vb_read8(uint32_t addr) {
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
        case 6: vb_stub_abort("cart-RAM read8 not modelled (no save ROM)", 0, addr);
        case 7: return s_rom[rom_off(p)];
    }
    vb_stub_abort("vb_read8 fall-through", 0, addr);
}

uint16_t vb_read16(uint32_t addr) {
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
        case 6: vb_stub_abort("cart-RAM read16 not modelled (no save ROM)", 0, addr);
        case 7: {
            uint32_t o = rom_off(p);
            return (uint16_t)s_rom[o] | ((uint16_t)s_rom[o + 1] << 8);
        }
    }
    vb_stub_abort("vb_read16 fall-through", 0, addr);
}

uint32_t vb_read32(uint32_t addr) {
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
        case 6: vb_stub_abort("cart-RAM read32 not modelled", 0, addr);
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
void vb_write8(uint32_t addr, uint8_t v) {
    vb_wtrace_record(addr, (uint32_t)v, 1u);
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
        case 6: vb_stub_abort("cart-RAM write8 not modelled (no save ROM)", 0, addr);
        case 7: vb_stub_abort("write to cart ROM (read-only)", 0, addr);
    }
    vb_stub_abort("vb_write8 fall-through", 0, addr);
}

void vb_write16(uint32_t addr, uint16_t v) {
    vb_wtrace_record(addr, (uint32_t)v, 2u);
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
        case 6: vb_stub_abort("cart-RAM write16 not modelled", 0, addr);
        case 7: vb_stub_abort("write to cart ROM (read-only)", 0, addr);
    }
    vb_stub_abort("vb_write16 fall-through", 0, addr);
}

void vb_write32(uint32_t addr, uint32_t v) {
    vb_wtrace_record(addr, v, 4u);
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
        case 6: vb_stub_abort("cart-RAM write32 not modelled", 0, addr);
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
