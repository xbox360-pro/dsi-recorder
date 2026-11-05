// DSi Recorder - records video (and mic audio) from the DSi cameras to the SD card.
//
// Output: sd:/DSiRecorder/REC_####.AVI  (256x192 uncompressed RGB555 + 16 kHz mono PCM)
// Requires DSi mode (Unlaunch / TWiLight Menu++ / hiyaCFW launching from SD).
//
// Controls:
//   A          start / stop recording
//   L / R      switch inner / outer camera
//   X          toggle microphone audio (when not recording)
//   UP / DOWN  change frame rate (when not recording)
//   START      quit

#include <dirent.h>
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include <fat.h>
#include <nds.h>

#include "avi.h"

#define CAMERA_NDMA_CHANNEL 1

#define FRAME_W       256
#define FRAME_H       192
#define FRAME_PIXELS  (FRAME_W * FRAME_H)
#define FRAME_BYTES   (FRAME_PIXELS * 2)
#define NUM_FRAMEBUF  3

#define AUDIO_RATE        16000
#define MIC_BUF_BYTES     4096              // double buffer, callback every 2048 bytes (64 ms)
#define AUDIO_RING_BYTES  (128 * 1024)      // ~4 s of audio slack

#define REC_DIR        "/DSiRecorder"
#define MAX_FILE_BYTES (1900u * 1024u * 1024u)   // stay well under RIFF/AVI 1.0 2 GB limit

// DS refresh rate is ~59.8261 Hz. Recording fps = 59.8261 / vblanksPerFrame.
#define VBLANK_RATE_NUM  598261
#define VBLANK_RATE_DEN  10000

static const u8 fpsChoices[] = { 2, 3, 4, 5, 6 };   // vblanks per frame
static const char *fpsNames[] = { "30", "20", "15", "12", "10" };
static int fpsSel = 2;                                // 15 fps default

// ---------------------------------------------------------------------------
// Camera frame ring (filled by NDMA, managed from the VBlank IRQ)

static u16 *frameBuf[NUM_FRAMEBUF];
static volatile int camWriting = -1;    // buffer the camera DMA is filling
static volatile int camLatest = -1;     // newest complete frame
static volatile int camLocked = -1;     // buffer the main loop is encoding
static volatile u32 camFrameSeq = 0;    // increments on each completed frame
static volatile bool camPaused = true;  // main loop owns the camera while true
static volatile u32 vblankCount = 0;
static u32 displayedSeq = 0;
static u16 *previewVram;

static int pickFreeBuffer(void)
{
    for (int i = 0; i < NUM_FRAMEBUF; i++)
    {
        if (i != camLatest && i != camLocked)
            return i;
    }
    return 0; // unreachable with 3 buffers
}

static void vblankHandler(void)
{
    vblankCount++;

    if (!camPaused)
    {
        if (!ndmaBusy(CAMERA_NDMA_CHANNEL))
        {
            // Previous transfer finished
            cameraStopTransfer();
            if (camWriting >= 0)
            {
                camLatest = camWriting;
                camFrameSeq++;
            }
            camWriting = pickFreeBuffer();
            cameraStartTransfer(frameBuf[camWriting], MCUREG_APT_SEQ_CMD_PREVIEW,
                                CAMERA_NDMA_CHANNEL);
        }
    }

    // Show newest frame on the top screen (DMA reads RAM directly, no cache issue)
    if (camLatest >= 0 && camFrameSeq != displayedSeq)
    {
        dmaCopyWords(3, frameBuf[camLatest], previewVram, FRAME_BYTES);
        displayedSeq = camFrameSeq;
    }
}

// Take ownership of the camera from the IRQ (waits for the running transfer)
static void cameraPause(void)
{
    camPaused = true;
    while (ndmaBusy(CAMERA_NDMA_CHANNEL))
        ;
    cameraStopTransfer();
    camWriting = -1;
}

// Start first transfer from main context (may send a mode command over FIFO),
// then hand the camera back to the IRQ.
static bool cameraResume(void)
{
    int old = enterCriticalSection();
    int b = pickFreeBuffer();
    leaveCriticalSection(old);

    if (!cameraStartTransfer(frameBuf[b], MCUREG_APT_SEQ_CMD_PREVIEW, CAMERA_NDMA_CHANNEL))
        return false;
    camWriting = b;
    camPaused = false;
    return true;
}

