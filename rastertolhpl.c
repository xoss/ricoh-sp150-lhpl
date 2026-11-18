/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * rastertolhpl - native CUPS raster filter for Ricoh SP 150 series printers
 * (SP 150 / 150w / 150SU / 150SUw), which speak "LHPL" behind a PJL header.
 *
 * Stream layout (derived from comparing captured print jobs):
 *   PJL header ... "@PJL ENTER LANGUAGE=LHPL\r\n"
 *   'sj' record (64 bytes)
 *   per page: 'sp' record (64 bytes) + JBIG BIE (T.85: 1 layer, L0=128,
 *             MX=0, LRLTWO, SDRST) + 'ep' record (64 bytes)
 *   "\033%-12345X@PJL EOJ\r\n"
 * Each record = "\033LH@" + 2-char command + payload, last byte = XOR of the
 * first 63 bytes.
 *
 * JBIG encoding uses JBIG-KIT (jbig.c / jbig_ar.c, GPL) by Markus Kuhn.
 *
 * Usage (CUPS filter): rastertolhpl job user title copies options [file]
 */
#include <cups/cups.h>
#include <cups/raster.h>
#include <fcntl.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "jbig.h"

typedef struct { unsigned char *buf; size_t len, cap; } membuf_t;

static void mb_put(unsigned char *s, size_t n, void *p)
{
    membuf_t *m = p;
    if (m->len + n > m->cap) {
        m->cap = (m->len + n) * 2;
        m->buf = realloc(m->buf, m->cap);
        if (!m->buf) { fputs("ERROR: out of memory\n", stderr); exit(1); }
    }
    memcpy(m->buf + m->len, s, n);
    m->len += n;
}

static void le16(unsigned char *p, unsigned v) { p[0] = v & 255; p[1] = (v >> 8) & 255; }
static void le32(unsigned char *p, uint32_t v)
{
    p[0] = v & 255; p[1] = (v >> 8) & 255; p[2] = (v >> 16) & 255; p[3] = (v >> 24) & 255;
}

static void put_record(const char *cmd, unsigned char rec[64])
{
    unsigned char x = 0;
    rec[0] = 0x1b; rec[1] = 'L'; rec[2] = 'H'; rec[3] = '@';
    rec[4] = (unsigned char)cmd[0]; rec[5] = (unsigned char)cmd[1];
    for (int i = 0; i < 63; i++) x ^= rec[i];
    rec[63] = x;
    fwrite(rec, 1, 64, stdout);
}

/* PJL-safe copy: printable ASCII only, no quotes, bounded length */
static void pjl_clean(char *dst, size_t dstlen, const char *src)
{
    size_t j = 0;
    for (; src && *src && j + 1 < dstlen; src++) {
        unsigned char c = (unsigned char)*src;
        dst[j++] = (c >= 0x20 && c < 0x7f && c != '"') ? (char)c : '_';
    }
    dst[j] = 0;
}

static const struct { int code; int w, h; } paper_table[] = {  /* points */
    {1, 612, 792},   /* Letter */
    {2, 595, 842},   /* A4 */
    {3, 420, 595},   /* A5 */
    {4, 297, 420},   /* A6 */
    {5, 516, 729},   /* B5 (JIS) */
    {6, 363, 516},   /* B6 (JIS) */
    {7, 522, 756},   /* Executive */
    {8, 524, 737},   /* 16K */
    {9, 612, 1008},  /* Legal */
    {10, 595, 420},  /* A5 landscape feed */
    {11, 516, 363},  /* B6 landscape feed */
};

static int paper_code(unsigned w, unsigned h)
{
    for (size_t i = 0; i < sizeof paper_table / sizeof paper_table[0]; i++)
        if (abs((int)w - paper_table[i].w) <= 3 && abs((int)h - paper_table[i].h) <= 3)
            return paper_table[i].code;
    return 0;
}

