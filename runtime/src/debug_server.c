#include "device_debug.h"
/* debug_server.c — TCP JSON debug server.
 *
 * Per CLAUDE.md Rule 3, this is the ONLY sanctioned debug surface.
 * Phase 1 ships the line-based JSON protocol with a small set of
 * commands: ping, frame, get_registers, read_ram, memory_map,
 * opcode_coverage (placeholder), pad_state, vip_state, vsu_state,
 * pause, continue, step, quit.
 *
 * Cross-platform sockets: WIN32 winsock2 + POSIX BSD. One client at a
 * time; non-blocking accept polled from the main loop.
 */
#include "debug_server.h"
#include "v810_interpreter.h"

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "input.h"
#include "interrupts.h"
#include "memory.h"
#include "png_write.h"
#include "vsu.h"
#include "vsu_shadow.h"
#include "ring_frame.h"
#include "timer.h"
#include "vip.h"
#include "vip_capture.h"
#include "asset_pack.h"
#include "recolor.h"
#include "renderer.h"
#include "watchdog.h"
#include "wtrace.h"
#include "fntrace.h"
#include "cpuhook.h"
#include "vip_phase.h"
#include "wram_hash.h"

#if defined(_WIN32)
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  if defined(_MSC_VER)
#    pragma comment(lib, "ws2_32.lib")
#  endif
   typedef SOCKET sock_t;
#  define VB_BAD_SOCKET INVALID_SOCKET
#  define vb_close_socket(s) closesocket(s)
#  define vb_socket_errno() WSAGetLastError()
#  define VB_EWOULDBLOCK WSAEWOULDBLOCK
#else
#  include <sys/types.h>
#  include <sys/socket.h>
#  include <netinet/in.h>
#  include <arpa/inet.h>
#  include <fcntl.h>
#  include <unistd.h>
#  include <errno.h>
   typedef int sock_t;
#  define VB_BAD_SOCKET (-1)
#  define vb_close_socket(s) close(s)
#  define vb_socket_errno() (errno)
#  define VB_EWOULDBLOCK EWOULDBLOCK
#endif

#include "debug_stream.h"

static sock_t s_listener = VB_BAD_SOCKET;
static sock_t s_client   = VB_BAD_SOCKET;
static CPUState* s_cpu   = NULL;
static int s_paused      = 0;
static uint32_t s_step_target = 0;
static int s_input_override;
int vb_debug_server_input_override(void) {
    if(s_input_override==2 && !vb_input_press_remaining()) s_input_override=0;
    return s_input_override;
}

void vb_debug_server_set_paused(int paused) {
    s_paused = paused != 0;
    s_step_target = 0;
    vb_execution.stopped=0;vb_execution.stepping=0;
    vb_execution.skip_break=1;
}
int vb_debug_server_is_paused(void) {
    /* HALT is a completed instruction: stepping must stop there even when
     * no following instruction is available to run the boundary hook. */
    if (vb_execution.stepping && s_cpu && s_cpu->halted)
        vb_execution.stopped=1;
    if (vb_execution.stopped) s_paused=1;
    if (s_step_target && s_cpu && s_cpu->frame >= s_step_target) {
        s_paused = 1;
        s_step_target = 0;
    }
    return s_paused;
}
static int s_quit        = 0;
static int s_winsock_inited = 0;

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

int vb_debug_server_start(int port, CPUState* cpu) {
    s_cpu = cpu;
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
    if (s_listener == VB_BAD_SOCKET) return -2;

    int yes = 1;
    setsockopt(s_listener, SOL_SOCKET, SO_REUSEADDR,
               (const char*)&yes, sizeof(yes));

    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t)port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    if (bind(s_listener, (struct sockaddr*)&a, sizeof(a)) != 0) {
        vb_close_socket(s_listener); s_listener = VB_BAD_SOCKET;
        return -3;
    }
    if (listen(s_listener, 1) != 0) {
        vb_close_socket(s_listener); s_listener = VB_BAD_SOCKET;
        return -4;
    }
    if (set_nonblocking(s_listener) != 0) {
        vb_close_socket(s_listener); s_listener = VB_BAD_SOCKET;
        return -5;
    }
    return 0;
}

void vb_debug_server_stop(void) {
    if (s_client != VB_BAD_SOCKET) {
        vb_close_socket(s_client); s_client = VB_BAD_SOCKET;
    }
    if (s_listener != VB_BAD_SOCKET) {
        vb_close_socket(s_listener); s_listener = VB_BAD_SOCKET;
    }
#if defined(_WIN32)
    if (s_winsock_inited) {
        WSACleanup();
        s_winsock_inited = 0;
    }
#endif
}

/* ---- Command parsing ---- */
/* The protocol is line-based. A request is one of:
 *   - bare command:   "ping\n"
 *   - JSON object:    {"cmd":"NAME", "id":N, ...}\n
 * The parser below extracts the cmd name + id by string-search; full
 * JSON parsing is overkill for the tiny request surface. Args use a
 * very small recursive scan (see arg_int_after / arg_str_after). */

