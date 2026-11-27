#include "diagnostics_service.h"
#include "mod_runtime.h"
#include "replay_writer.h"
#include "recomp_persistence/atomic_file.h"
#include <filesystem>
#include <memory>
#include <string>
#include <chrono>
#include <cstdio>
namespace {
std::filesystem::path recording_root;
bool enabled=true;
std::unique_ptr<recomp::diagnostics::ReplayWriter> writer;
void reset(){enabled=false;vb_diagnostics_shutdown();}
void activate(){enabled=true;}
VB_MOD_CONSTRUCTOR(register_diagnostics) {
 vb_mod_register_reset_callback(reset);
 vb_mod_register_activation_plugin("vb.gameplay-diagnostics",activate);
}
}
int vb_diagnostics_install(const char* root,const char* game,const char* sha) {
 try {
 auto base=std::filesystem::u8path(root);recording_root=base.parent_path()/"recordings";
 auto manifest=base/"packages/vb-gameplay-diagnostics/1.0.0/manifest.toml";
 std::filesystem::create_directories(manifest.parent_path());
 std::string text=R"(format_version = 1
id = "vb-gameplay-diagnostics"
version = "1.0.0"
name = "Gameplay diagnostics"
author = "Recomp contributors"
description = "Record gameplay inputs for reproducible diagnostics."

[[target]]
game_id = ")"+std::string(game)+"\"\nrom_sha256 = \""+sha+R"("

[[feature]]
id = "recording"
name = "Gameplay diagnostics and input recording"
description = "Keep a bounded background record of frame inputs. Your on/off choice is remembered independently of visual mods."
group = "Diagnostics"
default_enabled = true

[[plugin]]
feature = "recording"
id = "vb.gameplay-diagnostics"
)";
 return recomp_atomic_file_store(manifest.u8string().c_str(),text.data(),text.size());
 }catch(...){return 0;}
}
int vb_diagnostics_enabled(){return enabled;}
void vb_diagnostics_record(uint32_t frame,uint16_t pad) {
 if(!enabled)return;
 if(!writer) {
  std::error_code ec;std::filesystem::create_directories(recording_root,ec);
  writer=recomp::diagnostics::ReplayWriter::create(recomp::diagnostics::ReplayWriter::Format::Hashed64);
  auto stamp=std::chrono::system_clock::now().time_since_epoch().count();
  auto path=recording_root/("inputs-"+std::to_string(stamp)+".vbreplay");
  if(!writer || !writer->begin(path.u8string(),"VBINPUT1\n",{},{})){enabled=false;writer.reset();return;}
 }
 char line[80];std::snprintf(line,sizeof(line),"I %u %u\n",frame,pad);
 if(!writer->line(line)){std::fprintf(stderr,"Diagnostics input recording stopped: %s\n",writer->error());enabled=false;}
}
void vb_diagnostics_shutdown(){if(writer){writer->end();writer->shutdown();writer.reset();}}
