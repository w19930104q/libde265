#!/usr/bin/env python3
"""Patch the conformance-window offsets in the SPS of a raw H.265 stream.

Generation-time tool for the dec265 YUV-write test matrix.  x265 always
rounds the display size to even numbers, so to obtain test streams with
odd visible sizes (and odd chroma plane sizes) we rewrite the
conf_win_*_offset ue(v) codes in the SPS.  Only the bits of the four
offsets are spliced; the rest of the bitstream is kept bit-identical
(only shifted when the replacement codes have a different length).

Usage: sps_confwin_patch.py in.h265 out.h265 LEFT RIGHT TOP BOTTOM
"""

import sys


class Bits:
    def __init__(self, data: bytes):
        self.bits = ''.join(f'{b:08b}' for b in data)

    def uint(self, pos: int, n: int):
        return int(self.bits[pos:pos + n], 2), pos + n

    def ue(self, pos: int):
        """Decode ue(v); returns (value, end_pos)."""
        start = pos
        while self.bits[pos] == '0':
            pos += 1
        pos += 1  # the '1'
        prefix = pos - start - 1
        if prefix:
            suffix = int(self.bits[pos:pos + prefix], 2)
            pos += prefix
        else:
            suffix = 0
        return (1 << prefix) - 1 + suffix, pos


def ue_encode(v: int) -> str:
    b = bin(v + 1)[2:]
    return '0' * (len(b) - 1) + b


def unescape(nal: bytes) -> bytes:
    out = bytearray()
    i = 0
    zeros = 0
    while i < len(nal):
        b = nal[i]
        if zeros >= 2 and b == 3:
            i += 1
            zeros = 0
            continue
        out.append(b)
        zeros = zeros + 1 if b == 0 else 0
        i += 1
    return bytes(out)


def escape(rbsp: bytes) -> bytes:
    out = bytearray()
    zeros = 0
    for b in rbsp:
        if zeros >= 2 and b <= 3:
            out.append(3)
            zeros = 0
        out.append(b)
        zeros = zeros + 1 if b == 0 else 0
    return bytes(out)


def find_confwin_offsets(rbsp: bytes):
    """Parse the SPS up to the conformance window; returns a dict with the
    bit positions of the four ue(v) offset codes and their values."""
    bits = Bits(rbsp)
    pos = 16  # NAL unit header
    _, pos = bits.uint(pos, 4)   # sps_video_parameter_set_id
    max_sub_layers_minus1, pos = bits.uint(pos, 3)
    _, pos = bits.uint(pos, 1)   # sps_temporal_id_nesting_flag
    if max_sub_layers_minus1 != 0:
        raise ValueError('only single-layer SPS supported')
    pos += 96  # profile_tier_level, general part (96 bits for these streams)

    _, pos = bits.ue(pos)        # sps_seq_parameter_set_id
    chroma_format_idc, pos = bits.ue(pos)
    if chroma_format_idc == 3:
        _, pos = bits.uint(pos, 1)  # separate_colour_plane_flag
    pic_width, pos = bits.ue(pos)
    pic_height, pos = bits.ue(pos)
    conf_flag, pos = bits.uint(pos, 1)
    if not conf_flag:
        raise ValueError('SPS has no conformance window')

    offsets = {}
    for name in ('left', 'right', 'top', 'bottom'):
        start = pos
        value, pos = bits.ue(pos)
        offsets[name] = (start, pos, value)

    return bits.bits, offsets, (chroma_format_idc, pic_width, pic_height)


def split_annexb(data: bytes):
    """Split into (start_code, nal_payload) segments."""
    segments = []
    i = 0
    n = len(data)
    while i < n:
        # find start code
        sc_len = 0
        if data[i:i + 3] == b'\x00\x00\x01':
            sc_len = 3
        elif data[i:i + 4] == b'\x00\x00\x00\x01':
            sc_len = 4
        if sc_len:
            j = i + sc_len
            k = j
            while k < n:
                if data[k:k + 3] == b'\x00\x00\x01' or data[k:k + 4] == b'\x00\x00\x00\x01':
                    break
                k += 1
            segments.append((data[i:i + sc_len], data[j:k]))
            i = k
        else:
            i += 1
    return segments


def patch_stream(data: bytes, new_offsets: dict) -> bytes:
    out = bytearray()
    patched = False
    for start_code, nal in split_annexb(data):
        nal_type = (nal[0] >> 1) & 0x3F
        if nal_type == 33 and not patched:  # first SPS
            rbsp = unescape(nal)
            bitstr, offsets, _ = find_confwin_offsets(rbsp)
            # splice from the last offset backwards so positions stay valid
            for name in ('bottom', 'top', 'right', 'left'):
                start, end, _ = offsets[name]
                bitstr = bitstr[:start] + ue_encode(new_offsets[name]) + bitstr[end:]
            # repack bits to bytes (keep trailing rbsp_stop bit alignment)
            pad = (-len(bitstr)) % 8
            bitstr += '0' * pad
            new_rbsp = int(bitstr, 2).to_bytes(len(bitstr) // 8, 'big')
            nal = escape(new_rbsp)
            patched = True
        out += start_code
        out += nal
    if not patched:
        raise ValueError('no SPS found')
    return bytes(out)


def main():
    if len(sys.argv) != 7:
        sys.exit(__doc__)
    src, dst = sys.argv[1], sys.argv[2]
    left, right, top, bottom = (int(x) for x in sys.argv[3:7])
    data = open(src, 'rb').read()
    result = patch_stream(data, dict(left=left, right=right, top=top, bottom=bottom))
    open(dst, 'wb').write(result)


if __name__ == '__main__':
    main()
