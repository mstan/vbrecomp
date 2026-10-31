/* Allocation bounds and output/canonical-tail independence from drain chunks.
 * The source override is used only to demonstrate the frozen pre-fix defect. */
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef union Header {max_align_t alignment;struct {size_t bytes;uint64_t guard;} value;} Header;
static unsigned allocations,live;
static const uint64_t magic=UINT64_C(0x810b11fb0a29cafe);
static void check_guard(void* p) {
    Header* h=(Header*)p-1;assert(h->value.guard==magic);
    const unsigned char* end=(const unsigned char*)p+h->value.bytes;
    for(unsigned i=0;i<32;++i)assert(end[i]==0x5a);
}
static void guarded_free(void* p) {
    if(p){check_guard(p);Header* h=(Header*)p-1;h->value.guard=0;free(h);--live;}
}
static void* guarded_realloc(void* old,size_t bytes) {
    Header* h=(Header*)malloc(sizeof(*h)+bytes+32);assert(h);
    h->value.bytes=bytes;h->value.guard=magic;
    unsigned char* p=(unsigned char*)(h+1);
    memset(p,0xa5+(allocations++%16),bytes);memset(p+bytes,0x5a,32);++live;
    if(old) {
        check_guard(old);Header* before=(Header*)old-1;
        memcpy(p,old,bytes<before->value.bytes ? bytes:before->value.bytes);guarded_free(old);
    }
    return p;
}
#define realloc guarded_realloc
#define free guarded_free
#ifndef VB_BLIP_TEST_SOURCE
#define VB_BLIP_TEST_SOURCE "../src/Blip_Buffer.c"
#endif
#include VB_BLIP_TEST_SOURCE
#undef realloc
#undef free

static long drain(Blip_Buffer* b,blip_sample_t* pcm,long limit,long quantum) {
    long done=0,n;
    do {
        n=Blip_Buffer_read_samples(b,pcm+done*2,quantum);done+=n;
        assert(done<=limit);check_guard(b->buffer);
    } while(n==quantum);
    return done;
}
static int partitions(int partition_only) {
    Blip_Buffer a,b;Blip_Buffer_init(&a);Blip_Buffer_init(&b);
    assert(!Blip_Buffer_set_sample_rate(&a,44100,50));
    assert(!Blip_Buffer_set_sample_rate(&b,44100,50));
    if(!partition_only) {
        size_t requested=((Header*)a.buffer-1)->value.bytes;
        size_t expected=(a.buffer_size+blip_buffer_extra_)*sizeof(*a.buffer);
        if(requested!=expected){fprintf(stderr,"element allocation bytes: actual%zu expected%zu\n",requested,expected);return 1;}
    }
    Blip_Buffer_set_clock_rate(&a,5000000);Blip_Buffer_set_clock_rate(&b,5000000);
    Blip_Buffer_bass_freq(&a,20);Blip_Buffer_bass_freq(&b,20);
    Blip_Synth synth;Blip_Synth_set_volume(&synth,1.0/12,0x400);
    for(unsigned frame=0;frame<64;++frame) {
        blip_time_t clocks=frame%3 ? 39782:200000;
        for(blip_time_t t=0;t<clocks;t+=1379) {
            int delta=((t/1379+frame)&1) ? 101:-101;
            Blip_Synth_offset(&synth,t,delta,&a);Blip_Synth_offset(&synth,t,delta,&b);
        }
        Blip_Buffer_end_frame(&a,clocks);Blip_Buffer_end_frame(&b,clocks);
        blip_sample_t left[8192],right[8192];
        for(unsigned i=0;i<8192;++i)left[i]=right[i]=0x5a5a;
        long n=drain(&a,left,4096,256),m=drain(&b,right,4096,64);
        assert(n==m && !memcmp(left,right,sizeof(left)));
        for(unsigned i=1;i<8192;i+=2)assert(left[i]==0x5a5a);
        assert(a.offset==b.offset && a.reader_accum==b.reader_accum && a.modified==b.modified);
        size_t size=(a.buffer_size+blip_buffer_extra_)*sizeof(*a.buffer);
        if(memcmp(a.buffer,b.buffer,size)) {
            size_t i=0;while(i<size && ((unsigned char*)a.buffer)[i]==((unsigned char*)b.buffer)[i])++i;
            fprintf(stderr,"drain partition changes complete impulse buffer at byte%zu after frame%u; PCM was equal\n",i,frame);return 1;
        }
    }
    Blip_Buffer_deinit(&a);Blip_Buffer_deinit(&b);assert(!live);
    return 0;
}
int main(int argc,char** argv) {
    assert(argc==1 || (argc==2 && !strcmp(argv[1],"--partition-only")));
    int result=partitions(argc==2);if(result)return result;
    /* Resize allocation/clear uses the same element count and preserves guards. */
    Blip_Buffer b;Blip_Buffer_init(&b);
    for(int msec=1;msec<=100;msec+=9) {
        assert(!Blip_Buffer_set_sample_rate(&b,44100,msec));check_guard(b.buffer);
        assert(((Header*)b.buffer-1)->value.bytes==(b.buffer_size+blip_buffer_extra_)*sizeof(*b.buffer));
        for(long i=0;i<b.buffer_size+blip_buffer_extra_;++i)assert(b.buffer[i]==0);
    }
    Blip_Buffer_deinit(&b);assert(!live);
    puts("PASS: exact Blip element allocation, guarded clear/removal/resize,64 partition-invariant PCM and complete-tail frames");
    return 0;
}
