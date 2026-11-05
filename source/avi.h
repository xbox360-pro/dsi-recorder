// Minimal streaming AVI writer: uncompressed 16-bit RGB video + 16-bit PCM mono audio.
#ifndef AVI_H
#define AVI_H

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint32_t fourcc;
    uint32_t flags;
    uint32_t offset;   // relative to the 'movi' fourcc
    uint32_t size;
} AviIndexEntry;

typedef struct {
    int fd;
    bool hasAudio;
    int width, height;
    uint32_t rate, scale;          // video fps = rate / scale
    uint32_t audioRate;

    uint32_t fileSize;             // current write position (end of file)
    uint32_t moviFourccPos;        // file offset of 'movi' tag

    // header patch positions
    uint32_t posAvihTotalFrames;
    uint32_t posVidLength;
    uint32_t posAudLength;
    uint32_t posAvihMaxBytes;

    uint32_t videoFrames;          // includes empty (dropped) frames
    uint32_t realFrames;
    uint32_t audioSamples;

    AviIndexEntry *index;
    uint32_t indexCount, indexCap;
    bool failed;
} AviWriter;

bool aviOpen(AviWriter *w, const char *path, int width, int height,
             uint32_t rate, uint32_t scale, bool hasAudio, uint32_t audioRate);

// Batched writing. Build a batch in a caller buffer, then commit it in one write().
// Each helper returns the number of bytes appended to dst.
uint32_t aviBatchEmptyFrame(AviWriter *w, uint8_t *dst, uint32_t batchOffset);
// Writes chunk header for a video frame; caller fills `frameBytes` bytes right after it.
uint32_t aviBatchVideoHeader(AviWriter *w, uint8_t *dst, uint32_t batchOffset, uint32_t frameBytes);
uint32_t aviBatchAudioHeader(AviWriter *w, uint8_t *dst, uint32_t batchOffset, uint32_t audioBytes);

bool aviCommit(AviWriter *w, const uint8_t *batch, uint32_t len);
bool aviClose(AviWriter *w);

#endif
