#define VB_ORACLE_DEVICES 1
#include "device_debug.h"
/* beetle_debug_server.c — TCP/JSON debug server for vb-beetle.exe.
 *
 * Mirrors the wire protocol of `runtime/src/debug_server.c` (the
 * vb-runtime debug server). Per Rule 14, identical commands on a
 * different port — a tool that talks to vb-runtime works unchanged
 * against vb-beetle just by switching ports.
 *
 * Wire-shape command set:
 *
 *     ping          — liveness
 *     frame         — Beetle's emulated frame count
 *     pad_state     — Beetle's current pad mask
 *     read_ram      — WRAM / cart-RAM / cart-ROM read (VIP/VSU not
 *                     exposed by mednafen-vb without a patch)
 *     memory_map    — descriptive memory layout
 *     pause/continue— suspend retro_run() driven from main loop
 *     quit          — clean shutdown
 *     get_registers — UNAVAILABLE (V810 internals are static inside
 *                     mednafen-vb; surface as a structured error
 *                     rather than silently faking zeros)
 *
 * The transport (winsock/POSIX, line-based, JSON-ish responses) is
 * structurally the same as the runtime debug server — separate file
 * because the two processes have different state to expose and we
 * don't want #ifdefs in a single source.
 */
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vb_beetle.h"
#include "png_write.h"

#if defined(_WIN32)
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  if defined(_MSC_VER)
#    pragma comment(lib, "ws2_32.lib")
#  endif
   typedef SOCKET sock_t;
#  define VBB_BAD_SOCKET INVALID_SOCKET
#  define vbb_close_socket(s) closesocket(s)
#  define vbb_socket_errno() WSAGetLastError()
#  define VBB_EWOULDBLOCK WSAEWOULDBLOCK
#else
#  include <sys/types.h>
#  include <sys/socket.h>
#  include <netinet/in.h>
#  include <arpa/inet.h>
#  include <fcntl.h>
#  include <unistd.h>
#  include <errno.h>
   typedef int sock_t;
#  define VBB_BAD_SOCKET (-1)
#  define vbb_close_socket(s) close(s)
#  define vbb_socket_errno() (errno)
#  define VBB_EWOULDBLOCK EWOULDBLOCK
#endif


#include "debug_stream.h"

static sock_t s_listener = VBB_BAD_SOCKET;
static sock_t s_client   = VBB_BAD_SOCKET;
static int    s_paused      = 0;
static int    s_quit        = 0;
static int    s_winsock_inited = 0;
static uint32_t s_step_target = 0;
static int s_input_override;
int vb_beetle_debug_input_override(void) { return s_input_override; }

void vb_beetle_debug_set_paused(int paused) {
    s_paused = paused != 0;
    s_step_target = 0;
}
int vb_beetle_debug_is_paused(void) {
    if (s_step_target && vb_beetle_frame_count() >= s_step_target) {
        s_paused = 1;
        s_step_target = 0;
    }
    return s_paused;
}
int vb_beetle_debug_should_quit(void) { return s_quit && !vb_stream.size; }


static int set_nonblocking(sock_t s) {
#if defined(_WIN32)
    u_long mode = 1;
    return ioctlsocket(s, FIONBIO, &mode) == 0 ? 0 : -1;
#else
    int flags = fcntl(s, F_GETFL, 0);
    if (flags < 0) return -1;
    return fcntl(s, F_SETFL, flags | O_NONBLOCK);
#endif
}


int vb_beetle_debug_server_start(int port) {
    s_paused = 0;
    s_quit = 0;

#if defined(_WIN32)
    if (!s_winsock_inited) {
        WSADATA wsa;
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return -1;
        s_winsock_inited = 1;
    }
#endif

    s_listener = socket(AF_INET, SOCK_STREAM, 0);
    if (s_listener == VBB_BAD_SOCKET) return -2;

    int yes = 1;
    setsockopt(s_listener, SOL_SOCKET, SO_REUSEADDR,
               (const char*)&yes, sizeof(yes));

    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t)port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    if (bind(s_listener, (struct sockaddr*)&a, sizeof(a)) != 0) {
        vbb_close_socket(s_listener); s_listener = VBB_BAD_SOCKET;
        return -3;
    }
    if (listen(s_listener, 1) != 0) {
        vbb_close_socket(s_listener); s_listener = VBB_BAD_SOCKET;
        return -4;
    }
    if (set_nonblocking(s_listener) != 0) {
        vbb_close_socket(s_listener); s_listener = VBB_BAD_SOCKET;
        return -5;
    }
    return 0;
}