static int extract_str(const char* line, const char* key, char* out, size_t max) {
    /* Look for "key" followed by optional whitespace + ':' + optional ws + '"value"'. */
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

/* ---- JSON writer helpers ---- */
static void wprintf_(char** cur, char* end, const char* fmt, ...) {
    if (*cur >= end) return;
    va_list ap; va_start(ap, fmt);
    int n = vsnprintf(*cur, (size_t)(end - *cur), fmt, ap);
    va_end(ap);
    if (n > 0) *cur += (size_t)n;
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
    char buf[128];
    snprintf(buf, sizeof(buf),
             "{\"ok\":true,\"cmd\":\"ping\",\"id\":%lld,\"vbrecomp\":\"0.1.0\"}", id);
    send_response(buf);
}

static void handle_frame(long long id) {
    char buf[160];
    uint32_t f = s_cpu ? s_cpu->frame : 0;
    snprintf(buf, sizeof(buf),
             "{\"ok\":true,\"cmd\":\"frame\",\"id\":%lld,\"frame\":%u,\"seq\":%llu}",
             id, (unsigned)f, (unsigned long long)vb_ring_frame_seq());
    send_response(buf);
}

static void handle_get_registers(long long id) {
    char buf[4096];
    char* cur = buf;
    char* end = buf + sizeof(buf);
    if (!s_cpu) {
        send_response("{\"ok\":false,\"error\":\"no cpu state attached\"}");
        return;
    }
    wprintf_(&cur, end,
        "{\"ok\":true,\"cmd\":\"get_registers\",\"id\":%lld,\"pc\":\"0x%08X\",\"gpr\":[",
        id, s_cpu->pc);
    for (int i = 0; i < 32; ++i) {
        wprintf_(&cur, end, "%s\"0x%08X\"", (i == 0 ? "" : ","), s_cpu->gpr[i]);
    }
    wprintf_(&cur, end,
        "],\"psw\":\"0x%08X\","
        "\"eipc\":\"0x%08X\",\"eipsw\":\"0x%08X\","
        "\"fepc\":\"0x%08X\",\"fepsw\":\"0x%08X\","
        "\"ecr\":\"0x%08X\",\"pir\":\"0x%08X\","
        "\"halted\":%u,\"cycles\":%llu,\"frame\":%u}",
        vb_psw_pack(s_cpu),
        s_cpu->sysreg[VB_SR_EIPC],  s_cpu->sysreg[VB_SR_EIPSW],
        s_cpu->sysreg[VB_SR_FEPC],  s_cpu->sysreg[VB_SR_FEPSW],
        s_cpu->sysreg[VB_SR_ECR],   s_cpu->sysreg[VB_SR_PIR],
        (unsigned)s_cpu->halted,
        (unsigned long long)s_cpu->cycles,
        (unsigned)s_cpu->frame);
    *cur = 0;
    send_response(buf);
}

static void handle_psw_state(long long id) {
    if (!s_cpu) {
        send_response("{\"ok\":false,\"error\":\"no cpu state attached\"}");
        return;
    }
    char buf[512];
    snprintf(buf, sizeof(buf),
        "{\"ok\":true,\"cmd\":\"psw_state\",\"id\":%lld,"
        "\"packed\":\"0x%08X\","
        "\"z\":%u,\"s\":%u,\"ov\":%u,\"cy\":%u,"
        "\"fpr\":%u,\"fud\":%u,\"fov\":%u,\"fzd\":%u,"
        "\"fiv\":%u,\"fro\":%u,"
        "\"interrupt_disable\":%u,\"ae\":%u,\"ep\":%u,\"np\":%u,"
        "\"int_level\":%u}",
        id, vb_psw_pack(s_cpu),
        s_cpu->psw_z, s_cpu->psw_s, s_cpu->psw_ov, s_cpu->psw_cy,
        s_cpu->psw_fpr, s_cpu->psw_fud, s_cpu->psw_fov, s_cpu->psw_fzd,
        s_cpu->psw_fiv, s_cpu->psw_fro,
        s_cpu->psw_id, s_cpu->psw_ae, s_cpu->psw_ep, s_cpu->psw_np,
        s_cpu->psw_int_level);
    send_response(buf);
}

static void handle_vip_state(long long id) {
    char buf[1024];
    snprintf(buf, sizeof(buf),
        "{\"ok\":true,\"cmd\":\"vip_state\",\"id\":%lld,"
        "\"intpnd\":\"0x%04X\",\"intenb\":\"0x%04X\","
        "\"dpctrl\":\"0x%04X\",\"dpstts\":\"0x%04X\","
        "\"xpctrl\":\"0x%04X\",\"xpstts\":\"0x%04X\","
        "\"frmcyc\":%u,\"bkcol\":%u,"
        "\"brta\":%u,\"brtb\":%u,\"brtc\":%u,\"rest\":%u,"
        "\"column\":%d,\"column_counter\":%d,"
        "\"display_region\":%d,\"game_frame_counter\":%d,"
        "\"drawing_block\":%d,\"drawing_counter\":%d,"
        "\"drawing_active\":%d,\"display_active\":%d,"
        "\"display_fb\":%d,\"drawing_fb\":%d,"
        "\"vip_cycles\":%llu}",
        id,
        vb_vip_intpnd(), vb_vip_intenb(),
        vb_vip_dpctrl(), vb_vip_dpstts(),
        vb_vip_xpctrl(), vb_vip_xpstts(),
        vb_vip_frmcyc(), vb_vip_bkcol(),
        vb_vip_brta(), vb_vip_brtb(), vb_vip_brtc(), vb_vip_rest(),
        vb_vip_column(), vb_vip_column_counter(),
        vb_vip_display_region(), vb_vip_game_frame_counter(),
        vb_vip_drawing_block(), vb_vip_drawing_counter(),
        vb_vip_drawing_active(), vb_vip_display_active(),
        vb_vip_display_fb(), vb_vip_drawing_fb(),
        (unsigned long long)vb_vip_cycles());
    send_response(buf);
}

static void handle_timer_state(long long id) {
    char buf[256];
    snprintf(buf, sizeof(buf),
        "{\"ok\":true,\"cmd\":\"timer_state\",\"id\":%lld,"
        "\"tcr\":\"0x%02X\",\"counter\":%u,\"reload\":%u,\"divider\":%d}",
        id,
        (unsigned)vb_timer_control(),
        (unsigned)vb_timer_counter(),
        (unsigned)vb_timer_reload(),
        (int)vb_timer_divider());
    send_response(buf);
}

static VbDebugHostCapture s_host_capture;
static void source_put16(uint8_t* p, uint16_t v) { p[0]=(uint8_t)v; p[1]=(uint8_t)(v>>8); }
static void source_put32(uint8_t* p, uint32_t v) { source_put16(p,(uint16_t)v); source_put16(p+2,(uint16_t)(v>>16)); }
static void handle_source_dump(const char* line) {
    char path[1024] = {0}; long long eye = 0;
    extract_str(line, "\"path\"", path, sizeof(path));
    extract_int(line, "\"eye\"", &eye);
    if (!path[0] || !vb_renderer_tracks_texels()) {
        send_response("{\"ok\":false,\"error\":\"source tracking and path required\"}"); return;
    }
    FILE* file = fopen(path, "wb");
    if (!file) { send_response("{\"ok\":false,\"error\":\"cannot open source dump\"}"); return; }
    uint8_t header[24];memcpy(header,"VBSRC001",8);
    source_put32(header+8,384);source_put32(header+12,224);
    source_put32(header+16,eye != 0);source_put32(header+20,vb_vip_frame_seq());
    int ok = fwrite(header,1,sizeof(header),file)==sizeof(header);
    const VbSourceTexel* sources=vb_vip_source_buffer(eye != 0);
    /* Fixed little-endian wire format, independent of host struct padding. */
    for(int y=0;y<224 && ok;++y) {
        uint8_t row[384*16];
        for(int x=0;x<384;++x) {
            const VbSourceTexel* s=&sources[y*384+x];uint8_t* p=&row[x*16];
            source_put32(p,s->tile_hash);source_put16(p+4,s->x);
            source_put16(p+6,s->y);source_put16(p+8,s->tile);
            p[10]=s->u;p[11]=s->v;p[12]=s->map;p[13]=s->kind;p[14]=s->raw;p[15]=s->world;
        }
        ok=fwrite(row,1,sizeof(row),file)==sizeof(row);
    }
    if (fclose(file)) ok = 0;
    send_response(ok ? "{\"ok\":true,\"format\":\"VBSRC001\"}" : "{\"ok\":false,\"error\":\"source dump failed\"}");
}
static void* s_host_capture_context;
void vb_debug_server_set_host_capture(VbDebugHostCapture capture, void* context) {
    s_host_capture = capture; s_host_capture_context = context;
}

static void handle_screenshot(long long id, const char* line) {
    char path[256] = {0};
    long long eye = 0;
    extract_str(line, "\"path\"", path, sizeof(path));
    extract_int(line, "\"eye\"", &eye);
    if (!path[0]) {
        snprintf(path, sizeof(path),
                 (eye ? "vb-runtime-eye1.png" : "vb-runtime-eye0.png"));
    }
    long long host = 0;
    extract_int(line, "\"host\"", &host);
    if (host) {
        const int ok = s_host_capture && s_host_capture(path, s_host_capture_context) == 0;
        send_response(ok ? "{\"ok\":true,\"host\":true}" :
            "{\"ok\":false,\"error\":\"host capture unavailable or failed\"}");
        return;
    }
    uint32_t* buf = (uint32_t*)malloc(384u * 224u * 4u);
    if (!buf) {
        send_response("{\"ok\":false,\"error\":\"oom\"}");
        return;
    }
    /* Opt-in full-screen recolor render (experiment); default is the faithful
     * raw render so the oracle compare path stays byte-identical. */
    long long recolor = 0;
    extract_int(line, "\"recolor\"", &recolor);
    long long presented = 0;
    extract_int(line, "\"presented\"", &presented);
    if (presented && vb_renderer_active())
        vb_renderer_present((int)eye, buf);
    else if (recolor && vb_recolor_active())
        vb_vip_render_framebuffer_recolored((int)eye, buf);
    else
        vb_vip_render_framebuffer((int)eye, buf);

    /* Opt-in world false-color view (diagnostic): tint each pixel by the world
     * index that drew it (from the attribution buffer), modulated by the
     * faithful brightness so sprite shapes stay legible. Lets a probe SEE which
     * world owns an on-screen object — e.g. whether the far tennis opponent is
     * its own world or shares the court world. Needs attribution (recolor on). */
    long long attr = 0;
    extract_int(line, "\"attr\"", &attr);
    if (attr && vb_recolor_active()) {
        /* Self-contained: render recolored to populate the attribution buffer
         * for THIS exact frame (works headless, no dependence on a live present
         * loop), then tint each pixel by its world index modulated by the
         * pixel's brightness so sprite shapes stay legible. */
        vb_vip_render_framebuffer_recolored((int)eye, buf);
        /* 32 visually distinct world colors (index = world); 0 = no attribution. */
        static const uint32_t WC[32] = {
            0xFF0000,0x00FF00,0x4060FF,0xFFFF00,0xFF00FF,0x00FFFF,0xFF8000,0x8000FF,
            0x80FF00,0xFF0080,0x00FF80,0x0080FF,0xFF80FF,0x80FFFF,0xFFC080,0xC080FF,
            0xB00000,0x00B000,0x0000B0,0xB0B000,0xB000B0,0x00B0B0,0xB05000,0x5000B0,
            0x808080,0xE0E0E0,0x008040,0x804000,0x400080,0x408000,0xFFFFFF,0x80C0FF };
        const uint16_t* ab = vb_vip_attr_buffer((int)eye);
        for (int i = 0; i < 384 * 224; ++i) {
            uint16_t a = ab[i];                 /* 0 = none, else world+1 */
            uint32_t px = buf[i];
            uint32_t r0 = (px >> 16) & 0xFF, g0 = (px >> 8) & 0xFF, b0 = px & 0xFF;
            uint32_t luma = r0 > g0 ? (r0 > b0 ? r0 : b0) : (g0 > b0 ? g0 : b0);
            if (a == 0 || a > 32) { buf[i] = 0xFF000000u | (luma << 16) | (luma << 8) | luma; continue; }
            uint32_t c = WC[(a - 1) & 31];
            uint32_t r = (((c >> 16) & 0xFF) * luma) / 255;
            uint32_t g = (((c >> 8)  & 0xFF) * luma) / 255;
            uint32_t b = ( (c        & 0xFF) * luma) / 255;
            buf[i] = 0xFF000000u | (r << 16) | (g << 8) | b;
        }
    }
    /* Opt-in: composite the override overlays into the captured frame so the
     * enhanced result can be inspected headlessly. Default (no "overlay"
     * field) stays the faithful raw render — keeps the oracle compare path
     * byte-identical. */
    long long overlay = 0;
    extract_int(line, "\"overlay\"", &overlay);
    if (overlay && vb_overrides_active()) {
        vb_overlay_composite(buf, 384, 224, (int)eye, vb_vip_display_fb() & 1);
    }
    int rc = vb_write_png_32bpp(path, 384, 224, buf);
    free(buf);
    char body[384];
    if (rc != 0) {
        snprintf(body, sizeof(body),
                 "{\"ok\":false,\"cmd\":\"screenshot\",\"id\":%lld,"
                 "\"error\":\"failed to write %s\"}",
                 id, path);
    } else {
        snprintf(body, sizeof(body),
                 "{\"ok\":true,\"cmd\":\"screenshot\",\"id\":%lld,"
                 "\"eye\":%lld,\"path\":\"%s\",\"width\":384,\"height\":224}",
                 id, eye, path);
    }
    send_response(body);
}

/* Live watchdog snapshot (main-loop heartbeat). When the main thread is
 * wedged this command can't answer (the server is on that thread) — read
 * vb_freeze_heartbeat_<pid>.json / vb_freeze_dump_*.json instead. Useful while
 * alive to confirm the loop is healthy and see the current phase. */
static void handle_watchdog(long long id) {
    int phase = 0, stalled = 0;
    uint32_t pc = 0;
    uint64_t cycles = 0, frame = 0, beats = 0;
    vb_watchdog_status(&phase, &pc, &cycles, &frame, &beats, &stalled);
    char body[256];
    snprintf(body, sizeof(body),
             "{\"ok\":true,\"cmd\":\"watchdog\",\"id\":%lld,\"phase\":\"%s\","
             "\"pc\":\"0x%08X\",\"cycles\":%llu,\"frame\":%llu,\"beats\":%llu,"
             "\"stalled_ms\":%d}",
             id, vb_watchdog_phase_name(phase), pc,
             (unsigned long long)cycles, (unsigned long long)frame,
             (unsigned long long)beats, stalled);
    send_response(body);
}

/* Flush the opt-in graphics-capture catalog + per-frame layouts to disk.
 * No-op (returns ok=false) when VBRECOMP_CAPTURE was not set, so this never
 * fabricates output for a faithful run. */
static void handle_capture_dump(long long id, const char* line) {
    char dir[256] = {0};
    extract_str(line, "\"dir\"", dir, sizeof(dir));
    if (!dir[0]) snprintf(dir, sizeof(dir), "captures");

    char body[384];
    if (!vb_capture_active()) {
        snprintf(body, sizeof(body),
                 "{\"ok\":false,\"cmd\":\"capture_dump\",\"id\":%lld,"
                 "\"error\":\"capture not active (set VBRECOMP_CAPTURE)\"}", id);
        send_response(body);
        return;
    }
    int n = vb_capture_dump(dir);
    if (n < 0) {
        snprintf(body, sizeof(body),
                 "{\"ok\":false,\"cmd\":\"capture_dump\",\"id\":%lld,"
                 "\"error\":\"dump failed\"}", id);
    } else {
        snprintf(body, sizeof(body),
                 "{\"ok\":true,\"cmd\":\"capture_dump\",\"id\":%lld,"
                 "\"dir\":\"%s\",\"tiles\":%d}", id, dir, n);
    }
    send_response(body);
}

/* Introspect the opt-in override layer (Rule 3: TCP, not printf). Reports
 * whether it is active, how many replacement images loaded, and the current
 * overlay draw-list size for each framebuffer slot + the displayed slot. */
static void handle_overrides_state(long long id) {
    char body[256];
    int slot = vb_vip_display_fb() & 1;
    snprintf(body, sizeof(body),
             "{\"ok\":true,\"cmd\":\"overrides_state\",\"id\":%lld,"
             "\"active\":%d,\"images\":%d,\"display_slot\":%d,"
             "\"overlay0\":%d,\"overlay1\":%d}",
             id, vb_overrides_active(), vb_overrides_image_count(), slot,
             vb_overlay_count(0), vb_overlay_count(1));
    send_response(body);
}

/* Report the per-world on-screen footprint (for recolor identification): for
 * each world index drawn in the displayed eye, its pixel count + bounding box,
 * sorted by count. The attribution buffer stores world+1; this maps it back to
 * world. Requires attribution active (capture or recolor). */
static void handle_world_map(long long id, const char* line) {
    long long eye = 0;
    extract_int(line, "\"eye\"", &eye);
    const uint16_t* attr = vb_vip_attr_buffer((int)(eye & 1));
    int cnt[33] = {0};
    short x0[33], y0[33], x1[33], y1[33];
    for (int y = 0; y < 224; y++) {
        for (int x = 0; x < 384; x++) {
            uint16_t a = attr[y * 384 + x];
            if (a == 0 || a > 32) continue;
            if (cnt[a] == 0) { x0[a] = x1[a] = (short)x; y0[a] = y1[a] = (short)y; }
            else {
                if (x < x0[a]) x0[a] = (short)x;
                if (x > x1[a]) x1[a] = (short)x;
                if (y < y0[a]) y0[a] = (short)y;
                if (y > y1[a]) y1[a] = (short)y;
            }
            cnt[a]++;
        }
    }
    char buf[4096];
    char* p = buf;
    char* end = buf + sizeof(buf);
    p += snprintf(p, (size_t)(end - p),
                  "{\"ok\":true,\"cmd\":\"world_map\",\"id\":%lld,\"eye\":%lld,"
                  "\"worlds\":[", id, eye);
    int used = 0;
    for (int k = 0; k < 32; ++k) {
        int best = -1, bestc = 0;
        for (int a = 1; a <= 32; ++a) if (cnt[a] > bestc) { bestc = cnt[a]; best = a; }
        if (best < 0) break;
        p += snprintf(p, (size_t)(end - p),
                      "%s{\"world\":%d,\"count\":%d,\"x0\":%d,\"y0\":%d,\"x1\":%d,\"y1\":%d}",
                      used ? "," : "", best - 1, cnt[best],
                      x0[best], y0[best], x1[best], y1[best]);
        cnt[best] = 0;
        used++;
        if (end - p < 128) break;
    }
    snprintf(p, (size_t)(end - p), "]}");
    send_response(buf);
}

/* Introspect the opt-in recolor layer (TCP, not printf). */
static void handle_recolor_state(long long id) {
    char body[256];
    snprintf(body, sizeof(body),
             "{\"ok\":true,\"cmd\":\"recolor_state\",\"id\":%lld,"
             "\"active\":%d,\"entries\":%d,\"scenes\":%d,\"scene\":\"%s\"}",
             id, vb_recolor_active(), vb_recolor_entry_count(),
             vb_recolor_scene_count(), vb_recolor_current_scene());
    send_response(body);
}

/* Re-read the recolor pack file at runtime (author live, then reload). */
static void handle_recolor_reload(long long id) {
    vb_recolor_reload();
    char body[160];
    snprintf(body, sizeof(body),
             "{\"ok\":true,\"cmd\":\"recolor_reload\",\"id\":%lld,\"entries\":%d}",
             id, vb_recolor_entry_count());
    send_response(body);
}

/* Dump the always-on scene-selection decision ring (last `n`, default 64).
 * Each entry: {seq, mask (active-world bitmask, world i = bit i), scene index
 * + name (-1/"" = no match -> faithful frame)}. Query after a flicker to see
 * why frames chose nothing instead of arming a trace. */
static void handle_recolor_trace(long long id, const char* line) {
    long long n = 64;
    extract_int(line, "\"n\"", &n);
    int len = vb_recolor_trace_len();
    if (n < 1) n = 1;
    if (n > len) n = len;
    char* buf = (char*)malloc((size_t)(n * 96 + 128));
    if (!buf) { send_response("{\"ok\":false,\"error\":\"oom\"}"); return; }
    char* p = buf; char* end = buf + (size_t)(n * 96 + 128);
    p += snprintf(p, (size_t)(end - p),
                  "{\"ok\":true,\"cmd\":\"recolor_trace\",\"id\":%lld,\"entries\":[", id);
    int first = 1;
    for (int i = len - (int)n; i < len; ++i) {
        uint32_t seq = 0, mask = 0; int scene = -1;
        if (!vb_recolor_trace_get(i, &seq, &mask, &scene)) continue;
        p += snprintf(p, (size_t)(end - p),
                      "%s{\"seq\":%u,\"mask\":\"0x%08X\",\"scene\":%d,\"name\":\"%s\"}",
                      first ? "" : ",", seq, mask, scene,
                      scene >= 0 ? vb_recolor_scene_name(scene) : "");
        first = 0;
    }
    snprintf(p, (size_t)(end - p), "]}");
    send_response(buf);
    free(buf);
}

/* Max boxes per world-ring frame (matches vip.c WRING_MAXW; sized for the
 * stack buffer + payload estimate here). */
#define WRING_MAXW_HINT 20

/* Dump the last n frames of the session-spanning world ring: per displayed
 * frame, the active-world mask + each present world's on-screen bbox. Walk this
 * after the fact (e.g. find which world drew the far opponent during a rally) —
 * no timing race. n defaults to the whole ring. */
static void handle_world_trace(long long id, const char* line) {
    int len = vb_vip_wring_len();
    long long n = len;
    extract_int(line, "\"n\"", &n);
    if (n < 1) n = 1;
    if (n > len) n = len;
    size_t cap = (size_t)n * (WRING_MAXW_HINT * 80 + 64) + 160;
    char* buf = (char*)malloc(cap);
    if (!buf) { send_response("{\"ok\":false,\"error\":\"oom\"}"); return; }
    char* p = buf; char* e = buf + cap;
    p += snprintf(p, (size_t)(e - p),
                  "{\"ok\":true,\"cmd\":\"world_trace\",\"id\":%lld,\"frames\":[", id);
    VbWorldBox boxes[WRING_MAXW_HINT];
    int first = 1;
    for (int i = len - (int)n; i < len; ++i) {
        uint32_t seq = 0, mask = 0;
        int nb = vb_vip_wring_get(i, &seq, &mask, boxes, WRING_MAXW_HINT);
        if (nb < 0) continue;
        p += snprintf(p, (size_t)(e - p), "%s{\"seq\":%u,\"mask\":\"0x%08X\",\"w\":[",
                      first ? "" : ",", seq, mask);
        first = 0;
        for (int k = 0; k < nb; ++k)
            p += snprintf(p, (size_t)(e - p),
                "%s{\"world\":%d,\"x0\":%d,\"y0\":%d,\"x1\":%d,\"y1\":%d,\"count\":%u}",
                k ? "," : "", boxes[k].world, boxes[k].x0, boxes[k].y0,
                boxes[k].x1, boxes[k].y1, boxes[k].count);
        p += snprintf(p, (size_t)(e - p), "]}");
    }
    snprintf(p, (size_t)(e - p), "]}");
    send_response(buf);
    free(buf);
}

/* Dump every WRAM anchor (one per world-set transition) for a byte slice
 * [addr, addr+len) within the anchor window. Each anchor carries its seq + the
 * world mask it entered, so diffing the slice across anchors with different
 * masks pinpoints the byte that encodes the on-screen sub-state. */
static void handle_wram_anchors(long long id, const char* line) {
    long long addr = (long long)vb_vip_aring_base(), len = 64;
    extract_int(line, "\"addr\"", &addr);
    extract_int(line, "\"len\"", &len);
    uint32_t base = vb_vip_aring_base(), win = vb_vip_aring_window();
    if (len < 1 || len > 256 ||
        (uint64_t)addr < base || (uint64_t)addr + (uint64_t)len > base + win) {
        send_response("{\"ok\":false,\"error\":\"slice outside anchor window\"}");
        return;
    }
    uint32_t off = (uint32_t)addr - base;
    int n = vb_vip_aring_len();
    size_t cap = (size_t)n * ((size_t)len * 2 + 64) + 192;
    char* buf = (char*)malloc(cap);
    if (!buf) { send_response("{\"ok\":false,\"error\":\"oom\"}"); return; }
    char* p = buf; char* e = buf + cap;
    p += snprintf(p, (size_t)(e - p),
        "{\"ok\":true,\"cmd\":\"wram_anchors\",\"id\":%lld,\"addr\":\"0x%08X\","
        "\"len\":%lld,\"anchors\":[", id, (uint32_t)addr, len);
    uint8_t tmp[256];
    int first = 1;
    for (int i = 0; i < n; ++i) {
        uint32_t seq = 0, mask = 0;
        int got = vb_vip_aring_get(i, off, (int)len, &seq, &mask, tmp);
        if (got < 0) continue;
        p += snprintf(p, (size_t)(e - p), "%s{\"seq\":%u,\"mask\":\"0x%08X\",\"hex\":\"",
                      first ? "" : ",", seq, mask);
        first = 0;
        for (int k = 0; k < got; ++k) p += snprintf(p, (size_t)(e - p), "%02X", tmp[k]);
        p += snprintf(p, (size_t)(e - p), "\"}");
    }
    snprintf(p, (size_t)(e - p), "]}");
    send_response(buf);
    free(buf);
}

static void handle_psw_set(long long id, const char* line) {
    if (!s_cpu) {
        send_response("{\"ok\":false,\"error\":\"no cpu state attached\"}");
        return;
    }
    long long value = 0;
    if (!extract_int(line, "\"value\"", &value)) {
        send_response("{\"ok\":false,\"error\":\"missing 'value' field\"}");
        return;
    }
    vb_psw_unpack(s_cpu, (uint32_t)value);
    char buf[160];
    snprintf(buf, sizeof(buf),
        "{\"ok\":true,\"cmd\":\"psw_set\",\"id\":%lld,"
        "\"packed\":\"0x%08X\"}", id, vb_psw_pack(s_cpu));
    send_response(buf);
}

static void handle_irq_force(long long id, const char* line) {
    long long source = -1, asserted = 1;
    extract_int(line, "\"source\"", &source);
    extract_int(line, "\"asserted\"", &asserted);
    if (source < 0 || source >= VBIRQ_SOURCE_COUNT) {
        send_response("{\"ok\":false,\"error\":\"source out of range\"}");
        return;
    }
    vb_irq_assert((uint32_t)source, asserted ? 1 : 0);
    char buf[160];
    snprintf(buf, sizeof(buf),
        "{\"ok\":true,\"cmd\":\"irq_force\",\"id\":%lld,"
        "\"source\":%lld,\"asserted\":%d,\"pending\":\"0x%08X\"}",
        id, source, asserted ? 1 : 0, vb_irq_pending());
    send_response(buf);
}

static void handle_pad_state(long long id) {
    char buf[128];
    snprintf(buf, sizeof(buf),
             "{\"ok\":true,\"cmd\":\"pad_state\",\"id\":%lld,\"pad\":\"0x%04X\"}",
             id, vb_input_get_pad());
    send_response(buf);
}

/* set_input pad=0xNNNN — set the active-high 16-bit pad mask.
 * Works in --headless without a controller (main.cpp's per-frame
 * keyboard/XInput handler leaves the pad untouched in that case);
 * when the SDL window is open the keyboard handler will clobber
 * this on the next loop iteration. */
static void handle_set_input(long long id, const char* line) {
    long long pad = 0;
    if (!extract_int(line, "\"pad\"", &pad) && !extract_int(line, "\"mask\"", &pad)) {
        send_response("{\"ok\":false,\"error\":\"set_input requires 'pad' (16-bit mask)\"}");
        return;
    }
    if(pad<0 || pad>65535) { send_response("{\"ok\":false,\"error\":\"invalid input mask\"}");return; }
    s_input_override=1;
    vb_input_set_pad((uint16_t)pad);
    char buf[128];
    snprintf(buf, sizeof(buf),
             "{\"ok\":true,\"cmd\":\"set_input\",\"id\":%lld,\"pad\":\"0x%04X\"}",
             id, vb_input_get_pad());
    send_response(buf);
}

/* Frame-counted button press for deterministic headless navigation (mirrors
 * the psx/nes `press` command): {"buttons":<mask>,"frames":<n>}. The mask
 * uses the VB_PAD_* hardware bits (e.g. START=0x1000, A=0x4). The hold lasts
 * exactly `frames` VIP game-frames then auto-releases — independent of
 * headless wall-clock speed. */
static void handle_press(long long id, const char* line) {
    long long buttons = 0, frames = 4;
    if (!extract_int(line, "\"buttons\"", &buttons)) {
        send_response("{\"ok\":false,\"error\":\"press requires 'buttons' (VB_PAD_* mask)\"}");
        return;
    }
    extract_int(line, "\"frames\"", &frames);
    if (frames < 1) frames = 1;
    vb_input_press((uint16_t)(buttons & 0xFFFFu), (int)frames);
    s_input_override=2;
    char buf[160];
    snprintf(buf, sizeof(buf),
             "{\"ok\":true,\"cmd\":\"press\",\"id\":%lld,\"buttons\":\"0x%04X\","
             "\"frames\":%lld}", id, (unsigned)(buttons & 0xFFFFu), frames);
    send_response(buf);
}

static void handle_irq_state(long long id) {
    char buf[256];
    snprintf(buf, sizeof(buf),
             "{\"ok\":true,\"cmd\":\"irq_state\",\"id\":%lld,"
             "\"pending\":\"0x%08X\",\"in_service\":\"0x%08X\","
             "\"highest_pending_level\":%d}",
             id, vb_irq_pending(), vb_irq_in_service(),
             vb_irq_highest_pending_level());
    send_response(buf);
}

/* Append `s` to *p as a JSON string body, escaping the characters JSON
 * requires (", \, and control chars). Advances *p. */
static void json_escape_append(char** p, const char* s) {
    char* q = *p;
    for (; *s; ++s) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') { *q++ = '\\'; *q++ = (char)c; }
        else if (c == '\n')        { *q++ = '\\'; *q++ = 'n'; }
        else if (c == '\r')        { *q++ = '\\'; *q++ = 'r'; }
        else if (c == '\t')        { *q++ = '\\'; *q++ = 't'; }
        else if (c < 0x20)         { q += sprintf(q, "\\u%04x", c); }
        else                       { *q++ = (char)c; }
    }
    *p = q;
}

