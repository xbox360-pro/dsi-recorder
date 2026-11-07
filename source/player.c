// In-app video viewer for the AVIs written by DSi Recorder.
//
// Reads the 'movi' chunks sequentially (no index needed, so even files that
// weren't closed cleanly play), queues a few decoded frames ahead, and streams
// the PCM audio into a looping sound channel.
//
// Controls (list):    UP/DOWN select, A play, X delete, B back to camera
// Controls (playing): A pause/resume, R skip +5 s, L restart, Y snapshot, B stop
// L/R in the list switches between the Videos and Photos tabs.

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
#include "ui.h"
#include "photo.h"

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

static const UiBox pbRestart = { 8, 120, 56, 24 };
static const UiBox pbSkip = { 68, 120, 56, 24 };
static const UiBox pbPhoto = { 128, 120, 60, 24 };
static const UiBox pbBack = { 192, 120, 56, 24 };

static char snapMsg[32];
static u32 snapMsgUntil;

static const UiBox pbPlay = { 8, 148, 240, 32 };

static bool ended(void)
{
    return eof && qCount == 0;
}

static void drawPlayUi(const char *name, int pressedBtn)
{
    u32 cur = (u32)(((u64)playVbl * VB_DEN) / VB_NUM);
    u32 total = (u32)(((u64)totalFrames * vidScale) / vidRate);
    if (total && cur > total)
        cur = total;

    uiBegin();
    uiHeader("Now playing");

    uiCard(8, 32, 240, 48, COL_CARD, COL_CARD_EDGE);
    uiText(2, 5, TXT_WHITE, "%.28s", name);
    uiText(2, 6, TXT_MUTED, "%lu.%lu fps", (unsigned long)(vidRate * 10 / vidScale / 10),
           (unsigned long)(vidRate * 10 / vidScale % 10));
    uiText(14, 6, hasAudio ? TXT_GREEN : TXT_MUTED, "%s", hasAudio ? "with sound" : "no sound");
    if (snapMsg[0] && vblankCount < snapMsgUntil)
        uiText(2, 8, TXT_GREEN, "%.28s", snapMsg);
    else if (paused)
        uiText(2, 8, TXT_AMBER, "Paused  (Y = take photo)");
    else if (ended())
        uiText(2, 8, TXT_ACCENT, "Finished");
    else
    {
        uiCircle(19, 68, 3, COL_GREEN);
        uiText(3, 8, TXT_GREEN, "Playing");
    }

    // Progress
    uiCard(8, 84, 240, 32, COL_CARD, COL_CARD_EDGE);
    uiText(2, 11, TXT_WHITE, "%02lu:%02lu", (unsigned long)(cur / 60), (unsigned long)(cur % 60));
    if (total)
        uiText(25, 11, TXT_MUTED, "%02lu:%02lu", (unsigned long)(total / 60),
               (unsigned long)(total % 60));
    int pos = total ? (int)((u64)cur * 1000 / total) : (ended() ? 1000 : 0);
    uiSlider(24, 104, 208, pos, true, COL_ACCENT);

    uiButton(pbRestart, "Start", pressedBtn == 1 ? 1 : 0);
    uiButton(pbSkip, "+5s", ended() ? 2 : (pressedBtn == 2 ? 1 : 0));
    uiButton(pbPhoto, "Photo", pressedBtn == 5 ? 1 : 0);
    uiButton(pbBack, "Back", pressedBtn == 3 ? 1 : 0);

    // Big play / pause button
    uiCard(pbPlay.x, pbPlay.y, pbPlay.w, pbPlay.h, pressedBtn == 4 ? COL_PRESSED : UIC(5, 14, 22),
           COL_ACCENT);
    if (paused || ended())
    {
        uiTriangleRight(96, 157, 15, COL_WHITE);
        uiText(14, 20, TXT_WHITE, "%s", ended() ? "REPLAY" : "PLAY");
    }
    else
    {
        uiRect(94, 158, 4, 13, COL_WHITE);
        uiRect(101, 158, 4, 13, COL_WHITE);
        uiText(14, 20, TXT_WHITE, "PAUSE");
    }

    uiText(1, 23, TXT_MUTED, "A pause  Y photo  R +5s  L top");
    uiPresent();
}

static void playFile(const char *name, u16 *vram)
{
    char path[64];
    snprintf(path, sizeof(path), REC_DIR "/%s", name);

    if (!openVideo(path))
    {
        uiBegin();
        uiHeader("Now playing");
        uiCard(8, 40, 240, 80, COL_CARD, COL_CARD_EDGE);
        uiText(3, 7, TXT_AMBER, "Can't play this file");
        uiText(3, 9, TXT_WHITE, "%.26s", name);
        uiText(3, 11, TXT_MUTED, "Not a DSi Recorder video?");
        uiText(3, 13, TXT_MUTED, "Tap or press B to go back");
        uiPresent();
        while (1)
        {
            swiWaitForVBlank();
            scanKeys();
            if (keysDown() & (KEY_B | KEY_A | KEY_TOUCH))
                return;
        }
    }

    memset(vram, 0, 256 * 192 * 2);
    snapMsg[0] = 0;
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
    int pressedBtn = 0;

    while (1)
    {
        swiWaitForVBlank();
        scanKeys();
        u32 down = keysDown();

        // Touch buttons: highlight on press, act on release
        int action = 0;
        if (down & KEY_TOUCH)
        {
            touchPosition tp;
            touchRead(&tp);
            pressedBtn = uiHit(pbRestart, tp.px, tp.py) ? 1
                       : uiHit(pbSkip, tp.px, tp.py)    ? 2
                       : uiHit(pbBack, tp.px, tp.py)    ? 3
                       : uiHit(pbPlay, tp.px, tp.py)    ? 4
                       : uiHit(pbPhoto, tp.px, tp.py)   ? 5
                                                        : 0;
            uiTimer = 0;
        }
        if (keysUp() & KEY_TOUCH)
        {
            action = pressedBtn;
            pressedBtn = 0;
            uiTimer = 0;
        }
        if (action == 1)
            down |= KEY_L;
        if (action == 2)
            down |= KEY_R;
        if (action == 3)
            down |= KEY_B;
        if (action == 4)
            down |= KEY_A;
        if (action == 5)
            down |= KEY_Y;

        u32 now = vblankCount;
        if (!paused)
            playVbl += now - lastVbl;
        lastVbl = now;

        if (down & KEY_B)
            break;

        // Snapshot: pause on the frame and save it as a photo
        if (down & KEY_Y)
        {
            if (!paused && !ended())
            {
                paused = true;
                if (sndId >= 0)
                    soundPause(sndId);
            }
            char shot[16];
            if (photoSave(vram, 256, shot))
                snprintf(snapMsg, sizeof(snapMsg), "Saved %s to %.10s", shot,
                         photoAlbumLabel(photoSaveDir()));
            else
                snprintf(snapMsg, sizeof(snapMsg), "Couldn't save photo!");
            snapMsgUntil = vblankCount + 180;           // show for ~3 s
            uiTimer = 0;
        }

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
            drawPlayUi(name, pressedBtn);
            uiTimer = 15;
        }
    }

    stopSound();
    close(fd);
    fd = -1;
    memset(vram, 0, 256 * 192 * 2);
}