void vb_beetle_debug_server_stop(void) {
    if (s_client != VBB_BAD_SOCKET) {
        vbb_close_socket(s_client); s_client = VBB_BAD_SOCKET;
    }
    if (s_listener != VBB_BAD_SOCKET) {
        vbb_close_socket(s_listener); s_listener = VBB_BAD_SOCKET;
    }
#if defined(_WIN32)
    if (s_winsock_inited) {
        WSACleanup();
        s_winsock_inited = 0;
    }
#endif
}


/* ---- shared with vb-runtime debug_server.c ----
 * The two servers do their own copies rather than sharing because
 * tying them at the source level would couple two binaries whose
 * job is to be *independently* runnable (Rule 14).
 */

static int extract_str(const char* line, const char* key, char* out, size_t max) {
    const char* p = strstr(line, key);
    if (!p) return 0;
    p = strchr(p, ':');
    if (!p) return 0;
    p++;
    while (*p == ' ' || *p == '\t') p++;
    if (*p != '"') return 0;
    p++;
    size_t n = 0;
    while (*p && *p != '"' && n + 1 < max) { out[n++] = *p++; }
    out[n] = 0;
    return 1;
}

static int extract_int(const char* line, const char* key, long long* out) {
    const char* p = strstr(line, key);
    if (!p) return 0;
    p = strchr(p, ':');
    if (!p) return 0;
    p++;
    while (*p == ' ' || *p == '\t' || *p == '"') p++;
    int neg = 0;
    if (*p == '-') { neg = 1; p++; }
    int base = 10;
    if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) { base = 16; p += 2; }
    char* end = NULL;
    long long v = strtoll(p, &end, base);
    if (end == p) return 0;
    *out = neg ? -v : v;
    return 1;
}

static long long s_request_id;
static void send_response(const char* body) {
    if(body[0]=='{' && !strstr(body,"\"id\":")) {
        size_t size=strlen(body)+64;char* reply=(char*)malloc(size);
        if(!reply) { vb_stream.failed=1;return; }
        snprintf(reply,size,"{\"id\":%lld,%s",s_request_id,body+1);
        vb_stream_queue(reply);free(reply);
    } else vb_stream_queue(body);
}

/* ---- Handlers ---- */

static void handle_ping(long long id) {
    char buf[160];
    snprintf(buf, sizeof(buf),
        "{\"ok\":true,\"cmd\":\"ping\",\"id\":%lld,\"vbrecomp\":\"0.1.0\","
        "\"role\":\"oracle\",\"backend\":\"beetle-vb\"}",
        id);
    send_response(buf);
}

static void handle_frame(long long id) {
    char buf[160];
    snprintf(buf, sizeof(buf),
        "{\"ok\":true,\"cmd\":\"frame\",\"id\":%lld,\"frame\":%u}",
        id, (unsigned)vb_beetle_frame_count());
    send_response(buf);
}

static void handle_pad_state(long long id) {
    char buf[160];
    snprintf(buf, sizeof(buf),
        "{\"ok\":true,\"cmd\":\"pad_state\",\"id\":%lld,\"pad\":\"0x%04X\"}",
        id, (((unsigned)~vb_beetle_get_pad() << 2) & 0xfffc) | 2u);
    send_response(buf);
}

