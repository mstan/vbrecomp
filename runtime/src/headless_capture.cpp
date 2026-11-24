/* See headless_capture.h. */
#include "headless_capture.h"

#include "input.h"
#include "png_write.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <direct.h>
#define VB_CAP_MKDIR(p) _mkdir(p)
#define VB_CAP_SETENV(k, v) _putenv_s((k), (v))
#else
#include <sys/stat.h>
#define VB_CAP_MKDIR(p) mkdir((p), 0755)
#define VB_CAP_SETENV(k, v) setenv((k), (v), 1)
#endif

namespace {
constexpr double kFrameHz = 50.2735;      /* 20 MHz / (259*384*4) */
constexpr int kAudioHz = 44100;           /* VSU_OUTPUT_HZ */

struct Step { long frame; unsigned seat; uint16_t pad; };

struct State {
    bool enabled = false;
    std::string out_dir, wav_path;
    int width = 1920, height = 1080;
    long from = -1, to = -1;
    int every = 1;
    FILE* wav = nullptr;
    uint64_t wav_frames = 0, png_count = 0;
    std::vector<uint32_t> canvas;
    std::vector<Step> steps;
    size_t next_step = 0;
    uint16_t held[4] = {0, 0, 0, 0};
} g;

uint16_t parse_buttons(const char* s) {
    static const struct { const char* name; uint16_t bit; } kNames[] = {
        {"A", VB_PAD_A}, {"B", VB_PAD_B}, {"L", VB_PAD_LT}, {"R", VB_PAD_RT},
        {"START", VB_PAD_START}, {"SELECT", VB_PAD_SELECT},
        {"UP", VB_PAD_LUP}, {"DOWN", VB_PAD_LDOWN}, {"LEFT", VB_PAD_LLEFT}, {"RIGHT", VB_PAD_LRIGHT},
        {"LUP", VB_PAD_LUP}, {"LDOWN", VB_PAD_LDOWN}, {"LLEFT", VB_PAD_LLEFT}, {"LRIGHT", VB_PAD_LRIGHT},
        {"RUP", VB_PAD_RUP}, {"RDOWN", VB_PAD_RDOWN}, {"RLEFT", VB_PAD_RLEFT}, {"RRIGHT", VB_PAD_RRIGHT},
    };
    uint16_t m = 0;
    while (*s) {
        while (*s == '+' || *s == ' ' || *s == '\t') ++s;
        size_t n = 0;
        while (s[n] && s[n] != '+' && s[n] != ' ' && s[n] != '\t' && s[n] != '\r' && s[n] != '\n') ++n;
        if (!n) break;
        std::string word(s, n);
        for (auto& c : word) c = (char)std::toupper((unsigned char)c);
        for (const auto& k : kNames) if (word == k.name) { m |= k.bit; break; }
        s += n;
    }
    return m;
}

void load_input(const char* path) {
    FILE* f = std::fopen(path, "rb");
    if (!f) { std::fprintf(stderr, "capture: cannot open input script '%s'\n", path); return; }
    char line[512];
    while (std::fgets(line, sizeof(line), f)) {
        if (char* c = std::strchr(line, '#')) *c = 0;
        char* p = line;
        while (*p == ' ' || *p == '\t') ++p;
        if (!*p || *p == '\n' || *p == '\r') continue;
        char* end;
        long frame = std::strtol(p, &end, 10);
        if (end == p) continue;
        p = end;
        while (*p == ' ' || *p == '\t') ++p;
        unsigned seat = 0;
        if (*p >= '1' && *p <= '4' && p[1] == ':') { seat = (unsigned)(*p - '1'); p += 2; }
        while (*p == ' ' || *p == '\t') ++p;
        g.steps.push_back({frame, seat, *p == '-' ? (uint16_t)0 : parse_buttons(p)});
    }
    std::fclose(f);
    std::stable_sort(g.steps.begin(), g.steps.end(),
                     [](const Step& a, const Step& b) { return a.frame < b.frame; });
}

void wav_header(FILE* f, uint32_t data_bytes) {
    uint8_t h[44];
    std::memcpy(h, "RIFF", 4);
    uint32_t v = 36 + data_bytes;
    std::memcpy(h + 4, &v, 4);
    std::memcpy(h + 8, "WAVEfmt ", 8);
    uint32_t fmt = 16, rate = kAudioHz, bps = kAudioHz * 4;
    uint16_t pcm = 1, ch = 2, bits = 16, align = 4;
    std::memcpy(h + 16, &fmt, 4); std::memcpy(h + 20, &pcm, 2); std::memcpy(h + 22, &ch, 2);
    std::memcpy(h + 24, &rate, 4); std::memcpy(h + 28, &bps, 4);
    std::memcpy(h + 32, &align, 2); std::memcpy(h + 34, &bits, 2);
    std::memcpy(h + 36, "data", 4); std::memcpy(h + 40, &data_bytes, 4);
    std::fseek(f, 0, SEEK_SET);
    std::fwrite(h, 1, 44, f);
}
}  // namespace

