# HVI - VI Clone for CP/M

**Version 2.8.1**

A lightweight VI-compatible editor for CP/M 2.2 and CP/M 3.0, written in
HI-TECH C. Uses a gap buffer for efficient editing. Ships as an ANSI/VT100
build with automatic terminal-size detection, plus compile-time builds for
the common early-80s CP/M terminals (VT52, Heath/Zenith H19, Lear Siegler
ADM-3A, Kaypro 83/84, Televideo 9xx, Wyse 50, Hazeltine 1500, Osborne 1).
Implements most
of the basic movement and editing commands including the `.` operator for
repeat. Also has a single level undo.

**Author:** Juan Orlandini  
**License:** MIT

---

## Building

> **Prebuilt binaries** for every terminal family live in [`bin/`](bin/) —
> grab the `.COM` matching your terminal and skip straight to *Usage*.

### Requirements

- HI-TECH C Compiler for Z80/CP/M (V3.09 or later)
- CP/M 2.2 or CP/M 3.0 system with at least 48K TPA

### Source Files

```
cstart.as  hvi.c  hvi.h  termcfg.h  gap.c  term.c  screen.c  emove.c
edit.c  erepeat.c  ex.c  util.c  cpmio.c
```

### Build Steps (on CP/M)

1. Copy all source files to a CP/M disk/drive.

2. Prepare `LX.LIB` (one-time setup — removes `csv.obj` to avoid symbol conflicts
   with `cstart.as`):

```
PIP LX.LIB=LIBC.LIB
LIBR d LX.LIB csv.obj
```

3. Compile all source files:

```
C -CPM -O -C cstart.as
C -CPM -O -C hvi.c gap.c term.c screen.c emove.c edit.c erepeat.c ex.c util.c cpmio.c
```

4. Link (the backslash `\` continues the command past CP/M's 128-character line limit):

```
l
-Ptext=100H,data,bss -C100H -oh.com CSTART.OBJ CPMIO.OBJ UTIL.OBJ \
GAP.OBJ TERM.OBJ SCREEN.OBJ EMOVE.OBJ EREPEAT.OBJ EX.OBJ EDIT.OBJ HVI.OBJ LX.LIB
```

5. Rename the output:

```
REN HVI.COM=H.COM
```

> **Note:** The HI-TECH C linker command is `l` (lowercase L). The `-Ptext=100H,data,bss`
> flag is required to place data and BSS sections correctly. `cstart.as` must be
> first in the link order; it replaces the standard `CRTCPM.OBJ`.

### Building for a specific terminal

The steps above build `HVI.COM`, the **ANSI/VT100** edition (dynamic size
query, full feature set). To target a non-ANSI terminal, add its `-DTERM_xxx`
define to *every* compile (the family header `termcfg.h` is pulled into every
module, so a clean rebuild is required) and rename the result:

```
C -CPM -O -DTERM_VT52 -C cstart.as
C -CPM -O -DTERM_VT52 -C hvi.c gap.c term.c screen.c emove.c edit.c erepeat.c ex.c util.c cpmio.c
... link as above ...
REN HVIVT52.COM=H.COM
```

| Define | Binary | Terminal | Geometry |
|---|---|---|---|
| *(none)* | `HVI.COM` | ANSI / VT100 / xterm | detected, default 80×24 |
| `-DTERM_VT52` | `HVIVT52.COM` | DEC VT52 | 80×24 |
| `-DTERM_H19` | `HVIH19.COM` | Heath / Zenith H19 / H89 | 80×24 |
| `-DTERM_ADM3A` | `HVIADM3.COM` | Lear Siegler ADM-3A / 3A+ | 80×24 |
| `-DTERM_KPRO` | `HVIKPRO.COM` | Kaypro 83 / 84 | 80×24 |
| `-DTERM_TVI` | `HVITVI.COM` | Televideo 912/920/925/950 | 80×24 |
| `-DTERM_WYSE50` | `HVIWY50.COM` | Wyse 50 | 80×24 |
| `-DTERM_HAZ1500` | `HVIHZ15.COM` | Hazeltine 1500 | 80×24 |
| `-DTERM_OSB1` | `HVIOSB1.COM` | Osborne 1 | 52×24 |

The non-ANSI builds use a **fixed** screen size (they send no size query),
80×24 by default (Osborne 52×24) to match the common CP/M terminal. If your
unit differs, override at compile time with `-DTERM_ROWS=` and/or
`-DTERM_COLS=`, e.g. `C -CPM -O -DTERM_TVI -DTERM_ROWS=25 -C ...`.

A `Makefile` target wraps this as `make clean; make TERMDEF=-DTERM_VT52`.

### Cross-Compilation (Linux/macOS host)

If using the HI-TECH Z80 cross-compiler on a Unix host, use the same flags and
link order as above, then transfer `hvi.com` to your CP/M system via XMODEM,
ZMODEM, or disk image.

---

## Usage

```
HVI [filename]
```

- `filename` — file to open (created if it does not exist)

Any filename — on the command line or in `:w`, `:e`, `:r` — may carry a
ZCPR-style `du:` drive/user prefix: `B:FILE.TXT` (drive B), `3:FILE.TXT`
(user area 3), `B3:FILE.TXT` (both).  Drives A–P, user areas 0–15.
Unprefixed names use the drive and user area HVI was started from.

---

## Supported Commands

### Normal Mode — Movement

| Key        | Action                              |
|------------|-------------------------------------|
| `h` / `←`  | Move left one character             |
| `l` / `→`  | Move right one character            |
| `j` / `↓`  | Move down one line                  |
| `k` / `↑`  | Move up one line                    |
| `Enter`    | Move to first non-blank of next line |
| `w`        | Forward to start of next word       |
| `b`        | Backward to start of previous word  |
| `e`        | Forward to end of word              |
| `0`        | Move to beginning of line           |
| `^`        | Move to first non-blank of line     |
| `$`        | Move to end of line                 |
| `G`        | Go to last line (or line N with count) |
| `gg`       | Go to first line (or line N: `5gg`) |
| `Ctrl-F`   | Scroll forward one page; cursor lands in the middle of the new page. No-op if already at the end of the file. |
| `Ctrl-B`   | Scroll backward one page; cursor lands in the middle of the new page. No-op if already at the beginning of the file. |
| `Ctrl-D`   | Scroll forward half page            |
| `Ctrl-U`   | Scroll backward half page           |

### Normal Mode — Insert / Append

| Key | Action                                    |
|-----|-------------------------------------------|
| `i` | Insert before cursor                      |
| `a` | Append after cursor                       |
| `I` | Insert at beginning of line               |
| `A` | Append at end of line                     |
| `o` | Open new line below, enter insert mode    |
| `O` | Open new line above, enter insert mode    |
| `s` | Substitute character(s) (delete + insert) |
| `S` | Substitute entire line                    |

### Normal Mode — Delete / Change

| Key  | Action                              |
|------|-------------------------------------|
| `x`  | Delete character under cursor       |
| `X`  | Delete character before cursor      |
| `dd` | Delete current line                 |
| `dw` | Delete word forward                 |
| `db` | Delete word backward                |
| `d$` | Delete to end of line               |
| `d0` | Delete to beginning of line         |
| `dG` | Delete to end of file               |
| `` d`a `` | Delete to mark `a` (exclusive; `` d`` `` = to previous jump) |
| `D`  | Delete to end of line (same as `d$`)|
| `cc` | Change current line                 |
| `cw` | Change word                         |
| `c$` | Change to end of line               |
| `` c`a `` | Change to mark `a` (exclusive)      |
| `>>` / `<<` | Shift line(s) one tab stop right / left (count and all motions work: `>j`, `>G`, `` >`a ``, ...) |
| `C`  | Change to end of line (same as `c$`)|
| `r`  | Replace single character            |
| `J`  | Join line below to current line     |
| `~`  | Toggle case of character            |