static void handle_memory_map(long long id) {
    char buf[640];
    snprintf(buf, sizeof(buf),
        "{\"ok\":true,\"cmd\":\"memory_map\",\"id\":%lld,\"rom_size\":%u,"
        "\"regions\":["
            "{\"name\":\"VIP\",\"base\":\"0x00000000\",\"size\":\"0x01000000\","
                "\"readable\":false},"
            "{\"name\":\"VSU\",\"base\":\"0x01000000\",\"size\":\"0x01000000\","
                "\"readable\":false},"
            "{\"name\":\"MISC\",\"base\":\"0x02000000\",\"size\":\"0x01000000\","
                "\"readable\":false},"
            "{\"name\":\"WRAM\",\"base\":\"0x05000000\",\"size\":\"0x00010000\","
                "\"readable\":true},"
            "{\"name\":\"CART_RAM\",\"base\":\"0x06000000\",\"size\":\"0x01000000\","
                "\"readable\":true},"
            "{\"name\":\"CART_ROM\",\"base\":\"0x07000000\",\"size\":\"0x01000000\","
                "\"readable\":true}"
        "]}", id, (unsigned)vb_beetle_rom_size());
    send_response(buf);
}

static void handle_read_ram(long long id, const char* line) {
    long long addr = 0, len = 16;
    extract_int(line, "\"addr\"", &addr);
    extract_int(line, "\"len\"", &len);
    if (addr<0 || addr>UINT32_MAX || len<1 || len>65536 ||
        (uint64_t)addr+(uint64_t)len>0x100000000ull) {
        send_response("{\"ok\":false,\"error\":\"invalid read range\"}");return;
    }
    uint8_t* tmp = (uint8_t*)malloc((size_t)len);
    if (!tmp) {
        send_response("{\"ok\":false,\"error\":\"oom\"}");
        return;
    }
    size_t filled = vb_beetle_read_memory((uint32_t)addr, tmp, (size_t)len);
    char* body = (char*)malloc(256 + (size_t)len * 2);
    if (!body) {
        free(tmp);
        send_response("{\"ok\":false,\"error\":\"oom\"}");
        return;
    }
    char* p = body;
    p += sprintf(p,
        "{\"ok\":true,\"cmd\":\"read_ram\",\"id\":%lld,"
        "\"addr\":\"0x%08X\",\"len\":%u,\"filled\":%u,\"hex\":\"",
        id, (unsigned)addr, (unsigned)len, (unsigned)filled);
    for (size_t i = 0; i < filled; ++i) p += sprintf(p, "%02X", tmp[i]);
    p += sprintf(p, "\"}");
    send_response(body);
    free(body);
    free(tmp);
}

extern int vb_beetle_cpu_state(uint32_t*, uint32_t*, uint32_t*);
static void handle_get_registers(long long id) {
    uint32_t pc, gpr[32], sr[32];
    if (!vb_beetle_cpu_state(&pc,gpr,sr)) { send_response("{\"ok\":false,\"error\":\"no CPU\"}"); return; }
    char body[1400];
    int used=snprintf(body,sizeof(body),"{\"ok\":true,\"id\":%lld,\"pc\":\"0x%08X\",\"psw\":\"0x%08X\",\"gpr\":[",id,pc,sr[5]);
    for(int i=0;i<32;++i) used+=snprintf(body+used,sizeof(body)-used,"%s\"0x%08X\"",i?",":"",gpr[i]);
    snprintf(body+used,sizeof(body)-used,"],\"eipc\":\"0x%08X\",\"eipsw\":\"0x%08X\",\"fepc\":\"0x%08X\",\"fepsw\":\"0x%08X\",\"ecr\":\"0x%08X\",\"frame\":%u}",sr[0],sr[1],sr[2],sr[3],sr[4],vb_beetle_frame_count());
    send_response(body);
}

