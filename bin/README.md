# Prebuilt HVI binaries

Ready-to-run CP/M `.COM` files, one per terminal family (v2.8.0). Copy the
one matching your terminal to your CP/M system and run `HVI FILENAME`.

| File | Terminal | Screen |
|------|----------|--------|
| `HVI.COM` | ANSI / VT100 / xterm / most emulators | auto-detected (default 80×24) |
| `HVIVT52.COM` | DEC VT52 | 80×24 |
| `HVIH19.COM` | Heath / Zenith H19 / H89 | 80×24 |
| `HVIADM3.COM` | Lear Siegler ADM-3A / 3A+ | 80×24 |
| `HVITVI.COM` | Televideo 912 / 920 / 925 / 950 | 80×24 |
| `HVIWY50.COM` | Wyse 50 | 80×24 |
| `HVIHZ15.COM` | Hazeltine 1500 | 80×24 |
| `HVIOSB1.COM` | Osborne 1 | 52×24 |

Only the ANSI build queries the terminal for its size; the others use a fixed
size compiled in (80×24, Osborne 52×24). If your terminal is a different size,
rebuild from source with `-DTERM_ROWS=` / `-DTERM_COLS=` (see the top-level
README, *Building for a specific terminal*).

Arrow keys work on the ANSI, VT52 and H19 builds; use `h j k l` on the others.

These files are transferred to CP/M in **binary** mode (XMODEM/ZMODEM or a
disk image) — do not apply any LF↔CRLF text translation. Built with HI-TECH C
V3.09; see the top-level README to reproduce them from source.
