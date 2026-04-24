# HVI — Requirements and Implementation Reference

**Author:** Juan Orlandini  
**License:** MIT  
**Version:** 1.0  
**Date:** 2026-04-23

---

## 1. Purpose

This document specifies the complete requirements, architecture, design decisions, and compiler constraints for HVI — a VI-compatible text editor targeting the CP/M 2.2 and CP/M 3.0 operating systems. It is intended to serve as a self-contained reference that would allow the application to be fully recreated from scratch.

---

## 2. Target Platform

### 2.1 Operating System
- **Primary:** CP/M 2.2 and CP/M 3.0
- **CPU:** Zilog Z80 (8-bit, 16-bit address space, 64 KB total)
- **TPA (Transient Program Area):** Typically 48–56 KB available for program + data + heap + stack
- **Minimum TPA:** 48 KB recommended

### 2.2 Compiler: HI-TECH C V3.09 for Z80/CP/M
- K&R C (pre-ANSI), not C89 or later
- `int` is **2 bytes** on Z80 (not 4)
- `long` is **4 bytes**
- `char` is **1 byte**, default **unsigned** when used via `unsigned char` cast
- No `memmove` in the standard library — must be provided by the application
- `malloc` and `free` are available but with important limitations (see Section 7)
- `fopen`, `fclose`, `fgetc`, `fputc`, `fseek`, `ftell`, `fprintf`, `sprintf` are available
- `stdin`/`stdout`/`stderr` are available; `stderr` maps to the CP/M console
- `bios(fn, bc, de)` and `bdos(fn, de)` are available via `<cpm.h>` for direct system calls
- `getch()` is in the HI-TECH C runtime — reads a raw keypress without echo
- The compiler driver is named `C` (not `cc`); the linker is named `LINQ` (not `link`)
- CP/M filenames are 8.3 uppercase; HI-TECH C upcases filenames automatically

### 2.3 Terminal
- ANSI/VT100 escape sequences required
- Default assumed geometry: **80 columns × 24 rows**
- Terminal size queried at startup via ANSI CPR (cursor-position report); defaults used if terminal does not respond
- Compatible terminals: VT100, VT220, xterm, ANSI.SYS, and most modern terminal emulators connected via serial

---

## 3. Source Files

| File | Purpose |
|------|---------|
| `hvi.c` | Main entry point, argument parsing, startup sequence |
| `hvi.h` | Shared types, constants, and extern declarations |
| `gap.c` | Gap buffer: allocation, load, save, insert, delete |
| `term.c` | Terminal I/O: ANSI escape output, raw input, size query |
| `screen.c` | Viewport rendering: line drawing, cursor placement, status bar |
| `emove.c` | Cursor movement, operator application (`apply_op`), search |
| `edit.c` | VI normal mode, insert mode, ex command-line mode |
| `erepeat.c` | Dot-repeat: `dot_ins_position`, `dot_replay_c`, `dot_replay` |
| `ex.c` | Ex command execution (`:q`, `:w`, `:r`, `:N`, etc.) |

---

## 4. Build

### 4.1 Compile and Link (on CP/M)
```
C -C HVI.C
C -C GAP.C
C -C TERM.C
C -C SCREEN.C
C -C EMOVE.C
C -C EDIT.C
C -C EREPEAT.C
C -C EX.C
LINQ -Z -N -C100H -OHVI.COM CRTCPM.OBJ HVI.OBJ GAP.OBJ TERM.OBJ SCREEN.OBJ EMOVE.OBJ EDIT.OBJ EREPEAT.OBJ EX.OBJ LIBC.LIB
```

### 4.2 Debug Build
Add `-H` to each compile step to enable debug symbol output. The `-d` runtime flag enables in-application debug logging to `stderr`.

### 4.3 Cross-Compilation (Linux/macOS)
```
c -c hvi.c gap.c term.c screen.c emove.c edit.c erepeat.c ex.c
linq -Z -N -C100H -ohvi.com crtcpm.obj hvi.obj gap.obj term.obj screen.obj emove.obj edit.obj erepeat.obj ex.obj libc.lib
```

### 4.4 Load Address
`-C100H` sets the load address to 0x0100, which is the standard CP/M program load address.

---

## 5. Invocation

```
HVI [filename]
HVI -d [filename]
```

