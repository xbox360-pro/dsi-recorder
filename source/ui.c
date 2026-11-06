// Tiny bottom-screen UI kit. See ui.h.
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <nds.h>

#include "ui.h"

static u16 *vram;        // sub BG3 bitmap
static u16 *canvas;      // back buffer
static u16 *background;  // cached background gradient

static int isqrt(int v)
{
    if (v <= 0)
        return 0;
    int r = 0;
    while ((r + 1) * (r + 1) <= v)
        r++;
    return r;
}

static void hline(int x0, int x1, int y, u16 c)
{
    if (y < 0 || y >= UI_H)
        return;
    if (x0 < 0)
        x0 = 0;
    if (x1 > UI_W)
        x1 = UI_W;
    u16 *p = canvas + y * UI_W;
    for (int x = x0; x < x1; x++)
        p[x] = c;
}

static u16 mix(int r0, int g0, int b0, int r1, int g1, int b1, int t, int n)
{
    return UIC(r0 + (r1 - r0) * t / n, g0 + (g1 - g0) * t / n, b0 + (b1 - b0) * t / n);
}

void uiInit(void)
{
    videoSetModeSub(MODE_5_2D);
    vramSetBankC(VRAM_C_SUB_BG);

    // Bitmap at +32 KB, console map at 0 and font tiles at +16 KB
    int bg = bgInitSub(3, BgType_Bmp16, BgSize_B16_256x256, 2, 0);
    bgSetPriority(bg, 3);
    vram = bgGetGfxPtr(bg);

    PrintConsole *con = consoleInit(NULL, 0, BgType_Text4bpp, BgSize_T_256x256, 0, 1, false, true);
    bgSetPriority(con->bgId, 0);

    // Theme the text colors (slot n uses palette entry n * 16 + 15)
    u16 *pal = BG_PALETTE_SUB;
    pal[0] = 0;
    pal[TXT_DARK * 16 + 15] = RGB15(3, 4, 7);
    pal[TXT_RED * 16 + 15] = RGB15(31, 11, 11);
    pal[TXT_GREEN * 16 + 15] = RGB15(12, 29, 15);
    pal[TXT_AMBER * 16 + 15] = RGB15(31, 24, 9);
    pal[TXT_ACCENT * 16 + 15] = RGB15(12, 26, 31);
    pal[TXT_MUTED * 16 + 15] = RGB15(17, 19, 24);
    pal[TXT_WHITE * 16 + 15] = RGB15(31, 31, 31);

    canvas = malloc(UI_W * UI_H * 2);
    background = malloc(UI_W * UI_H * 2);

    // Background: deep navy fading to near black, with a soft glow at the top
    for (int y = 0; y < UI_H; y++)
    {
        u16 c = mix(4, 5, 11, 1, 1, 3, y, UI_H);
        for (int x = 0; x < UI_W; x++)
            background[y * UI_W + x] = c;
    }

    uiBegin();
    uiPresent();
}

void uiBegin(void)
{
    memcpy(canvas, background, UI_W * UI_H * 2);
    consoleClear();
}

void uiPresent(void)
{
    DC_FlushRange(canvas, UI_W * UI_H * 2);
    dmaCopyWords(2, canvas, vram, UI_W * UI_H * 2);
}

void uiRect(int x, int y, int w, int h, u16 c)
{
    for (int j = y; j < y + h; j++)
        hline(x, x + w, j, c);
}

void uiRound(int x, int y, int w, int h, int r, u16 c)
{
    if (r * 2 > h)
        r = h / 2;
    if (r * 2 > w)
        r = w / 2;
    for (int j = 0; j < h; j++)
    {
        int inset = 0;
        int k = j < r ? j : (j >= h - r ? h - 1 - j : -1);   // row index inside a corner
        if (k >= 0)
        {
            int d = 2 * (r - k) - 1;                          // 2 * distance from corner centre
            inset = r - isqrt(4 * r * r - d * d) / 2;
        }
        hline(x + inset, x + w - inset, y + j, c);
    }
}

void uiCircle(int cx, int cy, int r, u16 c)
{
    for (int dy = -r; dy <= r; dy++)
    {
        int half = isqrt(r * r - dy * dy + r);
        hline(cx - half, cx + half + 1, cy + dy, c);
    }
}

void uiTriangleRight(int x, int y, int h, u16 c)
{
    for (int j = 0; j < h; j++)
    {
        int len = (j < h / 2 ? j : h - 1 - j) + 1;
        hline(x, x + len, y + j, c);
    }
}

void uiCard(int x, int y, int w, int h, u16 fill, u16 edge)
{
    uiRound(x, y, w, h, 6, edge);
    uiRound(x + 1, y + 1, w - 2, h - 2, 5, fill);
}

void uiHeader(const char *title)
{
    for (int y = 0; y < 22; y++)
        hline(0, UI_W, y, mix(6, 8, 17, 3, 4, 10, y, 22));
    hline(0, UI_W, 22, COL_ACCENT);
    hline(0, UI_W, 23, UIC(3, 10, 15));
    uiCircle(13, 11, 4, COL_ACCENT);
    uiText(3, 1, TXT_WHITE, "%s", title);
}

void uiText(int col, int row, int color, const char *fmt, ...)
{
    char buf[40];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    consoleSetColor(NULL, (ConsoleColor)color);
    printf("\x1b[%d;%dH%s", row, col, buf);
}

void uiTextCenter(int centerX, int row, int color, const char *s)
{
    int len = strlen(s);
    int col = (centerX - len * 4 + 4) / 8;
    if (col < 0)
        col = 0;
    uiText(col, row, color, "%s", s);
}

void uiButton(UiBox b, const char *label, int style)
{
    u16 edge = style == 2 ? COL_DISABLED : (style == 1 ? COL_ACCENT : COL_BUTTON_EDGE);
    u16 fill = style == 2 ? COL_DISABLED : (style == 1 ? COL_PRESSED : COL_BUTTON);
    uiCard(b.x, b.y, b.w, b.h, fill, edge);
    if (style != 2)
        hline(b.x + 6, b.x + b.w - 6, b.y + 2, UIC(12, 15, 25));   // top highlight
    uiTextCenter(b.x + b.w / 2, (b.y + b.h / 2 - 4) / 8, style == 2 ? TXT_MUTED : TXT_WHITE,
                 label);
}

void uiSlider(int x, int y, int w, int pos, bool knob, u16 fill)
{
    if (pos < 0)
        pos = 0;
    if (pos > 1000)
        pos = 1000;
    int fx = x + (w * pos) / 1000;
    uiRound(x, y, w, 6, 3, COL_TRACK);
    if (fx > x)
        uiRound(x, y, fx - x + 3 > w ? w : fx - x + 3, 6, 3, fill);
    if (knob)
    {
        uiCircle(fx, y + 3, 8, UIC(2, 3, 6));
        uiCircle(fx, y + 3, 7, COL_WHITE);
        uiCircle(fx, y + 3, 3, fill);
    }
}

bool uiHit(UiBox b, int px, int py)
{
    return px >= b.x && px < b.x + b.w && py >= b.y && py < b.y + b.h;
}