// ---------------------------------------------------------------------------
// Gallery
//
// Videos tab: every AVI in sd:/DSiRecorder.
// Photos tab: albums (folders in sd:/DCIM, plus Pictures/Photos/Images and the
// DSi Camera's own photos on the NAND, read only) -> the pictures inside.

typedef struct
{
    char path[96];
    u32 kb;
    u32 mtime;
    bool system;          // on the NAND: read only
} Entry;

typedef struct
{
    char path[64];
    int depth;            // how many subfolder levels hold its pictures
    int count;
    bool system;
} Album;

#define MAX_ALBUMS  48
#define NAME_MAX_LEN 16

static Entry *ents;
static int numFiles;
static Album *albums;
static int numAlbums;
static int tab;            // 0 = videos, 1 = photos
static int curAlbum = -1;  // photos tab: -1 = album list, otherwise an album index

static char note[32];
static u32 noteUntil;

static void setNote(const char *fmt, const char *arg)
{
    snprintf(note, sizeof(note), fmt, arg);
    noteUntil = vblankCount + 180;
}

static const char *baseName(const char *p)
{
    const char *s = strrchr(p, '/');
    return s ? s + 1 : p;
}

static bool isSystemPath(const char *p)
{
    return strncmp(p, NAND_ALBUM, sizeof(NAND_ALBUM) - 1) == 0;
}

static bool wantFile(const char *name, bool images)
{
    if (name[0] == '.')
        return false;
    if (images)
        return photoIsImage(name);
    size_t n = strlen(name);
    return n > 4 && strcasecmp(name + n - 4, ".avi") == 0;
}

static int cmpEnt(const void *a, const void *b)
{
    const Entry *x = a, *y = b;
    if (x->mtime != y->mtime)
        return x->mtime < y->mtime ? 1 : -1;        // newest first
    return strcasecmp(y->path, x->path);
}

static void addFiles(const char *dir, bool images, int depth)
{
    DIR *d = opendir(dir);
    if (d == NULL)
        return;
    struct dirent *e;
    while ((e = readdir(d)) != NULL && numFiles < MAX_FILES)
    {
        if (e->d_name[0] == '.')
            continue;
        char p[96];
        if (snprintf(p, sizeof(p), "%s/%s", dir, e->d_name) >= (int)sizeof(p))
            continue;
        if (e->d_type == DT_DIR)
        {
            if (depth > 0)
                addFiles(p, images, depth - 1);
            continue;
        }
        if (!wantFile(e->d_name, images))
            continue;
        Entry *en = &ents[numFiles++];
        strcpy(en->path, p);
        en->kb = 0;
        en->mtime = 0;
        en->system = isSystemPath(p);
        struct stat st;
        if (stat(p, &st) == 0)
        {
            en->kb = (u32)(st.st_size / 1024);
            en->mtime = (u32)st.st_mtime;
        }
    }
    closedir(d);
}

static int countImages(const char *dir, int depth)
{
    DIR *d = opendir(dir);
    if (d == NULL)
        return -1;
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL)
    {
        if (e->d_name[0] == '.')
            continue;
        if (e->d_type == DT_DIR)
        {
            if (depth > 0)
            {
                char p[96];
                snprintf(p, sizeof(p), "%s/%s", dir, e->d_name);
                int c = countImages(p, depth - 1);
                if (c > 0)
                    n += c;
            }
        }
        else if (photoIsImage(e->d_name))
        {
            n++;
        }
    }
    closedir(d);
    return n;
}

static void addAlbum(const char *path, int depth, bool needPictures)
{
    if (numAlbums >= MAX_ALBUMS)
        return;
    int c = countImages(path, depth);
    if (c < 0 || (needPictures && c == 0))
        return;
    Album *a = &albums[numAlbums++];
    snprintf(a->path, sizeof(a->path), "%s", path);
    a->depth = depth;
    a->count = c;
    a->system = isSystemPath(path);
}

static int cmpAlbum(const void *a, const void *b)
{
    return strcasecmp(baseName(((const Album *)a)->path), baseName(((const Album *)b)->path));
}

