#include "rom_patch.h"
#include <string.h>

static uint8_t* s_rom;
static uint32_t s_size;
static const VbRomDataPatch* s_plan;
static unsigned s_count;
static int s_enabled;

static int valid(const VbRomDataPatch* p, unsigned count) {
    if (!p || !count || count > 4096) return 0;
    for (unsigned i = 0; i < count; ++i) {
        if (!p[i].size || !p[i].source || !p[i].target ||
            p[i].offset > UINT32_MAX - p[i].size) return 0;
        for (unsigned j = 0; j < i; ++j)
            if (p[i].offset < p[j].offset + p[j].size &&
                p[j].offset < p[i].offset + p[i].size) return 0;
    }
    return 1;
}

static int apply(uint8_t* rom, uint32_t size, const VbRomDataPatch* p,
                 unsigned count, int enabled) {
    for (unsigned i = 0; i < count; ++i) {
        if (p[i].offset > size || p[i].size > size - p[i].offset) return 0;
        const uint8_t* current = rom + p[i].offset;
        if (memcmp(current, p[i].source, p[i].size) &&
            memcmp(current, p[i].target, p[i].size)) return 0;
    }
    for (unsigned i = 0; i < count; ++i)
        memcpy(rom + p[i].offset, enabled ? p[i].target : p[i].source, p[i].size);
    return 1;
}

int vb_rom_patch_select(const VbRomDataPatch* p, unsigned count, int enabled) {
    if (!valid(p, count)) return 0;
    if (s_enabled && (p != s_plan || count != s_count)) return 0;
    if (s_rom && !apply(s_rom, s_size, p, count, enabled)) return 0;
    s_plan = p; s_count = count; s_enabled = !!enabled;
    return 1;
}

int vb_rom_patch_attach(uint8_t* rom, uint32_t size) {
    if (!rom) { s_rom = 0; s_size = 0; return 1; }
    if (!size || (s_plan && !apply(rom, size, s_plan, s_count, s_enabled))) return 0;
    s_rom = rom; s_size = size;
    return 1;
}
