// Minimal streaming AVI writer (AVI 1.0 / RIFF, with idx1 index).
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "avi.h"

#define FCC(a, b, c, d) ((uint32_t)(a) | ((uint32_t)(b) << 8) | ((uint32_t)(c) << 16) | ((uint32_t)(d) << 24))

#define AVIF_HASINDEX       0x00000010
#define AVIF_ISINTERLEAVED  0x00000100
#define AVIIF_KEYFRAME      0x00000010

#define HEADER_SIZE 512    // 'movi' LIST starts here (JUNK pads the gap)

static void put16(uint8_t *p, uint32_t *pos, uint16_t v)
{
    p[*pos] = v & 0xFF;
    p[*pos + 1] = v >> 8;
    *pos += 2;
}

static void put32(uint8_t *p, uint32_t *pos, uint32_t v)
{
    p[*pos] = v & 0xFF;
    p[*pos + 1] = (v >> 8) & 0xFF;
    p[*pos + 2] = (v >> 16) & 0xFF;
    p[*pos + 3] = v >> 24;
    *pos += 4;
}

static void set32(uint8_t *p, uint32_t pos, uint32_t v)
{
    put32(p, &pos, v);
}

static bool writeAt(int fd, uint32_t pos, uint32_t v)
{
    uint8_t b[4] = { v & 0xFF, (v >> 8) & 0xFF, (v >> 16) & 0xFF, v >> 24 };
    if (lseek(fd, pos, SEEK_SET) < 0)
        return false;
    return write(fd, b, 4) == 4;
}

static bool addIndex(AviWriter *w, uint32_t fourcc, uint32_t flags, uint32_t fileOffset, uint32_t size)
{
    if (w->indexCount == w->indexCap)
    {
        uint32_t newCap = w->indexCap ? w->indexCap * 2 : 8192;
        AviIndexEntry *n = realloc(w->index, newCap * sizeof(AviIndexEntry));
        if (n == NULL)
        {
            w->failed = true;
            return false;
        }
        w->index = n;
        w->indexCap = newCap;
    }
    AviIndexEntry *e = &w->index[w->indexCount++];
    e->fourcc = fourcc;
    e->flags = flags;
    e->offset = fileOffset - w->moviFourccPos;
    e->size = size;
    return true;
}