/* Query the always-on VSU shadow status ring (prove/degrade transitions +
 * current substitution state). Replaces the old stderr DEGRADED/proven log;
 * the probe reads the ring for the window of interest, never arms it. */
static void handle_audio_shadow_state(long long id) {
    VbVsuShadowStatus st;
    vb_vsu_shadow_get_status(&st);

    /* Header + fixed fields ~320B; each event ~ 80B + escaped reason (<=160*2
     * worst case). Budget generously. */
    size_t bodysz = 512 + (size_t)st.n_events * (96 + 2 * 160);
    char* body = (char*)malloc(bodysz);
    if (!body) { send_response("{\"ok\":false,\"error\":\"oom\"}"); return; }

    char* p = body;
    p += sprintf(p,
        "{\"ok\":true,\"cmd\":\"audio_shadow_state\",\"id\":%lld,"
        "\"enabled\":%s,\"substituting\":%s,"
        "\"last_r\":%.4f,\"last_ratio\":%.4f,\"gain\":%.4f,"
        "\"samples_seen\":%llu,\"engage_count\":%llu,\"degrade_count\":%llu,"
        "\"events\":[",
        id,
        st.enabled ? "true" : "false",
        st.substituting ? "true" : "false",
        (double)st.last_r, (double)st.last_ratio, (double)st.gain,
        (unsigned long long)st.samples_seen,
        (unsigned long long)st.engage_count,
        (unsigned long long)st.degrade_count);
    for (uint32_t i = 0; i < st.n_events; ++i) {
        const VbVsuShadowEvent* e = &st.events[i];
        p += sprintf(p,
            "%s{\"seq\":%llu,\"sample\":%llu,\"kind\":\"%s\","
            "\"r\":%.4f,\"ratio\":%.4f,\"gain\":%.4f,\"reason\":\"",
            i ? "," : "",
            (unsigned long long)e->seq, (unsigned long long)e->sample,
            e->kind == VB_VSU_SHADOW_EV_ENGAGE ? "engage" : "degrade",
            (double)e->r, (double)e->ratio, (double)e->gain);
        json_escape_append(&p, e->reason);
        p += sprintf(p, "\"}");
    }
    p += sprintf(p, "]}");
    send_response(body);
    free(body);
}

