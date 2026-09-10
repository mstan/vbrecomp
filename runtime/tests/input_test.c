#include "input.h"
#include "interrupts.h"
#include <assert.h>
static int pending;
void vb_irq_assert(uint32_t source,int asserted) { assert(source==VBIRQ_SOURCE_INPUT);pending=asserted; }
int main(void) {
    vb_input_init();vb_input_set_pad(VB_PAD_A|VB_PAD_START);
    assert(vb_input_read16(0x02000010)==0);
    assert(vb_input_read8(0x02000014)==0);
    vb_input_write8(0x02000028,4);
    vb_input_set_pad(0); /* Changing the physical pad cannot rewrite the latch. */
    assert(vb_input_read8(0x02000028)&2);
    vb_input_tick(10239);assert(!pending && (vb_input_read8(0x02000028)&2));
    vb_input_tick(1);assert(pending && !(vb_input_read8(0x02000028)&2));
    assert(vb_input_read16(0x02000010)==6 && vb_input_read8(0x02000014)==0x10);
    vb_input_write8(0x02000028,0x84);assert(!pending);
    vb_input_tick(10240);assert(!pending && !(vb_input_read8(0x02000028)&2));
    assert(vb_input_read16(0x02000010)==2 && vb_input_read8(0x02000014)==0);
    vb_input_write8(0x02000028,4);vb_input_tick(640);
    vb_input_write8(0x02000028,1);vb_input_tick(20000);
    assert(!pending && !(vb_input_read8(0x02000028)&2));
    vb_input_write8(0x02000028,4);assert(!(vb_input_read8(0x02000028)&2));
    vb_input_write8(0x02000028,4);assert(vb_input_read8(0x02000028)&2);
    return 0;
}
