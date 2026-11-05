// In-app video viewer for the AVIs written by DSi Recorder.
//
// Reads the 'movi' chunks sequentially (no index needed, so even files that
// weren't closed cleanly play), queues a few decoded frames ahead, and streams
// the PCM audio into a looping sound channel.
//
// Controls (list):    UP/DOWN select, A play, B back to camera
// Controls (playing): A pause/resume, R skip +5 s, L restart, B stop

#include <dirent.h>
#include <fcntl.h>
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include <nds.h>

#include "player.h"

extern volatile u32 vblankCount;     // incremented by main.c's VBlank handler

#define REC_DIR      "/DSiRecorder"
#define FRAME_W      256
#define FRAME_H      192
#define FRAME_BYTES  (FRAME_W * FRAME_H * 2)
#define HEADER_SIZE  512             // where the 'movi' LIST starts in our files

#define VB_NUM       598261          // VBlank rate = 59.8261 Hz
#define VB_DEN       10000

#define NQ           4               // decoded frames queued ahead
#define RING_BYTES   (512 * 1024)    // audio ring (16 s at 16 kHz mono 16-bit)
#define RING_SAMPLES (RING_BYTES / 2)
#define TMP_BYTES    (160 * 1024)    // largest audio chunk we accept

#define FCC(a, b, c, d) ((u32)(a) | ((u32)(b) << 8) | ((u32)(c) << 16) | ((u32)(d) << 24))

#define MAX_FILES    256
#define LIST_ROWS    16

// ---------------------------------------------------------------------------
// Playback state

static int fd = -1;
static u32 vidRate, vidScale;        // fps = rate / scale
static u32 totalFrames;
static bool hasAudio;
static u32 audioRate;
static bool eof;

static u32 vidIndex;                 // index of the next video chunk in the file
static u64 audSample;                // index of the next audio sample in the file

static u16 *q[NQ];
static u32 qIdx[NQ];
static int qHead, qCount;
static u8 *tmp;

static s16 *ring;
static int sndId = -1;
static bool audioPending;            // waiting for the clock to reach ringBase
static u64 ringBase;                 // file sample index that ring[0] maps to
static u64 zeroedTo;

static u32 playVbl;                  // playback clock in VBlanks
static bool paused;

static u32 rd32(const u8 *p)
{
    return p[0] | (p[1] << 8) | (p[2] << 16) | ((u32)p[3] << 24);
}

static u32 dueFrame(void)
{
    return (u32)(((u64)playVbl * VB_DEN * vidRate) / ((u64)VB_NUM * vidScale));
}

static u64 clockSample(void)
{
    return ((u64)playVbl * VB_DEN * audioRate) / VB_NUM;
}

// AVI pixels (RGB555 with blue in the low bits, bottom-up) -> DS bitmap
ITCM_CODE
static void showFrame(const u16 *src, u16 *dst)
{
    u32 *d = (u32 *)dst;
    for (int y = 0; y < FRAME_H; y++)
    {
        const u32 *s = (const u32 *)(src + (FRAME_H - 1 - y) * FRAME_W);
        for (int x = 0; x < FRAME_W / 2; x++)
        {
            u32 v = s[x];
            u32 r = (v >> 10) & 0x001F001F;
            u32 g = v & 0x03E003E0;
            u32 b = v & 0x001F001F;
            *d++ = r | g | (b << 10) | 0x80008000;
        }
    }
}

static void stopSound(void)
{
    if (sndId >= 0)
        soundKill(sndId);
    sndId = -1;
}

// Restart audio `leadSamples` after the current clock (used at start and after a skip)
static void resetAudio(u32 leadSamples)
{
    stopSound();
    memset(ring, 0, RING_BYTES);
    DC_FlushRange(ring, RING_BYTES);
    ringBase = clockSample() + leadSamples;
    zeroedTo = ringBase;
    audioPending = hasAudio;
}

static void writeAudio(const s16 *src, u64 first, u32 count)
{
    for (u32 i = 0; i < count; i++)
    {
        u64 s = first + i;
        if (s < ringBase)
            continue;
        ring[(s - ringBase) % RING_SAMPLES] = src[i];
    }
}