static void scanAlbums(void)
{
    numAlbums = 0;
    mkdir(ALBUM_ROOT, 0777);
    mkdir(PHOTO_DIR, 0777);
    addAlbum(PHOTO_DIR, 0, false);                     // "Camera roll" always first

    int first = numAlbums;
    DIR *d = opendir(ALBUM_ROOT);
    if (d)
    {
        struct dirent *e;
        while ((e = readdir(d)) != NULL)
        {
            if (e->d_name[0] == '.' || e->d_type != DT_DIR)
                continue;
            char p[64];
            snprintf(p, sizeof(p), ALBUM_ROOT "/%s", e->d_name);
            if (strcasecmp(p, PHOTO_DIR) != 0)
                addAlbum(p, 0, false);
        }
        closedir(d);
    }
    qsort(albums + first, numAlbums - first, sizeof(Album), cmpAlbum);

    addAlbum(ALBUM_ROOT, 0, true);                     // loose pictures in DCIM
    addAlbum("/Pictures", 0, false);
    addAlbum("/Photos", 0, false);
    addAlbum("/Images", 0, false);
    addAlbum(NAND_ALBUM, 1, true);                     // DSi Camera photos
}

static void scanFiles(void)
{
    numFiles = 0;
    if (tab == 0)
        addFiles(REC_DIR, false, 0);
    else if (curAlbum >= 0)
        addFiles(albums[curAlbum].path, true, albums[curAlbum].depth);
    qsort(ents, numFiles, sizeof(Entry), cmpEnt);
}

static const char *albumLabel(int i)
{
    if (strcmp(albums[i].path, ALBUM_ROOT) == 0)
        return "DCIM (loose)";
    return photoAlbumLabel(albums[i].path);
}

static bool canBeCameraAlbum(const char *path)
{
    return strncmp(path, ALBUM_ROOT "/", sizeof(ALBUM_ROOT)) == 0;
}

// ---------------------------------------------------------------------------
// On-screen keyboard (for album names)

static const char *kbRows[4] = { "1234567890", "QWERTYUIOP", "ASDFGHJKL-", "ZXCVBNM_" };
static const UiBox kbShift = { 8, 153, 36, 22 };
static const UiBox kbSpace = { 48, 153, 92, 22 };
static const UiBox kbBack = { 144, 153, 44, 22 };
static const UiBox kbOk = { 192, 153, 56, 22 };

static UiBox kbKey(int r, int c)
{
    int n = (int)strlen(kbRows[r]);
    int x0 = 8 + (10 - n) * 12;                        // centre shorter rows
    UiBox b = { x0 + c * 24, 57 + r * 24, 22, 22 };
    return b;
}

static void drawKeyboard(const char *title, const char *text, bool lower, int pr, int pc)
{
    uiBegin();
    uiHeader(title);
    uiCard(8, 30, 240, 22, COL_CARD, COL_ACCENT);
    uiText(2, 5, TXT_WHITE, "%s%s", text, ((vblankCount / 30) & 1) ? "_" : " ");
    for (int r = 0; r < 4; r++)
    {
        for (int c = 0; kbRows[r][c]; c++)
        {
            char s[2] = { kbRows[r][c], 0 };
            if (lower && s[0] >= 'A' && s[0] <= 'Z')
                s[0] += 'a' - 'A';
            uiButton(kbKey(r, c), s, (pr == r && pc == c) ? 1 : 0);
        }
    }
    uiButton(kbShift, lower ? "abc" : "ABC", pr == 4 && pc == 0 ? 1 : 0);
    uiButton(kbSpace, "space", pr == 4 && pc == 1 ? 1 : 0);
    uiButton(kbBack, "<-", pr == 4 && pc == 2 ? 1 : 0);
    uiButton(kbOk, "OK", text[0] ? (pr == 4 && pc == 3 ? 1 : 0) : 2);
    uiText(1, 23, TXT_MUTED, "A/START ok  Y erase  B cancel");
    uiPresent();
}

// Returns false if cancelled. 'out' must hold NAME_MAX_LEN + 1 bytes.
static bool kbdInput(const char *title, char *out)
{
    int len = 0;
    out[0] = 0;
    bool lower = true;
    int pr = -1, pc = -1;
    int redraw = 0;
    while (1)
    {
        if (--redraw <= 0)
        {
            drawKeyboard(title, out, lower, pr, pc);
            redraw = 15;
        }
        swiWaitForVBlank();
        scanKeys();
        u32 down = keysDown();
        if (down & KEY_TOUCH)
        {
            touchPosition tp;
            touchRead(&tp);
            pr = pc = -1;
            for (int r = 0; r < 4 && pr < 0; r++)
                for (int c = 0; kbRows[r][c]; c++)
                    if (uiHit(kbKey(r, c), tp.px, tp.py))
                    {
                        pr = r;
                        pc = c;
                        break;
                    }
            if (pr < 0)
            {
                pc = uiHit(kbShift, tp.px, tp.py) ? 0 : uiHit(kbSpace, tp.px, tp.py) ? 1
                   : uiHit(kbBack, tp.px, tp.py)  ? 2 : uiHit(kbOk, tp.px, tp.py)    ? 3 : -1;
                pr = pc >= 0 ? 4 : -1;
            }
            redraw = 0;
        }
        if (keysUp() & KEY_TOUCH)
        {
            char ch = 0;
            if (pr >= 0 && pr < 4)
            {
                ch = kbRows[pr][pc];
                if (lower && ch >= 'A' && ch <= 'Z')
                    ch += 'a' - 'A';
            }
            else if (pr == 4 && pc == 0)
                lower = !lower;
            else if (pr == 4 && pc == 1)
                ch = ' ';
            else if (pr == 4 && pc == 2)
                down |= KEY_Y;
            else if (pr == 4 && pc == 3)
                down |= KEY_A;
            if (ch && len < NAME_MAX_LEN && !(ch == ' ' && len == 0))
            {
                out[len++] = ch;
                out[len] = 0;
            }
            pr = pc = -1;
            redraw = 0;
        }
        if ((down & KEY_Y) && len > 0)
        {
            out[--len] = 0;
            redraw = 0;
        }
        if (down & KEY_B)
            return false;
        if ((down & (KEY_A | KEY_START)) && len > 0)
        {
            while (len > 0 && out[len - 1] == ' ')
                out[--len] = 0;                        // no trailing spaces
            return len > 0;
        }
    }
}

