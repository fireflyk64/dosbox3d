/* debris.h: a wreck's debris, drawn into an 8-bit frame buffer: chunks of armour plating tumbling away, cut from
 * the plate's damage stamps (tools/debris.py), lit as they turn, glowing while they are hot, and embers ahead of
 * them.  The user's design (2026-11-03): the pieces only where the picture is drawn on the host, "those FX could
 * simply not appear for dos-only players ... it's only visual": the sprite accelerator's E730h (src/hardware/rvgx.cpp,
 * docs/gx.md) and the native builds' own draw call it; RV.EXE has only the struct, to fill the card's block from
 * (explode.c), and on DOS without the card there is no debris.
 *
 * Every piece follows from the seed and the age alone, in world space: turning or flying through the cloud shows
 * real parallax, and nothing is kept from frame to frame. */
#ifndef DEBRIS_H
#define DEBRIS_H

#include <stdint.h>

#define DEBRIS_MAX 64                   /* shards a cloud at most (the count is capped to it) */
#define DEBRIS_METAL 8                  /* the metal's colours, dark to light */
#define DEBRIS_HOT 4                    /* the glow's, dim to bright */
#define DEBRIS_SHOW_AGE 6               /* steps before any piece shows: the blast's flash covers them */

#ifdef __WATCOMC__
#pragma pack(1)                         /* (RV.EXE writes it straight into the card's block, from byte 64: gx.c) */
#endif
typedef struct {
    uint16_t seed, age, life, count;    /* the cloud's: its seed, steps since the blast, its whole life, its shards */
    int16_t focal, cx, cy;              /* the projection: px per unit at distance 1, the screen's centre */
    int32_t x, y, z;                    /* the cloud's centre in camera space, 16ths of a km: right, up, ahead */
    int32_t spread;                     /* how far its fastest pieces have flown, 16ths of a km */
    int32_t piece;                      /* a middling shard's length, 16ths of a km */
    int16_t m[9];                       /* world to camera: the camera's right, up and front, 256 = 1 */
    uint8_t metal[DEBRIS_METAL];        /* palette entries: hull greys, dark to light */
    uint8_t hot[DEBRIS_HOT];            /* palette entries: glowing, dim to bright */
} RvDebris;
#ifdef __WATCOMC__
#pragma pack()
typedef char rv_debris_is_64_bytes[sizeof(RvDebris) == 64 ? 1 : -1];
#endif

#ifndef __WATCOMC__
void rv_debris_prepare(void);           /* the pieces' pictures made (once; the first draw makes them otherwise) */
/* d drawn into fb (the first pixel of screen row fb_y0, `stride` bytes a row): rows y0 .. y1-1, columns xa .. xb-1,
 * and each row within its extent when ext is given (two int16 a row from row ext_y0: the first column and one past
 * the last; a row outside the table gets nothing).  Returns the pixels written */
uint32_t rv_debris_draw(const RvDebris *d, uint8_t *fb, int fb_y0, int stride, int y0, int y1, int xa, int xb,
                        const int16_t *ext, int ext_y0, int ext_rows);
#endif

#endif