bool aviOpen(AviWriter *w, const char *path, int width, int height,
             uint32_t rate, uint32_t scale, bool hasAudio, uint32_t audioRate)
{
    memset(w, 0, sizeof(*w));
    w->width = width;
    w->height = height;
    w->rate = rate;
    w->scale = scale;
    w->hasAudio = hasAudio;
    w->audioRate = audioRate;

    w->fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (w->fd < 0)
        return false;

    uint8_t h[HEADER_SIZE];
    memset(h, 0, sizeof(h));
    uint32_t p = 0;
    uint32_t frameBytes = (uint32_t)width * height * 2;
    uint32_t usPerFrame = (uint32_t)((1000000ULL * scale) / rate);

    put32(h, &p, FCC('R', 'I', 'F', 'F'));
    put32(h, &p, 0);                                // patched on close
    put32(h, &p, FCC('A', 'V', 'I', ' '));

    put32(h, &p, FCC('L', 'I', 'S', 'T'));
    uint32_t hdrlSizePos = p;
    put32(h, &p, 0);
    put32(h, &p, FCC('h', 'd', 'r', 'l'));

    // Main AVI header
    put32(h, &p, FCC('a', 'v', 'i', 'h'));
    put32(h, &p, 56);
    put32(h, &p, usPerFrame);
    w->posAvihMaxBytes = p;
    put32(h, &p, 0);                                // dwMaxBytesPerSec
    put32(h, &p, 0);                                // dwPaddingGranularity
    put32(h, &p, AVIF_HASINDEX | AVIF_ISINTERLEAVED);
    w->posAvihTotalFrames = p;
    put32(h, &p, 0);                                // dwTotalFrames
    put32(h, &p, 0);                                // dwInitialFrames
    put32(h, &p, hasAudio ? 2 : 1);                 // dwStreams
    put32(h, &p, frameBytes + 8);                   // dwSuggestedBufferSize
    put32(h, &p, width);
    put32(h, &p, height);
    put32(h, &p, 0); put32(h, &p, 0); put32(h, &p, 0); put32(h, &p, 0);

    // Video stream
    put32(h, &p, FCC('L', 'I', 'S', 'T'));
    uint32_t strl1SizePos = p;
    put32(h, &p, 0);
    put32(h, &p, FCC('s', 't', 'r', 'l'));

    put32(h, &p, FCC('s', 't', 'r', 'h'));
    put32(h, &p, 56);
    put32(h, &p, FCC('v', 'i', 'd', 's'));
    put32(h, &p, FCC('D', 'I', 'B', ' '));
    put32(h, &p, 0);                                // dwFlags
    put16(h, &p, 0);                                // wPriority
    put16(h, &p, 0);                                // wLanguage
    put32(h, &p, 0);                                // dwInitialFrames
    put32(h, &p, scale);
    put32(h, &p, rate);
    put32(h, &p, 0);                                // dwStart
    w->posVidLength = p;
    put32(h, &p, 0);                                // dwLength
    put32(h, &p, frameBytes);
    put32(h, &p, 0xFFFFFFFF);                       // dwQuality
    put32(h, &p, 0);                                // dwSampleSize
    put16(h, &p, 0); put16(h, &p, 0);
    put16(h, &p, width); put16(h, &p, height);

    put32(h, &p, FCC('s', 't', 'r', 'f'));
    put32(h, &p, 40);
    put32(h, &p, 40);                               // biSize
    put32(h, &p, width);
    put32(h, &p, height);                           // positive = bottom-up rows
    put16(h, &p, 1);                                // biPlanes
    put16(h, &p, 16);                               // biBitCount (RGB555)
    put32(h, &p, 0);                                // BI_RGB
    put32(h, &p, frameBytes);
    put32(h, &p, 0); put32(h, &p, 0); put32(h, &p, 0); put32(h, &p, 0);

    set32(h, strl1SizePos, p - (strl1SizePos + 4));

    if (hasAudio)
    {
        put32(h, &p, FCC('L', 'I', 'S', 'T'));
        uint32_t strl2SizePos = p;
        put32(h, &p, 0);
        put32(h, &p, FCC('s', 't', 'r', 'l'));

        put32(h, &p, FCC('s', 't', 'r', 'h'));
        put32(h, &p, 56);
        put32(h, &p, FCC('a', 'u', 'd', 's'));
        put32(h, &p, 0);
        put32(h, &p, 0);
        put16(h, &p, 0);
        put16(h, &p, 0);
        put32(h, &p, 0);
        put32(h, &p, 1);                            // dwScale
        put32(h, &p, audioRate);                    // dwRate (samples/sec)
        put32(h, &p, 0);
        w->posAudLength = p;
        put32(h, &p, 0);                            // dwLength (samples)
        put32(h, &p, audioRate * 2);
        put32(h, &p, 0xFFFFFFFF);
        put32(h, &p, 2);                            // dwSampleSize (block align)
        put16(h, &p, 0); put16(h, &p, 0); put16(h, &p, 0); put16(h, &p, 0);

        put32(h, &p, FCC('s', 't', 'r', 'f'));
        put32(h, &p, 18);
        put16(h, &p, 1);                            // WAVE_FORMAT_PCM
        put16(h, &p, 1);                            // mono
        put32(h, &p, audioRate);
        put32(h, &p, audioRate * 2);
        put16(h, &p, 2);                            // nBlockAlign
        put16(h, &p, 16);                           // wBitsPerSample
        put16(h, &p, 0);                            // cbSize

        set32(h, strl2SizePos, p - (strl2SizePos + 4));
    }

    set32(h, hdrlSizePos, p - (hdrlSizePos + 4));

    // JUNK padding up to HEADER_SIZE
    put32(h, &p, FCC('J', 'U', 'N', 'K'));
    uint32_t junkSize = HEADER_SIZE - (p + 4);
    put32(h, &p, junkSize);
    p += junkSize;

    // 'movi' list header follows the header block
    uint8_t movi[12];
    uint32_t mp = 0;
    put32(movi, &mp, FCC('L', 'I', 'S', 'T'));
    put32(movi, &mp, 0);                            // patched on close
    put32(movi, &mp, FCC('m', 'o', 'v', 'i'));

    if (write(w->fd, h, HEADER_SIZE) != HEADER_SIZE || write(w->fd, movi, 12) != 12)
    {
        close(w->fd);
        w->fd = -1;
        return false;
    }

    w->moviFourccPos = HEADER_SIZE + 8;
    w->fileSize = HEADER_SIZE + 12;
    return true;
}

