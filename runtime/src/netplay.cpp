#include "netplay.h"
#include "savestate.h"
#include "game_host.h"
#include "input.h"
#include "machine.h"
#include "ring_frame.h"
#include "host.h"
#include "vb_crc32.h"
#include "v810_interpreter.h"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <array>
#include <filesystem>
#include <fstream>
#include <cerrno>
#include <thread>
#include <vector>
#include <algorithm>
#include <memory>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
#if VB_RUNTIME_HAVE_SDL
#include <SDL.h>
#endif
#if VB_RUNTIME_HAVE_NETPLAY
#include "recomp_net/session.h"
#include "recomp_net/rb_driver.h"
#include "recomp_net/startup_profiles.h"
#if defined(RECOMP_LAUNCHER)
#include "recomp_netplay_host.h"
#include "recomp_net/host_ice.h"
#endif
#endif
#ifndef VB_GAME_TITLE
#define VB_GAME_TITLE "Virtual Boy"
#define VB_GAME_SHA256 ""
#endif

namespace {
bool enabled=false;
int players=2,slot=0,delay=2,prediction=6;
uint32_t session_id=1;
std::string bind_address=":19000",peer_address,error;
std::atomic<bool> resim{false};
uint16_t sampled_buttons;
bool input_override=false;
bool from_lobby=false,native_setup=false,relay_transport=false,ice_transport=false;
std::array<int,4> racer_port{0,1,2,3};
int auto_role=0;
int auto_capacity=0,auto_host_seat=-1;
std::string auto_endpoint,auto_name="Player";
bool auto_online=false;
std::string auto_path="ice",auto_server;
#if VB_RUNTIME_HAVE_NETPLAY && defined(RECOMP_LAUNCHER)
std::array<int,4> lobby_port{0,1,2,3};
std::string lobby_version,profile_directory;
RecompLauncherCNetplayCallbacks lobby_callbacks{};
const RecompLauncherCNetplayCallbacks* shared_callbacks;
std::array<RNetIceAdoptSeat,RNET_MAX_SLOTS> pending_ice{};
int pending_ice_count;
RNetIceAgent* pending_ice_guest;
uint32_t pending_ice_session;
int pending_ice_players,pending_ice_slot;
std::array<int,4> pending_ice_ports{};
void discard_pending_ice() {
    for(int i=0;i<pending_ice_count;++i)rnet_host_ice_destroy_agent(pending_ice[i].agent);
    pending_ice={};pending_ice_count=0;
    if(pending_ice_guest)rnet_host_ice_destroy_agent(pending_ice_guest);
    pending_ice_guest=nullptr;
    pending_ice_session=0;pending_ice_players=0;pending_ice_slot=-1;pending_ice_ports={};
}
bool pending_ice_matches(const RecompLauncherCNetplayLaunch* launch) {
    if(!launch || launch->player_count<2 || launch->player_count>4 || !launch->transport_ice_hub || pending_ice_session!=launch->session_id
       || pending_ice_players!=launch->player_count || pending_ice_slot!=launch->local_slot)return false;
    for(int i=0;i<launch->player_count;++i)
        if(pending_ice_ports[i]!=(launch->slot_port_valid?launch->slot_port[i]:i))return false;
    return launch->local_slot==0?pending_ice_count==launch->player_count-1:pending_ice_guest!=nullptr;
}
bool take_launch_ice(const RecompLauncherCNetplayLaunch* launch) {
    if(pending_ice_matches(launch))return true;
    discard_pending_ice();
    const int seat_count=launch->player_count,local=launch->local_slot;
    pending_ice_session=launch->session_id;pending_ice_players=seat_count;pending_ice_slot=local;
    for(int i=0;i<seat_count;++i)pending_ice_ports[i]=launch->slot_port_valid?launch->slot_port[i]:i;
    if(local==0) {
        RNetLobbyIceSeat seats[RNET_MAX_SLOTS]{};
        const int count=rnet_lobby_ice_take_hub(seats,RNET_MAX_SLOTS);
        if(count<0 || count>RNET_MAX_SLOTS){discard_pending_ice();return false;}
        pending_ice_count=count;bool valid=count==seat_count-1;unsigned mapped_mask=0;
        for(int i=0;i<count;++i) {
            int mapped=-1;for(int s=1;s<pending_ice_players;++s)if(pending_ice_ports[s]==seats[i].lobby_slot)mapped=s;
            pending_ice[i]={mapped,seats[i].agent};
            if(mapped<1 || !seats[i].agent || (mapped_mask&(1u<<mapped)))valid=false;
            else mapped_mask|=1u<<mapped;
        }
        if(valid)return true;
    } else {
        pending_ice_guest=rnet_lobby_ice_take_guest_agent();if(pending_ice_guest)return true;
    }
    discard_pending_ice();return false;
}
int no_spectators(void*){return 0;}
int yes_rollback(void*){return 1;}
int connect_online(void* context) {
    if(!rnet_host_ice_available()) {
        recomp_netplay_host_set_runtime_error("This build supports LAN and Direct IP. Online rooms require ICE support.");return -1;
    }
    return shared_callbacks->connect(context);
}
void fill_caps(void*,const RecompLauncherCSettings*,RNetLobbyMatchCaps* caps){caps->rollback=1;}
int profile_path(void*,const char* leaf,char* out,size_t capacity) {
    const auto path=(std::filesystem::u8path(profile_directory)/std::filesystem::u8path(leaf)).u8string();
    if(path.size()>=capacity)return 0;
    std::memcpy(out,path.c_str(),path.size()+1);return 1;
}
int load_name(void*,char* out,size_t capacity) {
    std::string name;std::ifstream file(std::filesystem::u8path(profile_directory)/"netplay-name.txt");std::getline(file,name);
    if(name.empty() || name.size()>=capacity)return 0;
    std::memcpy(out,name.c_str(),name.size()+1);return 1;
}
int store_name(void*,const char* name) {
    std::ofstream file(std::filesystem::u8path(profile_directory)/"netplay-name.txt");file<<name;return bool(file);
}
int fill_launch(void* context,RecompLauncherCNetplayLaunch* out) {
    if(!shared_callbacks->fill_launch(context,out))return 0;
    if(out->is_spectator || out->host_spectates || out->player_count<2 || out->player_count>4 || out->player_count>int(vb_game_host_netplay_players()) || !out->rollback) {
        recomp_netplay_host_set_runtime_error("This VS mode requires two to four seated racers and rollback.");return 0;
    }
    if(out->transport_ice_hub && !take_launch_ice(out)) {
        recomp_netplay_host_set_runtime_error("Waiting-room ICE agents are incomplete or have invalid seats.");return 0;
    }
    return 1;
}
#endif
#if VB_RUNTIME_HAVE_NETPLAY
RNetSession* connection;
RNetRbDriver* driver;
VbRollbackHistory* history;
CPUState* cpu;
uint32_t* dispatch_pc;
uint64_t* frame_cycles;
bool settled=false,driver_started=false,return_requested=false;
uint32_t started_ms;
uint32_t fixed_profile,fixed_transcript;
std::string identity;
uint32_t now_ms(void*) {
    return uint32_t(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}
uint32_t hash(const void* data,size_t size){return vb_crc32_compute(static_cast<const uint8_t*>(data),size);}
void sample(uint32_t,RNetInputSample* out,void*);
void session_publish(uint32_t,const RNetInputSample*,int,void*) {
    // The episode driver owns admission and publishes its sanitized rows.
    // RNetSession still requires this delay-sync callback at construction.
}
int save(void*,uint32_t tick){return vb_rollback_history_save(history,tick);}
int load(void*,uint32_t tick){return vb_rollback_history_load(history,tick);}
int has(void*,uint32_t tick){return vb_rollback_history_has(history,tick);}
int oldest(void*,uint32_t* tick){return vb_rollback_history_oldest(history,tick);}
void drop(void*,uint32_t tick){vb_rollback_history_drop_after(history,tick);}
void publish(void*,uint32_t tick,const RNetRbFrame* rows,int count,int) {
    VbGameInputs bundle{};bundle.network_tick=tick;
    for(int i=0;i<count && i<players;++i)bundle.buttons[racer_port[i]]=rows[i].buttons&0xfffc;
    // The native title/setup controller is the lobby authority even when its
    // racer occupies another dense position. Game rows follow lobby seats.
    vb_game_host_seal_inputs(&bundle);vb_input_set_pad(rows[0].buttons&0xfffc);
}
int replay_tick(void*,uint32_t){return vb_state_replay_frames(1);}
void resim_begin(void*){resim=true;}
void resim_end(void*){resim=false;}
uint32_t digest(void*){return vb_state_digest();}
void parts(void*,RNetRbDigestParts* out){out->master=digest(nullptr);for(auto& p:out->part)p=out->master;}
void decode(void*,int,const RNetInputSample* in,RNetRbFrame* out) {
    out->buttons=in->size==2?uint16_t(in->bytes[0]|uint16_t(in->bytes[1])<<8)&0xfffc:0;
}
void sanitize(void*,int,RNetRbFrame* row){row->buttons&=0xfffc;row->stick_x=row->stick_y=0;row->analog=row->rx=row->ry=0;}
void neutral(void*,int,RNetRbFrame* row){row->buttons=0;row->stick_x=row->stick_y=0;row->analog=row->rx=row->ry=0;}
void request_return(void*){return_requested=true;}
int check_identity(const char* want,char* reason,uint32_t capacity) {
    if(want && identity==want)return 0;
    std::snprintf(reason,capacity,"Virtual Boy build, ROM, active features or qualified startup profile differs.");return 1;
}
std::string hex(const uint8_t* bytes,size_t count) {
    std::string out(count*2,'0');const char* digits="0123456789abcdef";
    for(size_t i=0;i<count;++i){out[i*2]=digits[bytes[i]>>4];out[i*2+1]=digits[bytes[i]&15];}return out;
}
bool start_driver(uint32_t profile,uint32_t transcript) {
    uint8_t* snapshot=nullptr;size_t size=0;
    if(!vb_state_capture(&snapshot,&size,0))return false;
    // Full byte identities, not only lossy fingerprints. The driver's mod-set
    // handshake confirms this entire string before admitting frame zero.
    identity="vb-race1;build="+hex(snapshot+16,32)+";rom="+hex(snapshot+48,32)
        +";features="+hex(snapshot+88,24)+";profile="+std::to_string(profile)+":"+std::to_string(transcript);
    identity+=";ports=";
    for(int i=0;i<players;++i)identity+=std::to_string(racer_port[i])+",";
    std::free(snapshot);
    driver=rnet_rb_driver_create();if(!driver)return false;
    rnet_rb_driver_set_identity(driver,hash(vb_state_build_id(),std::strlen(vb_state_build_id())),hash(identity.data(),identity.size()));
    rnet_rb_driver_set_modset(driver,identity.c_str(),check_identity,nullptr);
    RNetRbHost host{};host.snap_save=save;host.snap_load=load;host.snap_has=has;host.snap_oldest=oldest;
    host.snap_drop_after=drop;host.publish=publish;host.run_tick=replay_tick;
    host.resim_begin=resim_begin;host.resim_end=resim_end;host.digest_master=digest;host.digest_parts=parts;
    host.decode_sample=decode;host.sanitize_row=sanitize;host.neutral_row=neutral;
    host.request_return_to_lobby=request_return;host.now_ms=now_ms;
    RNetRbDriverConfig config{};config.session=&connection;config.local_slot=&slot;config.slot_count=&players;
    config.input_delay=&delay;config.input_prediction=&prediction;config.occupied_mask=(1u<<players)-1;
    config.replay_mode=RNET_RB_REPLAY_INLINE;config.part_names[0]="machine";config.part_names[1]="machine";config.part_names[2]="machine";
    config.snap_depth=40;config.log_prefix="vb_rb";config.env_alias="VB_RB";config.inject_flip_bits=16;
    return rnet_rb_driver_start(driver,&config,&host)!=0;
}
#include "netplay_state.inc"
void sample(uint32_t,RNetInputSample* out,void*) {
    const uint16_t buttons=state_txn.stage==StateStage::Idle?sampled_buttons:0;
    out->size=2;out->bytes[0]=uint8_t(buttons);out->bytes[1]=uint8_t(buttons>>8);
}
#endif
}
extern "C" {
int vb_netplay_option(int argc,char** argv,int* index) {
    const std::string option=argv[*index];
    if(option.rfind("--net-",0)!=0)return 0;
#if !VB_RUNTIME_HAVE_NETPLAY
    error="This build has no netplay support";return -1;
#else
    if(*index+1>=argc){error="Missing network argument";return -1;}
    const char* value=argv[++*index];
#if defined(VBRECOMP_DEBUG_TOOLS)
    if(option=="--net-test-lobby-map") {
        if(std::strlen(value)!=size_t(players)){error="Test seat map must name every racer";return -1;}
        unsigned used=0;
        for(int i=0;i<players;++i) {
            const int port=value[i]-'0';
            if(port<0 || port>=players || (used&(1u<<port))){error="Invalid test seat permutation";return -1;}
            used|=1u<<port;racer_port[i]=port;
        }
        native_setup=true;enabled=true;return 1;
    }
#endif
#if defined(VBRECOMP_DEBUG_TOOLS)
    if(option=="--net-online-host"){auto_role=1;auto_endpoint=value;auto_online=true;}
    else if(option=="--net-online-join"){auto_role=2;auto_endpoint=value;auto_online=true;}
    else if(option=="--net-online-path") {
        auto_path=value;if(auto_path!="ice"){error="Current online rooms use ICE; LAN and Direct IP use UDP";return -1;}
    }
    else if(option=="--net-online-url")auto_server=value;
    else
#endif
    if(option=="--net-room-host"){auto_role=1;auto_endpoint=value;}
    else if(option=="--net-room-join"){auto_role=2;auto_endpoint=value;}
    else if(option=="--net-name")auto_name=value;
    else if(option=="--net-bind")bind_address=value;
    else if(option=="--net-peer")peer_address=value;
    else {
        errno=0;char* end=nullptr;const auto n=std::strtoul(value,&end,10);
        if(errno || !*value || *value=='-' || *end || n>UINT32_MAX){error="Invalid network number";return -1;}
        if(option=="--net-slot" && n<4)slot=int(n);
        else if(option=="--net-players" && n>=2 && n<=4)players=int(n);
        else if(option=="--net-delay" && n>=2 && n<=20)delay=int(n);
        else if(option=="--net-prediction" && n>=6 && n<=24)prediction=int(n);
        else if(option=="--net-session" && n)session_id=uint32_t(n);
#if defined(VBRECOMP_DEBUG_TOOLS)
        else if(option=="--net-test-room-capacity" && n>=2 && n<=4)auto_capacity=int(n);
        else if(option=="--net-test-host-seat" && n<4)auto_host_seat=int(n);
#endif
        else {error="Invalid network option or range";return -1;}
    }
    enabled=true;return 1;
#endif
}
int vb_netplay_enabled(){return enabled;}
int vb_netplay_players(){return enabled?players:0;}
int vb_netplay_view_seat(){return enabled?racer_port[slot]:-1;}
int vb_netplay_authority_seat(){return enabled?racer_port[0]:0;}
int vb_netplay_from_lobby(){return from_lobby;}
int vb_netplay_native_setup(){return native_setup;}
int vb_netplay_auto_requested(){return auto_role!=0;}
int vb_netplay_auto_launch(const char* config_path) {
#if VB_RUNTIME_HAVE_NETPLAY && defined(RECOMP_LAUNCHER)
    if(!auto_role)return 1;
    RecompLauncherCGameInfo game{};game.config_path=config_path;vb_netplay_configure_launcher(&game);
    if(!game.netplay){error="This title has no shared lobby adapter";return 0;}
    const auto* callbacks=game.netplay;callbacks->set_player_name(callbacks->ctx,auto_name.c_str());
    char endpoint[64],bind[64];
    if(!auto_online && auto_endpoint.size()>=sizeof(endpoint)){error="Room endpoint is too long";return 0;}
    std::snprintf(endpoint,sizeof(endpoint),"%s",auto_online?bind_address.c_str():auto_endpoint.c_str());
    std::snprintf(bind,sizeof(bind),"%s",bind_address.c_str());
    RecompLauncherCSettings settings{};
    if(auto_online && !from_lobby) {
        if(!auto_server.empty())callbacks->set_lobby_url(callbacks->ctx,auto_server.c_str());
        if(callbacks->connect(callbacks->ctx)){error="Cannot connect to shared online lobby";return 0;}
        const uint32_t waiting=now_ms(nullptr);
        while(!rnet_lobby_ready()) {
            callbacks->pump(callbacks->ctx);
            const char* problem=callbacks->last_error(callbacks->ctx);
            if(problem && *problem){error=problem;return 0;}
            if(uint32_t(now_ms(nullptr)-waiting)>30000){error="Shared online lobby did not become ready";return 0;}
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        if(auto_role==1) {
            if(auto_path=="ice" && !rnet_host_ice_available()){error="This build has no ICE support";return 0;}
            rnet_lobby_set_relay_host_pref(1);rnet_lobby_set_relay_via_ice(1);
            if(callbacks->create(callbacks->ctx,"Virtual Boy private qualification",endpoint,"vb-qualification-only",&settings,0,
                                 auto_capacity?auto_capacity:players)){error="Cannot create private online room";return 0;}
        } else {
            std::string id;
            while(id.empty()) {
                callbacks->pump(callbacks->ctx);std::ifstream file(std::filesystem::u8path(auto_endpoint));std::getline(file,id);
                if(uint32_t(now_ms(nullptr)-waiting)>50000){error="Private online fixture ID timed out";return 0;}
                if(id.empty())std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            if(id.size()>=RNET_LOBBY_ID_LEN || callbacks->join(callbacks->ctx,id.c_str(),"vb-qualification-only",bind)) {
                error="Cannot join private online room";return 0;
            }
        }
    } else if(!from_lobby && auto_role==1) {
        if(callbacks->create(callbacks->ctx,"Zero Racers VS",endpoint,"",&settings,1,auto_capacity?auto_capacity:players)) {
            error="Cannot create shared LAN room";return 0;
        }
    } else if(!from_lobby) {
        const std::string id="lr4:"+auto_endpoint;
        if(callbacks->join(callbacks->ctx,id.c_str(),"",bind)){error="Cannot join shared LAN room";return 0;}
    }
    const uint32_t opened=now_ms(nullptr);uint32_t last_ready=opened-200,last_diagnostic=opened-2000,last_start=opened-500;
    bool start_sent=false,moved_host=auto_host_seat<0,published=false;
    for(;;) {
        callbacks->pump(callbacks->ctx);
        const char* problem=callbacks->last_error?callbacks->last_error(callbacks->ctx):nullptr;
        if(problem && *problem) {
            if(auto_online && auto_role==1 && !std::strcmp(problem,"relay_unavailable")) {
                // A guest's completed link and its server path report can
                // arrive in either order. The server owns the start gate.
                callbacks->clear_last_error(callbacks->ctx);start_sent=false;
            } else {error=problem;return 0;}
        }
        if(callbacks->launch_pending(callbacks->ctx)) {
            RecompLauncherCNetplayLaunch launch{};
            if(!callbacks->fill_launch(callbacks->ctx,&launch)){error="Shared room supplied an invalid launch";return 0;}
            if(!vb_netplay_apply_launch(&launch))return 0;
            callbacks->clear_launch_pending(callbacks->ctx);
            std::fprintf(stderr,"vb_net: shared room launch players=%d slot=%d racer=%d session=%u\n",players,slot,racer_port[slot],session_id);
            std::fflush(stderr);
            return 1;
        }
        const uint32_t now=now_ms(nullptr);
        if(auto_online && now-last_diagnostic>=2000) {
            RNetHostIceStatus status{};rnet_lobby_host_ice_status(&status);
            const auto* caps=rnet_lobby_match_caps();
            std::fprintf(stderr,"vb_net: online waiting role=%d members=%d local_ready=%d all_ready=%d caps_ice=%d ice_role=%d completed=%d/%d\n",
                         auto_role,callbacks->member_count(callbacks->ctx),callbacks->local_ready(callbacks->ctx),callbacks->all_ready(callbacks->ctx),
                         caps?caps->relay_via_ice:0,status.role,status.completed,status.peer_count);
            for(int i=0;i<rnet_lobby_member_count();++i) {
                RNetLobbyMember member{};if(rnet_lobby_member_get(i,&member))
                    std::fprintf(stderr,"vb_net: online member seat=%d ready=%d path=%s fresh=%d host=%d\n",member.slot,member.ready,member.path,member.path_fresh,
                                 !std::strcmp(member.player_id,rnet_lobby_host_player_id()));
            }
            std::fflush(stderr);last_diagnostic=now;
        }
        if(now-opened>30000){error="Shared room timed out";return 0;}
        if(auto_online && auto_role==1 && !published && callbacks->in_lobby(callbacks->ctx)) {
            const auto* info=rnet_lobby_join_info();
            if(info && info->lobby_id[0]) {
                auto path=std::filesystem::u8path(auto_endpoint),temporary=path;temporary+=".tmp";
                std::ofstream file(temporary,std::ios::trunc);file<<info->lobby_id<<'\n';file.close();
                if(!file){error="Cannot publish private fixture room ID";return 0;}
#ifdef _WIN32
                if(!MoveFileExW(temporary.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)) {
                    error="Cannot publish private fixture room ID";return 0;
                }
#else
                std::error_code ec;std::filesystem::rename(temporary,path,ec);
                if(ec){error="Cannot publish private fixture room ID";return 0;}
#endif
                published=true;
            }
        }
        if(callbacks->in_lobby(callbacks->ctx) && !callbacks->local_ready(callbacks->ctx) && now-last_ready>=200) {
            callbacks->set_ready(callbacks->ctx,1);last_ready=now;
        }
        if(auto_role==1 && !moved_host && callbacks->member_count(callbacks->ctx)==players) {
            int host_seat=-1;
            for(int i=0;i<callbacks->member_count(callbacks->ctx);++i) {
                RecompLauncherCNetplayMember member{};
                if(callbacks->member_get(callbacks->ctx,i,&member) && member.is_host)host_seat=member.slot;
            }
            if(host_seat<0 || (host_seat!=auto_host_seat && callbacks->move_member(callbacks->ctx,host_seat,auto_host_seat))) {
                error="Shared room refused the test host move";return 0;
            }
            moved_host=true;
        }
        if(auto_role==1 && !start_sent && now-last_start>=500 && callbacks->member_count(callbacks->ctx)==players && callbacks->all_ready(callbacks->ctx)) {
            if(auto_online && auto_path=="ice") {
                RNetHostIceStatus status{};
                if(!rnet_lobby_host_ice_status(&status) || status.completed!=players-1 || status.peer_count!=players-1) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(2));continue;
                }
            }
            if(callbacks->request_start(callbacks->ctx,&settings)){error="Shared room refused start";return 0;}start_sent=true;last_start=now;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
#else
    error="This build has no shared launcher network host";return 0;
#endif
}
void vb_netplay_configure_launcher(RecompLauncherCGameInfo* game) {
#if VB_RUNTIME_HAVE_NETPLAY && defined(RECOMP_LAUNCHER)
    if(vb_game_host_netplay_players()<2)return;
    if(!shared_callbacks) {
        char version[32];std::snprintf(version,sizeof(version),"vb-vs1-%.20s",vb_state_build_id());lobby_version=version;
        profile_directory=std::filesystem::u8path(game->config_path).parent_path().u8string();
        RecompNetplayHostHooks hooks{};hooks.game_name=VB_GAME_TITLE;hooks.game_version=lobby_version.c_str();
        hooks.content_fingerprint=VB_GAME_SHA256;hooks.default_lobby_name=VB_GAME_TITLE;hooks.platform="vb";
        hooks.lan_registry_path="netplay-lan.txt";hooks.exe_dir_path=profile_path;hooks.name_load=load_name;hooks.name_store=store_name;
        hooks.max_players=hooks.netplay_max_players=int(vb_game_host_netplay_players());hooks.input_player=0;
        hooks.lan_max_players=int(vb_game_host_netplay_players());
        hooks.slot_policy=RECOMP_NETPLAY_SLOTS_HOST_FIRST;hooks.fill_match_caps=fill_caps;
        if(recomp_netplay_host_init(&hooks))return;
        shared_callbacks=recomp_netplay_host_callbacks();lobby_callbacks=*shared_callbacks;
        lobby_callbacks.fill_launch=fill_launch;lobby_callbacks.host_can_spectate=no_spectators;
        lobby_callbacks.allow_spectators_get=nullptr;lobby_callbacks.allow_spectators_set=nullptr;
        lobby_callbacks.automatch_available=no_spectators;lobby_callbacks.rollback_get=yes_rollback;lobby_callbacks.rollback_set=nullptr;
        lobby_callbacks.connect=connect_online;
        // Current online rooms use host ICE; LAN/Direct IP use local UDP.
        // Keep the room controls consistent with the server's actual path.
        lobby_callbacks.force_input_relay_get=nullptr;lobby_callbacks.force_input_relay_set=nullptr;
        lobby_callbacks.force_turn_get=nullptr;lobby_callbacks.force_turn_set=nullptr;
        lobby_callbacks.relay_host_get=nullptr;lobby_callbacks.relay_host_set=nullptr;
        lobby_callbacks.relay_via_ice_get=nullptr;lobby_callbacks.relay_via_ice_set=nullptr;
        rnet_lobby_set_relay_host_pref(1);rnet_lobby_set_relay_via_ice(1);
    }
    game->netplay_supported=1;game->netplay=&lobby_callbacks;
#endif
}
void vb_netplay_resume_launcher(RecompLauncherCGameInfo* game) {
#if VB_RUNTIME_HAVE_NETPLAY && defined(RECOMP_LAUNCHER)
    error.clear();recomp_netplay_host_begin_soft_return(game,1);
#else
    (void)game;
#endif
}
int vb_netplay_apply_launch(const RecompLauncherCNetplayLaunch* launch) {
#if VB_RUNTIME_HAVE_NETPLAY && defined(RECOMP_LAUNCHER)
    if(!launch || !launch->enabled) {
        discard_pending_ice();
        enabled=from_lobby=native_setup=relay_transport=ice_transport=false;
        sampled_buttons=0;input_override=false;racer_port={0,1,2,3};error.clear();return 1;
    }
    if(launch->is_spectator || launch->host_spectates || launch->player_count<2 || launch->player_count>4
       || launch->local_slot<0 || launch->local_slot>=launch->player_count || !launch->rollback) {
        error="Unsupported network launch";return 0;
    }
    players=launch->player_count;slot=launch->local_slot;
    unsigned used=0;
    for(int i=0;i<players;++i) {
        const int port=recomp_launcher_netplay_dense_position(launch,i);
        if(port<0 || port>=players || (used&(1u<<port))){error="Invalid lobby-to-racer mapping";return 0;}
        used|=1u<<port;racer_port[i]=port;lobby_port[i]=launch->slot_port_valid?launch->slot_port[i]:i;
    }
    delay=launch->input_delay;prediction=launch->input_prediction>=2?launch->input_prediction:4+delay;
    session_id=launch->session_id;bind_address=launch->bind_hostport;peer_address=launch->peer_hostport;
    relay_transport=launch->force_input_relay!=0;ice_transport=launch->transport_ice_hub!=0;
    // Consume waiting-room agents before the launcher clears its launch.
    // Keep ownership through ROM/window startup; session adoption is all-or-none.
    if(ice_transport && !take_launch_ice(launch)){error="Waiting-room ICE agents are incomplete or have invalid seats";return 0;}
    if(!ice_transport)discard_pending_ice();
    enabled=from_lobby=native_setup=true;return 1;
#else
    return !launch;
#endif
}
const char* vb_netplay_error(){return error.c_str();}
int vb_netplay_in_resim(){return resim.load(std::memory_order_relaxed);}
void vb_netplay_physical_input(uint16_t pad){if(!input_override)sampled_buttons=pad&0xfffc;}
void vb_netplay_override_input(uint16_t pad,int active){input_override=active!=0;sampled_buttons=active?pad&0xfffc:0;}
int vb_netplay_start(CPUState* c,uint32_t* pc,uint64_t* cycles) {
    if(!enabled)return 1;
#if VB_RUNTIME_HAVE_NETPLAY
    if(auto_role && !from_lobby){error="Automatic rooms require the shared launcher build";return 0;}
    for(const char* key:{"VB_RB_ALLOW_BOOT_FORK","RNET_RB_ALLOW_BOOT_FORK","VB_RB_ALLOW_MOD_MISMATCH","RNET_RB_ALLOW_MOD_MISMATCH"}) {
        const char* value=std::getenv(key);
        if(value && *value && std::strcmp(value,"0")){error="Network compatibility bypasses are not supported";return 0;}
    }
    if(slot>=players || players>int(vb_game_host_netplay_players()) || (slot && peer_address.empty() && !ice_transport)){error="Invalid owned network seat, unsupported title or missing host endpoint";return 0;}
    cpu=c;dispatch_pc=pc;frame_cycles=cycles;history=vb_rollback_history_create(40);
    if(!history){error=vb_state_error();return 0;}
    RNetConfig config{};rnet_config_init_defaults(&config);config.slot_count=uint8_t(players);config.local_slot=uint8_t(slot);
    config.input_delay=uint8_t(delay);config.session_id=session_id;config.occupied_mask=(1u<<players)-1;
    RNetHostVTable host{};host.sample_local=sample;host.publish=session_publish;
    connection=rnet_session_create(&config,&host);if(!connection){error="Cannot allocate RNet session";return 0;}
    RNetProfileCaps caps{};caps.backends=1|(vb_execution.mode!=VB_EXEC_INTERPRETER?2:0);caps.profiles=1;
#if VB_RUNTIME_HAVE_SDL
    caps.cpu_features=(SDL_HasSSE2()?1u:0)|(SDL_HasSSE41()?2u:0)|(SDL_HasAVX2()?4u:0)|(SDL_HasNEON()?8u:0);
#endif
#if defined(__x86_64__) || defined(_M_X64)
    caps.architecture=1;
#elif defined(__aarch64__) || defined(_M_ARM64)
    caps.architecture=2;
#elif defined(__i386__) || defined(_M_IX86)
    caps.architecture=3;
#endif
    if(rnet_session_profiles_begin(connection,0x56420101,&caps)!=0){error="Cannot configure startup profile";return 0;}
    int status=-1;
#if defined(RECOMP_LAUNCHER)
    if(ice_transport) {
        if(slot==0) {
            if(pending_ice_count==players-1)status=rnet_session_start_ice_hub_adopt(connection,pending_ice.data(),pending_ice_count);
            if(!status){pending_ice={};pending_ice_count=0;}
        } else {
            if(pending_ice_guest)status=rnet_session_adopt_ice_agent(connection,pending_ice_guest);
            if(!status)pending_ice_guest=nullptr;
        }
        if(status)discard_pending_ice();
    } else
#endif
    status=slot==0 && players>2 && !relay_transport?rnet_session_start_lan_hub(connection,bind_address.c_str()):
        rnet_session_start_lan(connection,bind_address.c_str(),peer_address.c_str());
    if(status){error="Cannot bind network transport";return 0;}
    std::fprintf(stderr,"vb_net: transport=%s session=%u players=%d slot=%d\n",ice_transport?"ice":relay_transport?"relay":"udp",session_id,players,slot);
    sampled_buttons=0;input_override=false;
    started_ms=now_ms(nullptr);settled=driver_started=return_requested=false;
    state_txn={};state_serial=0;state_completed=0;state_message.clear();return 1;
#else
    error="This build has no netplay support";return 0;
#endif
}
int vb_netplay_poll() {
#if VB_RUNTIME_HAVE_NETPLAY
#if defined(RECOMP_LAUNCHER)
    if(from_lobby)rnet_lobby_pump();
#endif
    rnet_session_pump(connection);
    if(!settled) {
        uint32_t profile,transcript;const int status=rnet_session_profiles_status(connection,&profile,&transcript);
        if(status<0 || uint32_t(now_ms(nullptr)-started_ms)>30000){error="Network startup profile refused or timed out";return -1;}
        if(!status)return 0;
        fixed_profile=profile;fixed_transcript=transcript;
        settled=true;driver_started=start_driver(profile,transcript);
        if(!driver_started){error="Cannot start shared rollback driver";return -1;}
        std::fprintf(stderr,"vb_net: settled profile=%u transcript=%08x slot=%d players=%d\n",profile,transcript,slot,players);
    }
    int state_status;
    try {state_status=state_poll();}
    catch(const std::exception&) {return state_fail("Cannot allocate or access synchronized online state");}
    if(state_status<0)return -1;
    if(!state_status)return 0;
    const auto admission=rnet_rb_driver_poll_admit(driver);
    if(return_requested){error=rnet_rb_driver_refusal(driver)?rnet_rb_driver_refusal(driver):"Network match refused";return -1;}
    uint32_t requested_tick;
    if(vb_game_host_lobby_return(&requested_tick)
       && rnet_rb_driver_sim_tick(driver)>requested_tick
       && rnet_rb_driver_confirmed_through(driver)>=requested_tick)return -2;
    if(rnet_session_disconnected_peers(connection,10000)){error="A network player disconnected";return -1;}
    if(admission==RNET_RB_ADMIT_STALL)return 0;
    if(!vb_machine_run_frame(cpu,dispatch_pc,frame_cycles)){error="Network field execution stopped";return -1;}
    vb_ring_frame_record(cpu);rnet_rb_driver_finish_frame(driver);return 1;
#else
    return -1;
#endif
}
void vb_netplay_stop() {
#if VB_RUNTIME_HAVE_NETPLAY
#if defined(RECOMP_LAUNCHER)
    discard_pending_ice();
#endif
    if(driver) {
        std::fprintf(stderr,"vb_net: final tick=%u confirmed=%u episodes=%u resim=%llu desync=%u\n",
            rnet_rb_driver_sim_tick(driver),rnet_rb_driver_confirmed_through(driver),rnet_rb_driver_episode_count(driver),
            (unsigned long long)rnet_rb_driver_resim_ticks(driver),rnet_rb_driver_desync_count(driver));
        rnet_rb_driver_destroy(driver);driver=nullptr;
    }
    if(connection){rnet_session_send_bye(connection);rnet_session_destroy(connection);connection=nullptr;}
    vb_rollback_history_destroy(history);history=nullptr;driver_started=false;resim=false;
    state_txn={};state_completed=0;state_message.clear();
#endif
}
int vb_netplay_hold(uint32_t tick) {
#if VB_RUNTIME_HAVE_NETPLAY
    if(!driver_started || state_txn.stage!=StateStage::Idle || tick<rnet_rb_driver_sim_tick(driver))return 0;
    rnet_rb_driver_set_hold(driver,tick);return 1;
#else
    return 0;
#endif
}
void vb_netplay_release() {
#if VB_RUNTIME_HAVE_NETPLAY
    if(driver_started && state_txn.stage==StateStage::Idle)rnet_rb_driver_clear_hold(driver);
#endif
}
int vb_netplay_debug(const char* command,long long id,char* out,size_t capacity) {
    if(std::strcmp(command,"net_status"))return 0;
#if VB_RUNTIME_HAVE_NETPLAY
    std::snprintf(out,capacity,"{\"ok\":true,\"id\":%lld,\"enabled\":%s,\"settled\":%s,\"slot\":%d,\"players\":%d,\"session\":%u,\"tick\":%u,\"confirmed\":%u,\"episodes\":%u,\"resim\":%llu,\"desync\":%u,\"held\":%s,\"state_busy\":%s,\"state_result\":%d}",
        id,enabled?"true":"false",settled?"true":"false",slot,players,session_id,
        driver?rnet_rb_driver_sim_tick(driver):0,driver?rnet_rb_driver_confirmed_through(driver):0,
        driver?rnet_rb_driver_episode_count(driver):0,(unsigned long long)(driver?rnet_rb_driver_resim_ticks(driver):0),
        driver?rnet_rb_driver_desync_count(driver):0,driver && rnet_rb_driver_hold_settled(driver)?"true":"false",
        state_txn.stage!=StateStage::Idle?"true":"false",state_completed);
#else
    std::snprintf(out,capacity,"{\"ok\":true,\"id\":%lld,\"enabled\":false}",id);
#endif
    return 1;
}
int vb_netplay_state_can_open() {
#if VB_RUNTIME_HAVE_NETPLAY
    return enabled && driver_started && slot==0 && vb_game_host_can_pause()
        && state_txn.stage==StateStage::Idle && !rnet_rb_driver_episode_active(driver);
#else
    return 0;
#endif
}
int vb_netplay_state_request(const char* path,int saving,int ui_slot) {
#if VB_RUNTIME_HAVE_NETPLAY
    try {return state_request(path,saving!=0,ui_slot);}
    catch(const std::exception&) {state_completed=-1;state_message="Cannot allocate or access online snapshot";return 0;}
#else
    return 0;
#endif
}
int vb_netplay_state_busy() {
#if VB_RUNTIME_HAVE_NETPLAY
    return state_txn.stage!=StateStage::Idle;
#else
    return 0;
#endif
}
int vb_netplay_state_result() {
#if VB_RUNTIME_HAVE_NETPLAY
    return state_completed;
#else
    return -1;
#endif
}
int vb_netplay_inspect_ready() {
#if VB_RUNTIME_HAVE_NETPLAY
    return enabled && driver_started && state_txn.stage==StateStage::Idle && rnet_rb_driver_hold_settled(driver);
#else
    return 0;
#endif
}
const char* vb_netplay_state_message() {
#if VB_RUNTIME_HAVE_NETPLAY
    return state_message.c_str();
#else
    return "Online snapshots are unavailable";
#endif
}
}