static void handle_screenshot(long long id, const char* line) {
    char path[256] = {0};
    long long eye=0;
    extract_int(line,"\"eye\"",&eye);
    extract_str(line, "\"path\"", path, sizeof(path));
    if (!path[0]) snprintf(path, sizeof(path), "vb-beetle-fb.png");

    const uint32_t* pixels = NULL;
    unsigned w = 0, h = 0;
    int have = vb_beetle_get_eye((int)eye,&pixels, &w, &h);
    if (!have || !pixels || !w || !h) {
        char body[256];
        snprintf(body, sizeof(body),
            "{\"ok\":false,\"cmd\":\"screenshot\",\"id\":%lld,"
            "\"error\":\"no framebuffer yet (cart hasn't produced a frame)\"}",
            id);
        send_response(body);
        return;
    }
    int rc = vb_write_png_32bpp(path, (int)w, (int)h, pixels);
    char body[384];
    if (rc != 0) {
        snprintf(body, sizeof(body),
            "{\"ok\":false,\"cmd\":\"screenshot\",\"id\":%lld,"
            "\"error\":\"failed to write %s\"}", id, path);
    } else {
        snprintf(body, sizeof(body),
            "{\"ok\":true,\"cmd\":\"screenshot\",\"id\":%lld,"
            "\"path\":\"%s\",\"width\":%u,\"height\":%u}",
            id, path, w, h);
    }
    send_response(body);
}

static void handle_vip_state(long long id) {
    vb_beetle_vip_state vs;
    memset(&vs, 0, sizeof(vs));
    vb_beetle_get_vip_state(&vs);
    char buf[768];
    snprintf(buf, sizeof(buf),
        "{\"ok\":true,\"cmd\":\"vip_state\",\"id\":%lld,"
        "\"intpnd\":\"0x%04X\",\"intenb\":\"0x%04X\","
        "\"dpctrl\":\"0x%04X\",\"xpctrl\":\"0x%04X\","
        "\"frmcyc\":%u,\"bkcol\":%u,"
        "\"brta\":%u,\"brtb\":%u,\"brtc\":%u,\"rest\":%u,"
        "\"spt\":[%u,%u,%u,%u],"
        "\"gplt\":[%u,%u,%u,%u],"
        "\"jplt\":[%u,%u,%u,%u],"
        "\"_note\":\"DPSTTS/XPSTTS/state-machine vars not exposed by "
            "mednafen-vb without upstream patch\"}",
        id,
        vs.intpnd, vs.intenb, vs.dpctrl, vs.xpctrl,
        vs.frmcyc, vs.bkcol,
        vs.brta, vs.brtb, vs.brtc, vs.rest,
        vs.spt[0], vs.spt[1], vs.spt[2], vs.spt[3],
        vs.gplt[0], vs.gplt[1], vs.gplt[2], vs.gplt[3],
        vs.jplt[0], vs.jplt[1], vs.jplt[2], vs.jplt[3]);
    send_response(buf);
}

/* Stream a window of the always-on Beetle audio ring as little-endian
 * S16 stereo hex. Oracle-side mirror of vb-runtime's `audio_pcm`
 * (Rule 14: identical wire shape, different port). Probe-pulls by
 * absolute frame index — a ring QUERY, never an armed capture. */
static void handle_audio_pcm(long long id, const char* line) {
    long long start = 0, maxf = 8192;
    extract_int(line, "\"start\"", &start);
    extract_int(line, "\"max\"",   &maxf);
    if (start < 0) start = 0;
    if (maxf < 1)      maxf = 1;
    if (maxf > 16384)  maxf = 16384;

    int16_t* pcm = (int16_t*)malloc((size_t)maxf * 2 * sizeof(int16_t));
    if (!pcm) { send_response("{\"ok\":false,\"error\":\"oom\"}"); return; }

    uint64_t head = 0, resident_lo = 0;
    size_t got = vb_beetle_audio_read_abs((uint64_t)start, pcm, (size_t)maxf,
                                          &head, &resident_lo);
    uint64_t begin = ((uint64_t)start < resident_lo)
                         ? resident_lo : (uint64_t)start;

    char* body = (char*)malloc(256 + (size_t)got * 2 * 4);
    if (!body) { free(pcm); send_response("{\"ok\":false,\"error\":\"oom\"}"); return; }
    char* p = body;
    p += sprintf(p,
        "{\"ok\":true,\"cmd\":\"audio_pcm\",\"id\":%lld,\"rate\":%u,"
        "\"channels\":2,\"format\":\"s16le\",\"head\":%llu,"
        "\"resident_lo\":%llu,\"begin\":%llu,\"returned\":%u,\"hex\":\"",
        id, vb_beetle_audio_rate(),
        (unsigned long long)head, (unsigned long long)resident_lo,
        (unsigned long long)begin, (unsigned)got);
    for (size_t i = 0; i < got * 2; ++i) {
        uint16_t s = (uint16_t)pcm[i];
        p += sprintf(p, "%02X%02X", (unsigned)(s & 0xFF),
                                    (unsigned)((s >> 8) & 0xFF));
    }
    p += sprintf(p, "\"}");
    send_response(body);
    free(body);
    free(pcm);
}

