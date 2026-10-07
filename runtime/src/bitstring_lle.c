#include "v810_bitstring.h"
#include "interrupts.h"

void vb_bitstring_execute(CPUState* c, unsigned op) {
    if (op>=16 || (op>=4 && op<8)) {
        vb_exception(c,VB_INVALID_OP_HANDLER,VB_ECODE_INVALID_OP); return;
    }
    unsigned so=c->gpr[27]&31;
    uint32_t src=c->gpr[30]&~3u, length=c->gpr[28];
    if (op<4) {
        int delta=(op&1) ? -1:1, found=0;
        uint32_t skipped=c->gpr[29];
        while (length) {
            if (!c->bstr_src_valid) { c->bstr_src_cache=c->read32(src); c->bstr_src_valid=1; c->cycles+=5; }
            if (((c->bstr_src_cache>>so)&1)==((op>>1)&1)) {
                found=1; so-=delta;
                if (so&32) { src-=delta*4; so&=31; }
                break;
            }
            so=(so+delta)&31; ++skipped; --length;
            if (!so) { c->bstr_src_valid=0; src+=delta*4; break; }
        }
        c->gpr[27]=so; c->gpr[28]=length; c->gpr[29]=skipped; c->gpr[30]=src;
        if (found || !length) { c->psw_z=!found; c->pc+=2; c->bstr_src_valid=0; }
        return;
    }
    unsigned destoff=c->gpr[26]&31;
    uint32_t dest=c->gpr[29]&~3u;
    if (length) {
        uint32_t value=c->read32(dest); c->cycles+=4;
        do {
            if (!c->bstr_src_valid) { c->bstr_src_cache=c->read32(src); c->bstr_src_valid=1; c->cycles+=4; }
            unsigned bit=(c->bstr_src_cache>>so)&1;
            if (op&4) bit^=1;
            unsigned old=(value>>destoff)&1, result;
            switch(op&3) {
            case 0: result=old|bit; break;
            case 1: result=old&bit; break;
            case 2: result=old^bit; break;
            default: result=bit; break;
            }
            value=(value&~(1u<<destoff))|(result<<destoff);
            so=(so+1)&31; destoff=(destoff+1)&31; --length;
            if (!so) { src+=4; c->bstr_src_valid=0; }
        } while(length && destoff);
        c->write32(dest,value); c->cycles+=4;
        if (!destoff) dest+=4;
    }
    c->gpr[26]=destoff; c->gpr[27]=so; c->gpr[28]=length; c->gpr[29]=dest; c->gpr[30]=src;
    if (!length) { c->pc+=2; c->bstr_src_valid=0; }
}

const char *vb_bitstring_implementation(void) { return "LLE"; }
