#include "game_host.h"
#include "input.h"
#include <string.h>

static VbGameHost host;
static VbGameInputs inputs;
static int initialized,explicit_inputs;
int vb_game_host_register(const VbGameHost* adapter) {
    if(!adapter || !adapter->id || !*adapter->id || host.id || initialized)return 0;
    host=*adapter;return 1;
}
int vb_game_host_option(int argc,char** argv,int* index) {
    int candidate,result;
    if(!host.option || !index || *index<1 || *index>=argc)return 0;
    candidate=*index;result=host.option(argc,argv,&candidate,host.context);
    if(result==1) {
        if(candidate<*index || candidate>=argc)return -1;
        *index=candidate;
    }
    return result;
}
const char* vb_game_host_error(void) {
    const char* error=host.error?host.error(host.context):0;
    return error && *error?error:"Invalid game option or session initialization";
}
int vb_game_host_initialize(CPUState* cpu) {
    if(initialized || !cpu)return 0;
    initialized=1;
    return !host.initialize || host.initialize(cpu,host.context);
}
void vb_game_host_seal_inputs(const VbGameInputs* sealed) {
    explicit_inputs=sealed!=0;
    if(sealed)inputs=*sealed;else memset(&inputs,0,sizeof(inputs));
}
void vb_game_host_end_field(CPUState* cpu) {
    if(!host.end_field || !initialized)return;
    if(!explicit_inputs) {
        // Existing single-controller callers retain their guest pad behavior.
        // Multiplayer hosts/rollback explicitly supply every occupied seat.
        memset(&inputs,0,sizeof(inputs));inputs.buttons[0]=vb_input_get_pad();
    }
    host.end_field(cpu,&inputs,host.context);
}
int vb_game_host_can_pause(void) {return !host.can_pause || host.can_pause(host.context);}
int vb_game_host_debug(const char* command,long long id,char* out,size_t capacity) {
    return host.debug && host.debug(command,id,out,capacity,host.context);
}