- If `filename` is given and exists, it is loaded into the gap buffer.
- If `filename` is given but does not exist, the editor starts with an empty buffer and the status shows `[New File]`.
- If no filename is given, the editor starts with an empty unnamed buffer.
- `-d` enables debug output to `stderr`. On CP/M, `stderr` maps to CON: and cannot be redirected; debug messages appear on screen interleaved with the editor display. Debug mode is most useful for diagnosing startup or initialization problems (before full-screen editing begins), or when running under a Unix cross-compile environment where stderr can be redirected: `hvi -d myfile.txt 2>debug.txt`.

---

## 6. Editor Architecture

### 6.1 Global State
All editor state is in a single global `Editor ed` struct defined in `hvi.c` and externed everywhere else. This avoids passing pointers and keeps code simple for K&R C.

### 6.2 Modes
| Constant | Value | Description |
|----------|-------|-------------|
| `MODE_NORMAL` | 0 | Command (normal) mode |
| `MODE_INSERT` | 1 | Insert/append/open mode |
| `MODE_REPLACE` | 2 | Replace mode (reserved, not currently used) |
| `MODE_CMDLINE` | 3 | Ex command-line mode |

### 6.3 Gap Buffer (`gap.c`)
The file content is stored as a gap buffer:
```
[text before gap][  GAP space  ][text after gap]
```
- `gb.buf`: the raw allocated block
- `gb.size`: total allocation in bytes (includes gap)
- `gb.gstart`: index of first gap byte
- `gb.gend`: index of first non-gap byte after the gap
- **Content length** = `gb.size - (gb.gend - gb.gstart)`
- Insertions move the gap to the cursor position and fill from the front
- Deletions expand the gap (no data movement needed)
- `GAP_MIN = 256`: minimum gap size; if the gap is smaller than this, the buffer is considered full

Key functions:
- `gb_init()` — allocates the buffer
- `gb_free()` — releases the buffer
- `gb_content_len()` — returns logical content length
- `gb_char_at(pos)` — returns character at logical position (handles gap transparently)
- `gb_insert(pos, text, len)` — insert text at logical position
- `gb_delete(pos, len)` — delete len bytes at logical position (expands gap)
- `gb_load(filename)` — opens and loads a file
- `gb_load_fp(f, filename)` — loads from a pre-opened `FILE*` (see Section 7.2)
- `gb_save(filename)` — saves buffer to file
- `gb_load_more(n)` — loads next n bytes from tail (large-file paging)

### 6.4 Large File Support (Sliding Window)
When a file exceeds the available buffer, HVI loads the first `BUF_MAX` bytes and records the byte offset where loading stopped (`tail_offset`) and the source filename (`tail_file`). Additional state:
- `win_start`: byte offset in the original file where the in-memory buffer begins (for files scrolled forward)
- `tail_offset`: byte offset in `tail_file` where the unloaded portion begins (0 = fully loaded)
- `tail_file[PATH_MAX]`: filename of the original source

On every `:w` save, the full file is reconstructed: head (bytes before `win_start`) + in-memory buffer + tail (bytes from `tail_offset`). When saving to the same file that holds the tail, a temporary file `HVITMP.TMP` is used as an intermediate to avoid reading and writing the same file simultaneously.

Paging in: `gb_load_more(LOAD_CHUNK)` reads the next `LOAD_CHUNK = 4096` bytes from the tail. If the buffer is full, `gb_discard_head(n)` discards n bytes from the head (advancing `win_start`) to make room. The discard is capped at `ed.cur_pos` so the cursor is never lost.

Limitations: edits are restricted to the in-memory window. Content beyond `tail_offset` is preserved verbatim on save but cannot be edited until paged in.

### 6.5 Undo
Single-level undo. The `UndoRec` struct stores:
- `type`: `UNDO_NONE`, `UNDO_INSERT`, or `UNDO_DELETE`
- `pos`: buffer position of the operation
- `len`: number of characters
- `was_clean`: non-zero if the buffer was unmodified before this operation (allows `u` to clear the modified flag)
- `text[UNDO_MAX]`: saved text for delete operations (insert operations recover text from the buffer)

`UNDO_MAX = 1024` bytes. Delete operations that exceed this are truncated in the undo record.

### 6.6 Yank Buffer
- `yank[YANK_MAX]`: null-terminated yanked text, `YANK_MAX = 1024`
- `yank_len`: length of yanked text
- `yank_line`: non-zero if the yank was a whole-line operation (affects `p`/`P` paste behavior)

