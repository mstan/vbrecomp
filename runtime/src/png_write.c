/* png_write.c — see png_write.h.
 *
 * A self-contained PNG encoder: PNG signature + IHDR + IDAT + IEND.
 * The IDAT payload is a zlib stream whose deflate body uses only
 * "stored" (BTYPE=00, uncompressed) blocks, so we need no external
 * compression library. CRC-32 (PNG chunk CRC) and Adler-32 (zlib
 * trailer) are implemented inline.
 */
#include "png_write.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Adler-32 over the uncompressed (filtered) image data. */
static uint32_t png_adler32(const uint8_t* buf, size_t n) {
    uint32_t a = 1, b = 0;
    for (size_t i = 0; i < n; ++i) {
        a = (a + buf[i]) % 65521u;
        b = (b + a)      % 65521u;
    }
    return (b << 16) | a;
}

static void put_be32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >>  8); p[3] = (uint8_t)(v      );
}

/* Running CRC-32 helper: feed bytes across calls, bracket with
 * 0xFFFFFFFF init and final XOR at the boundaries. */
static uint32_t crc32_update(uint32_t crc, const uint8_t* buf, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        crc ^= buf[i];
        for (int b = 0; b < 8; ++b)
            crc = (crc >> 1) ^ (0xEDB88320u & -(int32_t)(crc & 1));
    }
    return crc;
}

/* Write one PNG chunk: length, type, data, CRC32(type+data). The CRC
 * runs over the type and data as one logical stream. */
static int write_chunk(FILE* f, const char type[4],
                       const uint8_t* data, uint32_t len) {
    uint8_t lenbuf[4];
    put_be32(lenbuf, len);
    if (fwrite(lenbuf, 1, 4, f) != 4) return -1;
    if (fwrite(type, 1, 4, f) != 4) return -1;
    if (len && fwrite(data, 1, len, f) != len) return -1;

    uint32_t crc = 0xFFFFFFFFu;
    crc = crc32_update(crc, (const uint8_t*)type, 4);
    if (len) crc = crc32_update(crc, data, len);
    crc ^= 0xFFFFFFFFu;

    uint8_t crcbuf[4];
    put_be32(crcbuf, crc);
    if (fwrite(crcbuf, 1, 4, f) != 4) return -1;
    return 0;
}

int vb_write_png_32bpp(const char* path, int w, int h, const uint32_t* argb) {
    if (w <= 0 || h <= 0 || !argb || !path) return -1;

    /* 1. Build the raw (filtered) image stream: each scanline is
     *    prefixed with a filter byte (0 = None) followed by RGBA. */
    const size_t row_bytes = (size_t)w * 4u;
    const size_t raw_len   = (size_t)h * (1u + row_bytes);
    uint8_t* raw = (uint8_t*)malloc(raw_len);
    if (!raw) return -1;
    {
        size_t o = 0;
        for (int y = 0; y < h; ++y) {
            raw[o++] = 0; /* filter: None */
            for (int x = 0; x < w; ++x) {
                uint32_t p = argb[(size_t)y * (size_t)w + (size_t)x];
                raw[o++] = (uint8_t)(p >> 16); /* R */
                raw[o++] = (uint8_t)(p >>  8); /* G */
                raw[o++] = (uint8_t)(p      ); /* B */
                raw[o++] = (uint8_t)(p >> 24); /* A */
            }
        }
    }

    /* 2. Wrap raw in a zlib stream using stored deflate blocks. */
    const size_t MAXBLK = 65535u;
    size_t nblocks = (raw_len + MAXBLK - 1) / MAXBLK;
    if (nblocks == 0) nblocks = 1;
    /* zlib: 2 header bytes + per-block(5 + payload) + 4 adler bytes. */
    size_t idat_len = 2u + nblocks * 5u + raw_len + 4u;
    uint8_t* idat = (uint8_t*)malloc(idat_len);
    if (!idat) { free(raw); return -1; }
    {
        size_t o = 0;
        idat[o++] = 0x78; /* CMF: 32K window, deflate */
        idat[o++] = 0x01; /* FLG: no preset dict, check bits ok */
        size_t left = raw_len, src = 0;
        do {
            size_t blk = left < MAXBLK ? left : MAXBLK;
            int final = (left - blk) == 0;
            idat[o++] = (uint8_t)(final ? 1 : 0); /* BFINAL, BTYPE=00 */
            idat[o++] = (uint8_t)(blk      );
            idat[o++] = (uint8_t)(blk >> 8 );
            idat[o++] = (uint8_t)(~blk      );
            idat[o++] = (uint8_t)(~blk >> 8 );
            memcpy(idat + o, raw + src, blk);
            o += blk; src += blk; left -= blk;
        } while (left > 0);
        put_be32(idat + o, png_adler32(raw, raw_len));
        o += 4;
    }
    free(raw);

    /* 3. Emit the file. */
    FILE* f = fopen(path, "wb");
    if (!f) { free(idat); return -1; }
    static const uint8_t sig[8] = {137,80,78,71,13,10,26,10};
    int rc = 0;
    if (fwrite(sig, 1, 8, f) != 8) rc = -1;

    uint8_t ihdr[13];
    put_be32(ihdr + 0, (uint32_t)w);
    put_be32(ihdr + 4, (uint32_t)h);
    ihdr[8]  = 8;  /* bit depth */
    ihdr[9]  = 6;  /* colour type: RGBA */
    ihdr[10] = 0;  /* compression: deflate */
    ihdr[11] = 0;  /* filter method 0 */
    ihdr[12] = 0;  /* no interlace */
    if (!rc) rc = write_chunk(f, "IHDR", ihdr, sizeof(ihdr));
    if (!rc) rc = write_chunk(f, "IDAT", idat, (uint32_t)idat_len);
    if (!rc) rc = write_chunk(f, "IEND", NULL, 0);

    free(idat);
    if (fclose(f) != 0) rc = -1;
    return rc;
}