### Normal Mode — Yank and Put

| Key  | Action                                        |
|------|-----------------------------------------------|
| `yy` | Yank (copy) current line                      |
| `Y`  | Yank current line (same as `yy`)              |
| `yw` | Yank word                                     |
| `y$` | Yank to end of line                           |
| `` y`a `` | Yank to mark `a` (exclusive)             |
| `p`  | Put (paste) after cursor / below current line |
| `P`  | Put before cursor / above current line        |

### Normal Mode — Search

| Key  | Action                         |
|------|--------------------------------|
| `/`  | Search forward for pattern     |
| `?`  | Search backward for pattern    |
| `n`  | Repeat last search             |
| `N`  | Repeat last search in reverse  |

Search is case-insensitive plain substring (no regular expressions). In a large
file, search scans the entire file — not just the loaded buffer. When the match
is found in an unloaded section HVI reloads the window around it automatically.
`search hit BOTTOM, continuing at TOP` (or `TOP … BOTTOM`) is shown only when
the search genuinely wraps past the end (or beginning) of the file.

### Normal Mode — Marks

| Key       | Action                                                  |
|-----------|---------------------------------------------------------|
| `m{a-z}`  | Set mark `{a-z}` at the cursor position                 |
| `` `{a-z} `` | Jump to mark `{a-z}` (exact line **and** column)     |
| `` `` ``  | Jump back to the position before the last jump          |
| `` d`{a-z} `` | Delete from the cursor to the mark (also `c`, `y`, and `` `` ``) |

A jump — for the purpose of `` `` `` — is `` ` ``, `G`, `gg`, `:N`, `:$`, or a
successful search (`/`, `?`, `n`, `N`). Pressing `` `` `` twice toggles between
the two positions.

Marks follow the text as it is edited: inserting or deleting above a mark
shifts it so it stays on the same character. A mark is cleared when the text
it points to is deleted, and all marks are cleared when a new file is loaded
(`:e`) or, in a large file, when the sliding window moves to a different part
of the file. Jumping to a cleared or never-set mark reports `Mark not set`.

Marks also work as operator motions: `` d`a ``, `` c`a `` and `` y`a ``
operate on the exclusive character range between the cursor and the mark
(either side may come first), and `` d`` `` uses the previous-jump
position. Operating toward a cleared or never-set mark reports
`Mark not set` and leaves the buffer untouched. `.` repeats a `` d`x ``
or `` c`x `` change against the mark's current (edit-adjusted) position.

### Normal Mode — Character Search (current line)

| Key      | Action                                              |
|----------|-----------------------------------------------------|
| `f{c}`   | Move to next occurrence of character `c` on line    |
| `F{c}`   | Move to previous occurrence of character `c` on line|
| `;`      | Repeat last `f` or `F` in the same direction        |
| `,`      | Repeat last `f` or `F` in the opposite direction    |

Both repeat commands accept a count prefix (e.g. `3;` skips to the third next match).

### Normal Mode — Miscellaneous

| Key      | Action                        |
|----------|-------------------------------|
| `.`      | Repeat last change            |
| `u`      | Undo last change              |
| `:`      | Enter ex command mode         |
| `Ctrl-L` | Redraw screen                 |

### Insert Mode

| Key        | Action                              |
|------------|-------------------------------------|
| (any char) | Insert character                    |
| `Enter`    | Insert newline                      |
| `Backspace`| Delete previous character           |
| `Ctrl-H`   | Delete previous character           |
| `Ctrl-W`   | Delete previous word                |
| `Ctrl-U`   | Delete to start of line             |
| `↑↓←→`    | Move cursor (stay in insert mode)   |
| `ESC`      | Return to normal mode               |

### Ex Commands

| Command      | Action                                      |
|--------------|---------------------------------------------|
| `:w`         | Write (save) current file                   |
| `:w filename`| Write to named file                         |
| `:q`         | Quit (fails if unsaved changes)             |
| `:q!`        | Quit without saving                         |
| `:wq`        | Write and quit                              |
| `:wq!`       | Write and quit (force)                      |
| `:x`         | Write if modified, then quit                |
| `:x!`        | Write and quit (force)                      |
| `:e filename`| Abandon current buffer and edit named file  |
| `:e! filename`| Abandon modified buffer and edit named file |
| `:r filename`| Read file and insert after current line     |
| `:N`         | Go to line number N                         |
| `:$`         | Go to last line                             |
| `:[range]s/old/new/[g]` | Substitute: plain text, case-sensitive; `g` = all occurrences in a line |
| `:[range]>` / `:[range]<` | Shift the range's lines one tab stop right / left |

`:s` matches plain text (no regular expressions), **case-sensitively**.
Without `g` the first occurrence in each line is replaced. The range is
one address or `addr1,addr2` (either order): a line number, `.` (the
cursor's line), `$` (the last line), or `'{a-z}` (the mark's line);
the default is the cursor's line. The trailing `/` may be omitted, and
an empty `new` deletes `old`. The cursor moves to the last substituted
line. A substitute cannot be undone with `u` (the edits are scattered;
the single-slot undo record is invalidated).

---

## Count Prefix

Most commands accept a numeric count prefix:

- `5j` — move down 5 lines
- `3w` — move forward 3 words
- `2dd` — delete 2 lines
- `10G` — go to line 10

---

## Status Line

The status line shows:

```
"filename" [+]
```

- `[+]` appears when the buffer has unsaved changes

Whenever HVI reads a chunk of a large file from disk it briefly shows
`[Loading...]` on the status line, replaced by the normal display once the
screen refreshes.

---

## Large File Support

HVI can open and edit files that are larger than available RAM. The in-memory
content is a sliding window: only a portion of the file is in memory at any
time, and the window shifts as you scroll.

### How the window works

At startup HVI allocates the largest contiguous free block in the TPA (up to
`BUF_MAX` bytes of content) and loads as much of the file as fits. It records
where loading stopped (`tail_offset`) and the source filename (`tail_file`).

As you scroll forward with `j`, `Ctrl-D`, or `Ctrl-F`, HVI loads the next
4 KB chunk from disk automatically. When the buffer is full, the same number of
bytes are discarded from the beginning to make room. The discarded content is
always before the cursor so the cursor is never lost.