### 6.7 Dot-Repeat Buffer
Tracks the last change command for replay with `.`:
- `dot_cmd`: primary command key (0 = none)
- `dot_motion`: motion for `d`/`c` operators
- `dot_arg`: extra argument (replacement character for `r`)
- `dot_count`: count when the command was originally issued
- `dot_len`: length of stored insertion text
- `dot_text[DOT_TEXT_MAX]`: text to re-insert for insert-mode commands, `DOT_TEXT_MAX = 128`

Insert text is captured when ESC exits insert mode, reading from `undo.pos` for `undo.len` bytes (the text is still in the buffer at that point). If inserted text exceeds `DOT_TEXT_MAX`, it is silently truncated.

### 6.8 Search
- `search[SEARCH_MAX]`: current search pattern, `SEARCH_MAX = 64`
- `search_dir`: `SEARCH_FWD` (+1) or `SEARCH_BWD` (-1)
- Search is a simple substring scan (no regex). Wraps around end/beginning of buffer.

### 6.9 Screen Model
- The text area occupies rows `0` through `scr_rows - 2`.
- Row `scr_rows - 1` is the status/command line.
- **Visual rows**: lines wider than `scr_cols` wrap to additional terminal rows. The unit of vertical measurement is one terminal line of content. `top_pos` may point to the middle of a logical line (start of any visual row).
- `TAB_STOP = 8`: tab characters expand to the next multiple of 8.
- Lines past the end of the file are shown as `~` in column 0 (except row 0).
- Status line shows: filename, `[+]` if modified, current line number, or a transient message (search result, error, mode indicator).

---

## 7. Compiler Constraints and Workarounds

### 7.1 K&R C Style
HI-TECH C V3.09 uses K&R (pre-ANSI) C syntax. All function definitions use the old-style parameter declaration syntax:
```c
int gb_insert(pos, text, len)
int   pos;
char *text;
int   len;
{
    ...
}
```
ANSI-style prototypes (`int gb_insert(int pos, char *text, int len)`) are **not** used. All extern declarations in `hvi.h` use empty parameter lists:
```c
int gb_insert(/* int pos, char *text, int len */);
```

### 7.2 Heap Exhaustion and `fopen()` Failure
**Problem:** HI-TECH C's `fopen()` allocates a `BUFSIZ`-sized I/O buffer from the heap for each file it opens. `gb_init()` allocates nearly the entire heap for the gap buffer, leaving no room for `fopen()` to succeed. Every subsequent file open returns `NULL`.

**Root cause:** The heap on this CP/M system is approximately 24–40 KB. A single `malloc` call can consume virtually all of it.

**Solution:** In `hvi.c::main()`, the input file is opened with `fopen()` **before** `gb_init()` is called — while the heap is still fully available. The resulting `FILE*` is passed to `gb_load_fp()`, a variant of `gb_load()` that accepts an already-open file handle rather than a filename. `gb_init()` then allocates the gap buffer from whatever heap remains.

