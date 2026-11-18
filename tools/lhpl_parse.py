#!/usr/bin/env python3
"""Dump the structure of an LHPL print stream (Ricoh SP 150 family).
Usage: lhpl_parse.py file.prn [jbig_out_prefix]"""
import sys
from functools import reduce

data = open(sys.argv[1], "rb").read()
prefix = sys.argv[2] if len(sys.argv) > 2 else None
start = data.index(b"LANGUAGE=LHPL\r\n") + len(b"LANGUAGE=LHPL\r\n")
print("PJL header:")
for line in data[:start].split(b"\r\n"):
    if line:
        print("   ", line.decode("latin1"))

pos, page = start, 0
while pos < len(data):
    if data.startswith(b"\x1bLH@", pos):
        rec = data[pos:pos + 64]
        cmd = rec[4:6].decode()
        x = reduce(lambda a, b: a ^ b, rec[:63], 0)
        ok = "ok" if x == rec[63] else f"BAD(calc {x:02x})"
        payload = rec[6:63]
        nz = [(i + 6, b) for i, b in enumerate(payload) if b]
        print(f"@{pos:#06x} rec '{cmd}' xor={rec[63]:02x} {ok}")
        print("     nonzero bytes (offset:value):",
              " ".join(f"{o}:{v:02x}" for o, v in nz))
        pos += 64
        if cmd == "sp":
            page += 1
            # LE uint32 view of the payload for convenience
            u32 = [int.from_bytes(rec[o:o + 4], "little") for o in range(6, 62, 4)]
            print("     u32 LE @6..:", u32)
            u16 = [int.from_bytes(rec[o:o + 2], "little") for o in range(6, 62, 2)]
            print("     u16 LE @6..:", u16)
            dlen = int.from_bytes(rec[20:24], "little")
            jb = data[pos:pos + dlen]
            bih = jb[:20]
            xd = int.from_bytes(bih[4:8], "big")
            yd = int.from_bytes(bih[8:12], "big")
            l0 = int.from_bytes(bih[12:16], "big")
            print(f"     JBIG len={dlen} BIH={bih.hex(' ')} Xd={xd} Yd={yd} L0={l0} "
                  f"MX={bih[16]} MY={bih[17]} order={bih[18]:#x} opts={bih[19]:#x}")
            if prefix:
                open(f"{prefix}{page}.jbg", "wb").write(jb)
            pos += dlen
    elif data.startswith(b"\x1b%-12345X", pos):
        print(f"@{pos:#06x} trailer: {data[pos:]!r}")
        break
    else:
        nxt = data.find(b"\x1b", pos)
        print(f"@{pos:#06x} UNKNOWN {data[pos:pos + 32].hex(' ')} ... next ESC at {nxt:#x}")
        pos = nxt if nxt > pos else len(data)
