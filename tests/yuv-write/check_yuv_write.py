#!/usr/bin/env python3
"""Acceptance test for dec265 YUV output.

Runs dec265 -o on the fixed sample matrix in streams/ and checks, for every
stream:

  a) the written YUV file is byte-identical to the hardcoded expected
     output (sha256 in expected.json) -- the expected bytes were computed
     independently from the libde265 image model plus the documented
     public YUV semantics (and cross-checked against an independent
     decoder);

  b) the plane size relations of the written file satisfy the chroma
     sub-sampling of the cropped picture, with hardcoded rules:
       4:2:0 -> chroma = ceil(luma/2) x ceil(luma/2)
       4:2:2 -> chroma = ceil(luma/2) x luma
       4:4:4 -> chroma = luma x luma
       mono  -> no chroma planes
     and the file size equals frames * sum(plane bytes).

Usage: check_yuv_write.py PATH_TO_DEC265 [TEST_DIR]
"""

import hashlib
import json
import os
import subprocess
import sys
import tempfile


def bytes_per_sample(bitdepth):
    return 1 if bitdepth <= 8 else 2


def expected_plane_dims(chroma, luma_w, luma_h):
    """Hardcoded sampling-ratio rules for the cropped picture."""
    if chroma == '420':
        return [(luma_w, luma_h), ((luma_w + 1) // 2, (luma_h + 1) // 2),
                ((luma_w + 1) // 2, (luma_h + 1) // 2)]
    if chroma == '422':
        return [(luma_w, luma_h), ((luma_w + 1) // 2, luma_h),
                ((luma_w + 1) // 2, luma_h)]
    if chroma == '444':
        return [(luma_w, luma_h), (luma_w, luma_h), (luma_w, luma_h)]
    if chroma == 'mono':
        return [(luma_w, luma_h)]
    raise AssertionError(f'unknown chroma format {chroma!r}')


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    dec265 = sys.argv[1]
    test_dir = sys.argv[2] if len(sys.argv) > 2 else os.path.dirname(
        os.path.abspath(__file__))

    with open(os.path.join(test_dir, 'expected.json')) as f:
        cases = json.load(f)['cases']

    failures = 0
    for case in cases:
        name = os.path.basename(case['stream'])
        stream = os.path.join(test_dir, case['stream'])
        chroma = case['chroma']
        bd = case['bitdepth']
        lw, lh = case['luma_width'], case['luma_height']
        frames = case['frames']

        # --- (b) hardcoded plane-size relations -------------------------
        planes = expected_plane_dims(chroma, lw, lh)

        # explicit sampling-ratio assertions (ceil semantics for odd sizes)
        if chroma == '420':
            assert planes[1] == ((lw + 1) // 2, (lh + 1) // 2)
        elif chroma == '422':
            assert planes[1] == ((lw + 1) // 2, lh)
        elif chroma == '444':
            assert planes[1] == (lw, lh)
        elif chroma == 'mono':
            assert len(planes) == 1
        if chroma != 'mono':
            assert planes[1] == planes[2], 'Cb and Cr must have equal size'

        bps = bytes_per_sample(bd)
        frame_bytes = sum(w * h * bps for (w, h) in planes)
        expected_size = frames * frame_bytes

        # --- run dec265 --------------------------------------------------
        with tempfile.NamedTemporaryFile(suffix='.yuv', delete=False) as tmp:
            out_path = tmp.name
        try:
            proc = subprocess.run(
                [dec265, '-q', '-o', out_path, stream],
                capture_output=True, timeout=300)
            if proc.returncode != 0:
                print(f'FAIL {name}: dec265 exited with {proc.returncode}\n'
                      f'{proc.stderr.decode(errors="replace")}')
                failures += 1
                continue

            with open(out_path, 'rb') as f:
                data = f.read()
        finally:
            os.unlink(out_path)

        ok = True

        # file size must match frames * sum(plane bytes)
        if len(data) != expected_size:
            print(f'FAIL {name}: size {len(data)} != expected '
                  f'{expected_size} ({frames} frames x {frame_bytes} bytes; '
                  f'planes {planes}, {bd} bit)')
            ok = False

        # --- (a) byte-exact comparison with hardcoded hash ---------------
        digest = hashlib.sha256(data).hexdigest()
        if digest != case['sha256']:
            print(f'FAIL {name}: sha256 {digest} != expected '
                  f'{case["sha256"]}')
            ok = False

        if ok:
            print(f'ok   {name}: {chroma} {bd}bit {lw}x{lh}, '
                  f'{frames} frames, {len(data)} bytes, sha256 match')
        else:
            failures += 1

    if failures:
        print(f'{failures} of {len(cases)} cases FAILED')
        return 1
    print(f'all {len(cases)} cases passed')
    return 0


if __name__ == '__main__':
    sys.exit(main())
