/*
 * cinepak.c - Cinepak (FOURCC "cvid") video decoder
 *
 * Clean-room implementation based on public format documentation.
 * License: MIT
 */

#include "cinepak.h"
#include <stdlib.h>
#include <string.h>

typedef struct {
    uint8_t y[4];
    uint8_t u, v;
} CvEntry;

/*
 * Each strip owns its own v4[256]/v1[256] slice. On inter frames,
 * strip s inherits strip s-1's codebooks so that partial updates
 * (chunks 0x21/0x23) accumulate correctly within the frame. On
 * intra frames the arrays start zeroed from cinepak_alloc.
 */

/* The num_strips field is a uint16 with no documented maximum and no limit
 * in the original Radius codec. Real-world Cinepak content uses 1-3 strips.
 * 4 adds one slot of headroom without dynamic allocation. */
#define CINEPAK_MAX_STRIPS 4

struct CinepakContext {
    int width, height;
    uint8_t *prev;    /* previous decoded frame, RGBA8888, width*4 stride */
    CvEntry v4[CINEPAK_MAX_STRIPS][256];  /* high-detail codebook, one slot per strip */
    CvEntry v1[CINEPAK_MAX_STRIPS][256];  /* low-detail  codebook, one slot per strip */
};

static uint16_t rb16(const uint8_t *p) {
    return (uint16_t)(((unsigned)p[0] << 8) | p[1]);
}

static uint32_t rb24(const uint8_t *p) {
    return ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | (uint32_t)p[2];
}

static uint32_t rb32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static uint8_t clamp8(int v) {
    return v < 0 ? 0 : v > 255 ? 255 : (uint8_t)v;
}

static void yuv_rgba(uint8_t y, uint8_t u, uint8_t v, uint8_t *out) {
    /* U/V are stored as signed values with 0 neutral. Y is unsigned [0, 255]. */
    int iy = (int)y;
    int cb = (int)(int8_t)u;
    int cr = (int)(int8_t)v;

    out[0] = clamp8(iy + ((cr * 22970) >> 14));
    out[1] = clamp8(iy - ((cb * 5638 + cr * 11700) >> 14));
    out[2] = clamp8(iy + ((cb * 29032) >> 14));
    out[3] = 0xFF;
}

static void paint_v1(uint8_t *rgba, int stride, int bx, int by, const CvEntry *e) {
    for (int qy = 0; qy < 2; ++qy) {
        for (int qx = 0; qx < 2; ++qx) {
            uint8_t px[4];
            yuv_rgba(e->y[qy * 2 + qx], e->u, e->v, px);
            uint8_t *base = rgba + (by + qy * 2) * stride + (bx + qx * 2) * 4;
            memcpy(base, px, 4);
            memcpy(base + 4, px, 4);
            memcpy(base + stride, px, 4);
            memcpy(base + stride + 4, px, 4);
        }
    }
}

static void paint_v4(uint8_t *rgba, int stride, int bx, int by,
                     const CvEntry *e0, const CvEntry *e1,
                     const CvEntry *e2, const CvEntry *e3) {
    const CvEntry *q[4] = { e0, e1, e2, e3 };
    for (int qy = 0; qy < 2; ++qy) {
        for (int qx = 0; qx < 2; ++qx) {
            const CvEntry *e = q[qy * 2 + qx];
            uint8_t *base = rgba + (by + qy * 2) * stride + (bx + qx * 2) * 4;
            uint8_t px[4];
            yuv_rgba(e->y[0], e->u, e->v, px); memcpy(base, px, 4);
            yuv_rgba(e->y[1], e->u, e->v, px); memcpy(base + 4, px, 4);
            yuv_rgba(e->y[2], e->u, e->v, px); memcpy(base + stride, px, 4);
            yuv_rgba(e->y[3], e->u, e->v, px); memcpy(base + stride + 4, px, 4);
        }
    }
}

static void paint_v1_clipped(uint8_t *rgba, int stride, int bx, int by,
                             int width, int height, const CvEntry *e) {
    for (int qy = 0; qy < 2; ++qy) {
        for (int qx = 0; qx < 2; ++qx) {
            uint8_t px[4];
            yuv_rgba(e->y[qy * 2 + qx], e->u, e->v, px);
            for (int py = 0; py < 2; ++py) {
                int y = by + qy * 2 + py;
                if (y >= height) continue;
                for (int pxi = 0; pxi < 2; ++pxi) {
                    int x = bx + qx * 2 + pxi;
                    if (x >= width) continue;
                    memcpy(rgba + y * stride + x * 4, px, 4);
                }
            }
        }
    }
}

