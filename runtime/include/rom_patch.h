/* Trusted, data-only cartridge assets. Never patch generated instructions. */
#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef struct VbRomDataPatch {
    uint32_t offset, size;
    const uint8_t *source, *target;
} VbRomDataPatch;

/* One exclusive ROM asset plan. Descriptors and payloads must remain alive
 * until disabled/replaced. Select before ROM load or on the paused runtime
 * thread. Every span is guarded before any write; source OR target is accepted
 * for idempotence/restoration. Disable the old plan before selecting another.
 * Return 0 on invalid/overlapping spans, bounds errors, or guard mismatch.
 * No file writes. Game authors must audit all spans against generated code. */
int vb_rom_patch_select(const VbRomDataPatch* patches, unsigned count, int enabled);

/* Host only: call after the loaded original ROM passes identity verification. */
int vb_memory_activate_rom_patches(void);

/* Memory lifecycle plumbing; not a plugin API. NULL detaches before free and
 * keeps the selected plan for the next verified load. */
int vb_rom_patch_attach(uint8_t* verified_rom, uint32_t size);
#ifdef __cplusplus
}
#endif
