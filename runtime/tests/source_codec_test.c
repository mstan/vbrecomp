#include "../src/source_codec.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static uint32_t seed=0xabcdef42u;
static uint32_t next(void){seed^=seed<<13;seed^=seed>>17;seed^=seed<<5;return seed;}
int main(void) {
    assert(sizeof(VbSourceTexel)==16 && sizeof(VbPackedSourceTexel)==12);
    for(unsigned i=0;i<250000;++i) {
        VbSourceTexel s={0};s.tile_hash=next();s.x=(uint16_t)next();s.y=(uint16_t)next();
        s.kind=(uint8_t)(i%5);s.raw=(uint8_t)(next()&3);s.world=(uint8_t)(next()%33);
        if(s.kind<3){s.tile=(uint16_t)(next()&2047);s.u=next()&7;s.v=next()&7;s.map=next()&15;}
        else{s.tile=(uint16_t)next();s.map=255;if(s.kind==3){s.u=(uint8_t)s.x;s.v=(uint8_t)s.y;}}
        VbSourceTexel out=vb_source_unpack(vb_source_pack(s));assert(!memcmp(&s,&out,sizeof(s)));
    }
    VbSourceTexel zero={0},out=vb_source_unpack(vb_source_pack(zero));assert(!memcmp(&zero,&out,sizeof(out)));
    puts("PASS:250000 exact compact provenance records; all BG/OBJ/CPU kinds, opaque tags, full coordinates/tile IDs and empty records");
    return 0;
}