static void paint_v4_clipped(uint8_t *rgba, int stride, int bx, int by,
                             int width, int height,
                             const CvEntry *e0, const CvEntry *e1,
                             const CvEntry *e2, const CvEntry *e3) {
    const CvEntry *q[4] = { e0, e1, e2, e3 };
    for (int qy = 0; qy < 2; ++qy) {
        for (int qx = 0; qx < 2; ++qx) {
            const CvEntry *e = q[qy * 2 + qx];
            for (int py = 0; py < 2; ++py) {
                int y = by + qy * 2 + py;
                if (y >= height) continue;
                for (int pxi = 0; pxi < 2; ++pxi) {
                    int x = bx + qx * 2 + pxi;
                    if (x >= width) continue;
                    uint8_t px[4];
                    yuv_rgba(e->y[py * 2 + pxi], e->u, e->v, px);
                    memcpy(rgba + y * stride + x * 4, px, 4);
                }
            }
        }
    }
}

/*
 * Codebook chunk variants (low nibble semantics):
 *  bit0: 1 => partial update using 1 flag bit per codebook entry
 *  bit2: 1 => only Y[4] bytes are present (U/V unchanged)
 */
static void load_codebook(CvEntry *cb, const uint8_t *data, int size,
                          int partial, int y_only) {
    const uint8_t *p = data;
    const uint8_t *end = data + size;
    uint32_t flags = 0;
    uint32_t mask = 0;
    int n = y_only ? 4 : 6;

    for (int i = 0; i < 256; ++i) {
        if (partial) {
            if (!(mask >>= 1)) {
                if (p + 4 > end) return;
                flags = rb32(p);
                p += 4;
                mask = 0x80000000u;
            }
            if (!(flags & mask))
                continue;
        }

        if (p + n > end) return;

        cb[i].y[0] = p[0];
        cb[i].y[1] = p[1];
        cb[i].y[2] = p[2];
        cb[i].y[3] = p[3];
        if (!y_only) {
            cb[i].u = p[4];
            cb[i].v = p[5];
        }
        p += n;
    }
}

/*
 * vec_mode bits (chunk_id & 0x03):
 *  bit0: skip bit present (0=copy prev, 1=paint)
 *  bit1: force V1 (no V1/V4 selector bit)
 */
static void decode_vectors(CinepakContext *ctx,
                           const uint8_t *data, int size,
                           int x1, int x2, int y1, int y2,
                           uint8_t *rgba, int stride,
                           int vec_mode,
                           const CvEntry *v1_decode,
                           const CvEntry *v4_decode) {
    const uint8_t *p = data;
    const uint8_t *end = data + size;
    uint32_t flags = 0;
    int flags_b = 0;
    int pw = ctx->width * 4;

    if (x1 < 0) x1 = 0;
    if (y1 < 0) y1 = 0;
    if (x2 > ctx->width) x2 = ctx->width;
    if (y2 > ctx->height) y2 = ctx->height;
    if (x1 >= x2 || y1 >= y2) return;

#define NEXT_BIT(w, b, out)                    \
    do {                                        \
        if (!(b)) {                             \
            if (p + 4 > end) return;            \
            (w) = rb32(p);                      \
            p += 4;                             \
            (b) = 32;                           \
        }                                       \
        (out) = (int)((w) >> 31);               \
        (w) <<= 1;                              \
        --(b);                                  \
    } while (0)

    for (int by = y1; by < y2 && by < ctx->height; by += 4) {
        for (int bx = x1; bx < x2 && bx < ctx->width; bx += 4) {
            if (vec_mode & 1) {
                int paint = 0;
                NEXT_BIT(flags, flags_b, paint);
                if (!paint) {
                    if (ctx->prev) {
                        int copy_bytes = (ctx->width - bx >= 4) ? 16 : (ctx->width - bx) * 4;
                        if (copy_bytes < 0) copy_bytes = 0;
                        for (int r = 0; r < 4 && by + r < ctx->height; ++r) {
                            memcpy(rgba + (by + r) * stride + bx * 4,
                                   ctx->prev + (by + r) * pw + bx * 4,
                                   (size_t)copy_bytes);
                        }
                    }
                    continue;
                }
            }

            if (vec_mode & 2) {
                if (p >= end) return;
                if (bx + 4 <= ctx->width && by + 4 <= ctx->height)
                    paint_v1(rgba, stride, bx, by, &v1_decode[*p]);
                else
                    paint_v1_clipped(rgba, stride, bx, by, ctx->width, ctx->height, &v1_decode[*p]);
                ++p;
            } else {
                int use_v4 = 0;
                NEXT_BIT(flags, flags_b, use_v4);
                if (!use_v4) {
                    if (p >= end) return;
                    if (bx + 4 <= ctx->width && by + 4 <= ctx->height)
                        paint_v1(rgba, stride, bx, by, &v1_decode[*p]);
                    else
                        paint_v1_clipped(rgba, stride, bx, by, ctx->width, ctx->height, &v1_decode[*p]);
                    ++p;
                } else {
                    if (p + 4 > end) return;
                    if (bx + 4 <= ctx->width && by + 4 <= ctx->height)
                        paint_v4(rgba, stride, bx, by,
                                 &v4_decode[p[0]], &v4_decode[p[1]],
                                 &v4_decode[p[2]], &v4_decode[p[3]]);
                    else
                        paint_v4_clipped(rgba, stride, bx, by, ctx->width, ctx->height,
                                         &v4_decode[p[0]], &v4_decode[p[1]],
                                         &v4_decode[p[2]], &v4_decode[p[3]]);
                    p += 4;
                }
            }
        }
    }

#undef NEXT_BIT
}

