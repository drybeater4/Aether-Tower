"""Convert a raw 960x540 RGBA dump (from fake_pt) to PNG on a checkerboard so transparency is visible."""
import sys, zlib, struct
src, dst = sys.argv[1], sys.argv[2]
W, H = 960, 540
d = open(src, "rb").read()
rows = bytearray()
for y in range(H):
    rows.append(0)
    for x in range(W):
        r, g, b, a = d[(y*W+x)*4:(y*W+x)*4+4]
        bg = 200 if ((x // 16 + y // 16) & 1) else 150
        rows += bytes(((r*a + bg*(255-a))//255, (g*a + bg*(255-a))//255, (b*a + bg*(255-a))//255))
def chunk(t, c): return struct.pack(">I", len(c)) + t + c + struct.pack(">I", zlib.crc32(t + c) & 0xffffffff)
open(dst, "wb").write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", W, H, 8, 2, 0, 0, 0)) + chunk(b"IDAT", zlib.compress(bytes(rows))) + chunk(b"IEND", b""))
