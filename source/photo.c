// Photos: saving snapshots as BMP, and loading any BMP / JPG / PNG / GIF to
// the top screen, scaled to fit.
//
// JPEGs use TJpgDec (streams the file and can decode at 1/2, 1/4 or 1/8 size,
// so big phone photos work). PNG, GIF, odd BMPs and progressive JPEGs use
// stb_image, which needs the whole picture in RAM, so those are size-limited.

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include <nds.h>

#include "photo.h"
#include "lib/tjpgd.h"

// stb_image lives in img_stb.c
unsigned char *imgStbLoad(const char *path, int *w, int *h);
int imgStbInfo(const char *path, int *w, int *h);
void imgStbFree(void *p);

#define OUT_W 256
#define OUT_H 192
#define STB_MAX_PIXELS  (2400 * 1000)   // ~2.4 MP => ~7 MB RGB while decoding

static u8 rowBuf[OUT_W * 4];

bool photoIsImage(const char *name)
{
    const char *dot = strrchr(name, '.');
    if (dot == NULL)
        return false;
    return strcasecmp(dot, ".bmp") == 0 || strcasecmp(dot, ".jpg") == 0 ||
           strcasecmp(dot, ".jpeg") == 0 || strcasecmp(dot, ".png") == 0 ||
           strcasecmp(dot, ".gif") == 0;
}

static u32 rd32(const u8 *p)
{
    return p[0] | (p[1] << 8) | (p[2] << 16) | ((u32)p[3] << 24);
}

static void put32(u8 *p, u32 v)
{
    p[0] = v;
    p[1] = v >> 8;
    p[2] = v >> 16;
    p[3] = v >> 24;
}

// ---------------------------------------------------------------------------
// Saving

#define ALBUM_FILE "/DSiRecorder/album.txt"

static char saveDir[64] = PHOTO_DIR;

void photoInit(void)
{
    FILE *f = fopen(ALBUM_FILE, "r");
    if (f)
    {
        char buf[64];
        if (fgets(buf, sizeof(buf), f))
        {
            buf[strcspn(buf, "\r\n")] = 0;
            struct stat st;
            if (strncmp(buf, ALBUM_ROOT "/", sizeof(ALBUM_ROOT)) == 0 && stat(buf, &st) == 0 &&
                S_ISDIR(st.st_mode))
                strcpy(saveDir, buf);
        }
        fclose(f);
    }
}

const char *photoSaveDir(void)
{
    return saveDir;
}

void photoSetSaveDir(const char *dir)
{
    snprintf(saveDir, sizeof(saveDir), "%s", dir);
    mkdir("/DSiRecorder", 0777);
    FILE *f = fopen(ALBUM_FILE, "w");
    if (f)
    {
        fprintf(f, "%s\n", saveDir);
        fclose(f);
    }
}

const char *photoAlbumLabel(const char *dir)
{
    if (strcasecmp(dir, PHOTO_DIR) == 0)
        return "Camera roll";
    if (strncmp(dir, NAND_ALBUM, sizeof(NAND_ALBUM) - 1) == 0)
        return "DSi Camera";
    const char *s = strrchr(dir, '/');
    return s ? s + 1 : dir;
}

static int nextPhotoNumber(void)
{
    int highest = 0;
    DIR *d = opendir(saveDir);
    if (d == NULL)
        return 1;
    struct dirent *e;
    while ((e = readdir(d)) != NULL)
    {
        if (strncasecmp(e->d_name, "IMG_", 4) == 0)
        {
            int n = atoi(e->d_name + 4);
            if (n > highest)
                highest = n;
        }
    }
    closedir(d);
    return highest + 1;
}