/* Stream a window of the always-on VSU output ring as little-endian S16
 * stereo hex. Probe-pulls by ABSOLUTE frame index so the accuracy
 * harness can drain continuously from boot without disturbing SDL
 * playback (vb_vsu_read_abs is non-destructive). Mirrors the oracle's
 * `audio_pcm` on port 4391 — same wire shape, different ring. */
static void handle_audio_pcm(long long id, const char* line) {
    long long start = 0, maxf = 8192;
    extract_int(line, "\"start\"", &start);
    extract_int(line, "\"max\"",   &maxf);
    if (start < 0) start = 0;
    if (maxf < 1)      maxf = 1;
    if (maxf > 16384)  maxf = 16384;   /* bound the response body */

    int16_t* pcm = (int16_t*)malloc((size_t)maxf * 2 * sizeof(int16_t));
    if (!pcm) { send_response("{\"ok\":false,\"error\":\"oom\"}"); return; }

    uint64_t head = 0, resident_lo = 0;
    size_t got = vb_vsu_read_abs((uint64_t)start, pcm, (size_t)maxf,
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
        id, vb_vsu_output_hz(),
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

/* Stream a window of the always-on per-instruction CPU-hook ring as hex
 * of the raw records (cpuhook.h vb_cpuhook_rec; identical wire shape to
 * the oracle's cpuhook on 4391, so one comparator parses both). Empty
 * unless the runtime was built -DVB_CPUHOOK_ENABLE. Ring QUERY by
 * absolute retire-seq — never an armed capture (Rule 3). */
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

    uint64_t head = 0, resident_lo = 0;
    size_t got = vb_cpuhook_read_abs((uint64_t)start, recs, (size_t)maxn,
                                     &head, &resident_lo);
    uint64_t begin = ((uint64_t)start < resident_lo)
                         ? resident_lo : (uint64_t)start;

    char* body = (char*)malloc(256 + got * sizeof(vb_cpuhook_rec) * 2);
    if (!body) { free(recs); send_response("{\"ok\":false,\"error\":\"oom\"}"); return; }
    char* p = body;
    p += sprintf(p,
        "{\"ok\":true,\"cmd\":\"cpuhook\",\"id\":%lld,\"recsize\":%u,"
        "\"head\":%llu,\"resident_lo\":%llu,\"begin\":%llu,\"returned\":%u,\"hex\":\"",
        id, (unsigned)sizeof(vb_cpuhook_rec),
        (unsigned long long)head, (unsigned long long)resident_lo,
        (unsigned long long)begin, (unsigned)got);
    const unsigned char* raw = (const unsigned char*)recs;
    for (size_t i = 0; i < got * sizeof(vb_cpuhook_rec); ++i)
        p += sprintf(p, "%02X", raw[i]);
    p += sprintf(p, "\"}");
    send_response(body);
    free(body);
    free(recs);
}

/* Stream a window of the always-on VIP draw-timing phase ring as hex of the
 * raw records (vip_phase.h vb_vipphase_rec; byte-identical to the oracle's
 * vip_phase ring on 4391, so one comparator parses both). Ring QUERY by
 * absolute event-seq — never an armed capture (Rule 3). */
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

    uint64_t head = 0, resident_lo = 0;
    size_t got = vb_vip_phase_read_abs((uint64_t)start, recs, (size_t)maxn,
                                       &head, &resident_lo);
    uint64_t begin = ((uint64_t)start < resident_lo)
                         ? resident_lo : (uint64_t)start;

    char* body = (char*)malloc(256 + got * sizeof(vb_vipphase_rec) * 2);
    if (!body) { free(recs); send_response("{\"ok\":false,\"error\":\"oom\"}"); return; }
    char* p = body;
    p += sprintf(p,
        "{\"ok\":true,\"cmd\":\"vip_phase\",\"id\":%lld,\"recsize\":%u,"
        "\"head\":%llu,\"resident_lo\":%llu,\"begin\":%llu,\"returned\":%u,\"hex\":\"",
        id, (unsigned)sizeof(vb_vipphase_rec),
        (unsigned long long)head, (unsigned long long)resident_lo,
        (unsigned long long)begin, (unsigned)got);
    const unsigned char* raw = (const unsigned char*)recs;
    for (size_t i = 0; i < got * sizeof(vb_vipphase_rec); ++i)
        p += sprintf(p, "%02X", raw[i]);
    p += sprintf(p, "\"}");
    send_response(body);
    free(body);
    free(recs);
}