/* Per-instruction CPU-hook ring (Axis 1/2/3/6), defined in the beetle-vb
 * core archive (libretro.cpp). RB_CPUHOOK records {pc, psw, regs-FNV,
 * cumulative cycle} for every retired guest instruction; this streams a
 * window by absolute retire-seq as little-endian hex of the raw record
 * (struct below MUST match libretro.cpp's vb_cpuhook_rec). Ring QUERY,
 * never an armed capture. */
typedef struct { uint32_t pc, psw, fnv, pad; uint64_t cycle; } vb_cpuhook_rec;
extern uint64_t vb_oracle_cpuhook_head(void);
extern uint32_t vb_oracle_cpuhook_query(uint64_t from, uint32_t max,
                                        vb_cpuhook_rec* out,
                                        uint64_t* out_resident_lo);

static void handle_cpuhook(long long id, const char* line) {
    long long start = 0, maxn = 8192;
    extract_int(line, "\"start\"", &start);
    extract_int(line, "\"max\"",   &maxn);
    if (start < 0)    start = 0;
    if (maxn < 1)     maxn = 1;
    if (maxn > 65536) maxn = 65536;

    vb_cpuhook_rec* recs =
        (vb_cpuhook_rec*)malloc((size_t)maxn * sizeof(vb_cpuhook_rec));
    if (!recs) { send_response("{\"ok\":false,\"error\":\"oom\"}"); return; }

    uint64_t head = vb_oracle_cpuhook_head();
    uint64_t resident_lo = 0;
    uint32_t got = vb_oracle_cpuhook_query((uint64_t)start, (uint32_t)maxn,
                                           recs, &resident_lo);
    uint64_t begin = ((uint64_t)start < resident_lo)
                         ? resident_lo : (uint64_t)start;

    char* body = (char*)malloc(256 + (size_t)got * sizeof(vb_cpuhook_rec) * 2);
    if (!body) { free(recs); send_response("{\"ok\":false,\"error\":\"oom\"}"); return; }
    char* p = body;
    p += sprintf(p,
        "{\"ok\":true,\"cmd\":\"cpuhook\",\"id\":%lld,\"recsize\":%u,"
        "\"head\":%llu,\"resident_lo\":%llu,\"begin\":%llu,\"returned\":%u,\"hex\":\"",
        id, (unsigned)sizeof(vb_cpuhook_rec),
        (unsigned long long)head, (unsigned long long)resident_lo,
        (unsigned long long)begin, (unsigned)got);
    const unsigned char* raw = (const unsigned char*)recs;
    for (size_t i = 0; i < (size_t)got * sizeof(vb_cpuhook_rec); ++i)
        p += sprintf(p, "%02X", raw[i]);
    p += sprintf(p, "\"}");
    send_response(body);
    free(body);
    free(recs);
}

/* VIP draw-timing phase ring (Axis-5a phase gate). The mednafen VIP
 * snapshots DPSTTS/XPSTTS at each INTPND-raise into a first-N-from-boot ring
 * in libretro.cpp; this streams a window by absolute event-seq as hex of the
 * raw record (struct MUST match runtime/include/vip_phase.h + libretro.cpp).
 * Ring QUERY, never an armed capture. */
