/* vsu.h — Virtual Sound Unit surface.
 *
 * Phase 1: log + fatal-on-unknown. Phase 6 implements 6-channel
 * waveform synthesis.
 */
#ifndef VB_VSU_H
#define VB_VSU_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void vb_vsu_init(void);
void vb_vsu_shutdown(void);

uint8_t  vb_vsu_read8 (uint32_t addr);
uint16_t vb_vsu_read16(uint32_t addr);
uint32_t vb_vsu_read32(uint32_t addr);
void     vb_vsu_write8 (uint32_t addr, uint8_t  v);
void     vb_vsu_write16(uint32_t addr, uint16_t v);
void     vb_vsu_write32(uint32_t addr, uint32_t v);

#ifdef __cplusplus
}
#endif

#endif /* VB_VSU_H */
