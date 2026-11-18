#!/usr/bin/env python3
"""Write a 1-page A4 vector test PDF (text, grey ramp, rulers, border).
Usage: make_testpdf.py out.pdf"""
import sys

W, H = 595.28, 841.89
MM = 72 / 25.4
ops = []


def text(x, y, size, s, font="F1"):
    s = s.replace("\\", "\\\\").replace("(", "\\(").replace(")", "\\)")
    ops.append(f"BT /{font} {size} Tf {x:.2f} {y:.2f} Td ({s}) Tj ET")


# border 10 mm in from the paper edge + corner crosshairs
m = 10 * MM
ops.append(f"0 G 0.6 w {m:.2f} {m:.2f} {W - 2 * m:.2f} {H - 2 * m:.2f} re S")
for cx, cy in [(m, m), (W - m, m), (m, H - m), (W - m, H - m)]:
    ops.append(f"{cx - 15:.2f} {cy:.2f} m {cx + 15:.2f} {cy:.2f} l S {cx:.2f} {cy - 15:.2f} m {cx:.2f} {cy + 15:.2f} l S")

y = H - 30 * MM
text(25 * MM, y, 22, "Ricoh SP 150SUw - native macOS driver", "F2")
y -= 12 * MM
for line in [
    "This page was rendered by macOS (cgpdftoraster) and encoded by rastertolhpl,",
    "an Apple-Silicon-native filter. No Ricoh software and no Rosetta were involved.",
    "The quick brown fox jumps over the lazy dog. 0123456789 !?&%$@#*()[]{}",
]:
    text(25 * MM, y, 11, line)
    y -= 6 * MM
for size in (6, 8, 10, 14):
    text(25 * MM, y, size, f"{size} pt: Sphinx of black quartz, judge my vow.")
    y -= (size + 6) * 0.3528 * MM * 2.2

# grey ramp 0..100 %
y -= 8 * MM
text(25 * MM, y + 14 * MM, 9, "Grey ramp 0% ... 100%")
for i in range(11):
    g = 1 - i / 10
    x = 25 * MM + i * 15 * MM
    ops.append(f"{g:.2f} g {x:.2f} {y:.2f} {15 * MM:.2f} {12 * MM:.2f} re f")
ops.append(f"0 G 0.4 w {25 * MM:.2f} {y:.2f} {165 * MM:.2f} {12 * MM:.2f} re S 0 g")

# line-width ladder
y -= 25 * MM
text(25 * MM, y + 18 * MM, 9, "Line widths 0.1 / 0.25 / 0.5 / 1 / 2 / 4 pt")
x = 25 * MM
for lw in (0.1, 0.25, 0.5, 1, 2, 4):
    ops.append(f"{lw} w {x:.2f} {y:.2f} m {x:.2f} {y + 15 * MM:.2f} l S")
    x += 10 * MM

# rulers: 10 cm with mm ticks, 4 inch with 1/8 inch ticks
y -= 20 * MM
text(25 * MM, y + 8 * MM, 9, "100 mm ruler (check with a real ruler)")
ops.append(f"0.5 w {25 * MM:.2f} {y:.2f} m {125 * MM:.2f} {y:.2f} l S")
for i in range(101):
    t = 5 * MM if i % 10 == 0 else (3 * MM if i % 5 == 0 else 1.5 * MM)
    xx = 25 * MM + i * MM
    ops.append(f"0.3 w {xx:.2f} {y:.2f} m {xx:.2f} {y - t:.2f} l S")
    if i % 10 == 0:
        text(xx - 2, y - 9 * MM, 7, str(i // 10))
y -= 25 * MM
text(25 * MM, y + 8 * MM, 9, "4 inch ruler")
ops.append(f"0.5 w {25 * MM:.2f} {y:.2f} m {25 * MM + 288:.2f} {y:.2f} l S")
for i in range(33):
    t = 5 * MM if i % 8 == 0 else (3 * MM if i % 4 == 0 else 1.5 * MM)
    xx = 25 * MM + i * 9
    ops.append(f"0.3 w {xx:.2f} {y:.2f} m {xx:.2f} {y - t:.2f} l S")

# circle (Bezier) for curve smoothness
cx, cy, r, k = W - 55 * MM, 70 * MM, 25 * MM, 0.5523
ops.append(f"1 w {cx + r:.2f} {cy:.2f} m "
           f"{cx + r:.2f} {cy + k * r:.2f} {cx + k * r:.2f} {cy + r:.2f} {cx:.2f} {cy + r:.2f} c "
           f"{cx - k * r:.2f} {cy + r:.2f} {cx - r:.2f} {cy + k * r:.2f} {cx - r:.2f} {cy:.2f} c "
           f"{cx - r:.2f} {cy - k * r:.2f} {cx - k * r:.2f} {cy - r:.2f} {cx:.2f} {cy - r:.2f} c "
           f"{cx + k * r:.2f} {cy - r:.2f} {cx + r:.2f} {cy - k * r:.2f} {cx + r:.2f} {cy:.2f} c S")
text(25 * MM, 15 * MM, 8, "Bottom edge text: should be fully visible above the border.")

content = "\n".join(ops).encode("latin-1")
objs = [
    b"<< /Type /Catalog /Pages 2 0 R >>",
    b"<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
    (f"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 {W} {H}] "
     f"/Resources << /Font << /F1 5 0 R /F2 6 0 R >> >> /Contents 4 0 R >>").encode(),
    b"<< /Length %d >>\nstream\n" % len(content) + content + b"\nendstream",
    b"<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>",
    b"<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica-Bold >>",
]
out = bytearray(b"%PDF-1.4\n")
offs = []
for i, o in enumerate(objs, 1):
    offs.append(len(out))
    out += b"%d 0 obj\n" % i + o + b"\nendobj\n"
xref = len(out)
out += b"xref\n0 %d\n0000000000 65535 f \n" % (len(objs) + 1)
for o in offs:
    out += b"%010d 00000 n \n" % o
out += b"trailer\n<< /Size %d /Root 1 0 R >>\nstartxref\n%d\n%%%%EOF\n" % (len(objs) + 1, xref)
open(sys.argv[1], "wb").write(out)