// ---------------------------------------------------------------------------
// Dialogs

static const UiBox dlgCancel = { 32, 120, 88, 24 };
static const UiBox dlgDelete = { 136, 120, 88, 24 };

static void drawConfirm(const char *question, const char *name, const char *extra, int pressedBtn)
{
    uiBegin();
    uiHeader("Gallery");
    uiCard(16, 40, 224, 112, COL_CARD, COL_RED_EDGE);
    uiCircle(36, 60, 7, COL_RED);
    uiRect(35, 55, 2, 6, COL_WHITE);
    uiRect(35, 63, 2, 2, COL_WHITE);
    uiText(6, 7, TXT_WHITE, "%s", question);
    uiText(4, 9, TXT_AMBER, "%.24s", name);
    uiText(4, 11, TXT_MUTED, "%s", extra ? extra : "This can't be undone.");

    uiButton(dlgCancel, "Cancel", pressedBtn == 1 ? 1 : 0);
    bool down = pressedBtn == 2;
    uiCard(dlgDelete.x, dlgDelete.y, dlgDelete.w, dlgDelete.h, down ? COL_RED_DARK : COL_RED,
           COL_RED_EDGE);
    uiTextCenter(dlgDelete.x + dlgDelete.w / 2, 16, TXT_WHITE, "Delete");

    uiText(1, 23, TXT_MUTED, "A delete   B cancel");
    uiPresent();
}

static bool confirmDelete(const char *question, const char *name, const char *extra)
{
    int pressedBtn = 0;
    drawConfirm(question, name, extra, 0);
    while (1)
    {
        swiWaitForVBlank();
        scanKeys();
        u32 down = keysDown();
        if (down & KEY_TOUCH)
        {
            touchPosition tp;
            touchRead(&tp);
            pressedBtn = uiHit(dlgCancel, tp.px, tp.py) ? 1 : uiHit(dlgDelete, tp.px, tp.py) ? 2 : 0;
            drawConfirm(question, name, extra, pressedBtn);
        }
        if (keysUp() & KEY_TOUCH)
        {
            if (pressedBtn == 1)
                down |= KEY_B;
            if (pressedBtn == 2)
                down |= KEY_A;
            pressedBtn = 0;
        }
        if (down & KEY_A)
            return true;
        if (down & KEY_B)
            return false;
    }
}

// ---------------------------------------------------------------------------
// Album operations

static bool validName(const char *s)
{
    for (; *s; s++)
        if (strchr("\\/:*?\"<>|", *s))
            return false;
    return true;
}

// Creates sd:/DCIM/<name>. Returns its index in albums[] or -1.
static int newAlbum(void)
{
    char name[NAME_MAX_LEN + 1];
    if (!kbdInput("New album", name))
        return -1;
    char p[64];
    snprintf(p, sizeof(p), ALBUM_ROOT "/%s", name);
    struct stat st;
    if (!validName(name) || (stat(p, &st) != 0 && mkdir(p, 0777) != 0))
    {
        setNote("Couldn't make %.18s", name);
        return -1;
    }
    scanAlbums();
    for (int i = 0; i < numAlbums; i++)
        if (strcasecmp(albums[i].path, p) == 0)
            return i;
    return -1;
}

