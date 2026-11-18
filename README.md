# rastertolhpl — native macOS driver for the Ricoh SP 150 series

Apple-Silicon-native tools for the Ricoh SP 150 / 150w / 150SU / 150SUw
(host-based "GDI" printers). No Ricoh software, no Rosetta.

- `rastertolhpl` — CUPS filter + PPD for printing over **USB**
- `sp150ctl` — set the printer's **Wi-Fi / IPv4** settings over USB

**Over Wi-Fi no driver is needed:** the printer's IPP server accepts PDF, so add the
Bonjour entry in System Settings with *Use: Generic PDF Printer*. The USB port only
speaks the host-based LHPL language, which is what `rastertolhpl` is for.

## Install / remove (USB printing)

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

## sp150ctl — Wi-Fi / network settings over USB

```sh
make sp150ctl
./sp150ctl info                                  # Wi-Fi + IPv4 (passphrase never printed)
./sp150ctl scan                                  # networks the printer sees (slow, up to ~1 min)
./sp150ctl wifi-set <SSID>                       # prompts for the passphrase on the TTY
./sp150ctl ip static <addr> <netmask> <gateway>
./sp150ctl ip dhcp
./sp150ctl drain                                 # collect a stuck reply after a timeout
```

The printer only joins 2.4 GHz networks with WPA2 (WPA2/WPA3 mixed mode is fine).
Note that a FRITZ!Box guest network is isolated from the home network, so a Mac
on the guest Wi-Fi will not see the printer.

### Settings protocol ("VOP", from the vendor's RICOH Printer utility)

USB vendor control transfers on the printer interface (interface 1):

- OUT: `bmRequestType 0x41, bRequest 0x4d, wValue 0x3c2b, wIndex 0x0100`
- IN: `bmRequestType 0xc1, bRequest 0x00, wValue 0, wIndex 0x0100`, read
  byte-wise until `4d 3c`, then the rest

Frame: `4d 3c 2b 1a | cmd (u16 LE) | len (u16 LE) = payload + 3 | 13 01 | sub | payload`.
The reply has the same header; byte 10 is the status for set commands (0 = OK) and
the payload starts at byte 11 for get commands.

| cmd/sub | function | payload |
|---|---|---|
| 0x106/0, 0x106/1 | get / set Wi-Fi block | 180 bytes: [0] enabled, [4] security (3 = WPA2-PSK seen), [8..39] SSID, [40..103] passphrase |
| 0x106/0xff | set Wi-Fi without reply (used when connected over Wi-Fi) | same 180 bytes |
| 0x106/7 | access point list | 10 × 34 bytes: SSID[32] + 2 flag bytes |
| 0x106/8 | Wi-Fi status | 1 byte (1 seen while associated) |
| 0x105/0, 0x105/1 | get / set IPv4 | 128 bytes, first 14 used: [0] mode (1 = static; 0 = DHCP per vendor UI order), [2..5] address, [6..9] netmask, [10..13] gateway |
| 0x105/2, 0x105/3 | get / set IPv6 | 340 bytes |
| 0x107/x | device settings (toner end, power save, power off, admin password, region) | 1 or 32 bytes |

## License

GPL-2.0-or-later (see `LICENSE`). JBIG-KIT (`jbig.c`, `jbig_ar.c`) © Markus Kuhn,
GPL-2.0-or-later, taken from foo2zjs. Ricoh is a trademark of Ricoh Co., Ltd.;
this project is not affiliated with or endorsed by Ricoh.