// ---------------------------------------------------------------------------
// Microphone ring buffer (filled from the mic callback IRQ)

static u16 micBuffer[MIC_BUF_BYTES / 2] ALIGN(32);
static u8 *audioRing;
static volatile u32 audioHead = 0;  // write position (bytes, monotonically increasing)
static volatile u32 audioTail = 0;  // read position
static volatile bool audioCapture = false;
static volatile u32 audioOverflows = 0;

static void micHandler(void *completedBuffer, int length)
{
    if (!audioCapture)
        return;

    const u8 *src = (const u8 *)memUncached(completedBuffer);
    length &= ~1;

    u32 used = audioHead - audioTail;
    if (used + length > AUDIO_RING_BYTES)
    {
        audioOverflows++;
        return;
    }

    u32 pos = audioHead % AUDIO_RING_BYTES;
    u32 first = AUDIO_RING_BYTES - pos;
    if (first > (u32)length)
        first = length;
    memcpy(audioRing + pos, src, first);
    if ((u32)length > first)
        memcpy(audioRing, src + first, length - first);

    audioHead += length;
}

// ---------------------------------------------------------------------------
// Recording state

static AviWriter avi;
static bool recording = false;
static bool useAudio = true;
static u8 *batchBuf;
static u32 batchCap;
static u32 recStartVblank;
static u32 lastWrittenSeq;
static u32 vblanksPerFrame;
static char recPath[64];
static char statusMsg[33] = "";

// DS RGB15 (R low bits) -> AVI RGB555 (B low bits), flipped to bottom-up.
ITCM_CODE
static void convertFrame(const u16 *src, u8 *dstBytes)
{
    u32 *dst = (u32 *)dstBytes;  // dstBytes is 4-byte aligned (see batch layout)
    for (int y = 0; y < FRAME_H; y++)
    {
        const u32 *s = (const u32 *)(src + (FRAME_H - 1 - y) * FRAME_W);
        for (int x = 0; x < FRAME_W / 2; x++)
        {
            u32 v = s[x];
            u32 r = v & 0x001F001F;
            u32 g = v & 0x03E003E0;
            u32 b = (v >> 10) & 0x001F001F;
            *dst++ = (r << 10) | g | b;
        }
    }
}

static int nextFileNumber(void)
{
    int highest = 0;
    DIR *d = opendir(REC_DIR);
    if (d == NULL)
        return 1;
    struct dirent *e;
    while ((e = readdir(d)) != NULL)
    {
        if (strncasecmp(e->d_name, "REC_", 4) == 0)
        {
            int n = atoi(e->d_name + 4);
            if (n > highest)
                highest = n;
        }
    }
    closedir(d);
    return highest + 1;
}

// Kept a multiple of 4 so the video data after it stays word-aligned.
static u32 audioAvailable(void)
{
    return (audioHead - audioTail) & ~3u;
}

// Appends an audio chunk with everything currently buffered. Returns bytes used.
static u32 batchAudio(u32 offset, u32 maxBytes)
{
    if (!useAudio)
        return 0;
    u32 avail = audioAvailable();
    if (avail == 0)
        return 0;
    if (avail > maxBytes - 8)
        avail = (maxBytes - 8) & ~3u;

    u32 n = aviBatchAudioHeader(&avi, batchBuf + offset, offset, avail);
    u8 *dst = batchBuf + offset + n;

    u32 pos = audioTail % AUDIO_RING_BYTES;
    u32 first = AUDIO_RING_BYTES - pos;
    if (first > avail)
        first = avail;
    memcpy(dst, audioRing + pos, first);
    if (avail > first)
        memcpy(dst + first, audioRing, avail - first);
    audioTail += avail;

    return n + avail;
}

static void startRecording(void)
{
    mkdir(REC_DIR, 0777);
    snprintf(recPath, sizeof(recPath), REC_DIR "/REC_%04d.AVI", nextFileNumber());

    vblanksPerFrame = fpsChoices[fpsSel];
    if (!aviOpen(&avi, recPath, FRAME_W, FRAME_H, VBLANK_RATE_NUM,
                 VBLANK_RATE_DEN * vblanksPerFrame, useAudio, AUDIO_RATE))
    {
        snprintf(statusMsg, sizeof(statusMsg), "Can't create file!");
        return;
    }

    if (useAudio)
    {
        audioHead = audioTail = 0;
        audioOverflows = 0;
        audioCapture = true;
        soundMicRecord(micBuffer, MIC_BUF_BYTES, MicFormat_12Bit, AUDIO_RATE, micHandler);
    }

    lastWrittenSeq = camFrameSeq;
    recStartVblank = vblankCount;
    recording = true;
    statusMsg[0] = '\0';
}