typedef struct {
    uint64_t seq;
    uint16_t event, dpstts, xpstts, reserved;
} vb_vipphase_rec;
extern uint64_t vb_oracle_vipphase_head(void);
extern uint32_t vb_oracle_vipphase_query(uint64_t from, uint32_t max,
                                         vb_vipphase_rec* out,
                                         uint64_t* out_resident_lo);

static void handle_vip_phase(long long id, const char* line) {
    long long start = 0, maxn = 8192;
    extract_int(line, "\"start\"", &start);
    extract_int(line, "\"max\"",   &maxn);
    if (start < 0)    start = 0;
    if (maxn < 1)     maxn = 1;
    if (maxn > 65536) maxn = 65536;

    vb_vipphase_rec* recs =
        (vb_vipphase_rec*)malloc((size_t)maxn * sizeof(vb_vipphase_rec));
    if (!recs) { send_response("{\"ok\":false,\"error\":\"oom\"}"); return; }

    uint64_t head = vb_oracle_vipphase_head();
    uint64_t resident_lo = 0;
    uint32_t got = vb_oracle_vipphase_query((uint64_t)start, (uint32_t)maxn,
                                            recs, &resident_lo);
    uint64_t begin = ((uint64_t)start < resident_lo)
                         ? resident_lo : (uint64_t)start;

    char* body = (char*)malloc(256 + (size_t)got * sizeof(vb_vipphase_rec) * 2);
    if (!body) { free(recs); send_response("{\"ok\":false,\"error\":\"oom\"}"); return; }
    char* p = body;
    p += sprintf(p,
        "{\"ok\":true,\"cmd\":\"vip_phase\",\"id\":%lld,\"recsize\":%u,"
        "\"head\":%llu,\"resident_lo\":%llu,\"begin\":%llu,\"returned\":%u,\"hex\":\"",
        id, (unsigned)sizeof(vb_vipphase_rec),
        (unsigned long long)head, (unsigned long long)resident_lo,
        (unsigned long long)begin, (unsigned)got);
    const unsigned char* raw = (const unsigned char*)recs;
    for (size_t i = 0; i < (size_t)got * sizeof(vb_vipphase_rec); ++i)
        p += sprintf(p, "%02X", raw[i]);
    p += sprintf(p, "\"}");
    send_response(body);
    free(body);
    free(recs);
}

/* Per-game-frame WRAM fingerprint ring (Axis-6 whole-session fidelity). The
 * core hashes WRAM at each GAME_START into a first-N-from-boot ring in
 * libretro.cpp; this streams a window as hex (struct MUST match
 * runtime/include/wram_hash.h). Ring QUERY, never an armed capture. */
typedef struct { uint64_t seq; uint32_t fnv[64]; } vb_wramhash_rec;
extern uint64_t vb_oracle_wramhash_head(void);
extern uint32_t vb_oracle_wramhash_query(uint64_t from, uint32_t max,
                                         vb_wramhash_rec* out,
                                         uint64_t* out_resident_lo);

static void handle_wram_hash(long long id, const char* line) {
    long long start = 0, maxn = 8192;
    extract_int(line, "\"start\"", &start);
    extract_int(line, "\"max\"",   &maxn);
    if (start < 0)    start = 0;
    if (maxn < 1)     maxn = 1;
    if (maxn > 65536) maxn = 65536;

    vb_wramhash_rec* recs =
        (vb_wramhash_rec*)malloc((size_t)maxn * sizeof(vb_wramhash_rec));
    if (!recs) { send_response("{\"ok\":false,\"error\":\"oom\"}"); return; }

    uint64_t head = vb_oracle_wramhash_head();
    uint64_t resident_lo = 0;
    uint32_t got = vb_oracle_wramhash_query((uint64_t)start, (uint32_t)maxn,
                                            recs, &resident_lo);
    uint64_t begin = ((uint64_t)start < resident_lo)
                         ? resident_lo : (uint64_t)start;

    char* body = (char*)malloc(256 + (size_t)got * sizeof(vb_wramhash_rec) * 2);
    if (!body) { free(recs); send_response("{\"ok\":false,\"error\":\"oom\"}"); return; }
    char* p = body;
    p += sprintf(p,
        "{\"ok\":true,\"cmd\":\"wram_hash\",\"id\":%lld,\"recsize\":%u,"
        "\"head\":%llu,\"resident_lo\":%llu,\"begin\":%llu,\"returned\":%u,\"hex\":\"",
        id, (unsigned)sizeof(vb_wramhash_rec),
        (unsigned long long)head, (unsigned long long)resident_lo,
        (unsigned long long)begin, (unsigned)got);
    const unsigned char* raw = (const unsigned char*)recs;
    for (size_t i = 0; i < (size_t)got * sizeof(vb_wramhash_rec); ++i)
        p += sprintf(p, "%02X", raw[i]);
    p += sprintf(p, "\"}");
    send_response(body);
    free(body);
    free(recs);
}

