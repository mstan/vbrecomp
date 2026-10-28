#ifndef VB_DRAW_INPUTS_H
#define VB_DRAW_INPUTS_H
#include <stdint.h>
/* Borrowed immutable eight-row capture. Owned by the viewport slot/row until
 * its next complete replacement. Native lazy jobs and expanded replay share
 * these exact inputs; no second DRAM/CHR copy or guest memory substitution. */
typedef struct VbVipDrawInputs {
    const uint16_t *dram,*chars,*spt,*suppress;
    const uint8_t (*gplt)[4],(*jplt)[4];
    unsigned backdrop;
} VbVipDrawInputs;
#ifdef __cplusplus
extern "C" {
#endif
/* Guest-thread-owned retention bits. A captured bank is borrowed until its
 * native row is materialized or its replacement event completes. */
extern uint32_t vb_vip_native_rows[2];
int vb_viewport_draw_inputs(int slot,int block,VbVipDrawInputs* out);
#ifdef __cplusplus
}
#endif
#endif