static uint32_t chunkHeader(uint8_t *dst, uint32_t fourcc, uint32_t size)
{
    uint32_t p = 0;
    put32(dst, &p, fourcc);
    put32(dst, &p, size);
    return 8;
}

uint32_t aviBatchEmptyFrame(AviWriter *w, uint8_t *dst, uint32_t batchOffset)
{
    // A zero-length video chunk = "repeat previous frame" in common players.
    addIndex(w, FCC('0', '0', 'd', 'b'), 0, w->fileSize + batchOffset, 0);
    w->videoFrames++;
    return chunkHeader(dst, FCC('0', '0', 'd', 'b'), 0);
}

uint32_t aviBatchVideoHeader(AviWriter *w, uint8_t *dst, uint32_t batchOffset, uint32_t frameBytes)
{
    addIndex(w, FCC('0', '0', 'd', 'b'), AVIIF_KEYFRAME, w->fileSize + batchOffset, frameBytes);
    w->videoFrames++;
    w->realFrames++;
    return chunkHeader(dst, FCC('0', '0', 'd', 'b'), frameBytes);
}

uint32_t aviBatchAudioHeader(AviWriter *w, uint8_t *dst, uint32_t batchOffset, uint32_t audioBytes)
{
    addIndex(w, FCC('0', '1', 'w', 'b'), AVIIF_KEYFRAME, w->fileSize + batchOffset, audioBytes);
    w->audioSamples += audioBytes / 2;
    return chunkHeader(dst, FCC('0', '1', 'w', 'b'), audioBytes);
}

bool aviCommit(AviWriter *w, const uint8_t *batch, uint32_t len)
{
    if (w->failed)
        return false;
    if (len == 0)
        return true;

    ssize_t r = write(w->fd, batch, len);
    if (r != (ssize_t)len)
    {
        w->failed = true;
        return false;
    }
    w->fileSize += len;
    return true;
}

bool aviClose(AviWriter *w)
{
    if (w->fd < 0)
        return false;

    bool ok = true;
    uint32_t moviEnd = w->fileSize;

    // idx1
    uint32_t idxBytes = w->indexCount * 16;
    uint8_t hdr[8];
    chunkHeader(hdr, FCC('i', 'd', 'x', '1'), idxBytes);
    if (write(w->fd, hdr, 8) != 8)
        ok = false;

    // Write index in blocks (entries are already little-endian on ARM)
    if (ok && idxBytes > 0)
    {
        const uint8_t *src = (const uint8_t *)w->index;
        uint32_t left = idxBytes;
        while (left > 0)
        {
            uint32_t n = left > 65536 ? 65536 : left;
            if (write(w->fd, src, n) != (ssize_t)n)
            {
                ok = false;
                break;
            }
            src += n;
            left -= n;
        }
    }

    uint32_t total = moviEnd + 8 + (ok ? idxBytes : 0);
    if (!ok)
        total = moviEnd; // index failed: file still playable by most players (no seeking)

    // Patch sizes and counts
    double seconds = (double)w->videoFrames * w->scale / w->rate;
    uint32_t maxBps = seconds > 0 ? (uint32_t)(total / seconds) : 0;

    writeAt(w->fd, 4, total - 8);
    writeAt(w->fd, HEADER_SIZE + 4, moviEnd - (HEADER_SIZE + 8));
    writeAt(w->fd, w->posAvihTotalFrames, w->videoFrames);
    writeAt(w->fd, w->posAvihMaxBytes, maxBps);
    writeAt(w->fd, w->posVidLength, w->videoFrames);
    if (w->hasAudio)
        writeAt(w->fd, w->posAudLength, w->audioSamples);

    close(w->fd);
    w->fd = -1;
    free(w->index);
    w->index = NULL;
    return ok;
}