int main(int argc, char **argv)
{
    int fd = 0;
    if (argc < 6 || argc > 7) {
        fputs("Usage: rastertolhpl job user title copies options [file]\n", stderr);
        return 1;
    }
    if (argc == 7 && (fd = open(argv[6], O_RDONLY)) < 0) {
        perror("ERROR: unable to open raster file");
        return 1;
    }
    cups_raster_t *ras = cupsRasterOpen(fd, CUPS_RASTER_READ);
    if (!ras) { fputs("ERROR: unable to read raster stream\n", stderr); return 1; }

    /* ---- PJL job header (same keys/order as the vendor driver) ---- */
    char host[64] = "mac", user[64], title[96], hn[256];
    if (gethostname(hn, sizeof hn) == 0) { hn[sizeof hn - 1] = 0; pjl_clean(host, sizeof host, hn); }
    pjl_clean(user, sizeof user, argv[2]);
    pjl_clean(title, sizeof title, argv[3]);
    time_t now = time(NULL);
    struct tm *tm = localtime(&now);
    printf("\033%%-12345X@PJL JOB NAME=PRINTER\r\n");
    printf("@PJL SET JOBATTR=HST:%s\r\n", host);
    printf("@PJL SET JOBATTR=USR:%s\r\n", user);
    printf("@PJL SET JOBATTR=DOC:%s\r\n", title);
    printf("@PJL SET JOBATTR=DATE:%02d/%02d/%04d\r\n", tm->tm_mon + 1, tm->tm_mday, tm->tm_year + 1900);
    printf("@PJL SET JOBATTR=TIME:%02d:%02d:%02d\r\n", tm->tm_hour, tm->tm_min, tm->tm_sec);
    printf("@PJL SET DUPLEX=OFF\r\n");
    printf("@PJL SET MEDIASOURCE=0\r\n");
    printf("@PJL SET RENDERMODE=GRAYSCALE\r\n");
    printf("@PJL SET RESOLUTION=600\r\n");
    printf("@PJL SET BITSPERPIXEL=1\r\n");
    printf("@PJL SET COPIES=1\r\n");            /* copies are generated upstream */
    printf("@PJL ENTER LANGUAGE=LHPL\r\n");

    unsigned char rec[64];
    memset(rec, 0, sizeof rec);
    rec[6] = 1;
    rec[8] = 1;
    put_record("sj", rec);

    cups_page_header2_t hdr;
    int page = 0;
    unsigned char *line = NULL;
    while (cupsRasterReadHeader2(ras, &hdr)) {
        page++;
        if (hdr.HWResolution[0] != 600 || hdr.HWResolution[1] != 600) {
            fprintf(stderr, "ERROR: unsupported resolution %ux%u (need 600x600)\n",
                    hdr.HWResolution[0], hdr.HWResolution[1]);
            return 1;
        }
        unsigned bpp = hdr.cupsBitsPerPixel;
        if (!(bpp == 1 || bpp == 8 || bpp == 24)) {
            fprintf(stderr, "ERROR: unsupported raster depth %u\n", bpp);
            return 1;
        }
        int white_is_high = (hdr.cupsColorSpace == CUPS_CSPACE_W ||
                             hdr.cupsColorSpace == CUPS_CSPACE_SW ||
                             hdr.cupsColorSpace == CUPS_CSPACE_RGB ||
                             hdr.cupsColorSpace == CUPS_CSPACE_SRGB ||
                             hdr.cupsColorSpace == CUPS_CSPACE_ADOBERGB);

        unsigned pw = hdr.PageSize[0], ph = hdr.PageSize[1];
        unsigned full_w = (unsigned)floor(pw * 600.0 / 72.0);
        unsigned full_h = (unsigned)floor(ph * 600.0 / 72.0);
        /* full-page raster -> crop the printer's 99 px margins; an
           imageable-area raster (as produced on Linux) is used as is */
        /* Bitmap geometry always matches the vendor driver: printable area =
           page minus 99 px on every side, width padded to a multiple of 512. */
        unsigned x0 = 0, y0 = 0, cw = full_w - 198;
        if (hdr.cupsWidth + 20 >= full_w)
            x0 = y0 = 99;
        unsigned W = ((cw + 511) / 512) * 512;
        unsigned H = full_h - 198;
        unsigned rb = W / 8;
        int code = paper_code(pw, ph);
        if (!code) {
            fprintf(stderr, "ERROR: unsupported paper size %ux%u pt\n", pw, ph);
            return 1;
        }
        unsigned media = (hdr.cupsMediaType >= 1 && hdr.cupsMediaType <= 4) ? hdr.cupsMediaType : 1;
        fprintf(stderr, "INFO: page %d: %ux%u pt, paper code %d, media %u, raster %ux%u@%ubpp cs=%u -> %ux%u\n",
                page, pw, ph, code, media, hdr.cupsWidth, hdr.cupsHeight, bpp, hdr.cupsColorSpace, W, H);

        unsigned char *bm = calloc((size_t)rb * H, 1);
        line = realloc(line, hdr.cupsBytesPerLine);
        /* Floyd-Steinberg error buffers (serpentine), in 1/16 units */
        int *err_cur = calloc(cw + 2, sizeof(int)), *err_nxt = calloc(cw + 2, sizeof(int));
        if (!bm || !line || !err_cur || !err_nxt) { fputs("ERROR: out of memory\n", stderr); return 1; }

        for (unsigned y = 0; y < hdr.cupsHeight; y++) {
            if (cupsRasterReadPixels(ras, line, hdr.cupsBytesPerLine) != hdr.cupsBytesPerLine) break;
            if (y < y0 || y - y0 >= H) continue;
            unsigned char *out = bm + (size_t)(y - y0) * rb;
            unsigned avail = hdr.cupsWidth > x0 ? hdr.cupsWidth - x0 : 0;
            unsigned n = avail < cw ? avail : cw;
            if (bpp == 1) {
                for (unsigned x = 0; x < n; x++) {
                    unsigned sx = x + x0;
                    int b = (line[sx >> 3] >> (7 - (sx & 7))) & 1;
                    if (white_is_high) b = !b;
                    if (b) out[x >> 3] |= 0x80 >> (x & 7);
                }
                continue;
            }
            int rtl = (y - y0) & 1;
            memset(err_nxt, 0, (cw + 2) * sizeof(int));
            for (unsigned i = 0; i < n; i++) {
                unsigned x = rtl ? n - 1 - i : i;
                unsigned sx = x + x0;
                int g;   /* darkness 0..255 */
                if (bpp == 8) g = line[sx];
                else g = (line[sx * 3] * 30 + line[sx * 3 + 1] * 59 + line[sx * 3 + 2] * 11) / 100;
                if (white_is_high) g = 255 - g;
                int v = g + err_cur[x + 1] / 16;
                int black = v >= 128;
                int e = v - (black ? 255 : 0);
                if (black) out[x >> 3] |= 0x80 >> (x & 7);
                if (!rtl) {
                    err_cur[x + 2] += e * 7; err_nxt[x] += e * 3; err_nxt[x + 1] += e * 5; err_nxt[x + 2] += e;
                } else {
                    err_cur[x] += e * 7; err_nxt[x + 2] += e * 3; err_nxt[x + 1] += e * 5; err_nxt[x] += e;
                }
            }
            int *t = err_cur; err_cur = err_nxt; err_nxt = t;
        }
        free(err_cur); free(err_nxt);

        membuf_t jb = {0};
        struct jbg_enc_state se;
        unsigned char *planes[1] = { bm };
        jbg_enc_init(&se, W, H, 1, planes, mb_put, &jb);
        jbg_enc_layers(&se, 0);
        jbg_enc_options(&se, 0, JBG_LRLTWO | JBG_SDRST, 128, 0, 0);
        jbg_enc_out(&se);
        jbg_enc_free(&se);
        free(bm);

        memset(rec, 0, sizeof rec);
        rec[6] = (unsigned char)code;
        rec[7] = (unsigned char)media;
        le32(rec + 8, W);
        le32(rec + 12, H);
        le32(rec + 16, rb * H);
        le32(rec + 20, (uint32_t)jb.len);
        le32(rec + 24, (uint32_t)jb.len);
        le16(rec + 42, 600);
        le16(rec + 44, (unsigned)floor(pw * 254.0 / 72.0));
        le16(rec + 46, (unsigned)floor(ph * 254.0 / 72.0));
        put_record("sp", rec);
        fwrite(jb.buf, 1, jb.len, stdout);
        free(jb.buf);
        memset(rec, 0, sizeof rec);
        put_record("ep", rec);
        fprintf(stderr, "PAGE: %d 1\n", page);
        fflush(stdout);
    }
    free(line);
    printf("\033%%-12345X@PJL EOJ\r\n");
    fflush(stdout);
    cupsRasterClose(ras);
    if (fd) close(fd);
    return page ? 0 : 1;
}
