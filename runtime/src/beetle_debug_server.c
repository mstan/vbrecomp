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


static sock_t s_listener = VBB_BAD_SOCKET;
static sock_t s_client   = VBB_BAD_SOCKET;
static int    s_paused      = 0;
static int    s_quit        = 0;
static int    s_winsock_inited = 0;


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

static void send_response(const char* body) {
    if (s_client == VBB_BAD_SOCKET) return;
    size_t n = strlen(body);
    send(s_client, body, (int)n, 0);
    send(s_client, "\n", 1, 0);
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
        id, vb_beetle_get_pad());
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
    if (len < 1) len = 1;
    if (len > 4096) len = 4096;
    uint8_t* tmp = (uint8_t*)malloc((size_t)len);
    if (!tmp) {
        send_response("{\"ok\":false,\"error\":\"oom\"}");
        return;
    }
    size_t filled = vb_beetle_read_memory((uint32_t)addr, tmp, (size_t)len);
    char* body = (char*)malloc(96 + (size_t)len * 2);
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

static void handle_get_registers(long long id) {
    /* mednafen-vb keeps the V810 register file inside a static C++
     * object — there is no extern accessor. Surface this as a
     * structured error so the user/tool knows the gap is real, not a
     * malformed response. A future upstream patch (tracked the
     * recompiler-patches way: project-side .patch, applied at cmake
     * configure time) can fill this in when a phase needs it. */
    char buf[256];
    snprintf(buf, sizeof(buf),
        "{\"ok\":false,\"id\":%lld,\"cmd\":\"get_registers\","
        "\"error\":\"V810 register file not exposed by mednafen-vb; "
        "requires upstream patch\"}",
        id);
    send_response(buf);
}

static void handle_screenshot(long long id, const char* line) {
    char path[256] = {0};
    extract_str(line, "\"path\"", path, sizeof(path));
    if (!path[0]) snprintf(path, sizeof(path), "vb-beetle-fb.png");

    const uint32_t* pixels = NULL;
    unsigned w = 0, h = 0;
    int have = vb_beetle_get_framebuffer(&pixels, &w, &h);
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

    if      (strcmp(cmd, "ping") == 0)         handle_ping(id);
    else if (strcmp(cmd, "frame") == 0)        handle_frame(id);
    else if (strcmp(cmd, "pad_state") == 0)    handle_pad_state(id);
    else if (strcmp(cmd, "read_ram") == 0)     handle_read_ram(id, line);
    else if (strcmp(cmd, "memory_map") == 0)   handle_memory_map(id);
    else if (strcmp(cmd, "get_registers") == 0)handle_get_registers(id);
    else if (strcmp(cmd, "vip_state") == 0)    handle_vip_state(id);
    else if (strcmp(cmd, "screenshot") == 0)   handle_screenshot(id, line);
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
        }
    }
    if (s_client != VBB_BAD_SOCKET) {
        char line[8192];
        int n = recv(s_client, line, (int)sizeof(line) - 1, 0);
        if (n > 0) {
            line[n] = 0;
            char* p = line;
            while (p && *p) {
                char* nl = strpbrk(p, "\r\n");
                if (nl) { *nl = 0; dispatch_line(p); p = nl + 1; }
                else    { dispatch_line(p); break; }
            }
        } else if (n == 0) {
            vbb_close_socket(s_client); s_client = VBB_BAD_SOCKET;
        } else {
            int e = vbb_socket_errno();
            if (e != VBB_EWOULDBLOCK
#if !defined(_WIN32)
                && e != EAGAIN
#endif
            ) {
                vbb_close_socket(s_client); s_client = VBB_BAD_SOCKET;
            }
        }
    }
    if (s_quit) {
        /* clean shutdown driven from main loop */
        vb_beetle_debug_server_stop();
    }
}
