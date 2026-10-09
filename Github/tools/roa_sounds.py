"""Reads Rivals of Aether's sound table (data.win SOND) and its audio groups (audiogroupN.dat AUDO), and can extract sounds to .ogg.

    python tools/roa_sounds.py list [filter]            list sounds (name, group, audio id, flags, size)
    python tools/roa_sounds.py extract OUTDIR [prefix]   write every sound whose name starts with prefix (default "music_")
"""
import os
import struct
import sys

ROA = os.environ.get("ROA_DIR", r"G:\games\steamapps\common\Rivals of Aether")


def chunks(buf):
    assert buf[:4] == b"FORM"
    pos, end = 8, 8 + struct.unpack_from("<I", buf, 4)[0]
    out = {}
    while pos + 8 <= end:
        cid = buf[pos:pos + 4].decode("latin1")
        size = struct.unpack_from("<I", buf, pos + 4)[0]
        out[cid] = (pos + 8, size)
        pos += 8 + size
    return out


def cstr(buf, off):
    e = buf.index(b"\0", off)
    return buf[off:e].decode("utf-8", "replace")


def read_sounds(win):
    ch = chunks(win)
    base, size = ch["SOND"]
    n = struct.unpack_from("<I", win, base)[0]
    offs = struct.unpack_from("<%dI" % n, win, base + 4)
    sounds = []
    for o in offs:
        name_p, flags, type_p, file_p, effects, vol, pitch, a, b = struct.unpack_from("<IIIIIffii", win, o)
        sounds.append({"name": cstr(win, name_p), "flags": flags, "type": cstr(win, type_p) if type_p else "", "file": cstr(win, file_p) if file_p else "",
                       "group": a, "audio": b, "offset": o})
    return sounds


def read_audo(buf):
    ch = chunks(buf)
    if "AUDO" not in ch:
        return []
    base, size = ch["AUDO"]
    n = struct.unpack_from("<I", buf, base)[0]
    offs = struct.unpack_from("<%dI" % n, buf, base + 4)
    ents = []
    for o in offs:
        ln = struct.unpack_from("<I", buf, o)[0]
        ents.append(buf[o + 4:o + 4 + ln])
    return ents


_cache = {}


def group_entries(gid):
    if gid in _cache:
        return _cache[gid]
    path = os.path.join(ROA, "data.win" if gid == 0 else "audiogroup%d.dat" % gid)
    with open(path, "rb") as f:
        buf = f.read()
    ents = read_audo(buf)
    _cache[gid] = ents
    return ents


def sound_data(s):
    ents = group_entries(s["group"])
    if 0 <= s["audio"] < len(ents):
        return ents[s["audio"]]
    return None


def main():
    with open(os.path.join(ROA, "data.win"), "rb") as f:
        win = f.read()
    sounds = read_sounds(win)
    cmd = sys.argv[1] if len(sys.argv) > 1 else "list"
    if cmd == "list":
        flt = sys.argv[2] if len(sys.argv) > 2 else ""
        for s in sounds:
            if flt in s["name"]:
                d = sound_data(s)
                print("%-40s grp=%d aud=%d flags=%#x size=%s head=%s" % (s["name"], s["group"], s["audio"], s["flags"], len(d) if d else None, d[:4] if d else None))
    elif cmd == "extract":
        out = sys.argv[2]
        prefix = sys.argv[3] if len(sys.argv) > 3 else "music_"
        os.makedirs(out, exist_ok=True)
        n = 0
        for s in sounds:
            if not s["name"].startswith(prefix):
                continue
            d = sound_data(s)
            if not d:
                continue
            ext = ".ogg" if d[:4] == b"OggS" else (".wav" if d[:4] == b"RIFF" else ".bin")
            with open(os.path.join(out, s["name"] + ext), "wb") as f:
                f.write(d)
            n += 1
        print("extracted", n)


if __name__ == "__main__":
    main()
