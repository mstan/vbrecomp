#ifndef VB_DEVICE_DEBUG_H
#define VB_DEVICE_DEBUG_H
#include <stdint.h>
#include <stdio.h>
#ifdef VB_ORACLE_DEVICES
#define VB_DEVICE(name) beetle_##name##_snapshot
#else
#define VB_DEVICE(name) vb_##name##_snapshot
#endif
unsigned VB_DEVICE(timer)(uint32_t*);
unsigned VB_DEVICE(input)(uint32_t*);
unsigned VB_DEVICE(vip)(uint32_t*);
unsigned VB_DEVICE(vsu)(uint32_t*);
/* Explicit unsigned words avoid padding, native widths and host pointers.
 * Signed counters are represented by their 32-bit two's-complement bits. */
static void vb_device_response(char* body, size_t capacity) {
    const char* names[]={"timer","input","vip","vsu"};
    unsigned (*readers[])(uint32_t*)={VB_DEVICE(timer),VB_DEVICE(input),VB_DEVICE(vip),VB_DEVICE(vsu)};
    size_t used=(size_t)snprintf(body,capacity,"{\"ok\":true,\"schema\":1");
    for(unsigned group=0;group<4;++group) {
        uint32_t words[512];
        unsigned count=readers[group](words);
        used+=(size_t)snprintf(body+used,capacity-used,",\"%s\":[",names[group]);
        for(unsigned i=0;i<count;++i)
            used+=(size_t)snprintf(body+used,capacity-used,"%s%u",i ? ",":"",words[i]);
        used+=(size_t)snprintf(body+used,capacity-used,"]");
    }
    snprintf(body+used,capacity-used,"}");
}
#undef VB_DEVICE
#endif
