#ifndef CINEPAK_H
#define CINEPAK_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Return codes */
#define CINEPAK_OK           0
#define CINEPAK_ERR_INVALID (-1)
#define CINEPAK_ERR_NOMEM   (-2)

/* Opaque decoder context (tracks inter-frame state). */
typedef struct CinepakContext CinepakContext;

/* Allocate a decoder for frames of the given dimensions.
 * Returns NULL on allocation failure. */
CinepakContext *cinepak_alloc(int width, int height);

/* Free a decoder context. Safe to call with NULL. */
void cinepak_free(CinepakContext *ctx);

/* Decode one Cinepak frame.
 *
 *   data   — raw frame bytes as found in the AVI stream chunk
 *   size   — byte count of data
 *   rgba   — output buffer, RGBA8888, must be >= height * stride bytes
 *   stride — bytes per output row (typically width * 4)
 *
 * Frames must be decoded in presentation order; inter-frame (delta)
 * state is maintained inside ctx.
 *
 * Returns CINEPAK_OK on success, negative error code otherwise.
 */
int cinepak_decode(CinepakContext *ctx,
                   const uint8_t  *data, int size,
                   uint8_t        *rgba, int stride);

#ifdef __cplusplus
}
#endif

#endif /* CINEPAK_H */
