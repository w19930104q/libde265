# dec265 YUV output tests

Acceptance tests for the YUV files written by `dec265 -o`.

## Output format (public semantics)

For every decoded picture, dec265 writes the *visible* picture (SPS
conformance window applied) as planar YUV, planes in the order Y, Cb, Cr
(monochrome: Y only), each plane row by row without padding.  Plane
dimensions follow the cropped luma size `W x H` and the chroma format:

| chroma | chroma plane size          |
|--------|----------------------------|
| 4:2:0  | `ceil(W/2) x ceil(H/2)`    |
| 4:2:2  | `ceil(W/2) x H`            |
| 4:4:4  | `W x H`                    |
| mono   | (no chroma planes)         |

Samples up to 8 bit are written as one byte per sample; 9–16 bit samples
are written as two bytes per sample, little-endian, right-aligned in the
16-bit word (same packing as FFmpeg's `yuv420p10le`/`yuv420p12le`).

## Tests

- `yuv-plane-geometry` (`yuv_plane_geometry_test.cc`): decodes every
  stream of the sample matrix and asserts the plane size relations,
  strides, and bit depths directly on the `de265_image` objects via the
  public API.
- `dec265-yuv-write` (`check_yuv_write.py`): runs `dec265 -o` on every
  stream and compares the output byte-exactly against the hardcoded
  sha256 hashes in `expected.json`; also asserts the plane size
  relations of the written files.

## Sample matrix (`streams/`)

`c*.h265`: 3 lossless frames each, 104x64 coded with a conformance
window to 100x60 visible, generated with

    ffmpeg -f lavfi -i testsrc2=size=WxH:rate=2:duration=1.5 \
           -pix_fmt <fmt> -c:v libx265 -x265-params lossless=1 out.h265

covering 4:2:0 / 4:2:2 / 4:4:4 / monochrome at 8/10/12 bit.

`p*_odd.h265`: same streams with the SPS conformance-window offsets
patched by `sps_confwin_patch.py` to obtain odd visible sizes (and odd
chroma plane sizes for 4:2:0/4:2:2), e.g.

    python3 -I sps_confwin_patch.py c444_b08.h265 p444_b08_odd.h265 0 3 0 3

The expected outputs in `expected.json` were cross-checked byte-for-byte
against an independent HEVC decoder (FFmpeg) before hashing.