Scrolling backward with `Ctrl-B` reloads a window from an earlier position in
the file via a direct BDOS seek (no sequential scan needed).

### Editing across the window boundary

Before shifting the window in either direction, HVI automatically saves all
in-memory edits to a swap file (`HVISWP.TMP`). Subsequent reads come from the
swap file, which contains the complete, up-to-date file content. This means
edits made at the beginning of a file are never lost when you scroll to the
end, and vice versa.

If the gap buffer fills up during editing, HVI saves to the swap file and
reloads a smaller window around the cursor — you can keep typing without
interruption.

### Searching in a large file

`/pattern` and `?pattern` search the entire file, not just the in-memory window.
The search proceeds in three phases:

1. Scan the in-memory buffer from the cursor forward (or backward).
2. If no unwrapped match is found, scan the unloaded file sections sequentially
   using direct BDOS reads — the tail (bytes after the buffer) for forward
   search, the head (bytes before the buffer) for backward search.
3. When a match is found in an unloaded section, HVI reloads the window around
   it and places the cursor there.

The `search hit BOTTOM, continuing at TOP` message is shown only when the match
required crossing the true end-of-file boundary, not merely the buffer boundary.
A match in the unloaded tail of a forward search is reported without any wrap
message because it is genuinely ahead of the cursor in file order.

### Navigating to a specific line in a large file

`nG` (e.g. `1000G`) works correctly in large files. HVI scans the source file
from the beginning to locate the byte offset of line N, positions the window
there, and places the cursor precisely on that line. Navigation speed is
proportional to the line number, not the file size.

### Saving large files

On every `:w` save, HVI reconstructs the full file:

1. Bytes before the current window (from `tail_file`) are copied verbatim.
2. The in-memory buffer is written in `CR+LF` format.
3. Bytes after the current window (from `tail_file`) are appended verbatim.
4. A `Ctrl-Z` (0x1A) EOF marker is written last.

When saving back to the same file that holds the unloaded portions, HVI writes
to `HVITMP.TMP` first, then renames, so the source is never overwritten before
it is fully read.

### Temporary files used

| File | Purpose |
|------|---------|
| `HVISWP.TMP` | Swap file written before any window shift or buffer overflow |
| `HVITMP.TMP` | Intermediate used when saving back to the tail source file |

---

## Terminal Requirements

The terminal type is selected **at compile time** (see *Building for a
specific terminal* above); each build emits only the control codes its
terminal understands, so nothing garbles a dumb terminal.

### ANSI / VT100 build (default `HVI.COM`)

Uses ANSI/VT100 escape sequences and queries the terminal for its actual
size at startup: it sends `ESC[999;999H` (cursor to extreme bottom-right)
followed by `ESC[6n` (ANSI cursor-position report) and parses the reply.
The response is read byte-by-byte via BDOS function 6, bypassing canonical
buffering. If the terminal does not respond within the polling timeout,
HVI silently falls back to 80 × 24 — it will not hang.

Compatible terminals: VT100, VT220, xterm, ANSI.SYS, and most modern
terminal emulators connected via a serial port.

### Non-ANSI builds

The other families use a **fixed** screen size (no size query is sent) and
each emit that terminal's native codes. Capabilities a terminal lacks are
handled by software fallbacks so editing stays correct:

| Build | Addressing | Clear-to-EOL | 1-line scroll | Reverse video | Arrow keys |
|---|---|---|---|---|---|
| VT52 | `ESC Y r c` | `ESC K` | full repaint | — | `ESC A`–`D` |
| H19 | `ESC Y r c` | `ESC K` | insert/delete line | `ESC p`/`ESC q` | `ESC A`–`D` |
| ADM-3A | `ESC = r c` | *(space-padded)* | full repaint | — | *(hjkl)* |
| Kaypro 83/84 | `ESC = r c` | *(space-padded)* | insert/delete line | — | *(hjkl)* |
| Televideo 9xx | `ESC = r c` | `ESC T` | insert/delete line | — | *(hjkl)* |
| Wyse 50 | `ESC = r c` | `ESC T` | insert/delete line | — | *(hjkl)* |
| Hazeltine 1500 | `~ DC1 c r` | `~ SI` | insert/delete line | — | *(hjkl)* |
| Osborne 1 | `ESC = r c` | `ESC T` | full repaint | — | *(hjkl)* |

Notes:

- Arrow keys are recognized only on the ANSI, VT52 and H19 builds. On the
  others the arrow keys send bare control characters that collide with vi
  bindings (`^L` redraw, etc.), so they are left unmapped — use `h j k l`.
- The default fixed size is 80 × 24 (Osborne 1 is 52 × 24), matching the
  common CP/M terminal. If your unit differs, build with `-DTERM_ROWS=` /
  `-DTERM_COLS=` to match.
- **ADM-3A**: clearing the screen uses `^Z`, which requires the clear-screen
  strap to be enabled on the terminal.
- **Kaypro 83/84**: an ADM-3A superset — same `ESC =` addressing and `^Z`
  clear, plus hardware insert-line (`ESC E`) / delete-line (`ESC R`), so
  scrolling and paging update just the newly exposed rows instead of
  repainting the whole screen. The `/84`'s screen attributes (reverse /
  blink / hi-lo) are not used, and the Kaypro cursor keys (`^H ^J ^K ^L`,
  which collide with `hjkl` and `^L` redraw) are left unmapped — use
  `h j k l`.
- **Hazeltine 1500**: reserves `~` as its command lead-in and cannot display
  it, so HVI shows `^` in its place on screen (files keep the real `~`).
- The escape codes for the non-ANSI families are drawn from the terminals'
  manuals; verify against your specific model/firmware, as some codes (e.g.
  Televideo insert/delete-char on the 912/920, Wyse native vs. emulation
  mode) vary by revision.

---

## File Format

HVI reads files in binary mode. A `CR` is stripped only where it forms a
`CR+LF` line-ending pair; a bare `CR` in the middle of a line is preserved
as content. On save, each `LF` is written as `CR+LF` per CP/M convention
(so a preserved bare `CR` round-trips unchanged), and the file is terminated
with `Ctrl-Z` (0x1A) per CP/M file format rules.

---

## Performance

HVI is designed for 9600 baud serial terminals. All screen updates are sized to
the minimum needed for the operation:

| Operation | Output sent |
|-----------|-------------|
| `h`, `l`, `0`, `^`, `$`, `f`, `F`, `;`, `,` | Cursor reposition only — no text redrawn, no status bar update |
| `j`, `k`, `Enter`, `w`, `b`, `e`, `/`, `?`, `n`, `N`, `gg`, `nG` | Terminal scroll + only the rows that came into view, or cursor reposition only when no scroll needed |
| `r` replace character | The replaced cell only (2–4 bytes); falls back to a row redraw around tabs or the last column |
| `~` | The toggled characters emitted in place |
| `x`, `X`, `s`, `D`, `C`, `dw`, `cw`, `d$`, charwise `p`/`P`, `u` | One screen row (the edited line) — when the line does not wrap |
| `ESC` leaving insert mode | Status bar only (insert mode keeps the text current as you type) |
| `J`, `o`, `O`, `dd`, linewise `p`/`P`, Enter in insert mode | Rows from cursor to bottom redrawn (rows above cursor skipped) |
| `Ctrl-D`, `Ctrl-U` | Terminal scroll + only the ~11 rows that came into view; cursor reposition only while the cursor stays in view |
| `G`, `Ctrl-F`, `Ctrl-B`, `:e`, `:r` | Full screen redrawn |

