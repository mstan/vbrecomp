/* Search/invalid-op behavior retained from the functioning LLE floor. */
static void vb_bitstring_search(CPUState* c, unsigned op) {
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
}
