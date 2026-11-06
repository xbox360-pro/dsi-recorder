// DSi Recorder - records video (and mic audio) from the DSi cameras to the SD card.
//
// Output: sd:/DSiRecorder/REC_####.AVI  (256x192 uncompressed RGB555 + 16 kHz mono PCM)
// Requires DSi mode (Unlaunch / TWiLight Menu++ / hiyaCFW launching from SD).
//
// Controls:
//   A          start / stop recording
//   L / R      switch inner / outer camera
//   X          toggle microphone audio (when not recording)
//   UP / DOWN  FPS slider 0.1 - 60 fps (hold to scroll, or tap/drag on touch screen)
//   SELECT     video viewer (play back recordings)
//   START      quit
//
// Custom look: put a 24-bit .bmp at sd:/DSiRecorder/theme.bmp (any size) and it is
// used as the header icon and a faded background pattern.

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
#include "player.h"
#include "ui.h"

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

// DS refresh rate is ~59.8261 Hz (used to turn VBlank counts into seconds).
#define VBLANK_RATE_NUM  598261
#define VBLANK_RATE_DEN  10000

// FPS slider, stored in tenths of a frame per second: 1 (0.1 fps) .. 600 (60.0 fps).
#define FPS_MIN_T     1
#define FPS_MAX_T     600
static int fpsT = 150;                                // 15.0 fps default

static int fpsStepUp(int t)   { return t < 10 ? t + 1 : (t < FPS_MAX_T ? ((t / 10) + 1) * 10 : t); }
static int fpsStepDown(int t) { return t > 10 ? ((t - 1) / 10) * 10 : (t > FPS_MIN_T ? t - 1 : t); }

// ---------------------------------------------------------------------------
// Camera frame ring (filled by NDMA, managed from the VBlank IRQ)

static u16 *frameBuf[NUM_FRAMEBUF];
static volatile int camWriting = -1;    // buffer the camera DMA is filling
static volatile int camLatest = -1;     // newest complete frame
static volatile int camLocked = -1;     // buffer the main loop is encoding
static volatile u32 camFrameSeq = 0;    // increments on each completed frame
static volatile bool camPaused = true;  // main loop owns the camera while true
volatile u32 vblankCount = 0;          // also used by the video viewer
static u32 displayedSeq = 0;
static u16 *previewVram;
static volatile u32 camBusyVbl = 0;     // VBlanks the current transfer has been running