CinepakContext *cinepak_alloc(int width, int height) {
    CinepakContext *ctx = (CinepakContext *)calloc(1, sizeof *ctx);
    if (!ctx) return NULL;

    ctx->width = width;
    ctx->height = height;
    ctx->prev = (uint8_t *)calloc((size_t)((height + 4) * width * 4), 1);
    if (!ctx->prev) {
        free(ctx);
        return NULL;
    }
    return ctx;
}

void cinepak_free(CinepakContext *ctx) {
    if (!ctx) return;
    free(ctx->prev);
    free(ctx);
}

int cinepak_decode(CinepakContext *ctx,
                   const uint8_t *data, int size,
                   uint8_t *rgba, int stride) {
    if (!ctx || !data || !rgba || size < 10) return CINEPAK_ERR_INVALID;

    uint8_t frame_flags = data[0];
    uint32_t frame_size = rb24(data + 1);
    uint16_t num_strips = rb16(data + 8);
    int y0 = 0;

    if ((int)frame_size > size) frame_size = (uint32_t)size;

    const uint8_t *p = data + 10;
    const uint8_t *end = data + frame_size;

    if (num_strips > CINEPAK_MAX_STRIPS)
        num_strips = CINEPAK_MAX_STRIPS;

    for (int s = 0; s < (int)num_strips && p + 12 <= end; ++s) {
        uint16_t strip_id   = rb16(p);
        uint16_t strip_size = rb16(p + 2);
        int strip_y1 = (int)rb16(p + 4);
        int strip_x1 = (int)rb16(p + 6);
        int strip_y2 = (int)rb16(p + 8);
        int strip_x2 = (int)rb16(p + 10);
        CvEntry *strip_v4 = ctx->v4[s];
        CvEntry *strip_v1 = ctx->v1[s];

        if (strip_size < 12)
            return CINEPAK_ERR_INVALID;

        if (!strip_y1)
            strip_y1 = y0;

        /* strip_id 0x1000 = key strip (bit 8 clear), 0x1100 = inter strip (bit 8 set).
         * Key strips on intra frames store absolute y2; inter strips store height. */
        if ((frame_flags & 1) && !(strip_id & 0x0100))
            strip_y2 = (int)rb16(p + 8);
        else
            strip_y2 = (strip_y1 = y0) + (int)rb16(p + 8);

        const uint8_t *strip_end = p + strip_size;
        if (strip_end > end) strip_end = end;
        p += 12;

        /*
         * Inherit codebooks from the previous strip within this frame.
         * Required so that partial-update chunks (0x21/0x23) in strip s
         * start from the same state as strip s-1 left off, not from stale
         * data from the previous frame. Skipped on intra frames (frame_flags
         * bit 0 set) where each strip is expected to define its own full
         * codebook from scratch.
         */
        if (s > 0 && !(frame_flags & 1)) {
            memcpy(strip_v4, ctx->v4[s - 1], sizeof ctx->v4[0]);
            memcpy(strip_v1, ctx->v1[s - 1], sizeof ctx->v1[0]);
        }

        while (p + 4 <= strip_end) {
            uint8_t chunk_id = p[0];
            uint32_t chunk_size = rb24(p + 1);

            if (chunk_size < 4) break;

            const uint8_t *cdata = p + 4;
            int csize = (int)chunk_size - 4;
            if (csize < 0 || cdata + csize > strip_end)
                break;

            switch (chunk_id) {
            case 0x20: load_codebook(strip_v4, cdata, csize, 0, 0); break;
            case 0x21: load_codebook(strip_v4, cdata, csize, 1, 0); break;
            case 0x24: load_codebook(strip_v4, cdata, csize, 0, 1); break;
            case 0x25: load_codebook(strip_v4, cdata, csize, 1, 1); break;

            case 0x22: load_codebook(strip_v1, cdata, csize, 0, 0); break;
            case 0x23: load_codebook(strip_v1, cdata, csize, 1, 0); break;
            case 0x26: load_codebook(strip_v1, cdata, csize, 0, 1); break;
            case 0x27: load_codebook(strip_v1, cdata, csize, 1, 1); break;

            case 0x30:
            case 0x31:
            case 0x32:
                decode_vectors(ctx, cdata, csize,
                               strip_x1, strip_x2, strip_y1, strip_y2,
                               rgba, stride, (int)(chunk_id & 0x03), strip_v1, strip_v4);
                break;
            default:
                break;
            }

            p += chunk_size;
        }

        p = strip_end;
        y0 = strip_y2;
    }

    {
        int pw = ctx->width * 4;
        for (int row = 0; row < ctx->height; ++row)
            memcpy(ctx->prev + row * pw, rgba + row * stride, (size_t)pw);
    }

    return CINEPAK_OK;
}