static void handle_pause(long long id, int state) {
    vb_beetle_debug_set_paused(state);
    char buf[128];
    snprintf(buf, sizeof(buf),
        "{\"ok\":true,\"cmd\":\"%s\",\"id\":%lld,\"paused\":%s}",
        state ? "pause" : "continue", id, state ? "true" : "false");
    send_response(buf);
}

static void handle_quit(long long id) {
    char buf[64];
    snprintf(buf, sizeof(buf),
        "{\"ok\":true,\"cmd\":\"quit\",\"id\":%lld}", id);
    send_response(buf);
    s_quit = 1;
}

static void handle_unknown(const char* cmd, long long id) {
    char buf[320];
    snprintf(buf, sizeof(buf),
        "{\"ok\":false,\"id\":%lld,\"error\":\"unknown command\","
        "\"cmd\":\"%s\","
        "\"hint\":\"vb-beetle command set is a strict subset of vb-runtime; "
        "see TCP.md\"}",
        id, cmd);
    send_response(buf);
}


/* ---- Dispatch ---- */

static void dispatch_line(char* line) {
    char cmd[64] = {0};
    long long id = 0;
    extract_int(line, "\"id\"", &id);
    s_request_id=id;

    char* nl = strpbrk(line, "\r\n");
    if (nl) *nl = 0;
    while (*line == ' ' || *line == '\t') line++;

    if (line[0] == '{') {
        if (!extract_str(line, "\"cmd\"", cmd, sizeof(cmd))) {
            send_response("{\"ok\":false,\"error\":\"missing 'cmd' field\"}");
            return;
        }
    } else {
        size_t i = 0;
        while (line[i] && line[i] != ' ' && line[i] != '\t'
               && i + 1 < sizeof(cmd)) {
            cmd[i] = line[i]; i++;
        }
        cmd[i] = 0;
    }

    if (cmd[0] == 0) {
        send_response("{\"ok\":false,\"error\":\"empty command\"}");
        return;
    }

    if (strcmp(cmd, "device_state") == 0) { char body[16384]; vb_device_response(body,sizeof(body)); send_response(body); }
    else if (strcmp(cmd, "capabilities") == 0) { send_response("{\"ok\":true,\"protocol\":2,\"commands\":[\"device_state\",\"audio_pcm\",\"capabilities\",\"clear_input\",\"continue\",\"cpuhook\",\"frame\",\"get_registers\",\"memory_map\",\"pad_state\",\"pause\",\"ping\",\"quit\",\"read_ram\",\"run_frames\",\"screenshot\",\"set_input\",\"vip_phase\",\"vip_state\",\"wram_hash\",\"write_ram\"],\"max_read_bytes\":65536,\"instruction_control\":false,\"trace_version\":2}"); }
    else if (strcmp(cmd, "ping") == 0)         handle_ping(id);
    else if (strcmp(cmd, "frame") == 0)        handle_frame(id);
    else if (strcmp(cmd, "pad_state") == 0)    handle_pad_state(id);
    else if (strcmp(cmd, "write_ram") == 0) {
        extern int vb_beetle_write_ram(uint32_t,uint8_t);
        long long addr=-1,value=-1;
        extract_int(line,"\"addr\"",&addr);extract_int(line,"\"val\"",&value);
        if(!vb_beetle_debug_is_paused() || addr<0 || addr>UINT32_MAX || value<0 || value>255 || !vb_beetle_write_ram((uint32_t)addr,(uint8_t)value)) {
            send_response("{\"ok\":false,\"error\":\"write_ram needs paused CPU, WRAM/SRAM address and byte val\"}");
        } else {
            char result[128];snprintf(result,sizeof(result),"{\"ok\":true,\"id\":%lld}",id);send_response(result);
        }
    }
    else if (strcmp(cmd, "read_ram") == 0)     handle_read_ram(id, line);
    else if (strcmp(cmd, "memory_map") == 0)   handle_memory_map(id);
    else if (strcmp(cmd, "get_registers") == 0)handle_get_registers(id);
    else if (strcmp(cmd, "vip_state") == 0)    handle_vip_state(id);
    else if (strcmp(cmd, "audio_pcm") == 0)    handle_audio_pcm(id, line);
    else if (strcmp(cmd, "cpuhook") == 0)      handle_cpuhook(id, line);
    else if (strcmp(cmd, "vip_phase") == 0)    handle_vip_phase(id, line);
    else if (strcmp(cmd, "wram_hash") == 0)    handle_wram_hash(id, line);
    else if (strcmp(cmd, "screenshot") == 0)   handle_screenshot(id, line);
    else if (strcmp(cmd, "run_frames") == 0) {
        long long frames = 1;
        extract_int(line, "\"frames\"", &frames);
        if (frames < 1 || frames > 10000 ||
            (uint64_t)vb_beetle_frame_count() + frames > UINT32_MAX) {
            send_response("{\"ok\":false,\"error\":\"invalid frame count\"}");
        } else {
            s_step_target = vb_beetle_frame_count() + (uint32_t)frames;
            s_paused = 0;
            char result[128];
            snprintf(result, sizeof(result), "{\"ok\":true,\"id\":%lld,\"target\":%u}", id, s_step_target);
            send_response(result);
        }
    }
    else if (strcmp(cmd, "set_input") == 0 || strcmp(cmd, "clear_input") == 0) {
        long long mask = 0;
        if (!extract_int(line, "\"pad\"", &mask)) extract_int(line, "\"mask\"", &mask);
        if (!strcmp(cmd, "clear_input")) mask = 0;
        if (mask < 0 || mask > 65535) {
            send_response("{\"ok\":false,\"error\":\"invalid input mask\"}");
        } else {
            /* Public protocol uses pressed bits; the libretro adapter is active-low. */
            vb_beetle_set_pad((uint16_t)~((uint32_t)mask >> 2));
            s_input_override=strcmp(cmd,"clear_input")!=0;
            char result[128];
            snprintf(result, sizeof(result), "{\"ok\":true,\"id\":%lld,\"mask\":%u}", id, (unsigned)mask);
            send_response(result);
        }
    }
    else if (strcmp(cmd, "pause") == 0)        handle_pause(id, 1);
    else if (strcmp(cmd, "continue") == 0)     handle_pause(id, 0);
    else if (strcmp(cmd, "quit") == 0)         handle_quit(id);
    else                                       handle_unknown(cmd, id);
}


void vb_beetle_debug_server_poll(void) {
    if (s_listener == VBB_BAD_SOCKET) return;

    if (s_client == VBB_BAD_SOCKET) {
        sock_t c = accept(s_listener, NULL, NULL);
        if (c != VBB_BAD_SOCKET) {
            set_nonblocking(c);
            s_client = c;
            vb_stream_reset();
        }
    }
    if (s_client != VBB_BAD_SOCKET && !vb_stream_poll(s_client, dispatch_line)) {
        vbb_close_socket(s_client); s_client = VBB_BAD_SOCKET;
        vb_stream_reset();
    }
    if (s_quit && vb_stream.size == 0) {
        /* clean shutdown driven from main loop */
        vb_beetle_debug_server_stop();
    }
}
