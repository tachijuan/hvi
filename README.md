# HVI - VI Clone for CP/M

A lightweight VI-compatible editor for CP/M 2.2 and CP/M 3.0, written in
HI-TECH C. Uses a gap buffer for efficient editing and ANSI escape sequences
for terminal control.

**Author:** Juan Orlandini  
**License:** MIT

---

## Building

### Requirements

- HI-TECH C Compiler for Z80/CP/M (V3.09 or later)
- CP/M 2.2 or CP/M 3.0 system with at least 48K TPA

### Build Steps (on CP/M)

1. Copy all source files to a CP/M disk/drive:
   - `HVI.C`, `GAP.C`, `TERM.C`, `SCREEN.C`, `EMOVE.C`, `EDIT.C`, `EX.C`, `HVI.H`

2. Compile each file individually (the C driver does not invoke the linker):

```
C -C HVI.C
C -C GAP.C
C -C TERM.C
C -C SCREEN.C
C -C EMOVE.C
C -C EDIT.C
C -C EX.C
```

3. Link the object files with `LINQ` to produce `HVI.COM`:

```
LINQ -Z -N -C100H -OHVI.COM CRTCPM.OBJ HVI.OBJ GAP.OBJ TERM.OBJ SCREEN.OBJ EMOVE.OBJ EDIT.OBJ EREPEAT.OBJ EX.OBJ LIBC.LIB
```

> **Note:** The HI-TECH C linker is named `LINQ`, not `LINK`. Adjust the
> `-C100H` load address if your TPA starts elsewhere.

4. For a debug build add `-H` to each compile step:

```
C -H -C HVI.C
C -H -C GAP.C
... (repeat for all files, including EREPEAT.C)
LINQ -Z -N -C100H -OHVI.COM CRTCPM.OBJ HVI.OBJ GAP.OBJ TERM.OBJ SCREEN.OBJ EMOVE.OBJ EDIT.OBJ EREPEAT.OBJ EX.OBJ LIBC.LIB
```

### Cross-Compilation (Linux/macOS host)

If using the HI-TECH Z80 cross-compiler on a Unix host:

```
c -c hvi.c gap.c term.c screen.c emove.c edit.c ex.c
linq -Z -N -C100H -ohvi.com crtcpm.obj hvi.obj gap.obj term.obj screen.obj emove.obj edit.obj ex.obj libc.lib
```

Transfer `hvi.com` to your CP/M system via XMODEM, Z-Modem, or disk image.

---

## Usage

```
HVI [filename]
HVI -d [filename]
```

- `filename` — file to open (created if it does not exist)
- `-d` — enable debug output to stderr (useful when redirecting stderr to a file)

### Debug Mode

```
HVI -d MYFILE.TXT 2>DEBUG.TXT
```

This logs every keypress and mode change to `DEBUG.TXT` for troubleshooting.

---

## Supported Commands

### Normal Mode — Movement

| Key        | Action                              |
|------------|-------------------------------------|
| `h`        | Move left one character             |
| `l`        | Move right one character            |
| `j`        | Move down one line                  |
| `k`        | Move up one line                    |
| `w`        | Forward to start of next word       |
| `b`        | Backward to start of previous word  |
| `e`        | Forward to end of word              |
| `0`        | Move to beginning of line           |
| `^`        | Move to first non-blank of line     |
| `$`        | Move to end of line                 |
| `G`        | Go to last line (or line N with count) |
| `gg`       | Go to first line (or line N: `5gg`) |
| `Ctrl-F`   | Scroll forward one page             |
| `Ctrl-B`   | Scroll backward one page            |
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

### Normal Mode — Character Search (current line)

| Key      | Action                                              |
|----------|-----------------------------------------------------|
| `f{c}`   | Move to next occurrence of character `c` on line    |
| `F{c}`   | Move to previous occurrence of character `c` on line|
| `;`      | Repeat last `f` or `F` in the same direction        |
| `,`      | Repeat last `f` or `F` in the opposite direction    |

Both repeat commands accept a count prefix (e.g. `3;` skips to the
third next match).

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

## Terminal Requirements

HVI uses ANSI/VT100 escape sequences. It defaults to 80 columns × 24 rows,
which is the standard CP/M terminal size.