Edits that stay on one unwrapped logical line repaint exactly one screen row;
anything that inserts or removes newlines repaints from the cursor down.  On a
24-row terminal that is ~80 bytes instead of ~600–1200 per keystroke (at 9600
baud, ~0.5–1 second saved per edit).

The status bar is rewritten only when its text actually changes; movement
keys never touch it, and repeated edits reuse the bar already on screen.

---

## Known Limitations

- Single-level undo only (`u` undoes the most recent change)
- Saving appends a final newline (vi convention) when the buffer does not end with one
- `nG` in a large file scans sequentially from byte 0 — navigating to line 1000 reads the first ~1000 lines from disk (fast); navigating to line 29000 in a 30K-line file reads most of the file (slow)
- No visual/block selection mode
- No macro recording/playback
- No window splitting
- No regex search — plain substring match only (case-insensitive)

---

## Changes

### 2.8.0 → 2.8.1

Adds a dedicated Kaypro build and fixes a latent flaw that made hardware
insert/delete-line scrolling impossible to enable on a new family.

- **New: Kaypro 83/84 build** (`-DTERM_KPRO` → `HVIKPRO.COM`). The Kaypro
  screen is an ADM-3A superset: same `ESC =` cursor addressing and `^Z`
  clear/home, plus hardware insert-line (`ESC E`) and delete-line
  (`ESC R`). Scrolling and paging now update only the newly exposed rows
  instead of repainting the whole screen (222 records). Reported in
  [issue #5](https://github.com/tachijuan/hvi/issues/5).
- **Fix: `TERM_HAS_SCROLL` is now derived, not hand-declared.** The
  insert/delete-line scroll fast path in `term.c`/`screen.c` keys off
  `TERM_HAS_SCROLL`, but a family had to remember to define *both*
  `TERM_HAS_ILDL` and `TERM_HAS_SCROLL` — declaring ins/del-line alone
  silently compiled it dead and fell back to full repaints (the original
  Kaypro symptom). `TERM_HAS_SCROLL` is now derived automatically from
  `TERM_HAS_REGION`/`TERM_HAS_ILDL` in `termcfg.h`, so a family only
  declares its mechanism. No change to the existing builds (verified: the
  ANSI, H19, TVI, Wyse and Hazeltine escape streams are unchanged).
- The Kaypro cursor keys (`^H ^J ^K ^L`) collide with `hjkl`/`^L` redraw
  and would need BIOS keymap patching to remap, so they are left unmapped
  as on the other non-ANSI builds — use `h j k l`.

### 2.7.2 → 2.8.0

Compile-time terminal selection: the ANSI/VT100 build is unchanged, and
six new fixed-size builds target the common early-80s CP/M terminals.

- **New: per-terminal builds** selected with `-DTERM_xxx` (see *Building
  for a specific terminal*): VT52, Heath/Zenith H19, Lear Siegler ADM-3A,
  Televideo 9xx, Wyse 50, Hazeltine 1500, and Osborne 1. No flag still
  builds the ANSI `HVI.COM`, byte-for-byte the same feature set as before.
- All escape sequences moved behind capability macros in the new
  `termcfg.h`; `term.c` emits the selected family's codes and `screen.c`/
  `edit.c` fall back in software where a terminal lacks a feature —
  space-padding for terminals with no clear-to-EOL (ADM-3A), a full
  text-area repaint where there is no hardware scroll (ADM-3A, VT52,
  Osborne), and whole-line repaints where there is no insert/delete-char.
- The non-ANSI builds omit the `ESC[6n` size query and the ANSI arrow-key
  parser entirely, so each is **smaller** than `HVI.COM` (218–223 records
  vs. 228) and never sends a byte a dumb terminal can't interpret.
- Hazeltine 1500 displays `^` for `~` (its command lead-in) while keeping
  the real `~` in the file. Osborne 1 defaults to its 52-column screen and
  clamps the status line to the screen width.
- New `tests/termtest.py` asserts each family's raw output stream (correct
  round-trip, no ANSI CSI, no size query, right address lead-in). ANSI
  build still passes all 189 + 18 functional tests; `HVI.COM` grew 1
  record (227 → 228) from the shared cursor-tracking and status changes.
- The per-terminal escape codes come from the terminals' manuals and are
  marked for verification against specific models in `termcfg.h`/`term.c`.
- **Fix: a bare `CR` in the middle of a line was stripped on load.** The
  loader dropped every `CR`; it now removes a `CR` only where it forms a
  `CR+LF` line ending, so a mid-line `CR` is kept as content and round-trips
  on save. Fixed in both the initial load and the large-file tail-paging
  paths (`gb_fill`/`gb_load_more`, gap.c); two regression tests added.

### 2.7.1 → 2.7.2

One display fix, no size change (still 227 records). All 189 + 18
tests pass (2 screen tests added).

- **Fix: typing before a tab shifted the rest of the line out of
  alignment** (reported editing tab-separated assembly columns: after
  `cw` or `dw`+`i`, each typed character pushed the tail one screen
  column right; `w` still moved to the correct place and `Ctrl-L`
  repainted the truth — the buffer was never wrong, only the display).
  Mid-line insert-mode typing uses the terminal's one-column
  insert-character escape (`ESC[@`), and backspace its delete-character
  twin (`ESC[P`); both assume every character in the tail occupies a
  fixed width, but a tab's rendered width shrinks or grows as text
  before it changes, so the on-screen tail drifted one column per
  keystroke. Both fast paths now fall back to a whole-line repaint when
  a tab lies between the cursor and the end of the line (`tail_has_tab`,
  edit.c); tab-free lines keep the cheap 2–4 byte update.

### 2.7 → 2.7.1

Size release plus one real-hardware portability fix. All 187 + 18
tests pass unchanged.

- **Fix: keyboard input unusable on real CP/M 2.2** (reported on a
  North Star Horizon under both North Star and Lifeboat CP/M 2.2:
  insert mode inserted characters by itself endlessly, and `d`/`c`
  operators failed with a flash of "Unknown motion"). Only register A
  carries the DRI-guaranteed byte result of a BDOS call — H mirrors
  B, which real BDOS implementations leave as internal junk (RunCPM
  returns a clean HL, which is why the test harness never caught
  it). `term_getch` consumed the BDOS 11 (Console Status) result
  unmasked: idle looked "ready", the BDOS 6 read then returned 00h,
  and the editor ate an endless stream of NUL keystrokes. Two
  hardenings: `bdos_disk` now returns A zero-extended (cleaning every
  BDOS result in the editor, file operations included), and input
  spins on BDOS 6 alone (E=FFh is non-blocking, 00h when idle) with
  no status pre-poll — also sidestepping BIOSes whose CONST strays
  from the specified 00h/FFh. Trade-off: a real `^@` keystroke is
  indistinguishable from idle and is ignored (vi binds nothing to
  NUL).

