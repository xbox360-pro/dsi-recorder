// Photos: saving snapshots and loading pictures (BMP / JPG / PNG / GIF)
#ifndef PHOTO_H
#define PHOTO_H

#include <stdbool.h>
#include <nds.h>

#define PHOTO_DIR  "/DCIM/100DSI00"    // default album ("Camera roll")
#define ALBUM_ROOT "/DCIM"              // every folder in here is an album
#define NAND_ALBUM "nand2:/photo/DCIM"  // the DSi Camera's own photos (read only)

// The album the camera saves into (remembered in sd:/DSiRecorder/album.txt)
void photoInit(void);
const char *photoSaveDir(void);
void photoSetSaveDir(const char *dir);
const char *photoAlbumLabel(const char *dir);   // "Camera roll", "park", ...

// Saves the 256x192 picture in `vram` (DS bitmap format) as a 24-bit BMP in
// the camera album. zoomQ8 crops the centre like the zoomed preview (256 = no zoom).
// Writes the file name (e.g. "IMG_0003.BMP") to nameOut (16 bytes).
bool photoSave(const u16 *vram, int zoomQ8, char *nameOut);

// Draws any BMP/JPG/PNG/GIF scaled to fit 256x192 (letterboxed) into `vram`.
// On failure writes a short reason to `err` (32 bytes) and returns false.
bool photoLoad(const char *path, u16 *vram, char *err);

// true for .BMP .JPG .JPEG .PNG .GIF
bool photoIsImage(const char *name);

#endif