// A camera FIFO overrun (e.g. the memory bus was busy for too long) leaves the
// NDMA transfer waiting forever. Abort it and clear the camera FIFO.
static void cameraKick(void)
{
    REG_NDMA_CR(CAMERA_NDMA_CHANNEL) = 0;
    cameraStopTransfer();
    REG_CAM_CNT |= CAM_CNT_TRANSFER_FLUSH;   // also clears the overrun flag
}

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
        bool busy = ndmaBusy(CAMERA_NDMA_CHANNEL);

        // Watchdog: a frame never takes this long; the transfer is stuck
        if (busy && (++camBusyVbl > 30 || (REG_CAM_CNT & CAM_CNT_TRANSFER_ERROR)))
        {
            cameraKick();
            camWriting = -1;                     // drop the partial frame
            busy = false;
        }

        if (!busy)
        {
            // Previous transfer finished
            cameraStopTransfer();
            if (camWriting >= 0)
            {
                camLatest = camWriting;
                camFrameSeq++;
            }
            camBusyVbl = 0;
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
    u32 start = vblankCount;
    while (ndmaBusy(CAMERA_NDMA_CHANNEL) && vblankCount - start < 30)
        ;
    if (ndmaBusy(CAMERA_NDMA_CHANNEL))
        cameraKick();                            // stuck: don't hang the app
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
    camBusyVbl = 0;
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
static u32 recFpsT;                // fps * 10 for the current recording
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

    recFpsT = fpsT;
    if (!aviOpen(&avi, recPath, FRAME_W, FRAME_H, recFpsT, 10, useAudio, AUDIO_RATE))
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
    // frames due = elapsed seconds * fps   (seconds = vblanks * DEN / NUM)
    u32 slotsDue = (u32)(((u64)elapsed * VBLANK_RATE_DEN * recFpsT) /
                         ((u64)VBLANK_RATE_NUM * 10)) + 1;
    if (slotsDue <= avi.videoFrames)
    {
        // No video slot yet (slow frame rates): still drain audio so the ring never overflows
        if (useAudio && audioAvailable() >= 32 * 1024)
        {
            u32 n = batchAudio(0, batchCap);
            if (!aviCommit(&avi, batchBuf, n) || avi.failed)
                stopRecording("SD write failed / full!");
        }
        return;
    }

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
// Bottom-screen UI

#define SLIDER_X   24
#define SLIDER_W   208
#define SLIDER_Y   104

static const UiBox btnCamera = { 8, 120, 76, 24 };
static const UiBox btnMic = { 90, 120, 76, 24 };
static const UiBox btnVideos = { 172, 120, 76, 24 };
static const UiBox btnRec = { 8, 148, 240, 32 };
static const UiBox sliderZone = { 0, 84, 256, 34 };

enum { PRESS_NONE, PRESS_CAMERA, PRESS_MIC, PRESS_VIDEOS, PRESS_REC };
static int pressed = PRESS_NONE;

// Slider position (0..1000) <-> fps in tenths (1 = 0.1 fps .. 600 = 60 fps)
static int fpsToPos(int t)
{
    return (t - FPS_MIN_T) * 1000 / (FPS_MAX_T - FPS_MIN_T);
}

static int fpsFromPos(int pos)
{
    if (pos < 0)
        pos = 0;
    if (pos > 1000)
        pos = 1000;
    int t = FPS_MIN_T + pos * (FPS_MAX_T - FPS_MIN_T) / 1000;
    if (t >= 10)
        t = ((t + 5) / 10) * 10;   // whole fps above 1 fps
    return t;
}

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

// Full-screen message (errors at startup)
static void showMessage(const char *title, const char *l1, const char *l2, const char *l3)
{
    uiBegin();
    uiHeader("DSi Recorder");
    uiCard(8, 40, 240, 96, COL_CARD, COL_CARD_EDGE);
    uiText(3, 7, TXT_AMBER, "%s", title);
    if (l1)
        uiText(3, 9, TXT_WHITE, "%s", l1);
    if (l2)
        uiText(3, 10, TXT_WHITE, "%s", l2);
    if (l3)
        uiText(3, 12, TXT_MUTED, "%s", l3);
    uiPresent();
}

static void drawUi(int camera)
{
    uiBegin();
    uiHeader("DSi Recorder");

    // Header right side: blinking REC badge or the camera name
    if (recording)
    {
        bool on = ((vblankCount / 30) & 1) == 0;
        uiRound(186, 4, 62, 14, 7, on ? COL_RED : COL_RED_DARK);
        uiCircle(197, 11, 3, COL_WHITE);
        uiText(26, 1, TXT_WHITE, "REC");
    }
    else
    {
        uiText(22, 1, TXT_MUTED, "%s cam",
               camera == CAMERA_INNER ? "Inner" : "Outer");
    }

    // Info card
    uiCard(8, 32, 240, 48, COL_CARD, COL_CARD_EDGE);
    if (recording)
    {
        u32 secs = (u32)(((u64)avi.videoFrames * 10) / recFpsT);
        u32 kb = avi.fileSize / 1024;
        uiCircle(23, 44, 4, COL_RED);
        uiText(4, 5, TXT_RED, "Recording");
        uiText(23, 5, TXT_WHITE, "%02lu:%02lu", (unsigned long)(secs / 60),
               (unsigned long)(secs % 60));
        uiText(2, 6, TXT_MUTED, "File");
        uiText(10, 6, TXT_WHITE, "%s", recPath + sizeof(REC_DIR));
        uiText(2, 7, TXT_MUTED, "Size");
        if (kb >= 1024)
            uiText(10, 7, TXT_WHITE, "%lu.%lu MB", (unsigned long)(kb / 1024),
                   (unsigned long)((kb % 1024) * 10 / 1024));
        else
            uiText(10, 7, TXT_WHITE, "%lu KB", (unsigned long)kb);
        uiText(2, 8, TXT_MUTED, "Frames");
        uiText(10, 8, TXT_WHITE, "%lu", (unsigned long)avi.videoFrames);
        uiText(18, 8, TXT_MUTED, "rep %lu", (unsigned long)(avi.videoFrames - avi.realFrames));
    }
    else
    {
        uiText(2, 5, TXT_MUTED, "Camera");
        uiText(13, 5, TXT_WHITE, "%s", camera == CAMERA_INNER ? "Inner (selfie)" : "Outer");
        uiText(2, 6, TXT_MUTED, "Microphone");
        uiText(13, 6, useAudio ? TXT_GREEN : TXT_MUTED, "%s", useAudio ? "On" : "Off");
        uiText(2, 7, TXT_MUTED, "Saves to");
        uiText(13, 7, TXT_WHITE, "sd:/DSiRecorder");
        if (statusMsg[0])
            uiText(2, 8, TXT_AMBER, "%.28s", statusMsg);
        else
            uiText(2, 8, TXT_ACCENT, "Ready to record");
    }

    // Frame-rate slider
    uiCard(8, 84, 240, 32, COL_CARD, COL_CARD_EDGE);
    uiText(2, 11, TXT_MUTED, "Frame rate");
    uiText(fpsT >= 100 ? 21 : 22, 11, TXT_WHITE, "%d.%d fps", fpsT / 10, fpsT % 10);
    uiSlider(SLIDER_X, SLIDER_Y, SLIDER_W, fpsToPos(fpsT), !recording,
             recording ? COL_DISABLED : COL_ACCENT);

    // Buttons
    uiButton(btnCamera, "Camera", pressed == PRESS_CAMERA ? 1 : 0);
    uiButton(btnMic, useAudio ? "Mic: On" : "Mic: Off",
             recording ? 2 : (pressed == PRESS_MIC ? 1 : 0));
    uiButton(btnVideos, "Videos", recording ? 2 : (pressed == PRESS_VIDEOS ? 1 : 0));

    // Big record / stop button
    bool down = pressed == PRESS_REC;
    if (recording)
    {
        uiCard(btnRec.x, btnRec.y, btnRec.w, btnRec.h, down ? COL_RED_DARK : UIC(10, 3, 5),
               COL_RED_EDGE);
        uiRect(100, 158, 12, 12, COL_WHITE);
        uiText(15, 20, TXT_WHITE, "STOP");
    }
    else
    {
        uiCard(btnRec.x, btnRec.y, btnRec.w, btnRec.h, down ? COL_RED_DARK : COL_RED,
               COL_RED_EDGE);
        uiRound(btnRec.x + 6, btnRec.y + 2, btnRec.w - 12, 3, 1, UIC(31, 12, 14));
        uiCircle(98, 164, 6, COL_WHITE);
        uiText(14, 20, TXT_WHITE, "RECORD");
    }

    uiText(1, 23, TXT_MUTED, "A rec  L/R cam  SELECT videos");
    uiPresent();
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    // Top screen: 16-bit bitmap for the camera preview
    videoSetMode(MODE_5_2D);
    vramSetBankA(VRAM_A_MAIN_BG);
    int bg = bgInit(3, BgType_Bmp16, BgSize_B16_256x256, 0, 0);
    previewVram = bgGetGfxPtr(bg);
    memset(previewVram, 0, 256 * 256 * 2);

    // Bottom screen: themed UI (bitmap + text)
    uiInit();

    if (!isDSiMode())
    {
        showMessage("This app needs DSi mode", "Run it on a DSi or 3DS,",
                    "launched from the SD card.", "Press START to exit");
        waitForStart();
        return 0;
    }

    showMessage("Starting up...", "Mounting SD card", NULL, NULL);
    if (!fatInitDefault())
    {
        showMessage("SD card error", "Couldn't open the SD card.", NULL, "Press START to exit");
        waitForStart();
        return 0;
    }

    // Optional custom picture: sd:/DSiRecorder/theme.bmp
    uiLoadTheme(REC_DIR "/theme.bmp");

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

    showMessage("Starting up...", "Starting camera", NULL, NULL);
    if (!cameraInit())
    {
        showMessage("Camera error", "The camera didn't start.", NULL, "Press START to exit");
        waitForStart();
        return 0;
    }

    int camera = CAMERA_OUTER;
    if (!cameraSelect(camera) || !cameraResume())
    {
        showMessage("Camera error", "Couldn't select the camera.", NULL, "Press START to exit");
        waitForStart();
        cameraDeinit();
        return 0;
    }

    keysSetRepeat(20, 4);
    irqSet(IRQ_VBLANK, vblankHandler);
    irqEnable(IRQ_VBLANK);

    int uiTimer = 0;
    bool dragging = false;
    while (1)
    {
        swiWaitForVBlank();
        scanKeys();
        u32 down = keysDown();
        u32 held = keysHeld();
        u32 up = keysUp();

        touchPosition tp = { 0 };
        if (held & KEY_TOUCH)
            touchRead(&tp);

        if (down & KEY_START)
            break;

        // Touch: find what was pressed, act on release (like a real button)
        int action = PRESS_NONE;
        if (down & KEY_TOUCH)
        {
            if (uiHit(btnRec, tp.px, tp.py))
                pressed = PRESS_REC;
            else if (uiHit(btnCamera, tp.px, tp.py))
                pressed = PRESS_CAMERA;
            else if (uiHit(btnMic, tp.px, tp.py) && !recording)
                pressed = PRESS_MIC;
            else if (uiHit(btnVideos, tp.px, tp.py) && !recording)
                pressed = PRESS_VIDEOS;
            else if (uiHit(sliderZone, tp.px, tp.py) && !recording)
                dragging = true;
            uiTimer = 0;
        }
        if (up & KEY_TOUCH)
        {
            action = pressed;
            pressed = PRESS_NONE;
            dragging = false;
            uiTimer = 0;
        }

        if ((down & KEY_A) || action == PRESS_REC)
        {
            if (recording)
                stopRecording(NULL);
            else
                startRecording();
            uiTimer = 0;
        }

        if ((down & (KEY_L | KEY_R)) || action == PRESS_CAMERA)
        {
            int want = (camera == CAMERA_INNER) ? CAMERA_OUTER : CAMERA_INNER;
            if (down & KEY_L)
                want = CAMERA_INNER;
            if (down & KEY_R)
                want = CAMERA_OUTER;
            if (want != camera)
            {
                cameraPause();
                if (cameraSelect(want))
                    camera = want;
                else
                    cameraSelect(camera);
                if (!cameraResume())
                    snprintf(statusMsg, sizeof(statusMsg), "Camera restart failed!");
            }
            uiTimer = 0;
        }

        if (((down & KEY_SELECT) || action == PRESS_VIDEOS) && !recording)
        {
            cameraPause();
            runViewer(previewVram);
            if (!cameraResume())
                snprintf(statusMsg, sizeof(statusMsg), "Camera restart failed!");
            uiTimer = 0;
            continue;
        }

        if (!recording)
        {
            if ((down & KEY_X) || action == PRESS_MIC)
            {
                useAudio = !useAudio;
                uiTimer = 0;
            }

            int oldFps = fpsT;
            u32 rep = keysDownRepeat();   // hold Up/Down to scroll
            if (rep & KEY_UP)
                fpsT = fpsStepUp(fpsT);
            if (rep & KEY_DOWN)
                fpsT = fpsStepDown(fpsT);
            if (dragging && (held & KEY_TOUCH))
                fpsT = fpsFromPos((tp.px - SLIDER_X) * 1000 / SLIDER_W);
            if (fpsT != oldFps)
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
    showMessage("Out of memory", "Not enough RAM to run.", NULL, "Press START to exit");
    waitForStart();
    return 0;
}
