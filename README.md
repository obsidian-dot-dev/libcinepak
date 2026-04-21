# libcinepak

A minimal, zero-dependency Cinepak (FOURCC `cvid`) video decoder written in C99.

## Overview

Cinepak is a lossy video codec developed by SuperMac Technology (later acquired
by Radius Inc.) and released in 1991.  It was the dominant CD-ROM video codec
throughout the early-to-mid 1990s, used in thousands of games and multimedia
titles on Windows, macOS, and game consoles.

All Cinepak patents have expired (filed ca. 1991–1993, 20-year term).

## Features

- Decodes both **intra** (key) and **inter** (delta) frames
- Full and partial codebook updates
- Output: RGBA8888, caller-supplied buffer and stride
- Stateful context tracks inter-frame reference automatically
- Pure C99, no external dependencies, no dynamic allocation after `cinepak_alloc`

## API

```c
#include "cinepak.h"

// Allocate a decoder context for frames of the given dimensions.
// Returns NULL on allocation failure.
CinepakContext *cinepak_alloc(int width, int height);

// Free a decoder context. Safe to call with NULL.
void cinepak_free(CinepakContext *ctx);

// Decode one Cinepak frame.
//   data   — raw frame bytes from the AVI stream chunk
//   size   — byte count of data
//   rgba   — output buffer, RGBA8888, must be >= height * stride bytes
//   stride — bytes per output row (typically width * 4)
// Frames must be decoded in presentation order.
// Returns CINEPAK_OK (0) on success, negative on error.
int cinepak_decode(CinepakContext *ctx,
                   const uint8_t  *data, int size,
                   uint8_t        *rgba, int stride);
```

## Usage example

```c
#include "cinepak.h"

// Create decoder once per video stream
CinepakContext *ctx = cinepak_alloc(width, height);

// For each frame (in order):
uint8_t *rgba = malloc(width * height * 4);
int stride    = width * 4;

if (cinepak_decode(ctx, frame_data, frame_size, rgba, stride) == CINEPAK_OK) {
    // use rgba...
}

// When done:
cinepak_free(ctx);
free(rgba);
```

## Building

Requires CMake 3.14+ and a C99 compiler.

```sh
mkdir build && cd build
cmake ..
cmake --build .
```

To use as a subdirectory in another CMake project:

```cmake
add_subdirectory(libcinepak)
target_link_libraries(your_target PRIVATE cinepak)
```

Or via FetchContent:

```cmake
include(FetchContent)
FetchContent_Declare(libcinepak
    GIT_REPOSITORY https://github.com/ifcaro/libcinepak
    GIT_TAG        main)
FetchContent_MakeAvailable(libcinepak)
target_link_libraries(your_target PRIVATE cinepak)
```

## Format reference

The implementation was written from scratch using the following public resources:

- **MultimediaWiki — Cinepak**
  https://wiki.multimedia.cx/index.php/Cinepak
  Primary format reference: frame header, strip header, chunk layout,
  codebook structure, V1/V4 encoding modes, intra/inter flag streams.

- **Apple Computer — Cinepak codec technical note** (historical)
  Original SuperMac/Radius documentation, later archived by Apple.

## Codec technical summary

| Property        | Value |
|----------------|-------|
| Color space    | YUV (BT.601, full range) |
| Block size     | 4×4 pixels |
| Codebooks      | Dynamic, loaded from the bitstream (no hardcoded tables) |
| Codebook size  | Up to 256 entries × 6 bytes (4×luma + U + V) |
| Frame types    | Intra (key) and inter (delta) |
| Encoding modes | V1 (1 entry per 4×4) and V4 (4 entries per 4×4) |
| Container      | AVI (RIFF), QuickTime (MOV) |
| FourCC         | `cvid` |

## License

MIT — see [LICENSE](LICENSE).