The ANSI DSR size query (ESC[6n) is disabled by default because many CP/M
terminals do not respond to it, which would cause HVI to hang at startup.
To enable dynamic size detection on terminals that do respond, recompile with
`-DTERM_QUERY`.

Compatible terminals: VT100, VT220, xterm, ANSI.SYS, and most modern
terminal emulators connected via a serial port.

---

## Known Limitations

- Single-level undo only (the `u` command undoes the most recent change)
- Files larger than available RAM are loaded partially; the unloaded
  tail is preserved and re-appended on every `:w` save, so no data is
  lost. HVI probes for the largest single allocation the system can
  provide (up to ~28 KB) and uses that as the in-memory edit window.
  Edits are limited to the loaded portion.
- No visual/block selection mode
- No macro recording/playback
- No window splitting

---

## Change History

### Terminal Size Detection

HVI queries the terminal size at startup using the ANSI cursor-position
report sequence (ESC[999;999H followed by ESC[6n). The terminal responds
with ESC[rows;colsR. On CP/M systems running under a Unix host in canonical
(line-buffered) mode, the ESC byte is delivered immediately but the rest of
the response is held until Enter is pressed. HVI uses BIOS CONIN (bios(3))
directly for the read phase, bypassing BDOS buffering, which resolves the
issue on most emulators. If startup pauses, press Enter once to flush the
terminal's canonical buffer.

### Long-Line Wrapping

Lines wider than the terminal are now wrapped across multiple screen rows
rather than clipped at the right edge. The unit of vertical measurement
is the "visual row" (one terminal line of content). `top_pos` may point
to the middle of a long logical line. Cursor movement commands (`j`/`k`)
still operate on logical lines; `h`/`l` move within a logical line
regardless of visual-row boundaries.

### Insert Mode Display Optimisation

Insert mode now emits minimal terminal traffic on slow connections:

- **Regular character, no wrap** — a single byte is sent; the terminal
  cursor auto-advances. No escape sequences are emitted.
- **Backspace at end of line** — BS + ESC[K (erase to end of line).
  No full-line redraw.
- **Character causing a visual-row wrap** — only the rows from the
  cursor to the bottom of the text area are redrawn (not the full screen).
- **Newline / backspace over newline / Ctrl-W** — full screen refresh
  (unavoidable; line count changes).
- **ESC (exit insert mode)** — only the edited line is redrawn unless
  the viewport had to scroll, in which case a full refresh is done.
- The `-- INSERT --` mode indicator is shown once on mode entry and is
  not refreshed on every keypress.

### Undo Restores Clean State

If the buffer was unmodified when a change was made, undoing that change
with `u` clears the modified flag. The `[+]` indicator in the status line
disappears and `:q` will succeed without requiring `:q!`.

### Large File Support

Files that exceed the in-memory buffer are now fully supported. When
loading a file larger than `BUF_MAX` (~28 KB), HVI records the exact
byte offset in the original file where loading stopped. Every subsequent
`:w` save appends the unloaded tail after the in-memory content, so no
data is lost. When the destination is the same as the source file, a
temporary file (`HVITMP.TMP`) is used as an intermediate to avoid
reading and writing the same file simultaneously. After each save the
tail pointer is updated to the correct offset in the newly written file
so repeated saves remain correct.

---

## File Format

HVI reads files in binary mode, stripping bare `CR` characters on load.
On save, each `LF` is written as `CR+LF` per CP/M convention, and the
file is terminated with `Ctrl-Z` (0x1A) per CP/M file format rules.

### Large File Support

When a file exceeds the in-memory buffer (`BUF_MAX` ≈ 28 KB), HVI loads
the first `BUF_MAX` characters and records the byte offset in the
original file where loading stopped. On every `:w` or `:wq` save:

1. The in-memory (edited) portion is written to the destination in
   `CR+LF` format as usual.
2. The unloaded tail is read from the original file and appended
   verbatim after the in-memory content.
3. The `Ctrl-Z` terminator is written last.

When saving to the **same filename** that holds the tail, HVI writes to
a temporary file `HVITMP.TMP` first, then replaces the original, so the
tail data is never overwritten before it is copied.

Edits are restricted to the first `BUF_MAX` bytes of the file. Content
beyond that boundary is preserved unchanged on save.

---

## License

MIT License. Copyright (c) Juan Orlandini.
