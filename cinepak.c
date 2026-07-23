/*
 * cinepak.c - Cinepak (FOURCC "cvid") video decoder
 *
 * Clean-room implementation based on public format documentation.
 * License: MIT
 */

#include "cinepak.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#ifndef OF_FASTTEXT
#  if defined(__has_include) && __has_include("of_fastram.h") && !defined(OF_PC) && defined(__ELF__)
#    include "of_fastram.h"
#  else
#    define OF_FASTTEXT
#    define OF_FASTDATA
#    define OF_FASTRODATA
#  endif
#endif

#ifdef __cplusplus
extern "C" {
#endif
__attribute__((weak)) void poll_dma_from_codec(void) {}
#ifdef __cplusplus
}
#endif

typedef struct __attribute__((aligned(4))) {
  uint8_t y[4];
  uint8_t u, v;
  uint8_t pad[2]; // Pad to align rgb to 4-byte boundary
  uint16_t rgb[4];
} CvEntry;

typedef struct __attribute__((aligned(4))) {
  uint8_t y[4];
  uint8_t u, v;
  uint8_t pad[2]; // Pad to align pairs to 4-byte boundary
  uint32_t pairs[4]; // Precalculated (rgb << 16) | rgb
} CvEntryV1;

OF_FASTDATA static int16_t R_diff[256];
OF_FASTDATA static int16_t G_diff_u[256];
OF_FASTDATA static int16_t G_diff_v[256];
OF_FASTDATA static int16_t B_diff[256];
static int yuv_tables_initialized = 0;

static void init_yuv_tables(void) {
  if (yuv_tables_initialized)
    return;
  for (int i = 0; i < 256; i++) {
    int8_t val = (int8_t)i;
    R_diff[i] = (int16_t)(1.402f * val);
    G_diff_u[i] = (int16_t)(0.344136f * val);
    G_diff_v[i] = (int16_t)(0.714136f * val);
    B_diff[i] = (int16_t)(1.772f * val);
  }
  yuv_tables_initialized = 1;
}

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
  uint8_t *prev; /* previous decoded frame, RGB565, width*2 stride */
  CvEntry v4[CINEPAK_MAX_STRIPS][256];  /* high-detail codebook, one slot per strip */
  CvEntryV1 v1[CINEPAK_MAX_STRIPS][256];/* low-detail codebook, one slot per strip */
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

OF_FASTTEXT static void paint_v1(uint8_t *rgba, int stride, int bx, int by,
                                 const CvEntryV1 *e) {
  uint32_t *line0 = (uint32_t *)(rgba + by * stride + bx * 2);
  uint32_t *line1 = (uint32_t *)(rgba + (by + 1) * stride + bx * 2);
  uint32_t *line2 = (uint32_t *)(rgba + (by + 2) * stride + bx * 2);
  uint32_t *line3 = (uint32_t *)(rgba + (by + 3) * stride + bx * 2);

  uint32_t pair0 = e->pairs[0];
  uint32_t pair1 = e->pairs[1];
  uint32_t pair2 = e->pairs[2];
  uint32_t pair3 = e->pairs[3];

  line0[0] = pair0;
  line0[1] = pair1;
  line1[0] = pair0;
  line1[1] = pair1;

  line2[0] = pair2;
  line2[1] = pair3;
  line3[0] = pair2;
  line3[1] = pair3;
}

OF_FASTTEXT static void paint_v4(uint8_t *rgba, int stride, int bx, int by,
                                 const CvEntry *e0, const CvEntry *e1,
                                 const CvEntry *e2, const CvEntry *e3) {
  uint32_t *line0 = (uint32_t *)(rgba + by * stride + bx * 2);
  uint32_t *line1 = (uint32_t *)(rgba + (by + 1) * stride + bx * 2);
  uint32_t *line2 = (uint32_t *)(rgba + (by + 2) * stride + bx * 2);
  uint32_t *line3 = (uint32_t *)(rgba + (by + 3) * stride + bx * 2);

  const uint32_t *e0_rgb = (const uint32_t *)e0->rgb;
  const uint32_t *e1_rgb = (const uint32_t *)e1->rgb;
  const uint32_t *e2_rgb = (const uint32_t *)e2->rgb;
  const uint32_t *e3_rgb = (const uint32_t *)e3->rgb;

  line0[0] = e0_rgb[0];
  line0[1] = e1_rgb[0];
  line1[0] = e0_rgb[1];
  line1[1] = e1_rgb[1];

  line2[0] = e2_rgb[0];
  line2[1] = e3_rgb[0];
  line3[0] = e2_rgb[1];
  line3[1] = e3_rgb[1];
}