/* Stream a window of the always-on per-game-frame WRAM fingerprint ring as
 * hex of the raw records (wram_hash.h vb_wramhash_rec; byte-identical to the
 * oracle's wram_hash ring on 4391). Ring QUERY by absolute GAME_START seq. */
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

    uint64_t head = 0, resident_lo = 0;
    size_t got = vb_wram_hash_read_abs((uint64_t)start, recs, (size_t)maxn,
                                       &head, &resident_lo);
    uint64_t begin = ((uint64_t)start < resident_lo)
                         ? resident_lo : (uint64_t)start;

    char* body = (char*)malloc(256 + got * sizeof(vb_wramhash_rec) * 2);
    if (!body) { free(recs); send_response("{\"ok\":false,\"error\":\"oom\"}"); return; }
    char* p = body;
    p += sprintf(p,
        "{\"ok\":true,\"cmd\":\"wram_hash\",\"id\":%lld,\"recsize\":%u,"
        "\"head\":%llu,\"resident_lo\":%llu,\"begin\":%llu,\"returned\":%u,\"hex\":\"",
        id, (unsigned)sizeof(vb_wramhash_rec),
        (unsigned long long)head, (unsigned long long)resident_lo,
        (unsigned long long)begin, (unsigned)got);
    const unsigned char* raw = (const unsigned char*)recs;
    for (size_t i = 0; i < got * sizeof(vb_wramhash_rec); ++i)
        p += sprintf(p, "%02X", raw[i]);
    p += sprintf(p, "\"}");
    send_response(body);
    free(body);
    free(recs);
}

