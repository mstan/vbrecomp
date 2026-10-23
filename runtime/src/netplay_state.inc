// Included inside netplay.cpp's private namespace. No new game wire: shared
// multi-receiver STATE transfer/probes provide retry, ownership and checksums.
// A proposal freezes transport admission briefly, then every driver advances
// to the proposed hold. No snapshot is applied until EVERY seat has validated.
// Transaction serials survive loads; stale/reordered probes cannot re-apply.
enum class StateStage {Idle,Proposal,Hold,Held,Hash,Commit,Applied,Release};
struct StateTransaction {
    StateStage stage=StateStage::Idle;
    uint32_t serial=0,target=0,started=0;
    int ui_slot=0;
    bool saving=false,valid=false,success=false;
    std::string path;
    std::vector<uint8_t> snapshot;
};
StateTransaction state_txn;
uint32_t state_serial;
int state_completed;
std::string state_message;
constexpr uint8_t state_hold=0x20,state_hash=0x40,state_commit=0x60,state_release=0x80,state_abort=0x90;
constexpr size_t online_limit=64u*1024u*1024u;
uint32_t state_word(const uint8_t* p){return p[0]|uint32_t(p[1])<<8|uint32_t(p[2])<<16|uint32_t(p[3])<<24;}
void state_word(std::vector<uint8_t>& v,uint32_t n){for(int i=0;i<4;++i)v.push_back(uint8_t(n>>(8*i)));}
int state_candidate_policy(void*){return vb_game_host_can_pause();}
bool state_candidate(const std::vector<uint8_t>& bytes) {
    return vb_state_validate(bytes.data(),bytes.size(),state_candidate_policy,nullptr)!=0;
}
bool state_capture(std::vector<uint8_t>& out) {
    uint8_t* data=nullptr;size_t size=0;
    if(!vb_state_capture(&data,&size,1))return false;
    std::unique_ptr<uint8_t,decltype(&std::free)> owned(data,&std::free);
    out.assign(data,data+size);return true;
}
// Online files carry the exact negotiated identity (including the dense seat
// map and fixed qualified profile). A raw local/racing snapshot is not a slot.
bool state_read(const char* path,std::vector<uint8_t>& out) {
    std::ifstream file(std::filesystem::u8path(path),std::ios::binary|std::ios::ate);
    const auto size=file.tellg();
    if(!file || size<20 || size>std::streamoff(online_limit))return false;
    std::vector<uint8_t> bytes(static_cast<size_t>(size));file.seekg(0);
    if(!file.read(reinterpret_cast<char*>(bytes.data()),bytes.size()))return false;
    const uint32_t text_size=state_word(bytes.data()+8),blob_size=state_word(bytes.data()+12);
    if(std::memcmp(bytes.data(),"VBVSNET1",8) || text_size!=identity.size()
       || uint64_t(20)+text_size+blob_size!=bytes.size()
       || std::memcmp(bytes.data()+20,identity.data(),text_size)
       || state_word(bytes.data()+16)!=hash(bytes.data()+20+text_size,blob_size))return false;
    out.assign(bytes.begin()+20+text_size,bytes.end());return state_candidate(out);
}
bool state_write(const char* path,const std::vector<uint8_t>& snapshot) {
    std::vector<uint8_t> bytes{'V','B','V','S','N','E','T','1'};
    state_word(bytes,uint32_t(identity.size()));state_word(bytes,uint32_t(snapshot.size()));
    state_word(bytes,hash(snapshot.data(),snapshot.size()));
    bytes.insert(bytes.end(),identity.begin(),identity.end());bytes.insert(bytes.end(),snapshot.begin(),snapshot.end());
    const auto destination=std::filesystem::u8path(path);
    auto temporary=destination;temporary+=".tmp";
    std::error_code ec;std::filesystem::create_directories(destination.parent_path(),ec);if(ec)return false;
    std::ofstream file(temporary,std::ios::binary|std::ios::trunc);
    file.write(reinterpret_cast<const char*>(bytes.data()),bytes.size());file.close();if(!file)return false;
#ifdef _WIN32
    return MoveFileExW(temporary.c_str(),destination.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)!=0;
#else
    std::filesystem::rename(temporary,destination,ec);return !ec;
#endif
}
bool state_probe(uint8_t phase,uint32_t size=0,uint32_t crc=0) {
    return rnet_session_state_probe(connection,RNET_STATE_OP_SAVE,uint8_t(phase|state_txn.ui_slot),
                                    size,size?crc:state_txn.serial)==0;
}
int state_fail(const char* why) {error=why;state_completed=-1;state_message=why;return -1;}
bool state_apply() {
    if(!state_txn.valid || !state_candidate(state_txn.snapshot)
       || !vb_state_restore(state_txn.snapshot.data(),state_txn.snapshot.size())) {
        std::fprintf(stderr,"vb_net: state apply validation failed valid=%d: %s\n",state_txn.valid,vb_state_error());return false;
    }
    // Keep the transport and its agreed profile. The shared resync increments
    // the INPUT and RB-control epoch, rejecting in-flight pre-load packets.
    rnet_rb_driver_destroy(driver);driver=nullptr;driver_started=false;
    vb_rollback_history_destroy(history);history=vb_rollback_history_create(40);
    if(!history){std::fprintf(stderr,"vb_net: state history allocation failed: %s\n",vb_state_error());return false;}
    rnet_session_hard_resync(connection);
    const uint8_t neutral_input[2]={0,0};rnet_session_prime_delay_inputs(connection,neutral_input,2);
    driver_started=start_driver(fixed_profile,fixed_transcript);
    return driver_started;
}
int state_request(const char* path,bool saving,int ui_slot) {
    state_completed=-1;state_message="Only the host can use online states between races";
    if(!path || !*path || ui_slot<0 || ui_slot>=12 || !vb_netplay_state_can_open() || state_serial==UINT32_MAX)return 0;
    StateTransaction next;next.saving=saving;next.ui_slot=ui_slot;next.path=path;
    if(!saving && !state_read(path,next.snapshot)) {
        state_message="Invalid, incompatible or racing online snapshot";return 0;
    }
    next.valid=true;
    const uint32_t tick=rnet_rb_driver_sim_tick(driver);
    if(tick>UINT32_MAX-64){state_message="Network tick limit reached";return 0;}
    next.serial=state_serial+1;next.target=tick+64;next.started=now_ms(nullptr);
    std::vector<uint8_t> proposal{'V','B','S','T','O','P','0','1'};
    state_word(proposal,next.serial);state_word(proposal,next.target);
    state_word(proposal,saving?1:0);state_word(proposal,uint32_t(ui_slot));
    proposal.insert(proposal.end(),next.snapshot.begin(),next.snapshot.end());
    if(rnet_session_state_begin(connection,RNET_STATE_OP_MENU,0,proposal.data(),proposal.size())) {
        state_message="Another synchronized operation is active";return 0;
    }
    state_serial=next.serial;state_txn=std::move(next);state_txn.stage=StateStage::Proposal;
    state_completed=0;state_message="Waiting for every racer";
    rnet_rb_driver_set_hold(driver,state_txn.target);return 1;
}
void state_finish_local(bool success) {
    rnet_rb_driver_clear_hold(driver);state_completed=success?1:-1;
    if(success)state_message=state_txn.saving?"Online slot saved":"Online slot loaded";
    else if(state_message=="Waiting for every racer")state_message="Online operation refused; race unchanged";
    std::fprintf(stderr,"vb_net: state serial=%u %s %s\n",state_txn.serial,
                 state_txn.saving?"save":"load",success?"complete":"refused");
    state_txn.stage=StateStage::Idle;state_txn.snapshot.clear();state_txn.path.clear();
}
// Returns 1 when the normal driver may run, 0 while the application barrier
// owns execution, -1 on a fatal protocol/apply/timeout error.
int state_poll() {
    if(state_txn.stage!=StateStage::Idle && uint32_t(now_ms(nullptr)-state_txn.started)>30000)
        return state_fail("Synchronized online state timed out");
    if(slot) {
        int from;uint8_t op,ui;const void* data;size_t size;
        if(rnet_session_state_take_ready_from(connection,&from,&op,&ui,&data,&size)) {
            if(from!=0 || op!=RNET_STATE_OP_MENU || ui || size<24 || size>online_limit
               || std::memcmp(data,"VBSTOP01",8))return state_fail("Invalid online state proposal");
            const auto* bytes=static_cast<const uint8_t*>(data);
            const uint32_t serial=state_word(bytes+8),target=state_word(bytes+12),saving=state_word(bytes+16),index=state_word(bytes+20);
            if(state_txn.stage!=StateStage::Idle || serial<=state_serial || !serial || saving>1 || index>=12
               || target<rnet_rb_driver_sim_tick(driver) || target-rnet_rb_driver_sim_tick(driver)>4096
               || (saving && size!=24))return state_fail("Stale or invalid online state proposal");
            state_txn={};state_txn.stage=StateStage::Hold;state_txn.serial=state_serial=serial;
            state_txn.target=target;state_txn.ui_slot=int(index);state_txn.saving=saving!=0;state_txn.started=now_ms(nullptr);
            state_txn.snapshot.assign(bytes+24,bytes+size);
            state_txn.valid=state_txn.saving || state_candidate(state_txn.snapshot);
            state_completed=0;state_message="Host is synchronizing an online slot";
            rnet_rb_driver_set_hold(driver,target);rnet_session_state_finish_from(connection,from,0);
        }
        uint32_t amount,crc;
        if(rnet_session_state_probe_pending(connection,&op,&ui,&amount,&crc)) {
            const uint8_t phase=ui&0xf0;const int index=ui&15;
            // Old probes are harmless even when reordered across a load.
            // They can never restore or reset a second time.
            if(op==RNET_STATE_OP_SAVE && index==state_txn.ui_slot && state_txn.stage==StateStage::Idle
               && crc==state_txn.serial && !amount && (phase==state_release || phase==state_abort)) {
                rnet_session_state_probe_reply(connection,1);
            } else if(op!=RNET_STATE_OP_SAVE || index!=state_txn.ui_slot || state_txn.stage==StateStage::Idle
               || (phase!=state_hash && crc!=state_txn.serial)) {
                rnet_session_state_probe_reply(connection,0);rnet_session_state_probe_finish(connection);
            } else if(phase==state_hold && !amount && state_txn.stage==StateStage::Hold) {
                if(rnet_rb_driver_hold_settled(driver)) {
                    state_txn.valid=state_txn.valid && vb_game_host_can_pause();
                    rnet_session_state_probe_reply(connection,state_txn.valid);state_txn.stage=StateStage::Held;
                }
            } else if(phase==state_hash && amount && state_txn.saving
                      && (state_txn.stage==StateStage::Held || state_txn.stage==StateStage::Hash)) {
                std::vector<uint8_t> current;
                const bool match=state_capture(current) && current.size()==amount && hash(current.data(),current.size())==crc;
                rnet_session_state_probe_reply(connection,match);state_txn.stage=StateStage::Hash;
            } else if(phase==state_commit && !amount && !state_txn.saving && state_txn.stage==StateStage::Held) {
                if(!state_apply())return state_fail("Cannot apply validated online snapshot");
                rnet_session_state_probe_reply(connection,1);state_txn.stage=StateStage::Applied;
            } else if((phase==state_release || phase==state_abort) && !amount) {
                rnet_session_state_probe_reply(connection,1);state_finish_local(phase==state_release);
            } else if((phase==state_hold && state_txn.stage!=StateStage::Hold)
                      || (phase==state_commit && state_txn.stage==StateStage::Applied)) {
                rnet_session_state_probe_reply(connection,state_txn.valid);
            } else if(phase==state_hash) {
                // A delayed hash from an earlier SAVE cannot apply a LOAD or
                // park its driver. The current host probe will be retried.
                rnet_session_state_probe_reply(connection,0);rnet_session_state_probe_finish(connection);
            } else return state_fail("Invalid online state barrier phase");
        }
        return state_txn.stage==StateStage::Idle || state_txn.stage==StateStage::Hold?1:0;
    }
    if(state_txn.stage==StateStage::Idle)return 1;
    if(state_txn.stage==StateStage::Proposal) {
        int from;uint8_t op,ui;const void* data;size_t size;
        if(!rnet_session_state_take_ready_from(connection,&from,&op,&ui,&data,&size))return 1;
        if(from!=0 || op!=RNET_STATE_OP_MENU || ui)return state_fail("Unexpected online state sender");
        rnet_session_state_finish(connection,0);
        if(!state_probe(state_hold))return state_fail("Cannot begin online hold barrier");
        state_txn.stage=StateStage::Hold;
    }
    if(state_txn.stage==StateStage::Hold) {
        if(!rnet_rb_driver_hold_settled(driver))return 1;
        int match;if(!rnet_session_state_probe_take_reply(connection,&match))return 1;
        rnet_session_state_probe_finish(connection);
        if(!match || !vb_game_host_can_pause()) {
            state_message="A racer left the between-race screen; operation refused";
            state_txn.success=false;
            if(!state_probe(state_abort))return state_fail("Cannot release refused online operation");
            state_txn.stage=StateStage::Release;
        } else if(state_txn.saving) {
            if(!state_capture(state_txn.snapshot) || !state_probe(state_hash,uint32_t(state_txn.snapshot.size()),
                                                               hash(state_txn.snapshot.data(),state_txn.snapshot.size())))
                return state_fail("Cannot verify the online save boundary");
            state_txn.stage=StateStage::Hash;
        } else {
            if(!state_apply() || !state_probe(state_commit))return state_fail("Cannot commit the validated online load");
            state_txn.stage=StateStage::Commit;
        }
    }
    if(state_txn.stage==StateStage::Hash || state_txn.stage==StateStage::Commit) {
        int match;if(!rnet_session_state_probe_take_reply(connection,&match))return 0;
        rnet_session_state_probe_finish(connection);
        if(!match && state_txn.stage==StateStage::Commit)return state_fail("A peer refused the online load commit");
        state_txn.success=match && (!state_txn.saving || state_write(state_txn.path.c_str(),state_txn.snapshot));
        if(!state_txn.success)state_message="Online slot refused or could not be written";
        if(!state_probe(state_txn.success?state_release:state_abort))return state_fail("Cannot release synchronized online slot");
        state_txn.stage=StateStage::Release;
    }
    if(state_txn.stage==StateStage::Release) {
        int match;if(!rnet_session_state_probe_take_reply(connection,&match))return 0;
        rnet_session_state_probe_finish(connection);
        if(!match)return state_fail("A peer refused the online release barrier");
        state_finish_local(state_txn.success);
    }
    return state_txn.stage==StateStage::Idle?1:0;
}
