/* vsu.c — VSU Phase 1 surface.
 *
 * Same shape as vip.c: a register shadow plus fatal-on-unknown. The
 * VSU register region lives around 0x01000000..0x010005FF for the
 * channel + wave-RAM blocks; we shadow the first 8 KB and abort
 * beyond.
 */
#include "vsu.h"

#include <string.h>

#include "stub_abort.h"

static uint8_t s_reg_shadow[0x2000];

static int in_reg_window(uint32_t phys) {
    return phys >= 0x01000000u && phys < 0x01002000u;
}

void vb_vsu_init(void) { memset(s_reg_shadow, 0, sizeof(s_reg_shadow)); }
void vb_vsu_shutdown(void) {}

uint8_t vb_vsu_read8(uint32_t addr) {
    if (in_reg_window(addr)) return s_reg_shadow[addr - 0x01000000u];
    vb_stub_abort("VSU read8 outside Phase 1 register window", 0, addr);
}

uint16_t vb_vsu_read16(uint32_t addr) {
    if (in_reg_window(addr)) {
        uint32_t o = addr - 0x01000000u;
        return (uint16_t)s_reg_shadow[o] | ((uint16_t)s_reg_shadow[o + 1] << 8);
    }
    vb_stub_abort("VSU read16 outside Phase 1 register window", 0, addr);
}

uint32_t vb_vsu_read32(uint32_t addr) {
    if (in_reg_window(addr)) {
        uint32_t o = addr - 0x01000000u;
        return  (uint32_t)s_reg_shadow[o]
              | ((uint32_t)s_reg_shadow[o + 1] << 8)
              | ((uint32_t)s_reg_shadow[o + 2] << 16)
              | ((uint32_t)s_reg_shadow[o + 3] << 24);
    }
    vb_stub_abort("VSU read32 outside Phase 1 register window", 0, addr);
}

void vb_vsu_write8(uint32_t addr, uint8_t v) {
    if (in_reg_window(addr)) { s_reg_shadow[addr - 0x01000000u] = v; return; }
    vb_stub_abort("VSU write8 outside Phase 1 register window", 0, addr);
}

void vb_vsu_write16(uint32_t addr, uint16_t v) {
    if (in_reg_window(addr)) {
        uint32_t o = addr - 0x01000000u;
        s_reg_shadow[o]     = (uint8_t)(v & 0xFF);
        s_reg_shadow[o + 1] = (uint8_t)((v >> 8) & 0xFF);
        return;
    }
    vb_stub_abort("VSU write16 outside Phase 1 register window", 0, addr);
}

void vb_vsu_write32(uint32_t addr, uint32_t v) {
    if (in_reg_window(addr)) {
        uint32_t o = addr - 0x01000000u;
        s_reg_shadow[o]     = (uint8_t)(v & 0xFF);
        s_reg_shadow[o + 1] = (uint8_t)((v >> 8) & 0xFF);
        s_reg_shadow[o + 2] = (uint8_t)((v >> 16) & 0xFF);
        s_reg_shadow[o + 3] = (uint8_t)((v >> 24) & 0xFF);
        return;
    }
    vb_stub_abort("VSU write32 outside Phase 1 register window", 0, addr);
}