static void handle_memory_map(long long id) {
    char buf[512];
    snprintf(buf, sizeof(buf),
        "{\"ok\":true,\"cmd\":\"memory_map\",\"id\":%lld,\"rom_size\":%u,"
        "\"regions\":["
            "{\"name\":\"VIP\",\"base\":\"0x00000000\",\"size\":\"0x01000000\"},"
            "{\"name\":\"VSU\",\"base\":\"0x01000000\",\"size\":\"0x01000000\"},"
            "{\"name\":\"MISC\",\"base\":\"0x02000000\",\"size\":\"0x01000000\"},"
            "{\"name\":\"WRAM\",\"base\":\"0x05000000\",\"size\":\"0x00010000\"},"
            "{\"name\":\"CART_RAM\",\"base\":\"0x06000000\",\"size\":\"0x01000000\"},"
            "{\"name\":\"CART_ROM\",\"base\":\"0x07000000\",\"size\":\"0x01000000\"}"
        "]}", id, vb_rom_size());
    send_response(buf);
}

static void handle_read_ram(long long id, const char* line) {
    long long addr = 0, len = 16;
    extract_int(line, "\"addr\"", &addr);
    extract_int(line, "\"len\"", &len);
    uint32_t begin=(uint32_t)addr&0x07ffffffu;
    uint64_t end=(uint64_t)begin+(uint64_t)len;
    if(addr<0 || addr>UINT32_MAX || len<1 || len>65536 || end>0x08000000u ||
       ((begin>>24)!=((end-1)>>24)) || (begin>=0x03000000u && begin<0x05000000u)) {
        send_response("{\"ok\":false,\"error\":\"invalid or unmapped read range\"}");return;
    }
    uint8_t* tmp = (uint8_t*)malloc((size_t)len);
    if (!tmp) {
        send_response("{\"ok\":false,\"error\":\"oom\"}");
        return;
    }
    /* Note: this will fatal-abort if any byte hits unmapped memory.
     * That's the correct behaviour — querying unmapped state is also
     * a bug worth surfacing. */
    vb_memory_dump((uint32_t)addr, tmp, (size_t)len);
    char* body = (char*)malloc(64 + (size_t)len * 2 + 64);
    if (!body) { free(tmp); send_response("{\"ok\":false,\"error\":\"oom\"}"); return; }
    char* p = body;
    p += sprintf(p, "{\"ok\":true,\"cmd\":\"read_ram\",\"id\":%lld,"
                    "\"addr\":\"0x%08X\",\"len\":%u,\"hex\":\"",
                 id, (unsigned)addr, (unsigned)len);
    for (long long i = 0; i < len; ++i) p += sprintf(p, "%02X", tmp[i]);
    p += sprintf(p, "\"}");
    send_response(body);
    free(body);
    free(tmp);
}

static void handle_pause(long long id, int state) {
    vb_debug_server_set_paused(state);
    char buf[128];
    snprintf(buf, sizeof(buf),
             "{\"ok\":true,\"cmd\":\"%s\",\"id\":%lld,\"paused\":%s}",
             state ? "pause" : "continue", id, state ? "true" : "false");
    send_response(buf);
}

static void handle_quit(long long id) {
    char buf[64];
    snprintf(buf, sizeof(buf), "{\"ok\":true,\"cmd\":\"quit\",\"id\":%lld}", id);
    send_response(buf);
    s_quit = 1;
}

static void handle_wtrace_stats(long long id) {
    uint64_t total = vb_wtrace_seq();
    size_t cap = vb_wtrace_capacity();
    int wrapped = (total > (uint64_t)cap) ? 1 : 0;
    uint64_t oldest = wrapped ? (total - (uint64_t)cap) : 0;
    char buf[256];
    snprintf(buf, sizeof(buf),
        "{\"ok\":true,\"cmd\":\"wtrace_stats\",\"id\":%lld,"
        "\"total\":%llu,\"capacity\":%zu,\"wrapped\":%s,"
        "\"oldest_seq\":%llu,\"newest_seq\":%llu}",
        id, (unsigned long long)total, cap, wrapped ? "true" : "false",
        (unsigned long long)oldest,
        (unsigned long long)(total ? total - 1 : 0));
    send_response(buf);
}

static void handle_wtrace_dump(long long id, const char* line) {
    long long addr_min = 0, addr_max = 0;
    long long pc_min   = 0, pc_max   = 0;
    long long from_seq = 0, to_seq   = 0;
    long long tail = 0, limit = 256;
    extract_int(line, "\"addr_min\"", &addr_min);
    extract_int(line, "\"addr_max\"", &addr_max);
    extract_int(line, "\"pc_min\"",   &pc_min);
    extract_int(line, "\"pc_max\"",   &pc_max);
    extract_int(line, "\"from_seq\"", &from_seq);
    extract_int(line, "\"to_seq\"",   &to_seq);
    extract_int(line, "\"tail\"",     &tail);
    extract_int(line, "\"limit\"",    &limit);
    if (limit < 1)    limit = 1;
    if (limit > 4096) limit = 4096;

    /* `tail`: shift the scan window so we cover at most the last N
     * absolute seqs (matching is still applied within that window). */
    if (tail > 0) {
        uint64_t avail = vb_wtrace_seq();
        uint64_t lo    = (uint64_t)tail > avail ? 0
                                                : avail - (uint64_t)tail;
        if (from_seq == 0 || (uint64_t)from_seq < lo) {
            from_seq = (long long)lo;
        }
    }

    VBWTraceFilter f;
    f.from_seq    = (uint64_t)from_seq;
    f.to_seq      = (uint64_t)to_seq;
    f.addr_min    = (uint32_t)addr_min;
    f.addr_max    = (uint32_t)addr_max;
    f.pc_min      = (uint32_t)pc_min;
    f.pc_max      = (uint32_t)pc_max;
    f.max_results = (uint32_t)limit;

    VBWTraceEntry* tmp = (VBWTraceEntry*)
        malloc(sizeof(VBWTraceEntry) * (size_t)limit);
    if (!tmp) {
        send_response("{\"ok\":false,\"error\":\"oom\"}");
        return;
    }
    size_t n = vb_wtrace_query(&f, tmp, (size_t)limit);

    /* Each entry serialises to roughly 130 bytes; budget conservatively. */
    size_t bodysz = 320 + n * 160;
    char* body = (char*)malloc(bodysz);
    if (!body) {
        free(tmp);
        send_response("{\"ok\":false,\"error\":\"oom\"}");
        return;
    }
    char* p = body;
    p += sprintf(p,
        "{\"ok\":true,\"cmd\":\"wtrace_dump\",\"id\":%lld,"
        "\"total\":%llu,\"count\":%zu,\"entries\":[",
        id, (unsigned long long)vb_wtrace_seq(), n);
    for (size_t i = 0; i < n; ++i) {
        const VBWTraceEntry* e = &tmp[i];
        p += sprintf(p,
            "%s{\"seq\":%llu,\"cycle\":%llu,\"pc\":\"0x%08X\","
            "\"addr\":\"0x%08X\",\"value\":\"0x%08X\","
            "\"width\":%u,\"region\":%u}",
            (i == 0 ? "" : ","),
            (unsigned long long)e->seq, (unsigned long long)e->cycle,
            e->pc, e->addr, e->value,
            (unsigned)e->width, (unsigned)e->region);
    }
    p += sprintf(p, "]}");
    send_response(body);
    free(body);
    free(tmp);
}

