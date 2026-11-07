// stb_image (public domain / MIT, by Sean Barrett) for PNG, GIF, other BMPs
// and progressive JPEGs.

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_GIF
#define STBI_ONLY_BMP
#define STBI_ONLY_JPEG
#define STBI_NO_SIMD
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#define STBI_NO_THREAD_LOCALS
#include "lib/stb_image.h"

unsigned char *imgStbLoad(const char *path, int *w, int *h)
{
    int n;
    return stbi_load(path, w, h, &n, 3);
}

int imgStbInfo(const char *path, int *w, int *h)
{
    int n;
    return stbi_info(path, w, h, &n);
}

void imgStbFree(void *p)
{
    stbi_image_free(p);
}