// Reads one chunk. Returns 1 if it read a lot of data (a frame), 0 otherwise.
static int readChunk(void)
{
    u8 h[8];
    if (read(fd, h, 8) != 8)
    {
        eof = true;
        return 0;
    }
    u32 fcc = rd32(h);
    u32 size = rd32(h + 4);
    u32 padded = (size + 1) & ~1u;

    if (fcc == FCC('0', '0', 'd', 'b') || fcc == FCC('0', '0', 'd', 'c'))
    {
        u32 idx = vidIndex++;
        if (size == 0)
            return 0;                       // "repeat previous frame"
        if (size != FRAME_BYTES || idx + 1 < dueFrame())
        {
            lseek(fd, padded, SEEK_CUR);    // wrong size, or already late: skip
            return 0;
        }
        int slot = (qHead + qCount) % NQ;
        if (read(fd, q[slot], FRAME_BYTES) != FRAME_BYTES)
        {
            eof = true;
            return 1;
        }
        qIdx[slot] = idx;
        qCount++;
        return 1;
    }

    if (fcc == FCC('0', '1', 'w', 'b'))
    {
        u64 first = audSample;
        u32 n = size / 2;
        audSample += n;
        if (!hasAudio || size > TMP_BYTES || first + n <= ringBase)
        {
            lseek(fd, padded, SEEK_CUR);
            return 0;
        }
        if (read(fd, tmp, padded) != (ssize_t)padded)
        {
            eof = true;
            return 0;
        }
        writeAudio((const s16 *)tmp, first, n);
        DC_FlushRange(ring, RING_BYTES);
        return size > 32768;
    }

    if (fcc == FCC('J', 'U', 'N', 'K'))
    {
        lseek(fd, padded, SEEK_CUR);
        return 0;
    }

    eof = true;   // 'idx1' or anything unexpected: end of the movie data
    return 0;
}

static bool openVideo(const char *path)
{
    fd = open(path, O_RDONLY);
    if (fd < 0)
        return false;

    u8 hdr[HEADER_SIZE + 12];
    if (read(fd, hdr, sizeof(hdr)) != (ssize_t)sizeof(hdr) ||
        rd32(hdr) != FCC('R', 'I', 'F', 'F') || rd32(hdr + 8) != FCC('A', 'V', 'I', ' ') ||
        rd32(hdr + 24) != FCC('a', 'v', 'i', 'h') || rd32(hdr + 108) != FCC('v', 'i', 'd', 's') ||
        rd32(hdr + HEADER_SIZE + 8) != FCC('m', 'o', 'v', 'i'))
    {
        close(fd);
        fd = -1;
        return false;
    }

    totalFrames = rd32(hdr + 48);
    hasAudio = rd32(hdr + 56) >= 2;
    vidScale = rd32(hdr + 128);
    vidRate = rd32(hdr + 132);
    if (vidScale == 0 || vidRate == 0)
    {
        vidScale = 1;
        vidRate = 15;
    }

    // Audio format: find the 'auds' strh after the video stream list
    audioRate = 16000;
    if (hasAudio)
    {
        u32 strl1Size = rd32(hdr + 92);
        u32 a = 96 + strl1Size;                 // start of second LIST strl
        if (a + 64 < HEADER_SIZE && rd32(hdr + a + 12) == FCC('s', 't', 'r', 'h') &&
            rd32(hdr + a + 20) == FCC('a', 'u', 'd', 's'))
        {
            u32 r = rd32(hdr + a + 20 + 24);    // dwRate
            u32 bits = 0;
            u32 f = a + 12 + 8 + 56;            // strf chunk
            if (rd32(hdr + f) == FCC('s', 't', 'r', 'f'))
            {
                bits = hdr[f + 8 + 14] | (hdr[f + 8 + 15] << 8);
                if ((hdr[f + 8 + 2] | (hdr[f + 8 + 3] << 8)) != 1)
                    bits = 0;                   // only mono supported
            }
            if (r >= 4000 && r <= 48000 && bits == 16)
                audioRate = r;
            else
                hasAudio = false;
        }
        else
        {
            hasAudio = false;
        }
    }

    lseek(fd, HEADER_SIZE + 12, SEEK_SET);
    eof = false;
    vidIndex = 0;
    audSample = 0;
    qHead = qCount = 0;
    playVbl = 0;
    paused = false;
    return true;
}

static void restartVideo(void)
{
    lseek(fd, HEADER_SIZE + 12, SEEK_SET);
    eof = false;
    vidIndex = 0;
    audSample = 0;
    qHead = qCount = 0;
    playVbl = 0;
    resetAudio(0);
}

static void fill(void)
{
    u64 limit = clockSample() + audioRate * 3;   // keep up to ~3 s of audio ahead
    int cost = 0;
    for (int guard = 0; guard < 600 && cost < 2 && !eof && qCount < NQ; guard++)
    {
        if (hasAudio && audSample > limit)
            break;
        cost += readChunk();
    }
}