static void handle_wtrace_reset(long long id) {
    vb_wtrace_reset();
    char buf[128];
    snprintf(buf, sizeof(buf),
        "{\"ok\":true,\"cmd\":\"wtrace_reset\",\"id\":%lld,\"total\":0}", id);
    send_response(buf);
}

static void handle_fntrace_stats(long long id) {
    uint64_t total = vb_fntrace_seq();
    size_t cap = vb_fntrace_capacity();
    int wrapped = (total > (uint64_t)cap) ? 1 : 0;
    uint64_t oldest = wrapped ? (total - (uint64_t)cap) : 0;
    char buf[256];
    snprintf(buf, sizeof(buf),
        "{\"ok\":true,\"cmd\":\"fntrace_stats\",\"id\":%lld,"
        "\"total\":%llu,\"capacity\":%zu,\"wrapped\":%s,"
        "\"oldest_seq\":%llu,\"newest_seq\":%llu}",
        id, (unsigned long long)total, cap, wrapped ? "true" : "false",
        (unsigned long long)oldest,
        (unsigned long long)(total ? total - 1 : 0));
    send_response(buf);
}

static void handle_fntrace_dump(long long id, const char* line) {
    long long pc_min = 0, pc_max = 0;
    long long lp_min = 0, lp_max = 0;
    long long from_seq = 0, to_seq = 0;
    long long tail = 0, limit = 256;
    extract_int(line, "\"pc_min\"",   &pc_min);
    extract_int(line, "\"pc_max\"",   &pc_max);
    extract_int(line, "\"lp_min\"",   &lp_min);
    extract_int(line, "\"lp_max\"",   &lp_max);
    extract_int(line, "\"from_seq\"", &from_seq);
    extract_int(line, "\"to_seq\"",   &to_seq);
    extract_int(line, "\"tail\"",     &tail);
    extract_int(line, "\"limit\"",    &limit);
    if (limit < 1)    limit = 1;
    if (limit > 4096) limit = 4096;

    if (tail > 0) {
        uint64_t avail = vb_fntrace_seq();
        uint64_t lo    = (uint64_t)tail > avail ? 0
                                                : avail - (uint64_t)tail;
        if (from_seq == 0 || (uint64_t)from_seq < lo) {
            from_seq = (long long)lo;
        }
    }

    VBFnTraceFilter f;
    f.from_seq    = (uint64_t)from_seq;
    f.to_seq      = (uint64_t)to_seq;
    f.pc_min      = (uint32_t)pc_min;
    f.pc_max      = (uint32_t)pc_max;
    f.lp_min      = (uint32_t)lp_min;
    f.lp_max      = (uint32_t)lp_max;
    f.max_results = (uint32_t)limit;

    VBFnTraceEntry* tmp = (VBFnTraceEntry*)
        malloc(sizeof(VBFnTraceEntry) * (size_t)limit);
    if (!tmp) {
        send_response("{\"ok\":false,\"error\":\"oom\"}");
        return;
    }
    size_t n = vb_fntrace_query(&f, tmp, (size_t)limit);

    size_t bodysz = 320 + n * 128;
    char* body = (char*)malloc(bodysz);
    if (!body) {
        free(tmp);
        send_response("{\"ok\":false,\"error\":\"oom\"}");
        return;
    }
    char* p = body;
    p += sprintf(p,
        "{\"ok\":true,\"cmd\":\"fntrace_dump\",\"id\":%lld,"
        "\"total\":%llu,\"count\":%zu,\"entries\":[",
        id, (unsigned long long)vb_fntrace_seq(), n);
    for (size_t i = 0; i < n; ++i) {
        const VBFnTraceEntry* e = &tmp[i];
        p += sprintf(p,
            "%s{\"seq\":%llu,\"cycle\":%llu,"
            "\"pc\":\"0x%08X\",\"lp\":\"0x%08X\"}",
            (i == 0 ? "" : ","),
            (unsigned long long)e->seq, (unsigned long long)e->cycle,
            e->pc, e->lp);
    }
    p += sprintf(p, "]}");
    send_response(body);
    free(body);
    free(tmp);
}

static void handle_fntrace_reset(long long id) {
    vb_fntrace_reset();
    char buf[128];
    snprintf(buf, sizeof(buf),
        "{\"ok\":true,\"cmd\":\"fntrace_reset\",\"id\":%lld,\"total\":0}", id);
    send_response(buf);
}

static void handle_unknown(const char* cmd, long long id) {
    char buf[256];
    snprintf(buf, sizeof(buf),
             "{\"ok\":false,\"id\":%lld,\"error\":\"unknown command\","
             "\"cmd\":\"%s\",\"hint\":\"see TCP.md for the command surface\"}",
             id, cmd);
    send_response(buf);
}

