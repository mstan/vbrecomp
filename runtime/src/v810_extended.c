#include "v810_interpreter.h"
#include "interrupts.h"
#include <math.h>
#include <fenv.h>
#include <string.h>

static float as_float(uint32_t u) { float f; memcpy(&f,&u,4); return f; }
static uint32_t as_bits(float f) { uint32_t u; memcpy(&u,&f,4); return u; }
static void fp_flags(CPUState* c, uint32_t r) {
    c->psw_ov=0; c->psw_z=(r&0x7fffffffu)==0;
    c->psw_s=c->psw_cy=c->psw_z ? 0:r>>31;
}
static int fp_input(CPUState* c, uint32_t u) {
    unsigned exp=(u>>23)&255;
    if ((u&0x7fffffffu) && (exp==0 || exp==255)) {
        c->psw_fro=1; vb_exception(c,VB_FPU_HANDLER,VB_ECODE_FRO); return 0;
    }
    return 1;
}
static void bitstring(CPUState* c, unsigned op) {
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
void vb_interpreter_extended(CPUState* c, uint16_t first, uint16_t second) {
    if ((first>>10)==0x1f) { bitstring(c,first&31); return; }
    unsigned r1=first&31, r2=(first>>5)&31, op=second>>10;
    uint32_t a=c->gpr[r2], b=c->gpr[r1], result=0;
    if (op>=8 && op<=12 && op!=11) {
        switch(op) {
        case 8: result=(a&0xffff0000u)|((a&255)<<8)|((a>>8)&255); c->cycles++; break;
        case 9: result=(a<<16)|(a>>16); c->cycles++; break;
        case 10:
            for(unsigned i=0;i<32;++i) result|=((b>>i)&1)<<(31-i);
            c->cycles+=21; break;
        default: result=(uint32_t)((int32_t)(int16_t)a*(int32_t)(int16_t)b); c->cycles+=8; break;
        }
        if(r2) c->gpr[r2]=result;
        c->pc+=4; return;
    }
    static const uint8_t extra[12]={6,0,5,8,8,11,7,43,0,0,0,7};
    if (op>11 || op==1 || op==8 || op==9 || op==10) {
        vb_exception(c,VB_INVALID_OP_HANDLER,VB_ECODE_INVALID_OP); return;
    }
    c->cycles+=extra[op];
    if (op!=2 && !fp_input(c,b)) return;
    if ((op==0 || (op>=4 && op<=7)) && !fp_input(c,a)) return;
    if (op==0) {
        c->psw_ov=0; c->psw_z=as_float(a)==as_float(b);
        c->psw_s=c->psw_cy=as_float(a)<as_float(b); c->pc+=4; return;
    }
    int saved_round=fegetround(); fesetround(FE_TONEAREST); feclearexcept(FE_ALL_EXCEPT);
    volatile float af=as_float(a), bf=as_float(b), rf=0;
    if(op==2) { volatile int32_t i=(int32_t)b; rf=(float)i; }
    else if(op==3 || op==11) {
        double rounded=op==3 ? nearbyint((double)bf):trunc((double)bf);
        if (rounded < -2147483648.0 || rounded > 2147483647.0) {
            fesetround(saved_round); c->psw_fiv=1; vb_exception(c,VB_FPU_HANDLER,VB_ECODE_FIV); return;
        }
        result=(uint32_t)(int32_t)rounded;
        if (rounded!=(double)bf) c->psw_fpr=1;
    } else switch(op) {
    case 4: rf=af+bf; break;
    case 5: rf=af-bf; break;
    case 6: rf=af*bf; break;
    case 7: rf=af/bf; break;
    }
    int flags=fetestexcept(FE_ALL_EXCEPT); fesetround(saved_round);
    if(flags&(FE_INVALID|FE_DIVBYZERO)) {
        if(flags&FE_INVALID) c->psw_fiv=1; else c->psw_fzd=1;
        vb_exception(c,VB_FPU_HANDLER,(flags&FE_INVALID)?VB_ECODE_FIV:VB_ECODE_FZD); return;
    }
    if(op==3 || op==11) { c->psw_ov=0; c->psw_z=result==0; c->psw_s=result>>31; }
    else {
        result=as_bits(rf);
        if(flags&FE_UNDERFLOW) { result&=0x80000000u; c->psw_fud=1; }
        fp_flags(c,result);
    }
    if(flags&FE_INEXACT) c->psw_fpr=1;
    if(r2) c->gpr[r2]=result;
    if(flags&FE_OVERFLOW) { c->psw_fov=1; vb_exception(c,VB_FPU_HANDLER,VB_ECODE_FOV); return; }
    c->pc+=4;
}