static void drawPlayUi(const char *name)
{
    u32 cur = (u32)(((u64)playVbl * VB_DEN) / VB_NUM);
    u32 total = (u32)(((u64)totalFrames * vidScale) / vidRate);
    printf("\x1b[0;0H");
    printf("        Video viewer            \n");
    printf("--------------------------------\n");
    printf("%-32.32s", name);
    printf("%2lu.%lu fps   audio: %-3s         \n", (unsigned long)(vidRate * 10 / vidScale / 10),
           (unsigned long)(vidRate * 10 / vidScale % 10), hasAudio ? "on" : "off");
    printf("--------------------------------\n");
    printf("%s  %02lu:%02lu / %02lu:%02lu         \n",
           paused ? "Paused " : (eof && qCount == 0 ? "Ended  " : "Playing"),
           (unsigned long)(cur / 60), (unsigned long)(cur % 60),
           (unsigned long)(total / 60), (unsigned long)(total % 60));

    // progress bar
    char bar[31];
    int p = total ? (int)((u64)cur * 30 / total) : 0;
    if (p > 30)
        p = 30;
    for (int i = 0; i < 30; i++)
        bar[i] = i < p ? '=' : '-';
    bar[30] = '\0';
    printf("[%s]\n", bar);

    printf("\x1b[16;0H");
    printf("--------------------------------\n");
    printf("A       Pause / play            \n");
    printf("R       Skip ahead 5 s          \n");
    printf("L       Restart                 \n");
    printf("B       Back to list            \n");
}

static void playFile(const char *name, u16 *vram)
{
    char path[64];
    snprintf(path, sizeof(path), REC_DIR "/%s", name);

    consoleClear();
    if (!openVideo(path))
    {
        printf("Can't play %s\n\n(not a DSi Recorder video?)\n\nPress B", name);
        while (1)
        {
            swiWaitForVBlank();
            scanKeys();
            if (keysDown() & (KEY_B | KEY_A))
                return;
        }
    }

    memset(vram, 0, 256 * 192 * 2);
    resetAudio(0);

    // Prefill before starting the clock
    for (int i = 0; i < 2000 && !eof && qCount < NQ; i++)
    {
        if (hasAudio && audSample > audioRate / 2 && qCount > 0)
            break;
        readChunk();
    }

    u32 lastVbl = vblankCount;
    int uiTimer = 0;

    while (1)
    {
        swiWaitForVBlank();
        scanKeys();
        u32 down = keysDown();

        u32 now = vblankCount;
        if (!paused)
            playVbl += now - lastVbl;
        lastVbl = now;

        if (down & KEY_B)
            break;

        if (down & KEY_A)
        {
            if (eof && qCount == 0)
            {
                restartVideo();                 // ended: A plays again
            }
            else
            {
                paused = !paused;
                if (sndId >= 0)
                {
                    if (paused)
                        soundPause(sndId);
                    else
                        soundResume(sndId);
                }
            }
            uiTimer = 0;
        }
        if (down & KEY_L)
        {
            restartVideo();
            uiTimer = 0;
        }
        if ((down & KEY_R) && !(eof && qCount == 0))
        {
            playVbl += (u32)((5ULL * VB_NUM) / VB_DEN);
            resetAudio(audioRate / 4);
            uiTimer = 0;
        }

        // Start audio once the clock reaches the ring's start
        if (audioPending && !paused && clockSample() >= ringBase)
        {
            sndId = soundPlaySample(ring, SoundFormat_16Bit, RING_BYTES, audioRate, 127, 64,
                                    true, 0);
            audioPending = false;
        }

        // Show the newest due frame
        u32 due = dueFrame();
        int show = -1;
        while (qCount > 0 && qIdx[qHead] <= due)
        {
            show = qHead;
            qHead = (qHead + 1) % NQ;
            qCount--;
        }
        if (show >= 0)
            showFrame(q[show], vram);

        // Silence audio already played, so an underrun or the end doesn't loop old sound
        if (sndId >= 0 && !paused)
        {
            u64 played = clockSample();
            u64 safe = played > 1600 ? played - 1600 : 0;
            if (safe > zeroedTo + 2048)
            {
                for (u64 s = zeroedTo; s < safe; s++)
                    ring[(s - ringBase) % RING_SAMPLES] = 0;
                DC_FlushRange(ring, RING_BYTES);
                zeroedTo = safe;
            }
            if (eof && qCount == 0 && played > audSample + audioRate / 2)
                stopSound();
        }

        if (!paused)
            fill();

        if (--uiTimer <= 0)
        {
            drawPlayUi(name);
            uiTimer = 10;
        }
    }

    stopSound();
    close(fd);
    fd = -1;
    memset(vram, 0, 256 * 192 * 2);
}