/* ---- Dispatch ---- */
static void dispatch_line(char* line) {
    char cmd[64] = {0};
    long long id = 0;
    extract_int(line, "\"id\"", &id);
    s_request_id=id;

    /* Bare-command shortcut: a single token line. */
    char* nl = strpbrk(line, "\r\n");
    if (nl) *nl = 0;
    /* Trim leading whitespace */
    while (*line == ' ' || *line == '\t') line++;

    if (line[0] == '{') {
        if (!extract_str(line, "\"cmd\"", cmd, sizeof(cmd))) {
            send_response("{\"ok\":false,\"error\":\"missing 'cmd' field\"}");
            return;
        }
    } else {
        size_t i = 0;
        while (line[i] && line[i] != ' ' && line[i] != '\t' && i + 1 < sizeof(cmd)) {
            cmd[i] = line[i]; i++;
        }
        cmd[i] = 0;
    }

    if (cmd[0] == 0) {
        send_response("{\"ok\":false,\"error\":\"empty command\"}");
        return;
    }

    if (strcmp(cmd, "device_state") == 0) { char body[16384]; vb_device_response(body,sizeof(body)); send_response(body); }
    else if (strcmp(cmd, "capabilities") == 0) { send_response("{\"ok\":true,\"protocol\":2,\"commands\":[\"device_state\",\"audio_pcm\",\"audio_shadow_state\",\"breakpoint\",\"capabilities\",\"capture_dump\",\"clear_input\",\"continue\",\"cpuhook\",\"execution_stats\",\"fntrace_dump\",\"fntrace_reset\",\"fntrace_stats\",\"frame\",\"get_frame\",\"get_registers\",\"history\",\"irq_force\",\"irq_state\",\"memory_map\",\"overrides_state\",\"pad_state\",\"pause\",\"ping\",\"press\",\"psw_set\",\"psw_state\",\"quit\",\"read_ram\",\"recolor_reload\",\"recolor_state\",\"recolor_trace\",\"run_frames\",\"screenshot\",\"set_input\",\"source_dump\",\"step\",\"timer_state\",\"vip_phase\",\"vip_state\",\"watchdog\",\"world_map\",\"world_trace\",\"wram_anchors\",\"wram_hash\",\"write_ram\",\"wtrace_dump\",\"wtrace_reset\",\"wtrace_stats\"],\"max_read_bytes\":65536,\"instruction_control\":true,\"trace_version\":2}"); }
    else if (strcmp(cmd, "ping") == 0)         handle_ping(id);
    else if (strcmp(cmd, "step") == 0) {
        long long count=1;extract_int(line,"\"count\"",&count);
        if(!s_cpu || !vb_debug_server_is_paused() || count<1 || count>1000000 || s_cpu->halted) {
            send_response("{\"ok\":false,\"error\":\"step requires paused, non-halted CPU and count 1..1000000\"}");
        } else {
            vb_debug_server_set_paused(0);vb_execution.stepping=1;vb_execution.step_remaining=(uint64_t)count;
            char result[128];snprintf(result,sizeof(result),"{\"ok\":true,\"id\":%lld,\"count\":%lld}",id,count);send_response(result);
        }
    }
    else if (strcmp(cmd, "breakpoint") == 0) {
        long long pc=-1;extract_int(line,"\"pc\"",&pc);
        if(pc < -1 || pc>UINT32_MAX || (pc!=-1 && (pc&1))) { send_response("{\"ok\":false,\"error\":\"invalid breakpoint PC\"}"); }
        else { vb_execution.breakpoint_enabled=pc!=-1;vb_execution.breakpoint_pc=(uint32_t)pc;
            char result[128];snprintf(result,sizeof(result),"{\"ok\":true,\"id\":%lld,\"enabled\":%d}",id,vb_execution.breakpoint_enabled);send_response(result);
        }
    }
    else if (strcmp(cmd, "execution_stats") == 0) {
        char result[384];
        snprintf(result,sizeof(result),"{\"ok\":true,\"id\":%lld,\"mode\":%d,\"interpreted\":%llu,\"fallback\":%llu,\"native_entries\":%llu,\"first_fallback_pc\":%u,\"last_fallback_pc\":%u,\"native_instructions\":%llu,\"stopped\":%d}",
                 id,vb_execution.mode,(unsigned long long)vb_execution.interpreted,
                 (unsigned long long)vb_execution.fallback,(unsigned long long)vb_execution.native_entries,
                 vb_execution.first_fallback_pc,vb_execution.last_fallback_pc,(unsigned long long)vb_execution.native_instructions,vb_execution.stopped);
        send_response(result);
    }
    else if (strcmp(cmd, "frame") == 0)        handle_frame(id);
    else if (strcmp(cmd, "get_registers") == 0)handle_get_registers(id);
    else if (strcmp(cmd, "psw_state") == 0)    handle_psw_state(id);
    else if (strcmp(cmd, "psw_set") == 0)      handle_psw_set(id, line);
    else if (strcmp(cmd, "write_ram") == 0) {
        long long addr=-1, value=-1;
        extract_int(line,"\"addr\"",&addr); extract_int(line,"\"val\"",&value);
        unsigned region=((uint32_t)addr>>24)&7;
        if (!vb_debug_server_is_paused() || addr<0 || addr>UINT32_MAX || value<0 || value>255 || (region!=5 && region!=6)) {
            send_response("{\"ok\":false,\"error\":\"write_ram needs paused CPU, WRAM/SRAM address and byte val\"}");
        } else {
            vb_write8((uint32_t)addr,(uint8_t)value);
            char result[128];snprintf(result,sizeof(result),"{\"ok\":true,\"id\":%lld}",id);send_response(result);
        }
    }
    else if (strcmp(cmd, "history") == 0 || strcmp(cmd, "get_frame") == 0) {
        long long start=0,count=1;
        extract_int(line,"\"start\"",&start);extract_int(line,"\"count\"",&count);
        if(start<0 || count<1 || count>256) { send_response("{\"ok\":false,\"error\":\"invalid history range\"}"); }
        else {
            VBFrameRecord records[256];size_t n=vb_ring_frame_dump((uint64_t)start,records,(size_t)count);
            char* body=(char*)malloc(n*640+256);
            if(!body) { send_response("{\"ok\":false,\"error\":\"oom\"}");return; }
            char* cur=body;
            cur+=sprintf(cur,"{\"ok\":true,\"id\":%lld,\"head\":%llu,\"records\":[",id,(unsigned long long)vb_ring_frame_seq());
            for(size_t i=0;i<n;++i) {
                cur+=sprintf(cur,"%s{\"seq\":%llu,\"frame\":%u,\"pc\":%u,\"psw\":%u,\"gpr\":[",i?",":"",(unsigned long long)records[i].seq,records[i].frame_idx,records[i].pc,records[i].sysreg_psw);
                for(unsigned j=0;j<32;++j) cur+=sprintf(cur,"%s%u",j?",":"",records[i].gpr_snapshot[j]);
                cur+=sprintf(cur,"]}");
            }
            sprintf(cur,"]}");send_response(body);free(body);
        }
    }
    else if (strcmp(cmd, "read_ram") == 0)     handle_read_ram(id, line);
    else if (strcmp(cmd, "pad_state") == 0)    handle_pad_state(id);
    else if (strcmp(cmd, "set_input") == 0)    handle_set_input(id, line);
    else if (strcmp(cmd, "clear_input") == 0) { handle_set_input(id, "{\"pad\":0}");s_input_override=0; }
    else if (strcmp(cmd, "press") == 0)        handle_press(id, line);
    else if (strcmp(cmd, "irq_state") == 0)    handle_irq_state(id);
    else if (strcmp(cmd, "irq_force") == 0)    handle_irq_force(id, line);
    else if (strcmp(cmd, "timer_state") == 0)  handle_timer_state(id);
    else if (strcmp(cmd, "vip_state") == 0)    handle_vip_state(id);
    else if (strcmp(cmd, "screenshot") == 0)   handle_screenshot(id, line);
    else if (strcmp(cmd, "watchdog") == 0)     handle_watchdog(id);
    else if (strcmp(cmd, "capture_dump") == 0) handle_capture_dump(id, line);
    else if (strcmp(cmd, "source_dump") == 0) handle_source_dump(line);
    else if (strcmp(cmd, "overrides_state") == 0) handle_overrides_state(id);
    else if (strcmp(cmd, "recolor_state") == 0) handle_recolor_state(id);
    else if (strcmp(cmd, "recolor_reload") == 0) handle_recolor_reload(id);
    else if (strcmp(cmd, "recolor_trace") == 0) handle_recolor_trace(id, line);
    else if (strcmp(cmd, "world_map") == 0)    handle_world_map(id, line);
    else if (strcmp(cmd, "world_trace") == 0)  handle_world_trace(id, line);
    else if (strcmp(cmd, "wram_anchors") == 0) handle_wram_anchors(id, line);
    else if (strcmp(cmd, "audio_shadow_state") == 0) handle_audio_shadow_state(id);
    else if (strcmp(cmd, "audio_pcm") == 0)    handle_audio_pcm(id, line);
    else if (strcmp(cmd, "cpuhook") == 0)      handle_cpuhook(id, line);
    else if (strcmp(cmd, "vip_phase") == 0)    handle_vip_phase(id, line);
    else if (strcmp(cmd, "wram_hash") == 0)    handle_wram_hash(id, line);
    else if (strcmp(cmd, "memory_map") == 0)   handle_memory_map(id);
    else if (strcmp(cmd, "wtrace_stats") == 0) handle_wtrace_stats(id);
    else if (strcmp(cmd, "wtrace_dump") == 0)  handle_wtrace_dump(id, line);
    else if (strcmp(cmd, "wtrace_reset") == 0) handle_wtrace_reset(id);
    else if (strcmp(cmd, "fntrace_stats") == 0) handle_fntrace_stats(id);
    else if (strcmp(cmd, "fntrace_dump") == 0)  handle_fntrace_dump(id, line);
    else if (strcmp(cmd, "fntrace_reset") == 0) handle_fntrace_reset(id);
    else if (strcmp(cmd, "run_frames") == 0) {
        long long frames = 0;
        extract_int(line, "\"frames\"", &frames);
        if (!s_cpu || frames < 1 || frames > 10000 ||
            (uint64_t)s_cpu->frame + (uint64_t)frames > UINT32_MAX) {
            send_response("{\"ok\":false,\"error\":\"frames must be 1..10000\"}");
        } else {
            vb_debug_server_set_paused(0);
            s_step_target = s_cpu->frame + (uint32_t)frames;
            char result[128];
            snprintf(result, sizeof(result), "{\"ok\":true,\"id\":%lld,\"target\":%u}", id, s_step_target);
            send_response(result);
        }
    }
    else if (strcmp(cmd, "pause") == 0)        handle_pause(id, 1);
    else if (strcmp(cmd, "continue") == 0)     handle_pause(id, 0);
    else if (strcmp(cmd, "quit") == 0)         handle_quit(id);
    else                                       handle_unknown(cmd, id);
}

int vb_debug_server_poll(void) {
    if (s_listener == VB_BAD_SOCKET) return s_quit;

    if (s_client == VB_BAD_SOCKET) {
        sock_t c = accept(s_listener, NULL, NULL);
        if (c != VB_BAD_SOCKET) {
            set_nonblocking(c);
            s_client = c;
            vb_stream_reset();
        }
    }
    if (s_client != VB_BAD_SOCKET && !vb_stream_poll(s_client, dispatch_line)) {
        vb_close_socket(s_client); s_client = VB_BAD_SOCKET;
        vb_stream_reset();
    }
    return s_quit && vb_stream.size == 0;
}
