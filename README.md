# HVI - VI Clone for CP/M

**Version 2.2**

A lightweight VI-compatible editor for CP/M 2.2 and CP/M 3.0, written in
HI-TECH C. Uses a gap buffer for efficient editing and ANSI escape sequences
for terminal control. Implements most of the basic movement and editing commands
including the `.` operator for repeat. Also has a single level undo.

**Author:** Juan Orlandini  
**License:** MIT

---

## Building

### Requirements

- HI-TECH C Compiler for Z80/CP/M (V3.09 or later)
- CP/M 2.2 or CP/M 3.0 system with at least 48K TPA

### Source Files

```
cstart.as  hvi.c  hvi.h  gap.c  term.c  screen.c  emove.c
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
| `D`  | Delete to end of line (same as `d$`)|
| `cc` | Change current line                 |
| `cw` | Change word                         |
| `c$` | Change to end of line               |
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

HVI uses ANSI/VT100 escape sequences. It defaults to 80 columns × 24 rows,
which is the standard CP/M terminal size.

At startup HVI always queries the terminal for its actual dimensions by
sending `ESC[999;999H` (cursor to extreme bottom-right) followed by
`ESC[6n` (ANSI cursor-position report). The response is read byte-by-byte
via BIOS CONIN, bypassing BDOS canonical buffering. If the terminal does
not respond within the polling timeout, HVI silently falls back to the
80 × 24 defaults — it will not hang. No recompilation flag is needed.

Compatible terminals: VT100, VT220, xterm, ANSI.SYS, and most modern
terminal emulators connected via a serial port.

---

## File Format

HVI reads files in binary mode, stripping bare `CR` characters on load.
On save, each `LF` is written as `CR+LF` per CP/M convention, and the
file is terminated with `Ctrl-Z` (0x1A) per CP/M file format rules.

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

### 2.1.1 → 2.2

Performance release (targeting 4 MHz Z80). No new commands; all 114 + 13
tests in `tests/` pass (19 screen-scrape tests were added for the new
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

Binary size: 243 CP/M records (~31K) — the speed and redraw work above
cost a net 7 records over 2.1.1's 236.

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