extern "C" {

int vb_capture_start(const VbCaptureConfig* c) {
    if (!c) return 0;
    if (c->width > 0) g.width = c->width;
    if (c->height > 0) g.height = c->height;
    g.from = c->from; g.to = c->to; g.every = c->every > 0 ? c->every : 1;
    if (c->input_script) load_input(c->input_script);
    if (!c->out_dir && !c->wav_path) return 0;
    g.enabled = true;
    if (c->out_dir) {
        g.out_dir = c->out_dir;
        VB_CAP_MKDIR(g.out_dir.c_str());
        g.canvas.assign((size_t)g.width * (size_t)g.height, 0xFF000000u);
    }
    if (c->wav_path) {
        g.wav_path = c->wav_path;
        g.wav = std::fopen(c->wav_path, "wb");
        if (g.wav) wav_header(g.wav, 0);
        else std::fprintf(stderr, "capture: cannot open %s\n", c->wav_path);
    }
    VB_CAP_SETENV("SDL_VIDEODRIVER", "dummy");
    VB_CAP_SETENV("SDL_AUDIODRIVER", "dummy");
    return 1;
}

int vb_capture_enabled(void) { return g.enabled ? 1 : 0; }
int vb_capture_has_input(void) { return g.steps.empty() ? 0 : 1; }
void vb_capture_size(int* w, int* h) { if (w) *w = g.width; if (h) *h = g.height; }

int vb_capture_frame_wanted(uint32_t frame) {
    if (g.from >= 0 && (long)frame < g.from) return 0;
    if (g.to >= 0 && (long)frame > g.to) return 0;
    long base = g.from >= 0 ? g.from : 1;
    return (((long)frame - base) % g.every) == 0;
}

void vb_capture_present(const uint32_t* argb, int w, int h,
                        int dx, int dy, int dw, int dh, uint32_t frame) {
    if (g.out_dir.empty() || !argb || w <= 0 || h <= 0 || !vb_capture_frame_wanted(frame)) return;
    const int W = g.width, H = g.height;
    std::fill(g.canvas.begin(), g.canvas.end(), 0xFF000000u);
    if (dw <= 0 || dh <= 0) { dx = 0; dy = 0; dw = W; dh = H; }
    for (int y = 0; y < dh; ++y) {
        const int oy = dy + y;
        if (oy < 0 || oy >= H) continue;
        const uint32_t* srow = argb + (size_t)((int64_t)y * h / dh) * (size_t)w;
        uint32_t* drow = &g.canvas[(size_t)oy * (size_t)W];
        for (int x = 0; x < dw; ++x) {
            const int ox = dx + x;
            if (ox < 0 || ox >= W) continue;
            drow[ox] = srow[(int64_t)x * w / dw] | 0xFF000000u;
        }
    }
    char path[1200];
    std::snprintf(path, sizeof(path), "%s/frame_%06u.png", g.out_dir.c_str(), frame);
    if (vb_write_png_32bpp(path, W, H, g.canvas.data()) == 0) ++g.png_count;
}

int vb_capture_audio_frames(uint32_t frame) {
    const int64_t b = (int64_t)(kAudioHz * (double)frame / kFrameHz + 0.5);
    const int64_t a = (int64_t)(kAudioHz * (double)(frame - 1) / kFrameHz + 0.5);
    return (int)(b - a);
}

void vb_capture_audio(const int16_t* stereo, int frames, uint32_t frame) {
    if (!g.wav || !vb_capture_frame_wanted(frame)) return;
    std::fwrite(stereo, 4, (size_t)frames, g.wav);
    g.wav_frames += (uint64_t)frames;
}

uint16_t vb_capture_pad(unsigned seat, uint32_t frame) {
    while (g.next_step < g.steps.size() && g.steps[g.next_step].frame <= (long)frame) {
        const Step& s = g.steps[g.next_step++];
        if (s.seat < 4) g.held[s.seat] = s.pad;
    }
    return seat < 4 ? g.held[seat] : (uint16_t)0;
}

void vb_capture_finish(void) {
    if (g.wav) {
        wav_header(g.wav, (uint32_t)(g.wav_frames * 4));
        std::fclose(g.wav);
        g.wav = nullptr;
    }
    if (g.enabled)
        std::fprintf(stderr, "capture: %llu frames written, %llu audio samples, present size %dx%d\n",
                     (unsigned long long)g.png_count, (unsigned long long)g.wav_frames, g.width, g.height);
}

}  // extern "C"