The size work: no new commands and no behavior change — every edit is
a provably equivalent, smaller (and on Z80, faster) form. Binary: 30,592 → 29,056 bytes, **239 → 227
records** (−1,604 bytes, −12 records) — smaller than v2.5 despite the
three feature releases since.

- **Auto locals became function statics** across every module
  (motion_endpoint, dot_replay, ex_execute, hvi_sprintf, gb_move_gap,
  hvi_rename, main, ...). HI-TECH C spills autos to 6-byte IX-relative
  accesses (19 T-states/byte); statics are 3-byte absolute (16
  T-states/byte) — smaller *and* faster, safe because nothing in HVI
  is reentrant. This alone bought 1,208 bytes.
- **Hot-function parameters copied to statics on entry** (insert_key,
  normal_cmd, apply_op, gb_insert, gb_delete, find_bol, gb_find_ch,
  gb_split, next_vrow, term_goto, term_putch, normal_page_cmd) — same
  IX-vs-absolute arithmetic, applied only where a parameter is read
  ~4+ times (measurement showed fewer-use copies cost more than they
  save, and those were reverted).
- **Relational tests became equality tests** where an operand is
  provably non-negative (`x > 0` ≡ `x != 0` for buffer positions,
  lengths, counts): each conversion trades a 13-byte `wrelop` library
  call for a 7-byte inline test.
- `:e`'s two load messages share one `"%s" %s` format.

### 2.6.1 → 2.7

Feature release: **`:[range]s/old/new/[g]` substitute** and the
**`>` / `<` shift operators**. All 187 + 18 tests pass (18 substitute
and 22 shift tests added).

