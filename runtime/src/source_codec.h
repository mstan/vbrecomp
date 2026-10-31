#ifndef VB_SOURCE_CODEC_H
#define VB_SOURCE_CODEC_H
#include "renderer.h"

/* Engine-produced provenance, not a replacement for the public full record.
 * BG0/1/2 use 11-bit CHR, 3-bit u/v and a 4-bit map. OBJ3 has map255 and
 * u/v equal to the low bytes of x/y (including arbitrary extra-sprite tile
 * IDs). CPU4 preserves its opaque32-bit tag, map255 and zero u/v. */
typedef struct VbPackedSourceTexel { uint32_t words[3]; } VbPackedSourceTexel;
static inline VbPackedSourceTexel vb_source_pack(VbSourceTexel s) {
    uint32_t meta=((uint32_t)s.kind<<21)|((uint32_t)s.raw<<24)|((uint32_t)s.world<<26);
    if(s.kind>=3)meta|=s.tile;
    else meta|=s.tile|((uint32_t)s.u<<11)|((uint32_t)s.v<<14)|((uint32_t)s.map<<17);
    VbPackedSourceTexel out={{s.tile_hash,(uint32_t)s.x|((uint32_t)s.y<<16),meta}};return out;
}
static inline VbSourceTexel vb_source_unpack(VbPackedSourceTexel p) {
    const uint32_t bits=p.words[2];
    VbSourceTexel s={0};s.tile_hash=p.words[0];s.x=(uint16_t)p.words[1];s.y=(uint16_t)(p.words[1]>>16);
    s.kind=(uint8_t)((bits>>21)&7);s.raw=(uint8_t)((bits>>24)&3);s.world=(uint8_t)(bits>>26);
    if(s.kind>=3){s.tile=(uint16_t)bits;s.map=255;if(s.kind==3){s.u=(uint8_t)s.x;s.v=(uint8_t)s.y;}}
    else{s.tile=(uint16_t)(bits&2047);s.u=(uint8_t)((bits>>11)&7);s.v=(uint8_t)((bits>>14)&7);s.map=(uint8_t)((bits>>17)&15);}
    return s;
}
#endif