static bool copyFile(const char *src, const char *dst)
{
    int in = open(src, O_RDONLY);
    if (in < 0)
        return false;
    int out = open(dst, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (out < 0)
    {
        close(in);
        return false;
    }
    bool ok = true;
    ssize_t n;
    while ((n = read(in, tmp, TMP_BYTES)) > 0)
        if (write(out, tmp, n) != n)
        {
            ok = false;
            break;
        }
    if (n < 0)
        ok = false;
    close(in);
    close(out);
    if (!ok)
        unlink(dst);
    return ok;
}

// Moves (or for NAND photos, copies) a picture into another album
static bool movePicture(const char *src, const char *dstDir, bool copyOnly)
{
    const char *name = baseName(src);
    const char *dot = strrchr(name, '.');
    int stem = dot ? (int)(dot - name) : (int)strlen(name);
    char dst[128];
    struct stat st;
    snprintf(dst, sizeof(dst), "%s/%s", dstDir, name);
    for (int k = 2; stat(dst, &st) == 0 && k < 1000; k++)
        snprintf(dst, sizeof(dst), "%s/%.*s_%d%s", dstDir, stem, name, k, dot ? dot : "");

    if (!copyOnly && rename(src, dst) == 0)
        return true;
    if (!copyFile(src, dst))
        return false;
    if (!copyOnly)
        unlink(src);
    return true;
}

static bool deleteAlbum(int i)
{
    // delete the pictures and anything else directly inside, then the folder
    DIR *d = opendir(albums[i].path);
    if (d)
    {
        struct dirent *e;
        char p[96];
        while ((e = readdir(d)) != NULL)
        {
            if (e->d_type == DT_DIR)
                continue;
            snprintf(p, sizeof(p), "%s/%s", albums[i].path, e->d_name);
            unlink(p);
        }
        closedir(d);
    }
    bool ok = rmdir(albums[i].path) == 0;
    if (strcasecmp(photoSaveDir(), albums[i].path) == 0)
        photoSetSaveDir(PHOTO_DIR);
    return ok;
}

// ---------------------------------------------------------------------------
// Lists

#define ITEM_Y0  32
#define ITEM_H   24
#define ITEMS    5
#define ROW_W    226

static const UiBox lbBack = { 8, 160, 44, 24 };
static const UiBox lbUp = { 56, 160, 44, 24 };
static const UiBox lbDown = { 104, 160, 44, 24 };
static const UiBox lbDelete = { 152, 160, 44, 24 };
static const UiBox lbPlay = { 200, 160, 48, 24 };
// album list / photo list use the same five slots with other labels

static const UiBox scrollUp = { 238, 32, 18, 58 };
static const UiBox scrollDown = { 238, 92, 18, 58 };

static const UiBox tabVideos = { 136, 4, 54, 15 };
static const UiBox tabPhotos = { 192, 4, 56, 15 };

static void arrow(int cx, int cy, bool up, u16 c)
{
    for (int i = 0; i < 5; i++)
        uiRect(cx - i, up ? cy + i : cy - i, 2 * i + 1, 1, c);
}

// What the list currently shows
static int listCount(void)
{
    return (tab == 1 && curAlbum < 0) ? numAlbums : numFiles;
}

static void drawList(int sel, int top, int pressedBtn, const char *pickTitle)
{
    bool albumMode = tab == 1 && curAlbum < 0;
    int count = listCount();

    uiBegin();
    if (pickTitle)
    {
        uiHeader(pickTitle);
    }
    else if (tab == 1 && curAlbum >= 0)
    {
        uiHeader(albumLabel(curAlbum));
        uiText(24, 1, TXT_MUTED, "%3d pics", numFiles);
    }
    else
    {
        uiHeader("Gallery");
        uiRound(tabVideos.x, tabVideos.y, tabVideos.w, tabVideos.h, 7, tab == 0 ? COL_ACCENT : COL_BUTTON);
        uiRound(tabPhotos.x, tabPhotos.y, tabPhotos.w, tabPhotos.h, 7, tab == 1 ? COL_ACCENT : COL_BUTTON);
        uiText(18, 1, tab == 0 ? TXT_DARK : TXT_MUTED, "Videos");
        uiText(25, 1, tab == 1 ? TXT_DARK : TXT_MUTED, "Photos");
    }

    if (count == 0)
    {
        uiCard(8, 40, 240, 80, COL_CARD, COL_CARD_EDGE);
        if (tab == 0)
        {
            uiText(3, 8, TXT_WHITE, "No videos yet");
            uiText(3, 10, TXT_MUTED, "Record something first, then");
            uiText(3, 11, TXT_MUTED, "it will show up here.");
        }
        else
        {
            uiText(3, 8, TXT_WHITE, "No photos in this album");
            uiText(3, 10, TXT_MUTED, "Take photos with PHOTO / Y,");
            uiText(3, 11, TXT_MUTED, "or copy pictures into");
            uiText(3, 12, TXT_MUTED, "sd:/DCIM/<album name>");
        }
    }

    const char *camDir = photoSaveDir();
    for (int r = 0; r < ITEMS; r++)
    {
        int i = top + r;
        if (i >= count)
            break;
        int y = ITEM_Y0 + r * ITEM_H;
        int row = (y + ITEM_H / 2 - 4) / 8;
        bool s = i == sel;
        uiCard(8, y + 1, ROW_W, ITEM_H - 2, s ? UIC(6, 15, 23) : COL_CARD, s ? COL_ACCENT : COL_CARD_EDGE);

        if (albumMode)
        {
            bool cam = strcasecmp(albums[i].path, camDir) == 0;
            // little folder icon
            uiRect(15, y + 8, 6, 2, cam ? COL_GREEN : COL_ACCENT);
            uiRect(15, y + 10, 11, 7, cam ? COL_GREEN : COL_ACCENT);
            uiText(4, row, cam ? TXT_GREEN : (albums[i].system ? TXT_AMBER : TXT_WHITE), "%.16s",
                   albumLabel(i));
            uiText(21, row, s ? TXT_WHITE : TXT_MUTED, "%4d pic", albums[i].count);
        }
        else
        {
            if (s)
                uiTriangleRight(16, y + 8, 9, COL_WHITE);
            else
                uiCircle(19, y + 12, 2, COL_CARD_EDGE);
            u32 kb = ents[i].kb;
            uiText(4, row, TXT_WHITE, "%.15s", baseName(ents[i].path));
            if (kb >= 1024)
                uiText(19, row, s ? TXT_WHITE : TXT_MUTED, "%5lu.%lu MB", (unsigned long)(kb / 1024),
                       (unsigned long)((kb % 1024) * 10 / 1024));
            else
                uiText(19, row, s ? TXT_WHITE : TXT_MUTED, "%7lu KB", (unsigned long)kb);
        }
    }

    // scroll arrows (tap to page)
    if (count > ITEMS)
    {
        uiRound(scrollUp.x + 2, scrollUp.y, 14, scrollUp.h, 4, top > 0 ? COL_BUTTON : COL_DISABLED);
        uiRound(scrollDown.x + 2, scrollDown.y, 14, scrollDown.h, 4,
                top + ITEMS < count ? COL_BUTTON : COL_DISABLED);
        arrow(247, 57, true, COL_WHITE);
        arrow(247, 125, false, COL_WHITE);
    }

    bool any = count > 0;
    bool sysSel = any && (albumMode ? albums[sel].system : (tab == 1 && ents[sel].system));
    uiButton(lbBack, "Back", pressedBtn == 1 ? 1 : 0);
    if (pickTitle)
    {
        uiButton(lbUp, "New", pressedBtn == 2 ? 1 : 0);
        uiButton(lbPlay, "Here", !any ? 2 : (pressedBtn == 4 ? 1 : 0));
    }
    else if (albumMode)
    {
        uiButton(lbUp, "New", pressedBtn == 2 ? 1 : 0);
        uiButton(lbDown, "Cam", !any || !canBeCameraAlbum(albums[sel].path) ? 2 : (pressedBtn == 3 ? 1 : 0));
        uiButton(lbDelete, "Del", !any || sysSel || sel == 0 ? 2 : (pressedBtn == 5 ? 1 : 0));
        uiButton(lbPlay, "Open", !any ? 2 : (pressedBtn == 4 ? 1 : 0));
    }
    else if (tab == 1)
    {
        uiButton(lbUp, sysSel ? "Copy" : "Move", !any ? 2 : (pressedBtn == 2 ? 1 : 0));
        uiButton(lbDown, "Cam", !canBeCameraAlbum(albums[curAlbum].path) ? 2 : (pressedBtn == 3 ? 1 : 0));
        uiButton(lbDelete, "Del", !any || sysSel ? 2 : (pressedBtn == 5 ? 1 : 0));
        uiButton(lbPlay, "View", !any ? 2 : (pressedBtn == 4 ? 1 : 0));
    }
    else
    {
        uiButton(lbUp, "Up", !any || sel == 0 ? 2 : (pressedBtn == 2 ? 1 : 0));
        uiButton(lbDown, "Down", !any || sel >= count - 1 ? 2 : (pressedBtn == 3 ? 1 : 0));
        uiButton(lbDelete, "Del", !any ? 2 : (pressedBtn == 5 ? 1 : 0));
        uiButton(lbPlay, "Play", !any ? 2 : (pressedBtn == 4 ? 1 : 0));
    }

    if (note[0] && vblankCount < noteUntil)
        uiText(1, 23, TXT_GREEN, "%.30s", note);
    else if (pickTitle)
        uiText(1, 23, TXT_MUTED, "A pick album   B cancel");
    else if (albumMode)
        uiText(1, 23, TXT_MUTED, "Green = camera album  L/R tab");
    else if (tab == 1)
        uiText(1, 23, TXT_MUTED, "Y move  X delete  B albums");
    else
        uiText(1, 23, TXT_MUTED, "%d %s   L/R switch tab", numFiles, numFiles == 1 ? "video" : "videos");
    uiPresent();
}

// Shared touch / key handling for the lists. Returns the "virtual" keys.
static u32 listInput(int *sel, int *top, int *pressedBtn, bool *redraw)
{
    u32 down = keysDown();
    u32 rep = keysDownRepeat();
    int count = listCount();

    if (down & KEY_TOUCH)
    {
        touchPosition tp;
        touchRead(&tp);
        *pressedBtn = uiHit(lbBack, tp.px, tp.py)   ? 1
                    : uiHit(lbUp, tp.px, tp.py)     ? 2
                    : uiHit(lbDown, tp.px, tp.py)   ? 3
                    : uiHit(lbPlay, tp.px, tp.py)   ? 4
                    : uiHit(lbDelete, tp.px, tp.py) ? 5
                                                    : 0;
        if (count > ITEMS && uiHit(scrollUp, tp.px, tp.py))
        {
            *sel = *sel - ITEMS < 0 ? 0 : *sel - ITEMS;
            *top = *top - ITEMS < 0 ? 0 : *top - ITEMS;
        }
        else if (count > ITEMS && uiHit(scrollDown, tp.px, tp.py))
        {
            *sel = *sel + ITEMS >= count ? count - 1 : *sel + ITEMS;
            *top = *top + ITEMS > count - ITEMS ? count - ITEMS : *top + ITEMS;
        }
        else if (uiHit(tabVideos, tp.px, tp.py) || uiHit(tabPhotos, tp.px, tp.py))
        {
            int want = uiHit(tabPhotos, tp.px, tp.py) ? 1 : 0;
            if (want != tab)
                down |= want ? KEY_R : KEY_L;
        }
        else if (*pressedBtn == 0 && tp.px < 8 + ROW_W && tp.py >= ITEM_Y0 &&
                 tp.py < ITEM_Y0 + ITEMS * ITEM_H)
        {
            int i = *top + (tp.py - ITEM_Y0) / ITEM_H;
            if (i < count)
            {
                if (i == *sel)
                    down |= KEY_A;
                *sel = i;
            }
        }
        *redraw = true;
    }
    if (keysUp() & KEY_TOUCH)
    {
        // slots: 1 Back, 2 Up/New/Move, 3 Down/Cam, 4 Play/Open/View, 5 Del
        bool plain = tab == 0;
        if (*pressedBtn == 1) down |= KEY_B;
        if (*pressedBtn == 2) { if (plain) rep |= KEY_UP; else down |= KEY_Y; }
        if (*pressedBtn == 3) { if (plain) rep |= KEY_DOWN; else down |= KEY_START; }
        if (*pressedBtn == 4) down |= KEY_A;
        if (*pressedBtn == 5) down |= KEY_X;
        *pressedBtn = 0;
        *redraw = true;
    }
    if ((rep & KEY_DOWN) && *sel < count - 1)
    {
        (*sel)++;
        *redraw = true;
    }
    if ((rep & KEY_UP) && *sel > 0)
    {
        (*sel)--;
        *redraw = true;
    }
    if (*sel >= count)
        *sel = count > 0 ? count - 1 : 0;
    if (*sel < *top)
        *top = *sel;
    if (*sel >= *top + ITEMS)
        *top = *sel - ITEMS + 1;
    if (note[0] && vblankCount >= noteUntil)
    {
        note[0] = 0;
        *redraw = true;
    }
    return down;
}

// "Move to..." album picker. Returns an album index, or -1.
static int pickAlbum(const char *title, int exclude)
{
    int saveTab = tab, saveAlbum = curAlbum;
    tab = 1;
    curAlbum = -1;
    int sel = 0, top = 0, pressedBtn = 0;
    bool redraw = true;
    int result = -1;
    while (1)
    {
        if (redraw)
        {
            drawList(sel, top, pressedBtn, title);
            redraw = false;
        }
        swiWaitForVBlank();
        scanKeys();
        u32 down = listInput(&sel, &top, &pressedBtn, &redraw);
        down &= ~(KEY_L | KEY_R | KEY_START);
        if (down & KEY_B)
            break;
        if (down & KEY_Y)                              // "New"
        {
            int n = newAlbum();
            if (n >= 0)
            {
                result = n;
                break;
            }
            redraw = true;
        }
        if ((down & KEY_A) && numAlbums > 0)
        {
            if (albums[sel].system || sel == exclude)
                setNote("Pick a different album%s", "");
            else
            {
                result = sel;
                break;
            }
            redraw = true;
        }
    }
    tab = saveTab;
    curAlbum = saveAlbum;
    return result;
}

// Move/copy ents[sel] to another album. Returns true if the list changed.
static bool doMove(int sel)
{
    char albumPath[64];
    snprintf(albumPath, sizeof(albumPath), "%s", albums[curAlbum].path);
    bool copy = ents[sel].system;
    int dst = pickAlbum(copy ? "Copy to..." : "Move to...", curAlbum);
    // the picker may have rescanned the albums: find ours again
    for (int i = 0; i < numAlbums; i++)
        if (strcmp(albums[i].path, albumPath) == 0)
            curAlbum = i;
    if (dst < 0)
        return false;
    char dstLabel[24];
    snprintf(dstLabel, sizeof(dstLabel), "%s", albumLabel(dst));
    if (movePicture(ents[sel].path, albums[dst].path, copy))
    {
        setNote(copy ? "Copied to %.18s" : "Moved to %.19s", dstLabel);
        albums[dst].count++;
        if (!copy)
            albums[curAlbum].count--;
    }
    else
    {
        setNote("Couldn't move to %.13s", dstLabel);
    }
    scanFiles();
    return true;
}

// ---------------------------------------------------------------------------
// Photo viewer

static const UiBox phBack = { 8, 148, 44, 32 };
static const UiBox phPrev = { 56, 148, 44, 32 };
static const UiBox phNext = { 104, 148, 44, 32 };
static const UiBox phMove = { 152, 148, 44, 32 };
static const UiBox phDel = { 200, 148, 48, 32 };

static void drawPhotoUi(int sel, bool ok, const char *err, int pressedBtn)
{
    uiBegin();
    uiHeader("Photo");
    uiText(23, 1, TXT_MUTED, "%3d / %-3d", sel + 1, numFiles);
    uiCard(8, 32, 240, 64, COL_CARD, COL_CARD_EDGE);
    uiText(2, 5, TXT_WHITE, "%.28s", baseName(ents[sel].path));
    uiText(2, 6, TXT_MUTED, "%lu KB", (unsigned long)ents[sel].kb);
    uiText(12, 6, ents[sel].system ? TXT_AMBER : TXT_ACCENT, "%.16s", albumLabel(curAlbum));
    if (note[0] && vblankCount < noteUntil)
        uiText(2, 8, TXT_GREEN, "%.28s", note);
    else if (ok)
        uiText(2, 8, TXT_MUTED, "Shown on the top screen");
    else
        uiText(2, 8, TXT_AMBER, "%.28s", err);
    if (ents[sel].system)
        uiText(2, 10, TXT_MUTED, "DSi Camera photo (read only)");

    bool sys = ents[sel].system;
    uiButton(phBack, "Back", pressedBtn == 1 ? 1 : 0);
    uiButton(phPrev, "Prev", sel <= 0 ? 2 : (pressedBtn == 2 ? 1 : 0));
    uiButton(phNext, "Next", sel >= numFiles - 1 ? 2 : (pressedBtn == 3 ? 1 : 0));
    uiButton(phMove, sys ? "Copy" : "Move", pressedBtn == 5 ? 1 : 0);
    uiButton(phDel, "Del", sys ? 2 : (pressedBtn == 4 ? 1 : 0));
    uiText(1, 23, TXT_MUTED, "L/R browse  Y move  X delete");
    uiPresent();
}

// Returns the selection to go back to in the list
static int viewPhotos(int sel, u16 *vram)
{
    int pressedBtn = 0;
    char err[32];
    bool ok = photoLoad(ents[sel].path, vram, err);
    drawPhotoUi(sel, ok, err, 0);
    bool noteShown = false;
    while (numFiles > 0)
    {
        swiWaitForVBlank();
        scanKeys();
        u32 down = keysDown();
        u32 rep = keysDownRepeat();
        if (down & KEY_TOUCH)
        {
            touchPosition tp;
            touchRead(&tp);
            pressedBtn = uiHit(phBack, tp.px, tp.py)   ? 1
                       : uiHit(phPrev, tp.px, tp.py)   ? 2
                       : uiHit(phNext, tp.px, tp.py)   ? 3
                       : uiHit(phDel, tp.px, tp.py)    ? 4
                       : uiHit(phMove, tp.px, tp.py)   ? 5
                                                       : 0;
            drawPhotoUi(sel, ok, err, pressedBtn);
        }
        if (keysUp() & KEY_TOUCH)
        {
            if (pressedBtn == 1) down |= KEY_B;
            if (pressedBtn == 2) rep |= KEY_LEFT;
            if (pressedBtn == 3) rep |= KEY_RIGHT;
            if (pressedBtn == 4) down |= KEY_X;
            if (pressedBtn == 5) down |= KEY_Y;
            pressedBtn = 0;
            drawPhotoUi(sel, ok, err, 0);
        }
        if (down & KEY_B)
            break;

        int old = sel;
        if ((rep & (KEY_RIGHT | KEY_DOWN | KEY_R)) && sel < numFiles - 1)
            sel++;
        if ((rep & (KEY_LEFT | KEY_UP | KEY_L)) && sel > 0)
            sel--;
        if ((down & KEY_X) && !ents[sel].system)
        {
            if (confirmDelete("Delete this photo?", baseName(ents[sel].path), NULL))
            {
                unlink(ents[sel].path);
                albums[curAlbum].count--;
                scanFiles();
                if (sel >= numFiles)
                    sel = numFiles > 0 ? numFiles - 1 : 0;
                if (numFiles == 0)
                    break;
            }
            old = -1;                                   // reload + redraw
        }
        if (down & KEY_Y)
        {
            char keep[96];
            snprintf(keep, sizeof(keep), "%s", ents[sel].path);
            doMove(sel);
            if (numFiles == 0)
                break;
            // stay on the same picture if it's still here, else the next one
            for (int i = 0; i < numFiles; i++)
                if (strcmp(ents[i].path, keep) == 0)
                    sel = i;
            if (sel >= numFiles)
                sel = numFiles - 1;
            old = -1;
            noteShown = true;
        }
        if (sel != old)
        {
            ok = photoLoad(ents[sel].path, vram, err);
            drawPhotoUi(sel, ok, err, 0);
        }
        else if (noteShown && vblankCount >= noteUntil)
        {
            noteShown = false;
            note[0] = 0;
            drawPhotoUi(sel, ok, err, 0);
        }
    }
    memset(vram, 0, 256 * 192 * 2);
    return sel;
}

// ---------------------------------------------------------------------------

void runViewer(u16 *vram)
{
    q[0] = NULL;
    for (int i = 0; i < NQ; i++)
        q[i] = malloc(FRAME_BYTES);
    tmp = malloc(TMP_BYTES);
    ring = memalign(32, RING_BYTES);
    ents = malloc(MAX_FILES * sizeof(Entry));
    albums = malloc(MAX_ALBUMS * sizeof(Album));

    bool ok = tmp && ring && ents && albums;
    for (int i = 0; i < NQ; i++)
        ok = ok && q[i];

    memset(vram, 0, 256 * 192 * 2);

    if (!ok)
    {
        uiBegin();
        uiHeader("Gallery");
        uiText(3, 8, TXT_AMBER, "Not enough memory for viewer");
        uiText(3, 10, TXT_MUTED, "Press B to go back");
        uiPresent();
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
        tab = 0;
        curAlbum = -1;
        note[0] = 0;
        numAlbums = 0;
        scanFiles();
        int sel = 0, top = 0;
        int albumSel = 0;                              // remembered when opening an album
        bool redraw = true;
        int pressedBtn = 0;
        while (1)
        {
            if (redraw)
            {
                drawList(sel, top, pressedBtn, NULL);
                redraw = false;
            }
            swiWaitForVBlank();
            scanKeys();
            u32 down = listInput(&sel, &top, &pressedBtn, &redraw);
            bool albumMode = tab == 1 && curAlbum < 0;
            int count = listCount();

            if (down & KEY_B)
            {
                if (tab == 1 && curAlbum >= 0)          // album -> album list
                {
                    curAlbum = -1;
                    scanAlbums();
                    sel = albumSel < numAlbums ? albumSel : 0;
                    top = sel >= ITEMS ? sel - ITEMS + 1 : 0;
                    redraw = true;
                    continue;
                }
                break;
            }

            // L/R: switch tab (at the top level)
            if ((((down & KEY_L) && tab == 1) || ((down & KEY_R) && tab == 0)) &&
                (tab == 0 || curAlbum < 0))
            {
                tab = !tab;
                curAlbum = -1;
                if (tab)
                    scanAlbums();
                else
                    scanFiles();
                sel = top = 0;
                redraw = true;
                continue;
            }

            if (tab == 0)
            {
                if ((down & KEY_X) && numFiles > 0)
                {
                    if (confirmDelete("Delete this video?", baseName(ents[sel].path), NULL))
                    {
                        unlink(ents[sel].path);
                        scanFiles();
                    }
                    redraw = true;
                }
                if ((down & KEY_A) && numFiles > 0)
                {
                    playFile(baseName(ents[sel].path), vram);
                    redraw = true;
                }
            }
            else if (albumMode)
            {
                if (down & KEY_Y)                      // New album
                {
                    int n = newAlbum();
                    if (n >= 0)
                    {
                        sel = n;
                        setNote("Made album %.19s", albumLabel(n));
                    }
                    redraw = true;
                }
                if ((down & KEY_START) && count > 0)   // use as camera album
                {
                    if (canBeCameraAlbum(albums[sel].path))
                    {
                        photoSetSaveDir(albums[sel].path);
                        setNote("Camera saves to %.14s", albumLabel(sel));
                    }
                    redraw = true;
                }
                if ((down & KEY_X) && count > 0 && sel > 0 && !albums[sel].system)
                {
                    char extra[32];
                    snprintf(extra, sizeof(extra), "and its %d picture%s", albums[sel].count,
                             albums[sel].count == 1 ? "" : "s");
                    if (confirmDelete("Delete this album?", albumLabel(sel), extra))
                    {
                        char label[24];
                        snprintf(label, sizeof(label), "%s", albumLabel(sel));
                        if (!deleteAlbum(sel))
                            setNote("Couldn't delete %.14s", label);
                        scanAlbums();
                    }
                    redraw = true;
                }
                if ((down & KEY_A) && count > 0)       // open
                {
                    albumSel = sel;
                    curAlbum = sel;
                    scanFiles();
                    sel = top = 0;
                    redraw = true;
                }
            }
            else
            {
                if ((down & KEY_START) && canBeCameraAlbum(albums[curAlbum].path))
                {
                    photoSetSaveDir(albums[curAlbum].path);
                    setNote("Camera saves here%s", "");
                    redraw = true;
                }
                if ((down & KEY_Y) && numFiles > 0)
                {
                    doMove(sel);
                    redraw = true;
                }
                if ((down & KEY_X) && numFiles > 0 && !ents[sel].system)
                {
                    if (confirmDelete("Delete this photo?", baseName(ents[sel].path), NULL))
                    {
                        unlink(ents[sel].path);
                        albums[curAlbum].count--;
                        scanFiles();
                    }
                    redraw = true;
                }
                if ((down & KEY_A) && numFiles > 0)
                {
                    sel = viewPhotos(sel, vram);
                    redraw = true;
                }
            }
        }
    }

    for (int i = 0; i < NQ; i++)
        free(q[i]);
    free(tmp);
    free(ring);
    free(ents);
    free(albums);
    ents = NULL;
    albums = NULL;
}