bool photoSave(const u16 *vram, int zoomQ8, char *nameOut)
{
    mkdir(ALBUM_ROOT, 0777);
    mkdir(saveDir, 0777);

    char path[96];
    snprintf(nameOut, 16, "IMG_%04d.BMP", nextPhotoNumber());
    snprintf(path, sizeof(path), "%s/%s", saveDir, nameOut);

    if (zoomQ8 < 256)
        zoomQ8 = 256;
    int cw = (OUT_W * 256) / zoomQ8, ch = (OUT_H * 256) / zoomQ8;
    int x0 = (OUT_W - cw) / 2, y0 = (OUT_H - ch) / 2;

    const u32 rowBytes = OUT_W * 3;       // 768: already a multiple of 4
    const u32 dataBytes = rowBytes * OUT_H;
    u8 hdr[54];
    memset(hdr, 0, sizeof(hdr));
    hdr[0] = 'B';
    hdr[1] = 'M';
    put32(hdr + 2, 54 + dataBytes);
    put32(hdr + 10, 54);
    put32(hdr + 14, 40);
    put32(hdr + 18, OUT_W);
    put32(hdr + 22, OUT_H);               // positive height = bottom-up rows
    hdr[26] = 1;
    hdr[28] = 24;
    put32(hdr + 34, dataBytes);
    put32(hdr + 38, 2835);
    put32(hdr + 42, 2835);

    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0)
        return false;
    bool ok = write(fd, hdr, 54) == 54;

    for (int y = OUT_H - 1; y >= 0 && ok; y--)
    {
        const u16 *s = vram + (y0 + (y * ch) / OUT_H) * 256;
        u8 *d = rowBuf;
        for (int x = 0; x < OUT_W; x++)
        {
            u16 v = s[x0 + (x * cw) / OUT_W];
            int r = v & 31, g = (v >> 5) & 31, b = (v >> 10) & 31;
            *d++ = (b << 3) | (b >> 2);
            *d++ = (g << 3) | (g >> 2);
            *d++ = (r << 3) | (r >> 2);
        }
        ok = write(fd, rowBuf, rowBytes) == (ssize_t)rowBytes;
    }
    close(fd);
    if (!ok)
        unlink(path);
    return ok;
}

// ---------------------------------------------------------------------------
// Fitting helpers

typedef struct
{
    int outW, outH, ox, oy;
} Fit;

static Fit fitBox(int w, int h)
{
    Fit f;
    f.outW = OUT_W;
    f.outH = (int)(((s64)h * OUT_W) / w);
    if (f.outH > OUT_H)
    {
        f.outH = OUT_H;
        f.outW = (int)(((s64)w * OUT_H) / h);
    }
    if (f.outW < 1)
        f.outW = 1;
    if (f.outH < 1)
        f.outH = 1;
    f.ox = (OUT_W - f.outW) / 2;
    f.oy = (OUT_H - f.outH) / 2;
    return f;
}

static void clearScreen(u16 *vram)
{
    memset(vram, 0, 256 * 192 * 2);
}

// Blit an RGB888 buffer (stb_image output)
static void blitRgb(const u8 *px, int w, int h, u16 *vram)
{
    Fit f = fitBox(w, h);
    for (int y = 0; y < f.outH; y++)
    {
        const u8 *row = px + (size_t)((y * h) / f.outH) * w * 3;
        u16 *d = vram + (f.oy + y) * 256 + f.ox;
        for (int x = 0; x < f.outW; x++)
        {
            const u8 *p = row + ((x * w) / f.outW) * 3;
            d[x] = RGB15(p[0] >> 3, p[1] >> 3, p[2] >> 3) | BIT(15);
        }
    }
}

// ---------------------------------------------------------------------------
// BMP (24/32-bit, streamed row by row so any size works)

static int loadBmp(const char *path, u16 *vram)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return -1;
    u8 h[54];
    bool ok = read(fd, h, 54) == 54 && h[0] == 'B' && h[1] == 'M';
    u32 off = ok ? rd32(h + 10) : 0;
    int w = ok ? (int)rd32(h + 18) : 0;
    int ht = ok ? (int)rd32(h + 22) : 0;
    int bpp = ok ? (h[28] | (h[29] << 8)) : 0;
    u32 comp = ok ? rd32(h + 30) : 1;
    bool topDown = ht < 0;
    if (topDown)
        ht = -ht;
    if (!ok || w <= 0 || ht <= 0 || w > 16384 || ht > 16384 || (bpp != 24 && bpp != 32) ||
        (comp != 0 && comp != 3))
    {
        close(fd);
        return 0;                   // not one we stream: let stb_image try
    }

    Fit f = fitBox(w, ht);
    int bytesPP = bpp / 8;
    u32 rowBytes = ((u32)w * bytesPP + 3) & ~3u;
    u8 *row = malloc(rowBytes);
    if (row == NULL)
    {
        close(fd);
        return -2;
    }
    for (int y = 0; y < f.outH && ok; y++)
    {
        int sy = (y * ht) / f.outH;
        int fileRow = topDown ? sy : (ht - 1 - sy);
        ok = lseek(fd, off + (u32)fileRow * rowBytes, SEEK_SET) >= 0 &&
             read(fd, row, rowBytes) == (ssize_t)rowBytes;
        u16 *d = vram + (f.oy + y) * 256 + f.ox;
        for (int x = 0; x < f.outW && ok; x++)
        {
            const u8 *p = row + ((x * w) / f.outW) * bytesPP;
            d[x] = RGB15(p[2] >> 3, p[1] >> 3, p[0] >> 3) | BIT(15);
        }
    }
    free(row);
    close(fd);
    return ok ? 1 : -1;
}

