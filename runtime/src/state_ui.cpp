/* The PSX/SNES host pattern: twelve thumbnail slots and a rewind filmstrip.
 * This module owns presentation/input state only, never serialized guest state.
 * All file loads and rewind commits go through the qualified snapshot API. */
#include "state_ui.h"
#include "savestate.h"
#include "renderer.h"
#include "recolor.h"
#include "vip.h"
#include "game_host.h"
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <deque>
#include <filesystem>
#include <string>
#include <vector>
#include <ctime>
#include <chrono>
#include <exception>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
#include "state_ui_font.h"

namespace {
namespace fs = std::filesystem;
constexpr int W=768,H=448,TW=128,TH=75,SLOTS=12;
using Thumb=std::array<uint32_t,TW*TH>;
struct Entry { uint32_t frame; Thumb image; };
struct Slot { bool exists=false,preview=false; Thumb image{}; std::string date; };
std::deque<Entry> history;
std::array<Slot,SLOTS> slots;
std::array<uint32_t,W*H> panel;
fs::path directory;
int mode,slot,selected,result;
std::string status;

FILE* open_file(const fs::path& path,const char* access) {
#ifdef _WIN32
    return _wfopen(path.c_str(),!std::strcmp(access,"rb")?L"rb":L"wb");
#else
    return std::fopen(path.c_str(),access);
#endif
}
fs::path slot_path(int n) {char name[32];std::snprintf(name,sizeof(name),"slot-%02d.state",n+1);return directory/name;}
bool header(const fs::path& path,std::array<uint8_t,112>& out) {
    FILE* f=open_file(path,"rb");if(!f)return false;
    bool ok=std::fread(out.data(),1,out.size(),f)==out.size();std::fclose(f);return ok;
}
void refresh_slots() {
    for(int i=0;i<SLOTS;++i) {
        auto& s=slots[i];s={};auto path=slot_path(i);std::error_code ec;
        s.exists=fs::exists(path,ec);if(!s.exists)continue;
        auto stamp=fs::last_write_time(path,ec);
        if(!ec) {
            auto system=std::chrono::time_point_cast<std::chrono::system_clock::duration>(
                stamp-fs::file_time_type::clock::now()+std::chrono::system_clock::now());
            auto time=std::chrono::system_clock::to_time_t(system);std::tm local{};
#ifdef _WIN32
            localtime_s(&local,&time);
#else
            localtime_r(&time,&local);
#endif
            char label[32];std::strftime(label,sizeof(label),"%Y-%m-%d %H:%M",&local);s.date=label;
        }
        std::array<uint8_t,112> expected{},actual{};
        if(!header(path,expected))continue;
        FILE* f=open_file(fs::u8path(path.u8string()+".thumb"),"rb");if(!f)continue;
        char magic[8];s.preview=std::fread(magic,1,8,f)==8 && !std::memcmp(magic,"VBTHUMB1",8)
            && std::fread(actual.data(),1,actual.size(),f)==actual.size() && actual==expected
            && std::fread(s.image.data(),sizeof(uint32_t),s.image.size(),f)==s.image.size()
            && std::fgetc(f)==EOF;
        std::fclose(f);
    }
}
Thumb capture() {
    int game_width=0,game_height=0;
    if(vb_game_host_video_size(-1,&game_width,&game_height)) {
        std::vector<uint32_t> pixels(size_t(game_width)*game_height);
        if(!vb_game_host_video_render(-1,0,pixels.data(),game_width,game_height))
            throw std::runtime_error("Game preview unavailable");
        const double scale=std::min(double(TW)/game_width,double(TH)/game_height);
        const int w=std::max(1,int(game_width*scale)),h=std::max(1,int(game_height*scale));
        Thumb image{};image.fill(0xff000000u);
        for(int y=0;y<h;++y)for(int x=0;x<w;++x)
            image[(y+(TH-h)/2)*TW+x+(TW-w)/2]=pixels[(y*game_height/h)*game_width+x*game_width/w]|0xff000000u;
        return image;
    }
    const int width=vb_renderer_present_width();std::vector<uint32_t> pixels((size_t)width*224);
    if(vb_renderer_active())vb_renderer_present_viewport(0,width,pixels.data());
    else if(vb_recolor_active())vb_vip_render_framebuffer_recolored(0,pixels.data());
    else vb_vip_render_framebuffer(0,pixels.data());
    Thumb image{};for(int y=0;y<TH;++y)for(int x=0;x<TW;++x)
        image[y*TW+x]=pixels[(y*224/TH)*width+x*width/TW]|0xff000000u;
    return image;
}
bool write_thumb(const fs::path& path,const Thumb& image) {
    std::array<uint8_t,112> identity{};if(!header(path,identity))return false;
    auto target=fs::u8path(path.u8string()+".thumb"),temporary=fs::u8path(target.u8string()+".tmp");
    FILE* f=open_file(temporary,"wb");if(!f)return false;
    bool ok=std::fwrite("VBTHUMB1",1,8,f)==8
        && std::fwrite(identity.data(),1,identity.size(),f)==identity.size()
        && std::fwrite(image.data(),sizeof(uint32_t),image.size(),f)==image.size();
    if(std::fclose(f))ok=false;
    if(!ok)return false;
#ifdef _WIN32
    return MoveFileExW(temporary.c_str(),target.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)!=0;
#else
    std::error_code ec;fs::rename(temporary,target,ec);return !ec;
#endif
}
void rect(int x,int y,int w,int h,uint32_t color) {
    for(int yy=std::max(0,y);yy<std::min(H,y+h);++yy)
        for(int xx=std::max(0,x);xx<std::min(W,x+w);++xx)panel[yy*W+xx]=color;
}
void border(int x,int y,int w,int h,uint32_t color) {
    rect(x,y,w,2,color);rect(x,y+h-2,w,2,color);rect(x,y,2,h,color);rect(x+w-2,y,2,h,color);
}
void text(int x,int y,const char* label,uint32_t color=0xffe2e5ebu,int scale=1) {
    for(;*label;++label,x+=8*scale) {
        unsigned c=(unsigned char)*label;if(c>='a'&&c<='z')c-=32;if(c<32||c>90)c='?';
        for(int yy=0;yy<8;++yy)for(int xx=0;xx<8;++xx)
            if(FONT8[c-32][yy]&(1u<<xx))rect(x+xx*scale,y+yy*scale,scale,scale,color);
    }
}
void button(int x,int y,const char* label,uint32_t color) {
    for(int yy=-9;yy<=9;++yy)for(int xx=-9;xx<=9;++xx)
        if(xx*xx+yy*yy<=81)rect(x+9+xx,y+9+yy,1,1,color);
    text(x+6,y+5,label,0xff171b25u);
}
void image(const Thumb& data,int x,int y,int w,int h) {
    for(int yy=0;yy<h;++yy)for(int xx=0;xx<w;++xx)
        if(x+xx>=0&&x+xx<W&&y+yy>=0&&y+yy<H)panel[(y+yy)*W+x+xx]=data[(yy*TH/h)*TW+xx*TW/w];
}
void fail(const char* reason) { result=-1;status=reason?reason:"Operation failed"; }
void close() { mode=0; }
void submit(bool save) {
    auto path=slot_path(slot);auto utf8=path.u8string();
    if(save) {
        std::error_code ec;fs::create_directories(directory,ec);
        if(ec){fail("Cannot create state directory");return;}
        auto preview=capture();
        if(!vb_state_save_file(utf8.c_str())){fail(vb_state_error());return;}
        bool ok=write_thumb(path,preview);refresh_slots();result=1;
        status=ok?"Saved slot "+std::to_string(slot+1):"Saved; preview unavailable";
    } else {
        if(!slots[slot].exists){fail("This slot is empty");return;}
        if(!vb_state_load_file(utf8.c_str())){fail(vb_state_error());return;}
        result=1;close();vb_state_ui_sync();
    }
}
}
extern "C" {
void vb_state_ui_init(const char* path) {
    vb_state_ui_shutdown();
    auto base=fs::absolute(fs::u8path(path)).parent_path();
    char content[65];for(int i=0;i<32;++i)std::snprintf(content+i*2,3,"%02x",vb_state_content_id()[i]);
    directory=base/"states"/content;
}
void vb_state_ui_shutdown(void){history.clear();slots={};mode=slot=selected=result=0;status.clear();}
int vb_state_ui_mode(void){return mode;}
int vb_state_ui_slot(void){return slot;}
int vb_state_ui_result(void){return result;}
unsigned vb_state_ui_history_count(void){return (unsigned)history.size();}
uint32_t vb_state_ui_selected_frame(void){return selected>=0 && (size_t)selected<history.size()?history[selected].frame:0;}
void vb_state_ui_sync(void) {
    history.erase(std::remove_if(history.begin(),history.end(),[](const Entry& e){return !vb_rewind_has(e.frame);}),history.end());
    selected=std::max(0,std::min(selected,(int)history.size()-1));
}
void vb_state_ui_record(uint32_t frame) {
    try {
    vb_state_ui_sync();if(!vb_rewind_has(frame))return;
    if(!history.empty()&&history.back().frame==frame)history.pop_back();
    history.push_back({frame,capture()});
    } catch(const std::exception&) {fail("Preview allocation failed");}
}
void vb_state_ui_select_slot(int n){if(mode==1&&n>=0&&n<SLOTS){slot=n;result=0;status.clear();}}
int vb_state_ui_action(int action) {
    try {
    if(action==VB_STATE_UI_SLOTS || action==VB_STATE_UI_REWIND) {
        if(!vb_game_host_can_pause())return 1;
        int next=action==VB_STATE_UI_SLOTS?1:2;
        if(mode==next){close();return 1;}
        mode=next;result=0;status.clear();vb_state_ui_sync();
        if(mode==1)refresh_slots();
        else {selected=std::max(0,(int)history.size()-2);if(history.size()<2)status=vb_rewind_enabled()?"History is collecting - return to the game":"Rewind is disabled - enable it in Settings";}
        return 1;
    }
    if(!mode)return 0;
    if(action==VB_STATE_UI_BACK){close();return 1;}
    if(mode==1) {
        if(action==VB_STATE_UI_LEFT||action==VB_STATE_UI_UP)vb_state_ui_select_slot((slot+SLOTS-1)%SLOTS);
        if(action==VB_STATE_UI_RIGHT||action==VB_STATE_UI_DOWN)vb_state_ui_select_slot((slot+1)%SLOTS);
        if(action==VB_STATE_UI_SAVE)submit(true);
        if(action==VB_STATE_UI_LOAD)submit(false);
    } else {
        if(action==VB_STATE_UI_LEFT||action==VB_STATE_UI_UP)selected=std::max(0,selected-1);
        if(action==VB_STATE_UI_RIGHT||action==VB_STATE_UI_DOWN)selected=std::max(0,std::min((int)history.size()-1,selected+1));
        if(action==VB_STATE_UI_LOAD&&!history.empty()) {
            if(vb_rewind_to(history[selected].frame)){result=1;close();vb_state_ui_sync();}
            else fail(vb_state_error());
        }
    }
    return 1;
    } catch(const std::exception&) {fail("Cannot access snapshot or preview");return 1;}
}
const uint32_t* vb_state_ui_render(int* width,int* height) {
    if(!mode)return nullptr;
    panel.fill(0xff0f1118u);char label[128];
    if(mode==1) {
        rect(0,0,W,44,0xff171b25u);text(24,14,"SAVE STATES",0xffffd24du,2);text(568,18,"F7 / SELECT+R",0xffb8bdc8u);
        int first=std::clamp(slot-1,0,SLOTS-3);
        for(int row=0;row<3;++row) {
            int i=first+row,y=56+row*110;auto& s=slots[i];bool active=i==slot;
            rect(24,y,720,102,active?0xff2b2830u:0xff191d27u);border(24,y,720,102,active?0xffffd24du:0xff303746u);
            std::snprintf(label,sizeof(label),"SLOT %02d",i+1);text(40,y+18,label,active?0xffffd24du:0xffe2e5ebu);
            if(s.preview)image(s.image,136,y+5,158,92);
            else {rect(136,y+5,158,92,0xff242a35u);text(176,y+45,s.exists?"SAVED":"NEW",0xff9aa3b2u);}
            text(320,y+26,s.exists?s.date.c_str():"EMPTY SLOT",0xffe2e5ebu,2);
            if(active)text(320,y+64,"SELECTED",0xffffd24du);
        }
        rect(0,390,W,58,0xff171b25u);
        button(24,395,"A",0xff6be06bu);text(50,400,"LOAD / ENTER");
        button(250,395,"X",0xff5fa8ffu);text(276,400,"SAVE / S");
        button(450,395,"B",0xffff6b6bu);text(476,400,"BACK / ESC");
        text(24,420,"ARROWS SELECT SLOT    1-9,0,-,= JUMP    SELECT+R MENU",0xffb8bdc8u);
    } else {
        if(!history.empty())image(history[selected].image,(W-290*TW/TH)/2,0,290*TW/TH,290);
        rect(0,290,W,158,0xff171b25u);text(20,300,"REWIND",0xffffd24du,2);
        double seconds=history.empty()?0:(history.back().frame-history[selected].frame)*397824.0/20000000.0;
        std::snprintf(label,sizeof(label),"-%.2fS  %d/%u",seconds,history.empty()?0:selected+1,(unsigned)history.size());
        text(440,305,label,0xffe2e5ebu);
        int first=std::clamp(selected-2,0,std::max(0,(int)history.size()-5));
        for(int i=first;i<std::min(first+5,(int)history.size());++i) {
            int x=20+(i-first)*148;image(history[i].image,x,328,136,80);
            border(x-2,326,140,84,i==selected?0xffffffffu:0xff404048u);
        }
        text(20,423,"LEFT / RIGHT SCRUB");
        button(240,414,"A",0xff6be06bu);text(266,420,"SELECT / ENTER");
        button(496,414,"B",0xffff6b6bu);text(522,420,"CANCEL / ESC");
    }
    if(!status.empty())text(24,436,status.substr(0,90).c_str(),result<0?0xffff6b6bu:0xffffd24du);
    if(width)*width=W;
    if(height)*height=H;
    return panel.data();
}
}