After `gb_load_fp()` closes the file with `fclose()`, the I/O buffer is returned to the heap (assuming HI-TECH C's `fclose()` calls `free()` internally), making it available for subsequent `fopen()` calls in `gb_save()` and `gb_load_more()`.

**Probe strategy (abandoned):** An earlier attempt used a probe allocation to find the largest available heap block, then freed it and re-allocated a smaller block to leave headroom. This failed because on CP/M, `free()` may not reliably return memory to the free list, causing the probe to permanently consume heap in addition to the actual allocation.

### 7.3 HI-TECH C Compiler Limits

#### 7.3.1 Optimizer Memory Limit: "optim: Out of memory"
**Problem:** HI-TECH C's optimizer has a fixed memory budget for processing a single function. Functions beyond a certain complexity cause the compiler to abort with `optim: Out of memory`.

**Solution:** Keep all functions small. When a function grows too large (typically beyond ~80–120 lines with many local variables and branches), split it into multiple smaller static helper functions.

**Affected functions in this codebase:**
- `gap.c::gb_save()` — split into `gb_write_head()`, `gb_write_buf()`, `gb_write_tail()`
- `gap.c::gb_load()` — split out `gb_record_tail()`
- `gap.c::gb_load_more()` — split out `gb_discard_head()`
- `edit.c::normal_cmd()` — split into `normal_edit_cmd()`, `normal_delchg_cmd()`, `normal_misc_cmd()`, `normal_find_cmd()`, `normal_page_cmd()`

#### 7.3.2 Per-File Label Limit: "Too many temporary labels"
**Problem:** HI-TECH C generates an assembly label for every `if`, `while`, `for`, `&&`, `||`, `switch` case, and ternary expression. There is a hard per-file limit on the total number of these temporary labels. When a `.C` file generates too many labels across all its functions combined, the assembler phase aborts with `Too many temporary labels`.

This is distinct from the optimizer memory limit: splitting a large function into smaller functions within the **same** `.C` file does not help, because the label count is per-file, not per-function.

**Solution:** Move code to a **new source file**. Choose a logically cohesive cluster of functions and extract them into a separate `.C` file that is compiled and linked independently.

**Affected in this codebase:**
- `erepeat.c` was created to hold `dot_ins_position()`, `dot_replay_c()`, and `dot_replay()`, which were extracted from `edit.c` to reduce `edit.c`'s total label count.

### 7.4 Variable Declaration Rules (K&R C)
All variable declarations must appear at the **top** of their enclosing block, before any statements. Declaring a variable after a statement in the same block is illegal in K&R C and will cause a compile error.

```c
/* ILLEGAL in K&R C: */
int x = 5;
foo();
int y = 10;   /* error: declaration after statement */

/* CORRECT: */
int x, y;
x = 5;
foo();
y = 10;
```

This applies inside `switch` case bodies as well — use explicit `{ }` blocks to create a new scope when you need local variables per case.

**Special caution with `long`:** Declaring a `long` variable inside a nested block (rather than at the function top) has been observed to cause optimizer failures on HI-TECH C. All `long` variables should be declared at the top of their function.

### 7.5 No `memmove()`
HI-TECH C V3.09 does not include `memmove()` in its library. The gap buffer requires an overlapping-safe memory copy for gap movement. A private implementation named `gb_memmove()` is provided in `gap.c`.

### 7.6 Integer Width
On Z80, `int` is 2 bytes (range −32768 to 32767). Buffer positions and sizes are stored as `int`, which limits the effective buffer to 32 KB. This is acceptable given CP/M TPA constraints. File offsets (e.g., `tail_offset`, `win_start`) are stored as `long` (4 bytes) to support files larger than 32 KB.

### 7.7 Terminal Input
CP/M does not have a `termios`-style raw mode API. HI-TECH C's `getch()` function reads a single character without echo using the BIOS CONIN call directly. No setup or teardown is required for raw mode.

### 7.8 Terminal Size Query and BIOS CONIN
**Problem:** The ANSI CPR response (`ESC[rows;colsR`) is fragmented on CP/M emulators running under a Unix host with canonical (line-buffered) terminal mode. The ESC byte arrives immediately, but the remaining bytes are buffered until Enter is pressed.

**Solution:** `term_getsize()` uses a two-phase approach:
1. **Phase 1:** Poll BIOS CONST (`bios(2,0,0)`) up to 30,000 times waiting for any byte.
2. **Phase 2:** Read the full CPR sequence using BIOS CONIN (`bios(3,0,0)`) directly, bypassing BDOS buffering. No BDOS console output is performed between CONIN reads.

All debug output from the size query is deferred until after Phase 2 completes, because BDOS console-write calls between reads can consume buffered response bytes on some CP/M implementations.

If the terminal does not respond, `DEF_ROWS = 24` and `DEF_COLS = 80` are used.

### 7.9 File I/O Buffer Count
`gb_save()` opens at most **two** files simultaneously: the output file and the source file (for head/tail reads). The pre-open strategy ensures the initial file's I/O buffer is allocated before the gap buffer consumes the heap. If `free()` works (returns memory to the heap on `fclose()`), the pattern of open-read/write-close ensures only one or two file handles are live at any moment.

---

## 8. File Format

- Files are read in binary mode (`"rb"`).
- On load: bare `CR` (0x0D) characters are stripped. `CR+LF` pairs are normalized to `LF`.
- On save: each `LF` is written as `CR+LF` (CP/M convention).
- CP/M EOF marker `Ctrl-Z` (0x1A) terminates saved files.
- On load: `Ctrl-Z` stops reading (CP/M EOF convention).
- Internal representation: pure `LF`-terminated lines.

---

## 9. Constants (`hvi.h`)

| Constant | Value | Purpose |
|----------|-------|---------|
| `DEF_COLS` | 80 | Default terminal width |
| `DEF_ROWS` | 24 | Default terminal height |
| `GAP_MIN` | 256 | Minimum gap size before buffer is considered full |
| `BUF_MAX` | 30000 | Target maximum content size for gap buffer allocation |
| `LOAD_CHUNK` | 4096 | Bytes loaded per `gb_load_more()` call |
| `UNDO_MAX` | 1024 | Maximum bytes saved in a single undo record |
| `DOT_TEXT_MAX` | 128 | Maximum bytes of inserted text stored for dot-repeat |
| `YANK_MAX` | 1024 | Maximum bytes in the yank buffer |
| `TAB_STOP` | 8 | Tab stop width in columns |
| `SEARCH_MAX` | 64 | Maximum search pattern length |
| `PATH_MAX` | 64 | Maximum filename length |
| `STATUS_MAX` | 128 | Maximum status message length |
| `CMD_MAX` | 128 | Maximum ex command-line length |

---

## 10. Supported Commands

### 10.1 Normal Mode — Movement

| Key | Action |
|-----|--------|
| `h` | Move left one character (within line) |
| `l` | Move right one character (within line) |
| `j` | Move down one line (preserves want_col) |
| `k` | Move up one line (preserves want_col) |
| `w` | Forward to start of next word |
| `b` | Backward to start of previous word |
| `e` | Forward to end of word |
| `0` | Move to beginning of line |
| `^` | Move to first non-blank of line |
| `$` | Move to end of line |
| `G` | Go to last line (or line N with count prefix) |
| `gg` | Go to first line (or line N: `5gg`) |
| `Ctrl-F` | Scroll forward one page |
| `Ctrl-B` | Scroll backward one page |
| `Ctrl-D` | Scroll forward half page |
| `Ctrl-U` | Scroll backward half page |

Vertical movement maintains a "wanted column" (`want_col`) so that `j`/`k` through short lines returns to the original column when a longer line is reached.

`j` triggers `gb_load_more()` when the cursor approaches the end of loaded content and a tail exists.

### 10.2 Normal Mode — Insert / Append

| Key | Action |
|-----|--------|
| `i` | Insert before cursor |
| `a` | Append after cursor |
| `I` | Insert at beginning of line (first non-blank) |
| `A` | Append at end of line |
| `o` | Open new line below, enter insert mode |
| `O` | Open new line above, enter insert mode |
| `s` | Substitute character(s): delete then insert |
| `S` | Substitute entire line: delete line content then insert |

### 10.3 Normal Mode — Delete / Change

| Key | Action |
|-----|--------|
| `x` | Delete character under cursor |
| `X` | Delete character before cursor |
| `dd` | Delete current line |
| `dw` | Delete word forward |
| `db` | Delete word backward |
| `d$` | Delete to end of line |
| `d0` | Delete to beginning of line |
| `dG` | Delete to end of file |
| `D` | Delete to end of line (alias for `d$`) |
| `cc` | Change current line |
| `cw` | Change word |
| `c$` | Change to end of line |
| `C` | Change to end of line (alias for `c$`) |
| `r` | Replace single character under cursor |
| `J` | Join line below to current line (inserts space) |
| `~` | Toggle case of character under cursor |

### 10.4 Normal Mode — Yank and Put

| Key | Action |
|-----|--------|
| `yy` | Yank (copy) current line |
| `Y` | Yank current line (alias for `yy`) |
| `yw` | Yank word |
| `y$` | Yank to end of line |
| `p` | Put after cursor / below current line (linewise) |
| `P` | Put before cursor / above current line (linewise) |

### 10.5 Normal Mode — Search

| Key | Action |
|-----|--------|
| `/` | Search forward for pattern |
| `?` | Search backward for pattern |
| `n` | Repeat last search in same direction |
| `N` | Repeat last search in opposite direction |

Search is a plain substring match (no regular expressions). Wraps around end of buffer.

### 10.6 Normal Mode — Character Search (within current line)

| Key | Action |
|-----|--------|
| `f{c}` | Move to next occurrence of `c` on current line |
| `F{c}` | Move to previous occurrence of `c` on current line |
| `;` | Repeat last `f`/`F` in same direction |
| `,` | Repeat last `f`/`F` in opposite direction |

All accept a count prefix (`3;` = skip to third match).

### 10.7 Normal Mode — Miscellaneous

| Key | Action |
|-----|--------|
| `.` | Repeat last change |
| `u` | Undo last change (single level) |
| `:` | Enter ex command mode |
| `Ctrl-L` | Redraw screen |

### 10.8 Dot-Repeat (`.`) Behavior

The `.` command repeats the last **change** at the current cursor position:

| Last command | Dot behavior |
|-------------|-------------|
| `x` / `X` | Delete same count of characters |
| `r{c}` | Replace current character with same `{c}` |
| `D` | Delete to end of line |
| `~` | Toggle case of same count of characters |
| `J` | Join same count of lines |
| `dd` / `dw` / `d$` etc. | Re-apply same delete motion |
| `cw<text>ESC` / `cc<text>ESC` | Delete same motion range and re-insert same text |
| `C<text>ESC` | Change to EOL and re-insert same text |
| `i<text>ESC` | Re-insert same text at current position |
| `a<text>ESC` | Re-append same text (cursor advances past current char first) |
| `A<text>ESC` | Re-append same text at end of current line |
| `I<text>ESC` | Re-insert same text at first non-blank of line |
| `o<text>ESC` | Open new line below and re-insert same text |
| `O<text>ESC` | Open new line above and re-insert same text |
| `s<text>ESC` / `S<text>ESC` | Re-insert same text at current position |

An explicit count prefix to `.` (e.g., `3.`) overrides the stored count. Bare `.` uses the stored count from the original command.

Inserted text for dot-repeat is captured at ESC time from `undo.pos` / `undo.len`, capped at `DOT_TEXT_MAX = 128` bytes. Yank (`y`) commands do not update the dot-repeat record.

### 10.9 Count Prefix

Most commands accept a numeric count prefix that repeats or scales the operation:
- `5j` — move down 5 lines
- `3dw` — delete 3 words
- `2dd` — delete 2 lines
- `10G` — go to line 10
- `3;` — repeat `f`/`F` search 3 times

### 10.10 Insert Mode

| Key | Action |
|-----|--------|
| (any printable) | Insert character at cursor |
| `Enter` / `Ctrl-M` | Insert newline |
| `Backspace` / `Ctrl-H` / `DEL` | Delete previous character |
| `Ctrl-W` | Delete previous word |
| `Ctrl-U` | Delete to start of line |
| `ESC` | Return to normal mode |

Insert mode optimises terminal output on slow (9600 baud) connections:
- Regular character (no wrap): emit the byte directly; no escape sequences.
- Backspace at end of line: `BS` + `ESC[K`.
- Character causing a visual-row wrap: redraw from cursor row to bottom.
- Newline / backspace over newline / `Ctrl-W`: full screen refresh.
- ESC: redraw only the edited line unless the viewport scrolled.
- The `-- INSERT --` mode indicator is shown once on mode entry, not refreshed on every keypress.

### 10.11 Ex Commands

| Command | Action |
|---------|--------|
| `:w` | Write (save) current file |
| `:w filename` | Write to named file |
| `:q` | Quit (fails if unsaved changes) |
| `:q!` | Quit and discard changes |
| `:wq` | Write then quit |
| `:wq!` | Write and quit (force) |
| `:x` | Write if modified, then quit |
| `:x!` | Write and quit (force) |
| `:r filename` | Read file and insert after current line |
| `:N` | Go to line number N (1-based) |
| `:$` | Go to last line (loads entire tail for large files) |

On quit (`ed.quit = 1`), the screen is **not** redrawn — the editor exits immediately after `term_restore()`.

---

## 11. Screen Rendering

### 11.1 Viewport
`top_pos` stores the buffer position of the first character visible on screen. It may point to the middle of a wide logical line (the start of a visual row resulting from line wrap). `scr_scroll_to_cursor()` adjusts `top_pos` when the cursor moves outside the visible area.

### 11.2 Long Line Wrapping
Lines wider than `scr_cols` wrap to additional screen rows. There is no horizontal scrolling. A visual row is one terminal line of content. The editor tracks visual rows using `next_vrow()` (advance one visual row) and `vrow_start_of()` (find the visual row containing a buffer position).

### 11.3 Status Bar
The bottom row (`scr_rows - 1`) shows either:
- A transient message stored in `ed.status` (search results, errors, mode indicators), displayed in reverse video; or
- The default status: `"filename" [+] L<current>/<total>` (filename in quotes, optional `[+]` if modified, current line number and total line count), also in reverse video. When the file has unloaded tail content (`ed.tail_offset > 0`), a `+` is appended to the total: `"filename" [+] L42/300+`.

Transient messages are cleared on the next keypress in normal mode, or replaced with `-- INSERT --` in insert mode.

---

## 12. Terminal Interface (`term.c`)

All output uses `putchar()` to `stdout`. ANSI escape sequences used:

| Sequence | Purpose |
|----------|---------|
| `ESC[2J` | Clear screen |
| `ESC[K` | Clear to end of line |
| `ESC[r;cH` | Move cursor to row r, column c (1-based) |
| `ESC[1m` | Bold attribute |
| `ESC[7m` | Reverse video |
| `ESC[0m` | Reset attributes |
| `ESC[999;999H` + `ESC[6n` | Terminal size query (CPR) |
| `ESC[r;cR` | Terminal size response (parsed in `term_getsize`) |

Input uses `getch()` from the HI-TECH C runtime, which calls BIOS CONIN directly and returns immediately without echo. Terminal size reads use `bios(3,0,0)` (BIOS CONIN) to bypass BDOS buffering.

---

## 13. Operator-Motion Model (`emove.c`)

The `apply_op(op, from, to, linewise)` function applies an operator to a range:
- `'d'`: saves undo, saves to yank buffer, deletes range
- `'c'`: saves undo, saves to yank buffer, deletes range, calls `undo_save_insert`, enters insert mode
- `'y'`: saves to yank buffer only

`motion_endpoint(ch, count, &linewise)` computes the endpoint position for a given motion character and count. Supported motions: `h`, `l`, `w`, `b`, `e`, `$`, `0`, `^`, `j`, `k`, `G`.

---

## 14. Known Limitations

1. **Single-level undo.** Only the most recent change can be undone. `u` after `u` is a no-op.
2. **Edits restricted to loaded window.** For large files, only the in-memory portion is editable. The unloaded tail is preserved verbatim on save.
3. **Dot-repeat text truncated at 128 bytes.** Long insertions are truncated silently.
4. **No backward paging for large files.** `gb_discard_head()` can discard the head to make room for forward paging, but there is no mechanism to reload previously discarded head content.
5. **No visual/block selection mode.**
6. **No macro recording or playback.**
7. **No window splitting.**
8. **No regex search** — plain substring match only.
9. **`~` (toggle case) is single-level undo per character.** With a count, only the last character's toggle is undoable.
10. **No `:s` (substitute) command.**
11. **Terminal size query may require Enter on some CP/M setups** if the BIOS CONIN is still line-buffered.
12. **Filenames limited to `PATH_MAX = 64` characters**, which exceeds CP/M's 8.3 limit but may be relevant on cross-platform use.

---

## 15. Design Decisions

| Decision | Rationale |
|----------|-----------|
| Single global `Editor` struct | Avoids pointer-passing overhead in K&R C; simplifies all function signatures |
| Gap buffer (not line array) | O(1) insert/delete at cursor; O(n) gap move is acceptable for typical edit patterns |
| Pre-allocated gap buffer (no resize) | CP/M has no virtual memory; `realloc` is unreliable; buffer is fixed at startup |
| Open file before `gb_init()` | Prevents heap exhaustion from leaving no room for `fopen()`'s I/O buffer |
| Single-level undo | Memory constraint: a full undo stack would require significant heap |
| Functions split for optimizer | HI-TECH C's optimizer has a per-function memory limit; large functions must be split |
| `erepeat.c` extracted from `edit.c` | HI-TECH C has a per-file total label limit; moving dot-repeat functions to a new file reduced `edit.c` below the limit |
| `long` variables at function top | Inner-block `long` declarations have been observed to cause optimizer failures |
| `gb_memmove()` in gap.c | HI-TECH C V3.09 does not include `memmove()` |
| BIOS CONIN for size query reads | BDOS console output between reads can consume buffered terminal response bytes |
| No `scr_refresh()` on quit | Avoids unnecessary terminal I/O when the user is about to see the shell prompt |
| `HVITMP.TMP` for large-file saves | Prevents reading and writing the same file simultaneously when saving to the tail source |
| Dot-repeat captures text at ESC | At ESC time the inserted text is contiguous in the buffer at `undo.pos`; simple loop copies it |

---

## 16. License

MIT License. Copyright (c) Juan Orlandini.
