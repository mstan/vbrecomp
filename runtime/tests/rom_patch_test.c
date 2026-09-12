#include "rom_patch.h"
#include <assert.h>
#include <string.h>

int main(void) {
    uint8_t rom[] = {0,1,2,3,4,5,6,7};
    const uint8_t original[] = {0,1,2,3,4,5,6,7};
    const uint8_t a[] = {1,2}, b[] = {11,12}, c[] = {5,6}, d[] = {15,16};
    const VbRomDataPatch p[] = {{1,2,a,b},{5,2,c,d}};
    assert(vb_rom_patch_select(p,2,1));
    assert(!memcmp(rom,original,8)); /* Pending until verified attachment. */
    assert(vb_rom_patch_attach(rom,8));
    assert(rom[1]==11 && rom[5]==15 && rom[0]==0 && rom[7]==7);
    assert(vb_rom_patch_select(p,2,1)); /* Repeated enable. */
    assert(vb_rom_patch_select(p,2,0));
    assert(!memcmp(rom,original,8));
    rom[5]=99;
    assert(!vb_rom_patch_select(p,2,1)); /* Last guard fails: no first write. */
    assert(rom[1]==1 && rom[5]==99);
    rom[5]=5;
    const VbRomDataPatch overlap[] = {{1,2,a,b},{2,2,c,d}};
    assert(!vb_rom_patch_select(overlap,2,1));
    const VbRomDataPatch overflow[] = {{UINT32_MAX,2,a,b}};
    assert(!vb_rom_patch_select(overflow,1,1));
    const VbRomDataPatch outside[] = {{7,2,a,b}};
    assert(!vb_rom_patch_select(outside,1,1));
    assert(!vb_rom_patch_select(0,0,1));
    assert(!memcmp(rom,original,8));
    assert(vb_rom_patch_select(p,2,1));
    assert(!vb_rom_patch_select(outside,1,1)); /* Active exclusive plan. */
    assert(vb_rom_patch_attach(0,0));
    memcpy(rom,original,8);
    assert(vb_rom_patch_attach(rom,8)); /* Reload uses selected language. */
    assert(rom[1]==11 && rom[5]==15);
    assert(vb_rom_patch_select(p,2,0));
    assert(!memcmp(rom,original,8));
    assert(vb_rom_patch_attach(0,0));
    assert(vb_rom_patch_select(outside,1,1));
    assert(!vb_rom_patch_attach(rom,8));
    assert(!memcmp(rom,original,8));
    return 0;
}
