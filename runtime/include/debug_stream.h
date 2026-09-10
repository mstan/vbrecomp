/* Shared nonblocking line framing. Each server owns one instance. */
#ifndef VB_DEBUG_STREAM_H
#define VB_DEBUG_STREAM_H
#include <stdlib.h>
#include <string.h>

static struct {
    char input[8192];
    size_t used;
    char* output;
    size_t size, sent;
    int failed;
} vb_stream;

static void vb_stream_reset(void) {
    free(vb_stream.output);
    memset(&vb_stream, 0, sizeof(vb_stream));
}
static void vb_stream_queue(const char* body) {
    size_t n = strlen(body);
    if (vb_stream.failed) return;
    if (n >= 64u*1024u*1024u || vb_stream.size > 64u*1024u*1024u - n - 1u) {
        vb_stream.failed = 1; return;
    }
    char* next = (char*)realloc(vb_stream.output, vb_stream.size + n + 1);
    if (!next) { vb_stream.failed = 1; return; }
    vb_stream.output = next;
    memcpy(next + vb_stream.size, body, n);
    vb_stream.size += n;
    next[vb_stream.size++] = '\n';
}
static int vb_stream_would_block(void) {
#ifdef _WIN32
    return WSAGetLastError() == WSAEWOULDBLOCK;
#else
    return errno == EWOULDBLOCK || errno == EAGAIN || errno == EINTR;
#endif
}
static int vb_stream_flush(sock_t client) {
    if (vb_stream.failed) return 0;
    while (vb_stream.sent < vb_stream.size) {
        size_t remaining = vb_stream.size - vb_stream.sent;
        int n = send(client, vb_stream.output + vb_stream.sent,
                     (int)(remaining > 65536 ? 65536 : remaining),
#ifdef MSG_NOSIGNAL
                     MSG_NOSIGNAL
#else
                     0
#endif
        );
        if (n <= 0) return n < 0 && vb_stream_would_block();
        vb_stream.sent += (size_t)n;
    }
    free(vb_stream.output);
    vb_stream.output = NULL;
    vb_stream.size = vb_stream.sent = 0;
    return 1;
}
static int vb_stream_poll(sock_t client, void (*dispatch)(char*)) {
    if (!vb_stream_flush(client)) return 0;
    /* Apply backpressure until the previous responses have drained. */
    if (vb_stream.size) return 1;
    char chunk[4096];
    int n = recv(client, chunk, sizeof(chunk), 0);
    if (n <= 0) return n < 0 && vb_stream_would_block();
    for (int i = 0; i < n; ++i) {
        if (chunk[i] == '\n') {
            if (vb_stream.used && vb_stream.input[vb_stream.used - 1] == '\r')
                --vb_stream.used;
            vb_stream.input[vb_stream.used] = 0;
            if (vb_stream.used) dispatch(vb_stream.input);
            vb_stream.used = 0;
        } else {
            if (!chunk[i] || vb_stream.used == sizeof(vb_stream.input)-1) return 0;
            vb_stream.input[vb_stream.used++] = chunk[i];
        }
    }
    return vb_stream_flush(client);
}
#endif
