# rastertolhpl — native macOS driver for the Ricoh SP 150 series

Apple-Silicon-native CUPS filter for the Ricoh SP 150 / 150w / 150SU / 150SUw
(host-based "GDI" printers). No Ricoh software, no Rosetta.

## Install / remove

```sh
make                 # build the filter
sudo make install    # copy filter to /usr/libexec/cups/filter, add USB queue "Ricoh_SP_150SUw"
make testpage        # print tools/make_testpdf.py output
sudo make uninstall
```

Pipeline: app → macOS `cgpdftoraster` (8-bit sGray, 600 dpi, printable area)
→ `rastertolhpl` (Floyd–Steinberg to 1 bit, JBIG, LHPL framing) → USB backend.

## LHPL stream format (reverse-engineered from the vendor's Linux filter output)

```
ESC%-12345X@PJL JOB NAME=PRINTER
@PJL SET JOBATTR=HST:<host> / USR:<user> / DOC:<title> / DATE:MM/DD/YYYY / TIME:HH:MM:SS
@PJL SET DUPLEX=OFF / MEDIASOURCE=0 / RENDERMODE=GRAYSCALE / RESOLUTION=600 / BITSPERPIXEL=1 / COPIES=n
@PJL ENTER LANGUAGE=LHPL
'sj' record
per page: 'sp' record, JBIG BIE, 'ep' record
ESC%-12345X@PJL EOJ
```

Records are 64 bytes: `ESC L H @ c1 c2`, payload, last byte = XOR of bytes 0..62.

| record | offset | meaning |
|---|---|---|
| sj | 6 | 1 |
| sj | 8 | copies (uncollated) |
| sp | 6 | paper: Letter 1, A4 2, A5 3, A6 4, B5 5, B6 6, Executive 7, 16K 8, Legal 9, A5 LEF 10, B6 LEF 11 |
| sp | 7 | media: Plain 1, Recycled 2, Thick 3, Thin 4 |
| sp | 8–11 | bitmap width px (LE) = (page_px − 198) rounded up to a multiple of 512 |
| sp | 12–15 | bitmap height px (LE) = page_px − 198 |
| sp | 16–19 | width/8 × height (uncompressed bytes) |
| sp | 20–23, 24–27 | JBIG BIE length |
| sp | 42–43 | resolution (600) |
| sp | 44–47 | paper width / height in 0.1 mm (floor of pt × 254 / 72) |
| ep | — | all zero |

The bitmap is the printable area: the page minus 99 px (≈ 4.2 mm) on each side.
JBIG: single layer (T.85 subset), L0 = 128, MX = 0, options LRLTWO, SDRST after
every stripe. The output is byte-identical to the vendor driver for the same bitmap.

Not implemented: manual duplex (`DUPLEX=ON`, `MDPXS=2`, `JOBATTR=PCNT`), 1200 dpi
(width doubles, `RESOLUTION=1200`), the vendor's toner-save/density curves.

`tools/lhpl_parse.py file.prn [prefix]` dumps a stream and extracts the JBIG pages.

## License

GPL-2.0-or-later (see `LICENSE`). JBIG-KIT (`jbig.c`, `jbig_ar.c`) © Markus Kuhn,
GPL-2.0-or-later, taken from foo2zjs. Ricoh is a trademark of Ricoh Co., Ltd.;
this project is not affiliated with or endorsed by Ricoh.
