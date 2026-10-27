#include "wram_hash.h"
#include <assert.h>
#include <string.h>
int main(void) {
    uint32_t fnv[VB_WRAMHASH_REGIONS];
    vb_wram_hash_reset();assert(vb_wram_hash_recording());
    unsigned count=0;
    while(vb_wram_hash_recording()) {
        for(unsigned i=0;i<VB_WRAMHASH_REGIONS;++i)fnv[i]=count*71+i;
        vb_wram_hash_record(fnv);++count;assert(count<=32768);
    }
    assert(count==32768 && vb_wram_hash_head()==count);
    vb_wramhash_rec first,last,after;uint64_t head,lo;
    assert(vb_wram_hash_read_abs(0,&first,1,&head,&lo)==1 && !lo && head==count);
    assert(vb_wram_hash_read_abs(count-1,&last,1,0,0)==1);
    for(unsigned i=0;i<VB_WRAMHASH_REGIONS;++i) {
        assert(first.fnv[i]==i && last.fnv[i]==(count-1)*71+i);
    }
    memset(fnv,0xff,sizeof(fnv));vb_wram_hash_record(fnv);
    assert(!vb_wram_hash_recording() && vb_wram_hash_head()==count);
    assert(vb_wram_hash_read_abs(count-1,&after,1,0,0)==1 && !memcmp(&last,&after,sizeof(last)));
    vb_wram_hash_reset();assert(vb_wram_hash_recording() && !vb_wram_hash_head());
    vb_wram_hash_record(fnv);assert(vb_wram_hash_head()==1);
    return 0;
}