- **`>>` and `<<`** shift lines one tab stop right/left, riding the
  existing operator-motion model — counts (`3>>`), every motion
  (`>j`, `>G`, `` >`a ``, ...) and `.` repeat come from the shared
  `apply_op`/dot machinery. Always linewise (vi); an exclusive motion
  ending in column 0 leaves that line out. `>` inserts one tab (empty
  lines skipped; the full-gap recovery from 2.6.1 applies), `<`
  removes up to one tab stop of leading blanks. The cursor lands on
  the first non-blank of the topmost shifted line. Single-line shifts
  are undoable; multi-line shifts invalidate the undo record (like
  `:s`). `:[range]>` and `:[range]<` drive the same engine through
  the ex range parser (numbers, `.`, `$`, `'a`–`'z`, either order).
- The shift engine walks lines bottom-up, so line positions above are
  unaffected by edits already made below — that also makes it safe
  across a large-file window swap (the loop is count-driven).
- Size pass for the release: the shift engine takes its arguments in
  globals (frameless, like `gb_insert_room`); the 1-byte room-making
  insert (`room1`) is one copy shared by put, `o`/`O` and the shift's
  tab; dot-replay's `d`/`>`/`<` cases collapsed into one; and several
  provable-invariant relationals became cheap equality tests. The
  shift feature nets 492 bytes (down from 647 before the pass) —
  binary lands on exactly 239 records (30,592 bytes).

- Plain-text and case-sensitive (no regex); `g` replaces every
  occurrence in a line, else the first. Ranges take line numbers,
  `.`, `$`, and `'{a-z}` marks in either order; default is the
  cursor's line. Zero matches report `Pattern not found`; if the gap
  fills mid-run, completed substitutions are kept and `Buffer full`
  is shown. Not undoable (scattered edits invalidate the single-slot
  undo record); marks track the edits as usual.
- Efficient scan: because the pattern cannot contain a newline, a
  match can never cross a line boundary — one flat pass over the
  range with no per-line loop. `find_eol` was generalised into
  `gb_find_ch(pos, c)` (CPIR, 21 T-states/byte) to hop between
  candidate first characters; candidates are staged with two LDIRs
  (`gb_copy_out`) and compared with one `hvi_strcmp` — no
  per-character `gb_char_at` calls anywhere in the scan.
- Size offsets: the `:N` handler now shares the substitute's address
  parser (one copy of the clamp/walk), `:r` uses `find_eol` instead
  of a hand-rolled byte loop, mark addresses resolve through the
  operators' `motion_endpoint('`')`, and `:e`/`:w` share one
  filename-recording helper. Net feature cost: 685 bytes — 230 → 235
  records.

### 2.6 → 2.6.1

Bug-fix release: **inserts larger than the free gap were silently
dropped** — `p`/`P` of a big yank, `o`/`O` at a zero-byte gap, and the
`.` replay of insert/change text. All 147 + 18 tests pass (5 large-file
regressions added).

- In a large (window-slid) file the buffer loads full, leaving only
  `GAP_MIN` (256) bytes of gap. `put_yank` called `gb_insert()` without
  checking the result, so putting a yank bigger than the gap did
  nothing — while still setting the modified flag and a bogus undo
  record. Easy to hit since 2.6: `y`a` yanks arbitrarily large ranges.
- Fixed with `gb_insert_room()` (gap.c): inserts in gap-sized chunks and
  swaps the window out (`gb_make_room`) between chunks — the same
  recovery insert-mode typing uses, generalised to block inserts. A
  single retry would not be enough: `gb_make_room`'s reload also leaves
  only a `GAP_MIN` gap.
- When a swap happens the reload invalidates marks and the undo record,
  so the put skips the undo save (`u` is then a no-op, as after any
  window shift) and repaints from scratch. On a mid-put disk-full error
  the put reports `Buffer full`.
- The same recovery now covers `o`/`O` (their newline is inserted
  through `nl_room`, reachable with a zero-byte gap after insert-mode
  typing exactly fills the window) and the `.` replay of insert/change
  text (`dot_ins` in erepeat.c, deduplicating the two copies of the
  replay-insert tail). Only `:r` still stops early at a full window
  (documented limitation; nothing is lost).
- Size: all three fixes cost 107 bytes net — one record, 229 → 230.
  `gb_insert_room` is hand assembly in cstart.as (args in globals,
  `gir_pos` chains consecutive inserts, a −1 failure passes through
  the chain); offsets came from deduplicating the o/O open-line code,
  the replay-insert tail, the `c`-operator's insert-mode entry
  (`enter_insert` now shared with `apply_op`), and the put's undo
  start derived as `end − total` instead of being tracked.

### 2.5 → 2.6

Feature release: **marks as operator motions**. All 147 + 13 tests in
`tests/` pass (7 operator-mark tests and 1 screen test were added).

- **`` ` `` is now a motion for `d`, `c` and `y`.** `` d`a `` deletes the
  exclusive character range between the cursor and mark `a` (vi
  semantics: whichever side is earlier in the buffer starts the range),
  `` c`a `` changes it, `` y`a `` yanks it, and `` d`` `` uses the
  previous-jump position. A count is accepted and ignored, as in vi.
  An unset (or edit-cleared) mark reports `Mark not set` and aborts the
  operator; an invalid mark character (e.g. `ESC`) aborts silently.
- **`.` replays mark-motion changes.** The mark character is recorded in
  `dot_arg`, so the repeat re-resolves the mark at its current
  (edit-adjusted) position rather than reusing a stale offset.
- One copy of the resolution logic: the mark motion lives in
  `motion_endpoint()` (emove.c) and the standalone `` `x `` jump now
  calls it instead of carrying its own resolve/validate code; the
  "Unknown motion" message also moved into `motion_endpoint()` so every
  caller just tests for a negative endpoint.
- `MARK_PREV` moved from slot 26 to slot 0, making the slot layout
  mirror ASCII (`slot = char - 0x60`): the whole resolve is one
  subtraction plus one unsigned range test.
- Size offsets so the feature fits without growing the binary: the
  mark-slot remap above, unsigned range tests instead of signed
  double compares, shared copies of the `"TOP"`/`"BOTTOM"` (edit.c)
  and `"rb"`/`"wb"` (gap.c) literals that HI-TECH C stored per use,
  and a file-static resolve temporary (absolute stores instead of
  IX-relative spills).

Binary size: 229 CP/M records (~29K) — identical to 2.5; the feature
nets +1 byte of text+data.

### 2.4 → 2.5

Feature release (drive/user-area file access) plus an assembly speed
pass.  All 139 + 13 tests in `tests/` pass, including 11 new
user-area tests.

- **CP/M user areas and drive prefixes.**  Every filename — command
  line, `:w`, `:e`, `:r` — accepts ZCPR-style `du:` prefixes:
  `B:FILE.TXT`, `3:FILE.TXT`, `B3:FILE.TXT` (drives A–P, users 0–15).
  CP/M has no per-FCB user number, so each `HFILE` records its user
  area and cpmio.c brackets every directory/data BDOS call with
  BDOS 32 set-user/restore.  Unprefixed names never issue a BDOS 32
  (zero overhead) and live where HVI was started.  When saving a
  large file back to itself, the `HVITMP.TMP` staging file is created
  with the destination's prefix — BDOS rename cannot cross a drive or
  user area.
- **`gb_char_at` is now assembly** (cstart.as, frameless): the hottest
  function in the editor, called once per character by every scanner.
- **`find_bol`/`find_eol` run on new CPIR/CPDR scanners**
  (`gb_memchr`/`gb_memrchr`, 21 T-states/byte vs ~300 for the compiled
  loop) — these back j/k, dd, J, o, G, `:N`, and `line_span`, so line
  motions in a full 24K buffer are an order of magnitude faster.
  `scr_line_start` hops lines via `find_eol` instead of scanning every
  byte.
- **Newline counting funnels into the CPIR counter** (`gb_cntnl`) in
  five more places (`gb_insert`, yank/undo/dot-replay checks), and
  `:r` reads through a 128-byte staging chunk instead of one
  `gb_insert` (gap move + 27-mark sweep) per character.
- Size offsets for the above: the FCB builder + `du:` parser and the
  BDOS-32 user bracket (`fill_fcb`, `dsk_u`) are assembly in
  cstart.as, `rd_sector`/`hvi_fseek` read the sector number's
  little-endian bytes instead of calling the 32-bit shift library,
  `gb_copy_out`/`gb_count_nl` share one clamp/split helper, the
  `G`/`^B`/`k`-at-top window jumps share `gb_load_last`/`gb_load_prev`,
  `fmt_int`'s power table moved to initialized data, and `bdos_puts`
  uses the `con_write` assembly loop.

Binary size: 229 CP/M records (~29K) — identical to 2.4 despite the
user-area feature.

### 2.3 → 2.4

Redundancy-elimination release: no new commands and no behavior change —
every fold replaces duplicated code with one shared copy, at equal or
better speed.  All 128 + 13 tests in `tests/` pass unchanged.

- **Word motions classify each character once.** The `w`/`b`/`e` scans
  (and their `d`/`c`/`y` operator ranges) called `gb_char_at` twice plus
  two classifier functions per character; a shared `chtype()` does it
  with one buffer read — the scans are now measurably cheaper as well
  as smaller.
- **screen.c's six copies of the visual-row walk loop** became two
  helpers (`vwalk_to`/`vwalk_n`), and its three copies of the
  tab-expanding column walk became `col_from()`.  A provably dead loop
  in `locate_cur_row()` (its condition can never hold after
  `vrow_start_of`) was removed.
- **`mv_eol`, and the `$`/`0`/`^` operator motions** re-implemented
  `find_eol`/`find_bol`/first-non-blank walks that already existed —
  they now call them.  The `dd`/`cc`/`yy` line-span walk, duplicated in
  edit.c and the `.` replay, is one `line_span()` in emove.c.
- **`gb_find_line_offset` and `gb_count_lines`** in gap.c were the same
  file-scan loop with different stop conditions — merged into one
  `gb_scan_lines()`.
- Smaller folds: Ctrl-L reuses term.c's scroll-region emitter instead
  of rebuilding the escape sequence, `^F`/`^B` share their header,
  `hvi_fopen`'s read/write tails merged, duplicate ex.c format strings
  now have one copy each (HI-TECH C does not pool identical literals),
  and several `old_top` temporaries were eliminated.
- Robustness: the status-bar filename buffer was sized for a worst-case
  63-char filename (48 bytes could overflow — same class as the 2.3
  `hvi_sprintf` fix, in BSS so the fix is free).

Binary size: 229 CP/M records (~29K), 10 records below 2.3's 239.

### 2.2 → 2.3

Feature release: **marks**. All 128 + 13 tests in `tests/` pass (9 mark
tests and 3 status-overflow regression tests were added).

- **New commands `m{a-z}`, `` `{a-z} `` and `` `` ``.** `m` sets one of 26
  named marks at the cursor; backtick jumps back to the exact position
  (line and column). `` `` `` returns to the position before the last jump
  (`` ` ``, `G`, `gg`, `:N`, `:$`, or a successful search), so pressing it
  twice toggles between two locations.
- Marks are stored as buffer positions and adjusted on every insert and
  delete, so they stay on the same character as the text above them
  changes. A mark whose text is deleted is cleared (matching vi, which
  drops marks on deleted lines). In a large file, all marks are cleared
  when the sliding window moves — a mark can't outlive the window that
  contains it.
- Jumps to marks reuse the minimal-redraw path from 2.2
  (`scr_update_after_move`), so a nearby jump scrolls the region instead
  of repainting the screen.

Also in 2.3, a third source-level size pass (no functionality or speed
change — every fold swaps an inlined sequence for one extra call on a
non-hot path):

- The ex `:` command line and the `/` `?` search prompts now share one
  line-reader (`read_line`) instead of carrying two copies of the same
  prompt/edit loop.
