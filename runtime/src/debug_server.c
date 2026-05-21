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

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "input.h"
#include "interrupts.h"
#include "memory.h"
#include "ring_frame.h"
#include "timer.h"
#include "vip.h"
#include "wtrace.h"
#include "fntrace.h"

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

static sock_t s_listener = VB_BAD_SOCKET;
static sock_t s_client   = VB_BAD_SOCKET;
static CPUState* s_cpu   = NULL;
static int s_paused      = 0;
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

static void send_response(const char* body) {
    if (s_client == VB_BAD_SOCKET) return;
    size_t n = strlen(body);
    /* Best-effort send; ignore partial-send for the skeleton. */
    send(s_client, body, (int)n, 0);
    send(s_client, "\n", 1, 0);
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
        "\"id\":%u,\"ae\":%u,\"ep\":%u,\"np\":%u,"
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

/* Minimal BMP writer (BI_RGB 32bpp, top-down via negative height). Used
 * by the screenshot command. The format is intentionally trivial — a
 * BITMAPFILEHEADER + BITMAPINFOHEADER + raw BGRA pixels. */
static int write_bmp_32bpp(const char* path, int w, int h,
                           const uint32_t* argb) {
    FILE* f = fopen(path, "wb");
    if (!f) return -1;
    uint32_t pixel_bytes = (uint32_t)(w * h * 4);
    uint32_t file_size   = 54u + pixel_bytes;
    uint8_t header[54] = {0};
    /* BITMAPFILEHEADER */
    header[0] = 'B'; header[1] = 'M';
    header[2]  = (uint8_t)(file_size      );
    header[3]  = (uint8_t)(file_size >>  8);
    header[4]  = (uint8_t)(file_size >> 16);
    header[5]  = (uint8_t)(file_size >> 24);
    header[10] = 54;
    /* BITMAPINFOHEADER */
    header[14] = 40;
    header[18] = (uint8_t)(w      );
    header[19] = (uint8_t)(w >>  8);
    header[20] = (uint8_t)(w >> 16);
    header[21] = (uint8_t)(w >> 24);
    /* Negative height for top-down. */
    int32_t neg_h = -h;
    header[22] = (uint8_t)((uint32_t)neg_h      );
    header[23] = (uint8_t)((uint32_t)neg_h >>  8);
    header[24] = (uint8_t)((uint32_t)neg_h >> 16);
    header[25] = (uint8_t)((uint32_t)neg_h >> 24);
    header[26] = 1;                          /* planes */
    header[28] = 32;                         /* bpp */
    fwrite(header, 1, sizeof(header), f);
    /* BMP stores BGRA, our buffer is ARGB → swap on write. */
    for (int i = 0; i < w * h; ++i) {
        uint32_t p = argb[i];
        uint8_t bgra[4] = {
            (uint8_t)(p      ),  /* B */
            (uint8_t)(p >>  8),  /* G */
            (uint8_t)(p >> 16),  /* R */
            (uint8_t)(p >> 24),  /* A */
        };
        fwrite(bgra, 1, 4, f);
    }
    fclose(f);
    return 0;
}

static void handle_screenshot(long long id, const char* line) {
    char path[256] = {0};
    long long eye = 0;
    extract_str(line, "\"path\"", path, sizeof(path));
    extract_int(line, "\"eye\"", &eye);
    if (!path[0]) {
        snprintf(path, sizeof(path),
                 (eye ? "vb-runtime-eye1.bmp" : "vb-runtime-eye0.bmp"));
    }
    uint32_t* buf = (uint32_t*)malloc(384u * 224u * 4u);
    if (!buf) {
        send_response("{\"ok\":false,\"error\":\"oom\"}");
        return;
    }
    vb_vip_render_framebuffer((int)eye, buf);
    int rc = write_bmp_32bpp(path, 384, 224, buf);
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
    if (!extract_int(line, "\"pad\"", &pad)) {
        send_response("{\"ok\":false,\"error\":\"set_input requires 'pad' (16-bit mask)\"}");
        return;
    }
    vb_input_set_pad((uint16_t)(pad & 0xFFFFu));
    char buf[128];
    snprintf(buf, sizeof(buf),
             "{\"ok\":true,\"cmd\":\"set_input\",\"id\":%lld,\"pad\":\"0x%04X\"}",
             id, vb_input_get_pad());
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
    if (len < 1) len = 1;
    if (len > 4096) len = 4096;
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
    s_paused = state;
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

    if      (strcmp(cmd, "ping") == 0)         handle_ping(id);
    else if (strcmp(cmd, "frame") == 0)        handle_frame(id);
    else if (strcmp(cmd, "get_registers") == 0)handle_get_registers(id);
    else if (strcmp(cmd, "psw_state") == 0)    handle_psw_state(id);
    else if (strcmp(cmd, "psw_set") == 0)      handle_psw_set(id, line);
    else if (strcmp(cmd, "read_ram") == 0)     handle_read_ram(id, line);
    else if (strcmp(cmd, "pad_state") == 0)    handle_pad_state(id);
    else if (strcmp(cmd, "set_input") == 0)    handle_set_input(id, line);
    else if (strcmp(cmd, "irq_state") == 0)    handle_irq_state(id);
    else if (strcmp(cmd, "irq_force") == 0)    handle_irq_force(id, line);
    else if (strcmp(cmd, "timer_state") == 0)  handle_timer_state(id);
    else if (strcmp(cmd, "vip_state") == 0)    handle_vip_state(id);
    else if (strcmp(cmd, "screenshot") == 0)   handle_screenshot(id, line);
    else if (strcmp(cmd, "memory_map") == 0)   handle_memory_map(id);
    else if (strcmp(cmd, "wtrace_stats") == 0) handle_wtrace_stats(id);
    else if (strcmp(cmd, "wtrace_dump") == 0)  handle_wtrace_dump(id, line);
    else if (strcmp(cmd, "wtrace_reset") == 0) handle_wtrace_reset(id);
    else if (strcmp(cmd, "fntrace_stats") == 0) handle_fntrace_stats(id);
    else if (strcmp(cmd, "fntrace_dump") == 0)  handle_fntrace_dump(id, line);
    else if (strcmp(cmd, "fntrace_reset") == 0) handle_fntrace_reset(id);
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
        }
    }
    if (s_client != VB_BAD_SOCKET) {
        char line[8192];
        int n = recv(s_client, line, (int)sizeof(line) - 1, 0);
        if (n > 0) {
            line[n] = 0;
            /* Multiple lines may arrive in one read; dispatch each. */
            char* p = line;
            while (p && *p) {
                char* nl = strpbrk(p, "\r\n");
                if (nl) { *nl = 0; dispatch_line(p); p = nl + 1; }
                else    { dispatch_line(p); break; }
            }
        } else if (n == 0) {
            vb_close_socket(s_client); s_client = VB_BAD_SOCKET;
        } else {
            int e = vb_socket_errno();
            if (e != VB_EWOULDBLOCK
#if !defined(_WIN32)
                && e != EAGAIN
#endif
            ) {
                vb_close_socket(s_client); s_client = VB_BAD_SOCKET;
            }
        }
    }
    return s_quit;
}
