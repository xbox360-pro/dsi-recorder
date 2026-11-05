// In-app video viewer for DSi Recorder AVIs.
#ifndef PLAYER_H
#define PLAYER_H

#include <nds.h>

// Shows the file browser on the bottom screen and plays videos on the top
// screen bitmap `vram` (256x192, 16-bit). Returns when the user presses B
// in the file list. The camera must be paused by the caller.
void runViewer(u16 *vram);

#endif
