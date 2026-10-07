#include "v810_bitstring.h"
#include "interrupts.h"
#include "bitstring_search.h"

/* Replace logical bit-string microsteps and repeated instruction dispatch with
 * one native buffer operation. Preserve bus ordering, cached-source overlap
 * semantics, complete register results and the floor's aggregate cycle charge.
 * Device scheduling/interrupt observation happens on operation completion. */
void vb_bitstring_execute(CPUState *c, unsigned op) {
    if (op < 8 || op >= 16) { vb_bitstring_search(c, op); return; }
    unsigned so = c->gpr[27] & 31, destoff = c->gpr[26] & 31;
    uint32_t src = c->gpr[30] & ~3u, dest = c->gpr[29] & ~3u;
    uint32_t length = c->gpr[28];
    while (length) {
        uint32_t value = c->read32(dest); c->cycles += 4;
        do {
            if (!c->bstr_src_valid) {
                c->bstr_src_cache = c->read32(src);
                c->bstr_src_valid = 1; c->cycles += 4;
            }
            unsigned count = 32 - destoff;
            if (count > 32 - so) count = 32 - so;
            if (count > length) count = length;
            uint32_t mask = count == 32 ? UINT32_MAX : (1u << count) - 1;
            uint32_t bits = (c->bstr_src_cache >> so) & mask;
            if (op & 4) bits ^= mask;
            uint32_t destmask = mask << destoff, shifted = bits << destoff;
            switch (op & 3) {
            case 0: value |= shifted; break;
            case 1: value &= shifted | ~destmask; break;
            case 2: value ^= shifted; break;
            default: value = (value & ~destmask) | shifted; break;
            }
            length -= count; so += count; destoff += count;
            if (so == 32) { so = 0; src += 4; c->bstr_src_valid = 0; }
            if (destoff == 32) destoff = 0;
        } while (length && destoff);
        c->write32(dest, value); c->cycles += 4;
        if (!destoff) dest += 4;
        /* The caller charges the first BSU base cycle. Account for the base
         * charge of each removed destination-word redispatch as well, so a
         * shorter host operation does not artificially advance more frames. */
        if (length) ++c->cycles;
    }
    c->gpr[26] = destoff; c->gpr[27] = so; c->gpr[28] = length;
    c->gpr[29] = dest; c->gpr[30] = src;
    c->pc += 2; c->bstr_src_valid = 0;
}
const char *vb_bitstring_implementation(void) { return "HLE"; }