OF_FASTTEXT static void paint_v1_clipped(uint8_t *rgba, int stride, int bx,
                                         int by, int width, int height,
                                         const CvEntryV1 *e) {
  for (int qy = 0; qy < 2; ++qy) {
    for (int qx = 0; qx < 2; ++qx) {
      uint16_t color = (uint16_t)e->pairs[qy * 2 + qx];
      for (int py = 0; py < 2; ++py) {
        int y = by + qy * 2 + py;
        if (y >= height)
          continue;
        for (int pxi = 0; pxi < 2; ++pxi) {
          int x = bx + qx * 2 + pxi;
          if (x >= width)
            continue;
          *(uint16_t *)(rgba + y * stride + x * 2) = color;
        }
      }
    }
  }
}

OF_FASTTEXT static void paint_v4_clipped(uint8_t *rgba, int stride, int bx,
                                         int by, int width, int height,
                                         const CvEntry *e0, const CvEntry *e1,
                                         const CvEntry *e2, const CvEntry *e3) {
  const CvEntry *q[4] = {e0, e1, e2, e3};
  for (int qy = 0; qy < 2; ++qy) {
    for (int qx = 0; qx < 2; ++qx) {
      const CvEntry *e = q[qy * 2 + qx];
      for (int py = 0; py < 2; ++py) {
        int y = by + qy * 2 + py;
        if (y >= height)
          continue;
        for (int pxi = 0; pxi < 2; ++pxi) {
          int x = bx + qx * 2 + pxi;
          if (x >= width)
            continue;
          uint16_t color = e->rgb[py * 2 + pxi];
          *(uint16_t *)(rgba + y * stride + x * 2) = color;
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
OF_FASTTEXT static void load_codebook(CvEntry *cb, const uint8_t *data,
                                      int size, int partial, int y_only) {
  const uint8_t *p = data;
  const uint8_t *end = data + size;
  uint32_t flags = 0;
  uint32_t mask = 0;
  int n = y_only ? 4 : 6;

  for (int i = 0; i < 256; ++i) {
    if (partial) {
      if (!(mask >>= 1)) {
        if (p + 4 > end)
          return;
        flags = rb32(p);
        p += 4;
        mask = 0x80000000u;
      }
      if (!(flags & mask))
        continue;
    }

    if (p + n > end)
      return;

    cb[i].y[0] = p[0];
    cb[i].y[1] = p[1];
    cb[i].y[2] = p[2];
    cb[i].y[3] = p[3];
    if (!y_only) {
      cb[i].u = p[4];
      cb[i].v = p[5];
    }

    int rv = R_diff[cb[i].v];
    int guv = G_diff_u[cb[i].u] + G_diff_v[cb[i].v];
    int bu = B_diff[cb[i].u];

    for (int k = 0; k < 4; k++) {
      int y = cb[i].y[k];
      int r = y + rv;
      int g = y - guv;
      int b = y + bu;
      if (r < 0) r = 0; else if (r > 255) r = 255;
      if (g < 0) g = 0; else if (g > 255) g = 255;
      if (b < 0) b = 0; else if (b > 255) b = 255;
      cb[i].rgb[k] = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
    }

    p += n;
  }
}

OF_FASTTEXT static void load_codebook_v1(CvEntryV1 *cb, const uint8_t *data,
                                         int size, int partial, int y_only) {
  const uint8_t *p = data;
  const uint8_t *end = data + size;
  uint32_t flags = 0;
  uint32_t mask = 0;
  int n = y_only ? 4 : 6;

  for (int i = 0; i < 256; ++i) {
    if (partial) {
      if (!(mask >>= 1)) {
        if (p + 4 > end)
          return;
        flags = rb32(p);
        p += 4;
        mask = 0x80000000u;
      }
      if (!(flags & mask))
        continue;
    }

    if (p + n > end)
      return;

    cb[i].y[0] = p[0];
    cb[i].y[1] = p[1];
    cb[i].y[2] = p[2];
    cb[i].y[3] = p[3];
    if (!y_only) {
      cb[i].u = p[4];
      cb[i].v = p[5];
    }

    int rv = R_diff[cb[i].v];
    int guv = G_diff_u[cb[i].u] + G_diff_v[cb[i].v];
    int bu = B_diff[cb[i].u];

    for (int k = 0; k < 4; k++) {
      int y = cb[i].y[k];
      int r = y + rv;
      int g = y - guv;
      int b = y + bu;
      if (r < 0) r = 0; else if (r > 255) r = 255;
      if (g < 0) g = 0; else if (g > 255) g = 255;
      if (b < 0) b = 0; else if (b > 255) b = 255;
      uint32_t rgb = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
      cb[i].pairs[k] = (rgb << 16) | rgb;
    }

    p += n;
  }
}

/*
 * vec_mode bits (chunk_id & 0x03):
 *  bit0: skip bit present (0=copy prev, 1=paint)
 *  bit1: force V1 (no V1/V4 selector bit)
 */
OF_FASTTEXT static void decode_vectors(CinepakContext *ctx, const uint8_t *data,
                                       int size, int x1, int x2, int y1, int y2,
                                       uint8_t *rgba, int stride, int vec_mode,
                                       const CvEntryV1 *v1_decode,
                                       const CvEntry *v4_decode) {
  const uint8_t *p = data;
  const uint8_t *end = data + size;
  uint32_t flags = 0;
  int flags_b = 0;
  int pw = ctx->width * 2;

  if (x1 < 0)
    x1 = 0;
  if (y1 < 0)
    y1 = 0;
  if (x2 > ctx->width)
    x2 = ctx->width;
  if (y2 > ctx->height)
    y2 = ctx->height;
  if (x1 >= x2 || y1 >= y2)
    return;

#define NEXT_BIT(w, b, out)                                                    \
  do {                                                                         \
    if (!(b)) {                                                                \
      if (p + 4 > end)                                                         \
        return;                                                                \
      (w) = rb32(p);                                                           \
      p += 4;                                                                  \
      (b) = 32;                                                                \
    }                                                                          \
    (out) = (int)((w) >> 31);                                                  \
    (w) <<= 1;                                                                 \
    --(b);                                                                     \
  } while (0)

  // Check if the strip region is fully aligned (fits within screen boundaries)
  int is_aligned = (x1 % 4 == 0) && (x2 % 4 == 0) && (y1 % 4 == 0) &&
                   (y2 % 4 == 0) && (x2 <= ctx->width) && (y2 <= ctx->height);

  if (is_aligned) {
    for (int by = y1; by < y2; by += 4) {
      for (int bx = x1; bx < x2; bx += 4) {
        if (vec_mode & 1) {
          int paint = 0;
          NEXT_BIT(flags, flags_b, paint);
          if (!paint) {
            if (ctx->prev) {
              uint32_t *dst0 = (uint32_t *)(rgba + by * stride + bx * 2);
              uint32_t *dst1 = (uint32_t *)(rgba + (by + 1) * stride + bx * 2);
              uint32_t *dst2 = (uint32_t *)(rgba + (by + 2) * stride + bx * 2);
              uint32_t *dst3 = (uint32_t *)(rgba + (by + 3) * stride + bx * 2);

              uint32_t *src0 = (uint32_t *)(ctx->prev + by * pw + bx * 2);
              uint32_t *src1 = (uint32_t *)(ctx->prev + (by + 1) * pw + bx * 2);
              uint32_t *src2 = (uint32_t *)(ctx->prev + (by + 2) * pw + bx * 2);
              uint32_t *src3 = (uint32_t *)(ctx->prev + (by + 3) * pw + bx * 2);

              dst0[0] = src0[0];
              dst0[1] = src0[1];
              dst1[0] = src1[0];
              dst1[1] = src1[1];
              dst2[0] = src2[0];
              dst2[1] = src2[1];
              dst3[0] = src3[0];
              dst3[1] = src3[1];
            }
            continue;
          }
        }

        if (vec_mode & 2) {
          if (p >= end)
            return;
          paint_v1(rgba, stride, bx, by, &v1_decode[*p]);
          ++p;
        } else {
          int use_v4 = 0;
          NEXT_BIT(flags, flags_b, use_v4);
          if (!use_v4) {
            if (p >= end)
              return;
            paint_v1(rgba, stride, bx, by, &v1_decode[*p]);
            ++p;
          } else {
            if (p + 4 > end)
              return;
            paint_v4(rgba, stride, bx, by, &v4_decode[p[0]], &v4_decode[p[1]],
                     &v4_decode[p[2]], &v4_decode[p[3]]);
            p += 4;
          }
        }
      }
    }
  } else {
    // Fallback for clipped/non-aligned regions
    for (int by = y1; by < y2; by += 4) {
      for (int bx = x1; bx < x2; bx += 4) {
        if (vec_mode & 1) {
          int paint = 0;
          NEXT_BIT(flags, flags_b, paint);
          if (!paint) {
            if (ctx->prev) {
              int copy_bytes =
                  (ctx->width - bx >= 4) ? 8 : (ctx->width - bx) * 2;
              if (copy_bytes < 0)
                copy_bytes = 0;
              for (int r = 0; r < 4 && by + r < ctx->height; ++r) {
                memcpy(rgba + (by + r) * stride + bx * 2,
                       ctx->prev + (by + r) * pw + bx * 2, (size_t)copy_bytes);
              }
            }
            continue;
          }
        }

        if (vec_mode & 2) {
          if (p >= end)
            return;
          if (bx + 4 <= ctx->width && by + 4 <= ctx->height)
            paint_v1(rgba, stride, bx, by, &v1_decode[*p]);
          else
            paint_v1_clipped(rgba, stride, bx, by, ctx->width, ctx->height,
                             &v1_decode[*p]);
          ++p;
        } else {
          int use_v4 = 0;
          NEXT_BIT(flags, flags_b, use_v4);
          if (!use_v4) {
            if (p >= end)
              return;
            if (bx + 4 <= ctx->width && by + 4 <= ctx->height)
              paint_v1(rgba, stride, bx, by, &v1_decode[*p]);
            else
              paint_v1_clipped(rgba, stride, bx, by, ctx->width, ctx->height,
                               &v1_decode[*p]);
            ++p;
          } else {
            if (p + 4 > end)
              return;
            if (bx + 4 <= ctx->width && by + 4 <= ctx->height)
              paint_v4(rgba, stride, bx, by, &v4_decode[p[0]], &v4_decode[p[1]],
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
  }

#undef NEXT_BIT
}

CinepakContext *cinepak_alloc(int width, int height) {
  CinepakContext *ctx = (CinepakContext *)calloc(1, sizeof *ctx);
  if (!ctx)
    return NULL;

  ctx->width = width;
  ctx->height = height;
  ctx->prev = (uint8_t *)calloc((size_t)((height + 4) * width * 2), 1);
  if (!ctx->prev) {
    free(ctx);
    return NULL;
  }
  init_yuv_tables();
  return ctx;
}

void cinepak_free(CinepakContext *ctx) {
  if (!ctx)
    return;
  free(ctx->prev);
  free(ctx);
}

#ifndef OF_PC
OF_FASTDATA static CvEntryV1 fast_v1[256];
#endif

OF_FASTTEXT int cinepak_decode(CinepakContext *ctx,
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

    // Auto-detect 12-byte Cinepak headers (some encoders insert 2 bytes of padding)
    if (p + 4 <= end) {
        uint16_t id10 = rb16(p);
        uint16_t id12 = rb16(p + 2);
        if (id10 != 0x1000 && id10 != 0x1100 && (id12 == 0x1000 || id12 == 0x1100)) {
            p = data + 12;
            end = data + size;
        }
    }

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
        CvEntryV1 *strip_v1 = ctx->v1[s];

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
            poll_dma_from_codec();
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

            case 0x22: load_codebook_v1(strip_v1, cdata, csize, 0, 0); break;
            case 0x23: load_codebook_v1(strip_v1, cdata, csize, 1, 0); break;
            case 0x26: load_codebook_v1(strip_v1, cdata, csize, 0, 1); break;
            case 0x27: load_codebook_v1(strip_v1, cdata, csize, 1, 1); break;

            case 0x30:
            case 0x31:
            case 0x32:
#ifndef OF_PC
                // Copy strip codebooks to zero-latency BRAM for hot-path lookups
                memcpy(fast_v1, strip_v1, sizeof fast_v1);
                decode_vectors(ctx, cdata, csize,
                               strip_x1, strip_x2, strip_y1, strip_y2,
                               rgba, stride, (int)(chunk_id & 0x03), fast_v1, strip_v4);
#else
                decode_vectors(ctx, cdata, csize,
                               strip_x1, strip_x2, strip_y1, strip_y2,
                               rgba, stride, (int)(chunk_id & 0x03), strip_v1, strip_v4);
#endif
                break;
            default:
                break;
            }

            p += chunk_size;
        }

        p = strip_end;
        y0 = strip_y2;
    }
    if (rgba != ctx->prev) {
        int pw = ctx->width * 2;
        if (stride == pw) {
            memcpy(ctx->prev, rgba, (size_t)(ctx->height * pw));
        } else {
            for (int row = 0; row < ctx->height; ++row)
                memcpy(ctx->prev + row * pw, rgba + row * stride, (size_t)pw);
        }
    }

    return CINEPAK_OK;
}

int cinepak_decode_seek(CinepakContext *ctx, const uint8_t *data, int size) {
  if (!ctx)
    return CINEPAK_ERR_INVALID;
  return cinepak_decode(ctx, data, size, ctx->prev, ctx->width * 2);
}

uint8_t *cinepak_get_prev(CinepakContext *ctx) {
  return ctx ? ctx->prev : NULL;
}
