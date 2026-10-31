/* Include the implementation only here to inject failing/poisoned allocation
 * without adding allocator hooks or linker assumptions to the public API. */
#include <stdlib.h>
#include <assert.h>
#include <string.h>
static unsigned allocations,releases;
static int fail_allocation;
static void* preview_allocate(size_t bytes) {
    ++allocations;
    if(fail_allocation)return NULL;
    void* result=malloc(bytes);assert(result);memset(result,0xa5,bytes);return result;
}
static void preview_release(void* ptr) {if(ptr)++releases;free(ptr);}
#define malloc preview_allocate
#define free preview_release
#include "../src/renderer.c"
#undef malloc
#undef free
#define main renderer_contract_main
#include "renderer_contract_test.c"
#undef main
int main(void) {
    assert(renderer_contract_main()==0);
    assert(allocations==1 && releases==0); /* Eligible previews reuse one cache. */
    vb_renderer_reset();assert(releases==1 && !thumbnail.allocation);
    assert(vb_renderer_register_local_pixels("allocation",render_local,&local_calls));viewport_local=1;
    uint32_t result[128*75];memset(result,0x72,sizeof(result));
    fail_allocation=1;
    assert(!vb_renderer_present_thumbnail(0,384,128,75,result) && allocations==1);
    assert(!vb_renderer_present_thumbnail(0,512,128,75,result) && allocations==2);
    for(unsigned i=0;i<128*75;++i)assert(result[i]==0x72727272u);
    fail_allocation=0;
    assert(vb_renderer_present_thumbnail(0,512,128,75,result) && allocations==3);
    assert(thumbnail.pixels==512u*224);
    void* original=thumbnail.allocation;
    fail_allocation=1;
    assert(!vb_renderer_present_thumbnail(0,768,128,75,result) && allocations==4);
    assert(thumbnail.allocation==original && releases==1);
    assert(vb_renderer_present_thumbnail(1,512,128,75,result) && allocations==4);
    fail_allocation=0;
    assert(vb_renderer_present_thumbnail(1,768,128,75,result) && allocations==5 && releases==2);
    assert(thumbnail.pixels==768u*224);
    vb_renderer_reset();assert(releases==3 && !thumbnail.allocation);
    puts("PASS: lazy width-sized preview storage, poisoned memory, failed first/growing allocation, unchanged output/fallback, cache reuse and reset release");
    return 0;
}