// ---------------------------------------------------------------------------
// File list

static char (*names)[32];
static u32 *sizesKb;
static int numFiles;

static int cmpNames(const void *a, const void *b)
{
    return strcasecmp((const char *)b, (const char *)a);   // newest (highest number) first
}

static void scanFiles(void)
{
    numFiles = 0;
    DIR *d = opendir(REC_DIR);
    if (d == NULL)
        return;
    struct dirent *e;
    while ((e = readdir(d)) != NULL && numFiles < MAX_FILES)
    {
        size_t len = strlen(e->d_name);
        if (len < 5 || len >= 32 || strcasecmp(e->d_name + len - 4, ".AVI") != 0)
            continue;
        strcpy(names[numFiles], e->d_name);
        numFiles++;
    }
    closedir(d);
    qsort(names, numFiles, sizeof(names[0]), cmpNames);

    for (int i = 0; i < numFiles; i++)
    {
        char path[64];
        struct stat st;
        snprintf(path, sizeof(path), REC_DIR "/%s", names[i]);
        sizesKb[i] = stat(path, &st) == 0 ? (u32)(st.st_size / 1024) : 0;
    }
}

static void drawList(int sel, int top)
{
    printf("\x1b[0;0H");
    printf("    Video viewer - %3d files    \n", numFiles);
    printf("--------------------------------\n");
    for (int r = 0; r < LIST_ROWS; r++)
    {
        int i = top + r;
        if (i < numFiles)
        {
            u32 kb = sizesKb[i];
            char sz[12];
            if (kb >= 1024)
                snprintf(sz, sizeof(sz), "%lu.%lu MB", (unsigned long)(kb / 1024),
                         (unsigned long)((kb % 1024) * 10 / 1024));
            else
                snprintf(sz, sizeof(sz), "%lu KB", (unsigned long)kb);
            printf("%c %-18.18s %10s\n", i == sel ? '>' : ' ', names[i], sz);
        }
        else if (i == 0 && numFiles == 0)
        {
            printf("  No videos yet.                \n");
        }
        else
        {
            printf("                                \n");
        }
    }
    printf("--------------------------------\n");
    printf("UP/DOWN select   A play         \n");
    printf("B       back to camera          ");
}

void runViewer(u16 *vram)
{
    q[0] = NULL;
    for (int i = 0; i < NQ; i++)
        q[i] = malloc(FRAME_BYTES);
    tmp = malloc(TMP_BYTES);
    ring = memalign(32, RING_BYTES);
    names = malloc(MAX_FILES * sizeof(names[0]));
    sizesKb = malloc(MAX_FILES * sizeof(u32));

    bool ok = tmp && ring && names && sizesKb;
    for (int i = 0; i < NQ; i++)
        ok = ok && q[i];

    consoleClear();
    memset(vram, 0, 256 * 192 * 2);

    if (!ok)
    {
        printf("Not enough memory for viewer.\n\nPress B");
        while (1)
        {
            swiWaitForVBlank();
            scanKeys();
            if (keysDown() & KEY_B)
                break;
        }
    }
    else
    {
        scanFiles();
        int sel = 0, top = 0;
        bool redraw = true;
        while (1)
        {
            swiWaitForVBlank();
            scanKeys();
            u32 down = keysDown();
            u32 rep = keysDownRepeat();

            if (down & KEY_B)
                break;
            if ((rep & KEY_DOWN) && sel < numFiles - 1)
            {
                sel++;
                redraw = true;
            }
            if ((rep & KEY_UP) && sel > 0)
            {
                sel--;
                redraw = true;
            }
            if ((down & KEY_A) && numFiles > 0)
            {
                playFile(names[sel], vram);
                consoleClear();
                redraw = true;
            }

            if (sel < top)
                top = sel;
            if (sel >= top + LIST_ROWS)
                top = sel - LIST_ROWS + 1;

            if (redraw)
            {
                drawList(sel, top);
                redraw = false;
            }
        }
    }

    for (int i = 0; i < NQ; i++)
        free(q[i]);
    free(tmp);
    free(ring);
    free(names);
    free(sizesKb);
    consoleClear();
}