- `r` is now implemented by its own `.`-replay (as `J` and `~` already
  were), removing a duplicated replace routine; `a`/`I`/`A` position the
  cursor through the same code their replay uses; `o`/`O` share one
  open-line body.
- Repeated fragments were factored into tiny helpers: status-line
  updates, `want_col` capture, scroll-plus-repaint after motions,
  pull-cursor-off-newline, the insert-mode wrap check, dot-state
  recording, and the status-row clear; duplicate status format strings
  were merged across modules.

Security/robustness fix (found in code review): status messages that
echo ex-command input (`Unknown command: %s`, `Cannot open: %s`,
`"%s" [New File]`, ...) could overflow the 128-byte status buffer into
the adjacent editor state when given a 120+ character command or
filename, corrupting cursor and dot-repeat state. `hvi_sprintf` now
caps each `%s` expansion so the longest fixed prefix plus one
user-supplied string always fits `STATUS_MAX`; real CP/M filenames
(~14 chars) are never affected — only the on-screen echo of
over-long junk input is truncated.

Binary size: 239 CP/M records (~30K) — the mark feature cost 4 records
over 2.2's 240, the size pass bought back 6, and the `%s` bounds check
cost 1.

### 2.1.1 → 2.2

Performance release (targeting 4 MHz Z80). No new commands; all 116 + 13
tests in `tests/` pass (21 screen-scrape tests were added for the new
minimal-redraw paths, plus 4 put/undo regression tests).

The theme of this release is removing per-character function-call overhead:
a compiled call on HI-TECH C costs ~150-200 T-states before any work is done,
so loops that funnel every byte through `gb_char_at()` or `bdos()` were paying
roughly ten times the cost of the actual work.

- **Linewise operators no longer scan the whole buffer.** `dd`, `cc`, `yy`,
  `dj`, `dk`, `dG` (and the `.` replay of `dd`) used to convert between line
  numbers and buffer positions with up to three full-buffer scans — deleting
  one line near the end of a full 24 KB buffer cost 2-3 seconds at 4 MHz.
  They now walk only the affected lines with `find_bol`/`find_eol`:
  effectively instant.
- **New CPIR newline scanner in assembly** (`gb_cntnl` in `cstart.as`, used
  via `gb_count_nl()`). Line counting and line-number scans (`G`, `nG`, `:N`,
  `Ctrl-F`/`Ctrl-B`, delete bookkeeping, line-count cache rebuilds) run at
  21 T-states/byte instead of ~150+ — a full-buffer line count drops from
  over a second to ~0.13 s at 4 MHz.
- **`gb_insert` is now a bulk operation**: one gap move plus one LDIR block
  copy instead of a gap-move function call per character. A 1 KB paste
  previously made ~3000 calls. Insertion is also all-or-nothing when the
  buffer is nearly full (previously a too-large paste could be inserted
  partially).
- **Yank, undo capture, and dot-repeat capture use `gb_copy_out()`** — at
  most two LDIR block copies instead of one `gb_char_at()` call per byte.
- **File loading appends directly into the gap.** The initial load, window
  reloads, and forward window shifts (`gb_fill`, `gb_load_more`) previously
  went through ~6 function calls per byte; the `[Loading...]` pause when
  paging through a large file is several times shorter.
- **`:w` walks the two raw gap segments** with a pointer instead of calling
  `gb_char_at()` for every byte written.
- **Search inner loops make no function calls per comparison.** The pattern
  is stored pre-lowered when typed (search was already case-insensitive) and
  the buffer-side case folding is inlined, in both the in-memory search and
  the large-file disk scan — roughly 2-3x faster `/`, `?`, `n`.
- **Terminal output is flushed by `con_write`** (`cstart.as`): the output
  buffer is written by an assembly loop around `CALL 5` using BDOS function 6
  (direct console output), replacing one C-level BDOS-2 call per byte. A
  full-screen repaint saves ~1200 call round-trips, and function 6 skips
  function 2's `Ctrl-S`/`Ctrl-P` status polling.

Behaviour note: because output now uses BDOS function 6, the BDOS no longer
pauses output on `Ctrl-S` mid-redraw (real terminals handle flow control in
hardware; full-screen editors conventionally use direct console I/O).

**Screen redraw** (second pass in this release — minimising bytes sent to
the terminal, which dominate at 9600 baud):

- **Single-line edits repaint one screen row.** `x`, `X`, `s`, `D`, `C`,
  `dw`, `cw`, `d$`, charwise `p`/`P`, `u`, and their `.` replays used to
  repaint from the cursor to the bottom of the screen (~600–1200 bytes);
  when the edit adds/removes no newline and the line does not wrap, they
  now repaint only the edited row (~80 bytes).
- **`r` emits just the replaced cell** (2–4 bytes) and **`~` emits only
  the toggled characters in place**, instead of redrawing the logical line.
- **Leaving insert mode sends only the status bar.** Insert mode keeps the
  text display current as you type, so the old whole-line redraw on `ESC`
  was almost always redundant.  A dirty flag preserves the redraw when the
  session touched a wrapped line (where the incremental byte-level updates
  can leave a stale row).
- **The one-row scroll optimisation now handles N rows.** Any movement that
  shifts the viewport by fewer than a screenful (`Ctrl-D`, `Ctrl-U`, counted
  `j`/`k`, nearby searches) scrolls the region and repaints only the rows
  that came into view; `Ctrl-D`/`Ctrl-U` previously always repainted the
  full screen — and when the cursor stays inside the viewport they now send
  only a cursor reposition.
- **The status bar is rewritten only when its text changes.** The filename
  bar and `-- INSERT --` indicator were retransmitted (~40 bytes) on every
  edit and insert keystroke.
- **`:N` / `:$` skip the full-screen redraw** when the target line is
  already inside the viewport.

Editing bug fixes:

- **Linewise `p` below a final line that lacks its newline** glued the
  pasted text onto that line (`ddp` on the last two lines of a file
  without a trailing newline produced `threetwo`).  A newline is now
  inserted first so the paste starts on a fresh line.
- **`u` after `p`/`P` removed the wrong bytes.**  The undo record was
  written before the insert position was known, so it pointed at the
  cursor instead of the actual paste (which for linewise `p` starts on
  the next line, and may include an added newline).  The record now
  covers exactly the inserted span.
- **`a`/`A` at the true end of the buffer painted at column 0.**  With
  no trailing newline, the append position equals the content length,
  and `next_vrow()` returned that same value for "line ends at the
  buffer end" — so the cursor-placement code took the append point for
  a phantom new row starting at column 0 and typing visually overwrote
  the line start (the buffer itself was updated correctly; `Ctrl-L`
  repainted the truth).  `next_vrow()` now returns content length + 1
  in that case, which also corrects the same off-by-one-row miscount in
  the scroll and row-location walks.