static void stopRecording(const char *reason)
{
    if (!recording)
        return;
    recording = false;

    if (useAudio)
    {
        soundMicOff();
        audioCapture = false;
        // Flush leftover audio
        u32 n = batchAudio(0, batchCap);
        aviCommit(&avi, batchBuf, n);
    }

    bool ok = aviClose(&avi);
    if (reason)
        snprintf(statusMsg, sizeof(statusMsg), "%s", reason);
    else
        snprintf(statusMsg, sizeof(statusMsg), ok ? "Saved %s" : "Saved (no index) %s",
                 recPath + sizeof(REC_DIR));
}

// Called once per VBlank while recording.
static void recordTick(void)
{
    u32 elapsed = vblankCount - recStartVblank;
    u32 slotsDue = elapsed / vblanksPerFrame + 1;
    if (slotsDue <= avi.videoFrames)
        return;

    u32 missing = slotsDue - avi.videoFrames;
    if (missing > 64)
        missing = 64; // cap catch-up per batch; the rest is caught next tick

    bool newFrame = (camFrameSeq != lastWrittenSeq) && (camLatest >= 0);
    u32 off = 0;

    // Audio first (keeps it slightly ahead of video, which players like)
    off += batchAudio(off, batchCap - (FRAME_BYTES + 8) - 64 * 8);

    // Empty "repeat" chunks for slots we couldn't fill
    u32 empties = newFrame ? missing - 1 : missing;
    for (u32 i = 0; i < empties; i++)
        off += aviBatchEmptyFrame(&avi, batchBuf + off, off);

    if (newFrame)
    {
        int old = enterCriticalSection();
        int buf = camLatest;
        camLocked = buf;
        u32 seq = camFrameSeq;
        leaveCriticalSection(old);

        // Keep the frame data 4-byte aligned in the batch
        off += aviBatchVideoHeader(&avi, batchBuf + off, off, FRAME_BYTES);
        DC_InvalidateRange(frameBuf[buf], FRAME_BYTES);
        convertFrame(frameBuf[buf], batchBuf + off);
        off += FRAME_BYTES;

        camLocked = -1;
        lastWrittenSeq = seq;
    }

    if (avi.fileSize + off + (avi.indexCount + 64) * 16 > MAX_FILE_BYTES)
    {
        aviCommit(&avi, batchBuf, off);
        stopRecording("Max file size reached, saved");
        return;
    }

    if (!aviCommit(&avi, batchBuf, off) || avi.failed)
        stopRecording("SD write failed / full!");
}

// ---------------------------------------------------------------------------

static void waitForStart(void)
{
    while (1)
    {
        swiWaitForVBlank();
        scanKeys();
        if (keysDown() & KEY_START)
            break;
    }
}

static void drawUi(int camera)
{
    printf("\x1b[0;0H");
    printf("        DSi Recorder\n");
    printf("--------------------------------\n");
    printf("Camera: %-6s   Audio: %-3s    \n", camera == CAMERA_INNER ? "inner" : "outer",
           useAudio ? "on" : "off");
    printf("FPS:    %-3s                    \n", fpsNames[fpsSel]);
    printf("--------------------------------\n");

    if (recording)
    {
        u32 secs = (avi.videoFrames * vblanksPerFrame * VBLANK_RATE_DEN) / VBLANK_RATE_NUM;
        u32 dropped = avi.videoFrames - avi.realFrames;
        printf("\x1b[31m** REC **\x1b[39m  %02lu:%02lu            \n",
               (unsigned long)(secs / 60), (unsigned long)(secs % 60));
        printf("File: %-26s\n", recPath + sizeof(REC_DIR));
        printf("Size: %lu KB                 \n", (unsigned long)(avi.fileSize / 1024));
        printf("Frames: %lu (repeated %lu)     \n", (unsigned long)avi.videoFrames,
               (unsigned long)dropped);
        printf("Audio drops: %lu               \n", (unsigned long)audioOverflows);
    }
    else
    {
        printf("Ready.                          \n");
        printf("                                \n");
        printf("                                \n");
        printf("                                \n");
        printf("                                \n");
    }
    printf("%-32s", statusMsg);

    printf("\x1b[15;0H");
    printf("--------------------------------\n");
    printf("A       Start / stop recording\n");
    printf("L / R   Switch camera\n");
    printf("X       Toggle audio\n");
    printf("UP/DOWN Change FPS\n");
    printf("START   Quit\n");
    printf("\nSaves to sd:/DSiRecorder/\n");
}

