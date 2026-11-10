#!/usr/bin/env python3
# aout2lda.py - convert a PDP-11 a.out (0407, linked with -Ttext) into
# absolute loader ("papertape", .lda) format, which the QBone/UniBone demo
# loads with "m lp <file>" and the PDP-11 absolute loader reads.
#
#   aout2lda.py <a.out> <out.lda> <load address, octal>
#
# Block: 001 000, byte count (incl. the 6 header bytes) lo hi, address lo hi,
# data, checksum (all bytes of the block plus checksum sum to 0 mod 256).
# The last block has count 6 and holds the start address.
import struct, sys

def main():
    src, dst, base = sys.argv[1], sys.argv[2], int(sys.argv[3], 8)
    d = open(src, "rb").read()
    magic, tsize, dsize, bsize, ssize, entry, _, flag = struct.unpack("<8H", d[:16])
    if magic not in (0o407, 0o410, 0o411):
        sys.exit("%s: not a PDP-11 a.out (magic %o)" % (src, magic))
    image = d[16:16 + tsize + dsize]
    if magic != 0o407:
        sys.exit("%s: link with -N (magic 0407): text and data must be contiguous" % src)
    out = bytearray()
    blk = 256
    for off in range(0, len(image), blk):
        chunk = image[off:off + blk]
        addr = base + off
        hdr = bytes([1, 0]) + struct.pack("<HH", len(chunk) + 6, addr)
        b = hdr + chunk
        out += b + bytes([(-sum(b)) & 0xff])
        out += bytes(4)                     # leader between blocks
    b = bytes([1, 0]) + struct.pack("<HH", 6, entry)
    out += b + bytes([(-sum(b)) & 0xff])
    open(dst, "wb").write(bytes(8) + out + bytes(8))
    end = base + tsize + dsize + bsize
    print("%s: text %o data %o bss %o bytes, load %06o..%06o, start %06o"
          % (dst, tsize, dsize, bsize, base, end - 1, entry))

main()