- **A blank line typed between text lines kept a stray `~`.**  Pressing
  Enter at the end of the buffer parks the cursor on the empty last
  line (position == content length, after the final newline), and
  `draw_row_at()` painted that row as a past-the-end `~`; the next
  Enter shifted the redraw window down and stranded the tilde on what
  was now a real blank line.  That row is left blank while the cursor
  occupies it.
- **A mid-line Enter left the moved-down tail on the row above.**  The
  after-edit redraw starts at the cursor's (new) row, so the first half
  of the split line was never repainted (`aX<Enter>` on `ab` displayed
  `aXb` above `Yb`).  The split row is now repainted when the new line
  is non-empty.

Screen bug fixes (both caught by the 16 new screen-scrape tests):

- **`:N` and `:$` left the cursor-row cache stale**, so the terminal cursor
  could be placed on the wrong screen row after a line jump (buffer edits
  still landed correctly, but `r`/`~` then repainted the wrong row).
- **Typing across the wrap boundary painted the continuation row at the
  top of the screen**: the cursor-row cache was not invalidated when an
  inserted character pushed the cursor onto the next visual row.

**Size** (third pass in this release — clawing back the bytes the speed
work added; every byte saved also enlarges the gap buffer, since
`gb_init()` takes the largest free TPA block left after the code):

- Removed ~110 lines of unreachable `.`-repeat code: `x`, `X`, `D`, `C`
  never occur as a recorded dot command — they expand through the
  operator path and replay as `d`/`c` plus a motion.
- `hvi_sprintf` accepts 2 format arguments instead of 5.  No HVI message
  uses more, and every unused slot cost an argument push at all ~17 call
  sites.
- Deduplicated code paths: `p`/`P` share one put routine; the `G`, `gg`,
  `Ctrl-F`, `Ctrl-B` and window-reload handlers share their cursor-
  placement and repaint tails; `f`/`F`/`;`/`,` share one movement tail;
  the two buffer-full retry blocks in insert mode are one helper; the
  scroll-up and scroll-down halves of the smart scroller are merged; the
  yank-copy block, the one-row/full refresh dispatch (`scr_edit_end`),
  and the last-line computation each exist exactly once.
- The terminal-size probe is one 14-byte `con_write` block instead of 13
  separate BDOS calls.
- Mirror-image message strings merged into single format strings
  (`search hit …`, the two `Modified buffer` hints); the temp-file name
  literals are stored once.
- Second pass: the BDOS-33 sector-read setup shared by `hvi_fgetc` and
  `hvi_fseek` is one `rd_sector()`; the sector flush shared by
  `hvi_fputc` and `flush_write` is one `wr_sector()`; the save-time
  head and tail copies are one `gb_copy_file(from, to)`; the three
  word-motion wrappers are one `mv_word(c, n)` and `w`/`b`/`e` share
  one dispatcher case; `i`/`a`/`I`/`A` share `enter_insert()`; the six
  copies of the console-status poll countdown (arrow-key
  disambiguation, terminal-size handshake) are one `con_wait()` and
  the two size-report digit parsers are one `tgs_num()`; the two
  advance-top loops in `scr_scroll_to_cursor()` are merged; the
  write-only `count` field left the Editor struct.

Binary size: 240 CP/M records (~30K) — the speed and redraw work above
cost a net 4 records over 2.1.1's 236.

### 2.1 → 2.1.1

Size reductions (no functionality or speed lost):

- Removed the unused `term_bold()` routine.
- `raw_num` (terminal escape numbers) now shares `fmt_int` from util.c
  instead of carrying a second decimal converter.
- The `w`/`b`/`e` word motions delegate to `motion_endpoint()`, which
  implemented the identical scans for the `d`/`c`/`y` operators.
- `J` and `~` share one implementation with their `.`-repeat handlers in
  erepeat.c. As a side effect `~` now honors a count prefix (`3~` toggles
  three characters), matching vi.
- The `x`/`X`/`s`/`S`/`D`/`C`/`Y` aliases expand through one small helper.
- `scr_redraw_cur_line()` / `scr_redraw_from_cur()` share their
  row-locating preamble.

Bug fix:

- **`J` follows vi's spacing rules**: leading whitespace of the joined line
  is removed and exactly one separating space is inserted — none when the
  current line already ends in a blank or either side of the join is empty.
  (Previously `one ` joined with `two` produced a double space, and leading
  indentation of the next line was kept.)

Binary size: 236 CP/M records (~30K), down from 245 in 2.0/2.1.

### 2.0 → 2.1

Bug fixes (all found by the automated test suite in `tests/`):

- **Fixed the C runtime stack frame in `cstart.as`.** The custom `csv`/`ncsv`
  prologues left SP two bytes above the frame base, so the deepest auto
  (local) variable of small functions sat below the stack pointer and was
  overwritten by the next argument push. Visible symptom: `r` replaced the
  character under the cursor with a NUL (0x00) byte instead of the typed
  character. This was also the root cause of the long-standing
  "HI-TECH C `-O` corrupts IX-relative locals" behaviour that forced the
  static-local workarounds throughout the code — auto variables are fully
  reliable now. The corrected frame matches the real HI-TECH `csv`, which
  saves both IX and IY (arguments at IX+6, SP == IX at function entry).
- **`c$` / `C` inserted at the wrong position.** The change operator applied
  the delete-style cursor back-off before entering insert mode, so the typed
  replacement landed one character to the left (`2lc$XY` on `abcdef` gave
  `aXYb` instead of `abXY`). Change now inserts exactly where the text was
  removed.
- **`cc` / `S` deleted the line's newline**, merging the replacement text
  with the following line (`ccnew` on `foo`/`bar` gave `newbar`). The
  linewise change now keeps the newline, and `cc` on an empty line enters
  insert mode instead of joining lines.
- **`cw` deleted the word's trailing spaces** (`cwqux` on `foo bar` gave
  `quxbar`). `cw` now changes the word only, matching vi; `dw` still deletes
  through the following whitespace. `.` repeat of `cw` uses the same rule.
- **A key typed quickly after `ESC` was swallowed** by the arrow-key
  disambiguation in `term_getch()`. The byte is now pushed back and processed
  as the next command.
- **Files are saved with a final newline** (vi convention) when the buffer
  does not already end with one.

Performance (targeting 4 MHz Z80):

- **`gb_memmove` rewritten in assembly using LDIR/LDDR** (`cstart.as`),
  about 7x faster than the compiled loop. This runs on every gap move, i.e.
  whenever an edit happens away from the previous edit point, and is ~90
  bytes smaller than the compiled version.
- **`gb_char_at` no longer calls `gb_content_len()` per character** — the
  bounds check is done inline on the gap-adjusted index. Every scanner
  (line counting, drawing, search) calls this once per character, so
  full-buffer scans are roughly twice as fast.
- Removed the unused `_bios_conin` routine from `cstart.as`.

Also new in 2.1: an automated test harness under `tests/` (91 functional
tests plus 13 large-file tests) that builds HVI with HI-TECH C V3.09 inside
RunCPM and drives it through a pseudo-terminal; see `tests/README.md`.

---

## License

MIT License. Copyright (c) Juan Orlandini.
