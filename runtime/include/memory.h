/* memory.h — Virtual Boy bus + memory map.
 *
 * Implements the 27-bit decoded address window documented in
 * docs/HARDWARE_NOTES.md. Every region routes to a typed handler;
 * unmapped accesses fatal-abort via vb_stub_abort().
 */
#ifndef VB_MEMORY_H
#define VB_MEMORY_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define VB_PHYS_MASK   0x07FFFFFFu

#define VB_VIP_BASE    0x00000000u
#define VB_VSU_BASE    0x01000000u
#define VB_MISC_BASE   0x02000000u
#define VB_CART_EXP    0x04000000u
#define VB_WRAM_BASE   0x05000000u
#define VB_CART_RAM    0x06000000u
#define VB_ROM_BASE    0x07000000u

#define VB_WRAM_SIZE   0x10000u    /* 64 KB */

/* Reset vector folds to 0x07FFFFF0 under VB_PHYS_MASK. */
#define VB_RESET_VECTOR 0xFFFFFFF0u

/* Returns 0 on success, non-zero on failure (file missing, too small, etc.). */
int vb_memory_init(const char* rom_path);
void vb_memory_shutdown(void);

uint8_t  vb_read8 (uint32_t addr);
uint16_t vb_read16(uint32_t addr);
uint32_t vb_read32(uint32_t addr);
void     vb_write8 (uint32_t addr, uint8_t  v);
void     vb_write16(uint32_t addr, uint16_t v);
void     vb_write32(uint32_t addr, uint32_t v);

/* Direct accessors for the debug server `read_ram` family. They route
 * through the same path as vb_read*, so an unmapped query aborts the
 * way an executed access would. */
size_t   vb_memory_dump(uint32_t addr, uint8_t* out, size_t len);

/* Introspection. */
uint32_t vb_rom_size(void);
const uint8_t* vb_rom_data(void);
const uint8_t* vb_wram_data(void);

#ifdef __cplusplus
}
#endif

#endif /* VB_MEMORY_H */
