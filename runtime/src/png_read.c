/* png_read.c — see png_read.h. Self-contained stored-block PNG decoder. */
#include "png_read.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t rd_be32(const uint8_t* p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

/* Paeth predictor (PNG filter type 4). */
static int paeth(int a, int b, int c) {
    int p = a + b - c;
    int pa = p > a ? p - a : a - p;
    int pb = p > b ? p - b : b - p;
    int pc = p > c ? p - c : c - p;
    if (pa <= pb && pa <= pc) return a;
    return (pb <= pc) ? b : c;
}

int vb_read_png_rgba(const char* path, uint32_t** out_argb,
                     int* out_w, int* out_h) {
    FILE* f = fopen(path, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    long flen = ftell(f);
    if (flen < 8) { fclose(f); return -1; }
    fseek(f, 0, SEEK_SET);
    uint8_t* file = (uint8_t*)malloc((size_t)flen);
    if (!file) { fclose(f); return -1; }
    if (fread(file, 1, (size_t)flen, f) != (size_t)flen) {
        free(file); fclose(f); return -1;
    }
    fclose(f);

    static const uint8_t sig[8] = {137,80,78,71,13,10,26,10};
    if (memcmp(file, sig, 8) != 0) { free(file); return -1; }

    int w = 0, h = 0, channels = 0;
    int have_ihdr = 0;
    /* Gather IDAT payload. */
    uint8_t* idat = NULL;
    size_t idat_len = 0;

    size_t pos = 8;
    int rc = -1;
    while (pos + 12 <= (size_t)flen) {
        uint32_t len = rd_be32(file + pos);
        const uint8_t* type = file + pos + 4;
        const uint8_t* data = file + pos + 8;
        if (pos + 12 + len > (size_t)flen) break;
        if (memcmp(type, "IHDR", 4) == 0 && len >= 13) {
            w = (int)rd_be32(data);
            h = (int)rd_be32(data + 4);
            int bitdepth = data[8];
            int colourtype = data[9];
            int interlace = data[12];
            if (bitdepth != 8 || interlace != 0) goto done;
            if (colourtype == 6) channels = 4;
            else if (colourtype == 2) channels = 3;
            else goto done;
            if (w <= 0 || h <= 0 || w > 4096 || h > 4096) goto done;
            have_ihdr = 1;
        } else if (memcmp(type, "IDAT", 4) == 0) {
            uint8_t* n = (uint8_t*)realloc(idat, idat_len + len);
            if (!n) goto done;
            idat = n;
            memcpy(idat + idat_len, data, len);
            idat_len += len;
        } else if (memcmp(type, "IEND", 4) == 0) {
            break;
        }
        pos += 12 + len;
    }
    if (!have_ihdr || !idat || idat_len < 2) goto done;

    /* zlib stream: 2-byte header, deflate body of STORED blocks, 4-byte
     * adler. Concatenate stored payloads into the filtered raw stream. */
    size_t raw_cap = (size_t)h * (1 + (size_t)w * (size_t)channels);
    uint8_t* raw = (uint8_t*)malloc(raw_cap ? raw_cap : 1);
    if (!raw) goto done;
    size_t raw_len = 0;
    {
        size_t o = 2;  /* skip CMF/FLG */
        int final = 0;
        while (!final && o < idat_len) {
            uint8_t hdr = idat[o++];
            final = hdr & 1;
            int btype = (hdr >> 1) & 3;
            if (btype != 0) { free(raw); goto done; }  /* only stored */
            if (o + 4 > idat_len) { free(raw); goto done; }
            uint16_t blen = (uint16_t)(idat[o] | (idat[o + 1] << 8));
            o += 4;  /* LEN + NLEN */
            if (o + blen > idat_len || raw_len + blen > raw_cap) {
                free(raw); goto done;
            }
            memcpy(raw + raw_len, idat + o, blen);
            raw_len += blen; o += blen;
        }
    }
    if (raw_len != raw_cap) { free(raw); goto done; }

    /* Reverse PNG scanline filters into ARGB8888. */
    uint32_t* argb = (uint32_t*)malloc((size_t)w * (size_t)h * sizeof(uint32_t));
    if (!argb) { free(raw); goto done; }
    {
        const int bpp = channels;
        const size_t stride = (size_t)w * bpp;
        uint8_t* line = (uint8_t*)malloc(stride);
        uint8_t* prev = (uint8_t*)calloc(stride, 1);
        if (!line || !prev) { free(line); free(prev); free(argb); free(raw); goto done; }
        size_t p = 0;
        for (int y = 0; y < h; ++y) {
            uint8_t ft = raw[p++];
            memcpy(line, raw + p, stride);
            p += stride;
            for (size_t i = 0; i < stride; ++i) {
                int a = (i >= (size_t)bpp) ? line[i - bpp] : 0;
                int b = prev[i];
                int c = (i >= (size_t)bpp) ? prev[i - bpp] : 0;
                int v = line[i];
                switch (ft) {
                    case 0: break;
                    case 1: v += a; break;
                    case 2: v += b; break;
                    case 3: v += (a + b) >> 1; break;
                    case 4: v += paeth(a, b, c); break;
                    default: free(line); free(prev); free(argb); free(raw); goto done;
                }
                line[i] = (uint8_t)v;
            }
            for (int x = 0; x < w; ++x) {
                uint8_t r = line[x * bpp + 0];
                uint8_t g = line[x * bpp + 1];
                uint8_t bb = line[x * bpp + 2];
                uint8_t al = (bpp == 4) ? line[x * bpp + 3] : 0xFF;
                argb[(size_t)y * w + x] =
                    ((uint32_t)al << 24) | ((uint32_t)r << 16) |
                    ((uint32_t)g << 8) | bb;
            }
            uint8_t* tmp = prev; prev = line; line = tmp;
        }
        free(line); free(prev);
    }
    free(raw);

    *out_argb = argb;
    *out_w = w;
    *out_h = h;
    rc = 0;

done:
    free(idat);
    free(file);
    return rc;
}