// ---------------------------------------------------------------------------
// JPEG via TJpgDec

typedef struct
{
    FILE *f;
    u16 *buf;          // decoded picture at 1/2^scale size
    int bw, bh;
} JpgCtx;

static u8 jpgPool[0x8000] ALIGN(4);

static size_t jpgIn(JDEC *jd, uint8_t *out, size_t len)
{
    JpgCtx *c = (JpgCtx *)jd->device;
    if (out == NULL)
        return fseek(c->f, len, SEEK_CUR) == 0 ? len : 0;
    return fread(out, 1, len, c->f);
}

static int jpgOut(JDEC *jd, void *data, JRECT *r)
{
    JpgCtx *c = (JpgCtx *)jd->device;
    const u16 *s = (const u16 *)data;
    int rw = r->right - r->left + 1;
    for (int y = r->top; y <= r->bottom; y++)
    {
        if (y < c->bh)
        {
            u16 *d = c->buf + y * c->bw;
            for (int i = 0; i < rw; i++)
            {
                int x = r->left + i;
                if (x < c->bw)
                    d[x] = s[i];
            }
        }
        s += rw;
    }
    return 1;
}

// 1 = shown, 0 = TJpgDec can't do it (progressive etc), <0 = error
static int loadJpeg(const char *path, u16 *vram)
{
    JpgCtx c = { 0 };
    c.f = fopen(path, "rb");
    if (c.f == NULL)
        return -1;
    setvbuf(c.f, NULL, _IOFBF, 16 * 1024);

    JDEC jd;
    JRESULT res = jd_prepare(&jd, jpgIn, jpgPool, sizeof(jpgPool), &c);
    if (res != JDR_OK)
    {
        fclose(c.f);
        return res == JDR_FMT3 ? 0 : -1;    // FMT3 = progressive / unsupported
    }

    // Biggest reduction that still gives at least screen size, and fits in RAM
    int scale = 0;
    while (scale < 3 && (jd.width >> (scale + 1)) >= OUT_W && (jd.height >> (scale + 1)) >= OUT_H)
        scale++;
    while (scale < 3 && (u32)(jd.width >> scale) * (jd.height >> scale) > 1200 * 1000)
        scale++;

    c.bw = (jd.width + (1 << scale) - 1) >> scale;
    c.bh = (jd.height + (1 << scale) - 1) >> scale;
    c.buf = malloc((size_t)c.bw * c.bh * 2);
    if (c.buf == NULL)
    {
        fclose(c.f);
        return -2;
    }
    memset(c.buf, 0, (size_t)c.bw * c.bh * 2);

    res = jd_decomp(&jd, jpgOut, scale);
    fclose(c.f);
    if (res == JDR_OK)
    {
        Fit f = fitBox(c.bw, c.bh);
        for (int y = 0; y < f.outH; y++)
        {
            const u16 *row = c.buf + ((y * c.bh) / f.outH) * c.bw;
            u16 *d = vram + (f.oy + y) * 256 + f.ox;
            for (int x = 0; x < f.outW; x++)
                d[x] = row[(x * c.bw) / f.outW];
        }
    }
    free(c.buf);
    return res == JDR_OK ? 1 : -1;
}

// ---------------------------------------------------------------------------

static int loadStb(const char *path, u16 *vram, char *err)
{
    int w, h;
    if (!imgStbInfo(path, &w, &h))
    {
        snprintf(err, 32, "Unsupported picture");
        return -1;
    }
    if ((s64)w * h > STB_MAX_PIXELS)
    {
        snprintf(err, 32, "Too big (%dx%d)", w, h);
        return -1;
    }
    u8 *px = imgStbLoad(path, &w, &h);
    if (px == NULL)
    {
        snprintf(err, 32, "Couldn't decode picture");
        return -1;
    }
    blitRgb(px, w, h, vram);
    imgStbFree(px);
    return 1;
}

bool photoLoad(const char *path, u16 *vram, char *err)
{
    err[0] = 0;
    clearScreen(vram);

    const char *dot = strrchr(path, '.');
    int r = 0;
    if (dot && (strcasecmp(dot, ".jpg") == 0 || strcasecmp(dot, ".jpeg") == 0))
        r = loadJpeg(path, vram);
    else if (dot && strcasecmp(dot, ".bmp") == 0)
        r = loadBmp(path, vram);

    if (r == 0)
        r = loadStb(path, vram, err);
    if (r == -2)
        snprintf(err, 32, "Not enough memory");
    if (r < 0 && err[0] == 0)
        snprintf(err, 32, "Couldn't read this picture");
    if (r < 0)
        clearScreen(vram);
    return r > 0;
}