int main(int argc, char **argv)
{
    // Top screen: 16-bit bitmap for the camera preview
    videoSetMode(MODE_5_2D);
    vramSetBankA(VRAM_A_MAIN_BG);
    int bg = bgInit(3, BgType_Bmp16, BgSize_B16_256x256, 0, 0);
    previewVram = bgGetGfxPtr(bg);
    memset(previewVram, 0, 256 * 256 * 2);

    // Bottom screen: text console
    consoleDemoInit();

    if (!isDSiMode())
    {
        printf("DSi Recorder\n\nThis app needs DSi mode\n(DSi/3DS console, launched from\nthe SD card).\n\nPress START to exit.");
        waitForStart();
        return 0;
    }

    printf("Mounting SD card...\n");
    if (!fatInitDefault())
    {
        printf("SD card init failed!\n\nPress START to exit.");
        waitForStart();
        return 0;
    }

    // Allocate buffers
    for (int i = 0; i < NUM_FRAMEBUF; i++)
    {
        frameBuf[i] = memalign(32, FRAME_BYTES);
        if (frameBuf[i] == NULL)
            goto oom;
        memset(frameBuf[i], 0, FRAME_BYTES);
        DC_FlushRange(frameBuf[i], FRAME_BYTES);
    }
    audioRing = malloc(AUDIO_RING_BYTES);
    batchCap = FRAME_BYTES + 8 + 64 * 8 + AUDIO_RING_BYTES + 8 + 64;
    batchBuf = memalign(32, batchCap);
    if (audioRing == NULL || batchBuf == NULL)
        goto oom;
    DC_FlushRange(micBuffer, sizeof(micBuffer));
    soundEnable();

    printf("Starting camera...\n");
    if (!cameraInit())
    {
        printf("Camera init failed!\n\nPress START to exit.");
        waitForStart();
        return 0;
    }

    int camera = CAMERA_OUTER;
    if (!cameraSelect(camera) || !cameraResume())
    {
        printf("Camera select failed!\n\nPress START to exit.");
        waitForStart();
        cameraDeinit();
        return 0;
    }

    irqSet(IRQ_VBLANK, vblankHandler);
    irqEnable(IRQ_VBLANK);

    consoleClear();

    int uiTimer = 0;
    while (1)
    {
        swiWaitForVBlank();
        scanKeys();
        u16 down = keysDown();

        if (down & KEY_START)
            break;

        if (down & KEY_A)
        {
            if (recording)
                stopRecording(NULL);
            else
                startRecording();
            uiTimer = 0;
        }

        if (down & (KEY_L | KEY_R))
        {
            int want = (down & KEY_L) ? CAMERA_INNER : CAMERA_OUTER;
            if (want == camera)
                want = (camera == CAMERA_INNER) ? CAMERA_OUTER : CAMERA_INNER;
            cameraPause();
            if (cameraSelect(want))
                camera = want;
            else
                cameraSelect(camera);
            if (!cameraResume())
                snprintf(statusMsg, sizeof(statusMsg), "Camera restart failed!");
            uiTimer = 0;
        }

        if (!recording)
        {
            if (down & KEY_X)
                useAudio = !useAudio;
            if ((down & KEY_UP) && fpsSel > 0)
                fpsSel--;
            if ((down & KEY_DOWN) && fpsSel < (int)sizeof(fpsChoices) - 1)
                fpsSel++;
            if (down & (KEY_X | KEY_UP | KEY_DOWN))
                uiTimer = 0;
        }

        if (recording)
            recordTick();

        if (--uiTimer <= 0)
        {
            drawUi(camera);
            uiTimer = 15;
        }
    }

    stopRecording(NULL);
    cameraPause();
    irqSet(IRQ_VBLANK, NULL);
    cameraDeinit();
    return 0;

oom:
    printf("Out of memory!\n\nPress START to exit.");
    waitForStart();
    return 0;
}
