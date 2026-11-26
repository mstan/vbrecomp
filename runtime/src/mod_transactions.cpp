#include "mod_runtime.h"
#include "memory.h"
#include "debug_server.h"
#include "recomp_mod.h"
#include <cstring>
#include <string>
#ifndef VB_GAME_ID
#define VB_GAME_ID ""
#define VB_GAME_SHA256 ""
#endif
extern "C" void vb_mod_runtime_publish_transaction_identity(const uint8_t value[32]);
namespace {
int begin(void*) { return vb_debug_server_is_paused(); }
bool range(const char* space,uint64_t at,size_t n) {
 if(!space || !n || at>UINT32_MAX || n>UINT32_MAX-at)return false;
 if(!std::strcmp(space,"cartridge"))return at<=vb_rom_size() && n<=vb_rom_size()-at;
 if(std::strcmp(space,"ram"))return false;
 return (at>=VB_WRAM_BASE && at+n<=VB_WRAM_BASE+VB_WRAM_SIZE) ||
        (at>=VB_CART_RAM && at+n<=VB_CART_RAM+65536);
}
int read(void*,const char* space,uint64_t at,uint8_t* out,size_t n) {
 if(!range(space,at,n))return 0;
 if(!std::strcmp(space,"cartridge"))std::memcpy(out,vb_rom_data()+at,n);
 else for(size_t i=0;i<n;++i)out[i]=vb_read8((uint32_t)(at+i));
 return 1;
}
int write(void*,const char* space,uint64_t at,const uint8_t* bytes,size_t n) {
 if(!range(space,at,n))return 0;
 // The disk input is never writable. This buffer is owned loaded cartridge data.
 if(!std::strcmp(space,"cartridge"))std::memcpy(const_cast<uint8_t*>(vb_rom_data())+at,bytes,n);
 else for(size_t i=0;i<n;++i)vb_write8((uint32_t)(at+i),bytes[i]);
 return 1;
}
void finish(void*,int) {}
}
extern "C" int vb_mod_guarded_patch(const char* space,uint32_t address,
 const uint8_t* expected,const uint8_t* replacement,unsigned size) {
 uint8_t identity[32],result[32];vb_mod_runtime_identity_c(identity);
 const char digits[]="0123456789abcdef";std::string hex(64,'0');
 for(unsigned i=0;i<32;++i){hex[i*2]=digits[identity[i]>>4];hex[i*2+1]=digits[identity[i]&15];}
 rmod_context* context=rmod_create();if(!context)return 0;
 int ok=rmod_bind_plan(context,VB_GAME_ID,VB_GAME_SHA256,hex.c_str());
 rmod_patch patch{space,address,expected,replacement,size};
 rmod_engine engine{nullptr,VB_GAME_ID,VB_GAME_SHA256,begin,read,write,nullptr,finish};
 if(ok)ok=rmod_apply(context,&patch,1,nullptr,0,&engine,result);
 if(ok>0)vb_mod_runtime_publish_transaction_identity(result);
 rmod_destroy(context);return ok;
}
