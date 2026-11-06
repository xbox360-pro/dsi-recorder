// Tiny bottom-screen UI kit: a 16-bit bitmap layer for shapes (BG3) with the
// text console layered on top (BG0), both on the sub engine.
#ifndef UI_KIT_H
#define UI_KIT_H

#include <stdbool.h>
#include <nds.h>

#define UI_W 256
#define UI_H 192

#define UIC(r, g, b) ((u16)(RGB15(r, g, b) | BIT(15)))

// Theme colors (bitmap)
#define COL_HEADER_TOP   UIC(6, 8, 17)
#define COL_HEADER_BOT   UIC(3, 4, 10)
#define COL_ACCENT       UIC(6, 22, 31)
#define COL_CARD         UIC(5, 6, 12)
#define COL_CARD_EDGE    UIC(8, 10, 19)
#define COL_BUTTON       UIC(7, 9, 17)
#define COL_BUTTON_EDGE  UIC(11, 14, 25)
#define COL_PRESSED      UIC(9, 17, 27)
#define COL_DISABLED     UIC(4, 5, 9)
#define COL_RED          UIC(26, 5, 7)
#define COL_RED_DARK     UIC(16, 3, 5)
#define COL_RED_EDGE     UIC(31, 12, 13)
#define COL_TRACK        UIC(8, 10, 18)
#define COL_WHITE        UIC(31, 31, 31)
#define COL_GREEN        UIC(8, 25, 12)

// Text colors (console palette slots, use with uiText)
enum
{
    TXT_DARK = 0,
    TXT_RED = 1,
    TXT_GREEN = 2,
    TXT_AMBER = 3,
    TXT_ACCENT = 6,
    TXT_MUTED = 7,
    TXT_WHITE = 15,
};

typedef struct
{
    int x, y, w, h;
} UiBox;

void uiInit(void);

// Optional custom theme: loads a .bmp (24/32-bit, any size) and uses it as the
// header icon and as a faded tiled pattern behind the UI. Returns false if the
// file is missing or not a supported BMP (the default look is kept).
bool uiLoadTheme(const char *path);

// Start a new frame: background into the canvas, text layer cleared
void uiBegin(void);
// Push the canvas to the screen
void uiPresent(void);

void uiRect(int x, int y, int w, int h, u16 c);
void uiRound(int x, int y, int w, int h, int r, u16 c);
void uiCircle(int cx, int cy, int r, u16 c);
void uiTriangleRight(int x, int y, int h, u16 c);   // play icon, h tall

// Rounded panel with a 1 px edge
void uiCard(int x, int y, int w, int h, u16 fill, u16 edge);
// Header bar (rows 0-2) with a title
void uiHeader(const char *title);

void uiText(int col, int row, int color, const char *fmt, ...)
    __attribute__((format(printf, 4, 5)));
void uiTextCenter(int centerX, int row, int color, const char *s);

// Button: style 0 normal, 1 pressed, 2 disabled
void uiButton(UiBox b, const char *label, int style);

// Horizontal slider/progress bar. pos in 0..1000
void uiSlider(int x, int y, int w, int pos, bool knob, u16 fill);

bool uiHit(UiBox b, int px, int py);

#endif
