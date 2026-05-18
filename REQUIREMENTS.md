# HVI — Requirements and Implementation Reference

**Author:** Juan Orlandini  
**License:** MIT  
**Version:** 2.0  
**Date:** 2026-05-16

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
| `cstart.as` | Custom Z80 startup: sets stack, heap_base, bdos_base; replaces CRTCPM.OBJ |
| `hvi.c` | Main entry point, argument parsing, startup sequence |
| `hvi.h` | Shared types, constants, and extern declarations |
| `gap.c` | Gap buffer: allocation, load, save, insert, delete, large-file paging |
| `cpmio.c` | Direct BDOS file I/O and heap allocator (replaces stdio and malloc) |
| `util.c` | String utilities and `hvi_sprintf` (replaces `<string.h>` and `<stdio.h>` sprintf) |
| `term.c` | Terminal I/O: ANSI escape output, raw input, size query |
| `screen.c` | Viewport rendering: line drawing, cursor placement, status bar |
| `emove.c` | Cursor movement, operator application (`apply_op`), search |
| `edit.c` | VI normal mode, insert mode, ex command-line mode |
| `erepeat.c` | Dot-repeat: `dot_ins_position`, `dot_replay_c`, `dot_replay` |
| `ex.c` | Ex command execution (`:q`, `:w`, `:r`, `:N`, etc.) |

---

## 4. Build

### 4.1 Prepare LX.LIB (one-time)
`cstart.as` provides its own `csv`/`cret` symbols, which conflict with those in
`LIBC.LIB`. Create `LX.LIB` once by removing `csv.obj`:
```
PIP LX.LIB=LIBC.LIB
LIBR d LX.LIB csv.obj
```

### 4.2 Compile and Link (on CP/M)
```
C -CPM -O -C cstart.as
C -CPM -O -C hvi.c gap.c term.c screen.c emove.c edit.c erepeat.c ex.c util.c cpmio.c
```
Link (backslash `\` continues past CP/M's 128-character line limit):
```
l
-Ptext=100H,data,bss -C100H -oh.com CSTART.OBJ CPMIO.OBJ UTIL.OBJ \
GAP.OBJ TERM.OBJ SCREEN.OBJ EMOVE.OBJ EREPEAT.OBJ EX.OBJ EDIT.OBJ HVI.OBJ LX.LIB
```
```
REN HVI.COM=H.COM
```

**Link order is significant.** The HI-TECH linker is single-pass for object
files: a `CALL` to an undefined symbol resolves to 0x0000 (below the 0x0100
file base), producing "code below file base" errors. Every module must appear
after all modules it calls. `CSTART.OBJ` must be first so it lands at 0x0100.
`LX.LIB` is last so library symbols resolve to any already-defined module
symbols.

**`-Ptext=100H,data,bss` is required.** Without it the linker places `data`
and `bss` psects at address 0, causing "code below file base" for every static
variable reference.

### 4.3 Debug Build
Add `-H` to each compile step to include a symbol table for use with a
debugger.

To enable I/O tracing (BDOS 33 sector reads, buffer reloads), uncomment
`#define HVI_DEBUG` in `hvi.h` and recompile all files. Debug output goes
directly to the CP/M console via BDOS 2 and will be overwritten by the next
`scr_refresh()`. To compile with the debug flag defined from the command line
(if your HI-TECH C version supports it): add `-DHVI_DEBUG` to each `C` call.

### 4.4 Cross-Compilation (Linux/macOS)
Use the same flags and link order on a Unix host with the HI-TECH Z80
cross-compiler, then transfer the resulting `.com` to CP/M via XMODEM, ZMODEM,
or disk image.

---

## 5. Invocation

```
HVI [filename]
```

- If `filename` is given and exists, it is loaded into the gap buffer.
- If `filename` is given but does not exist, the editor starts with an empty buffer and the status shows `[New File]`.
- If no filename is given, the editor starts with an empty unnamed buffer.

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
- `gb_load(filename, fp)` — opens and loads a file; when `fp` is non-NULL, uses that already-open handle instead of calling `fopen()` (see Section 7.2)
- `gb_save(filename)` — saves buffer to file
- `gb_load_more(n)` — loads next n bytes from tail (large-file paging)

### 6.4 Large File Support (Sliding Window)

When a file exceeds the available buffer, HVI loads the first `BUF_MAX` bytes
and records where loading stopped. Relevant `Editor` fields:

| Field | Meaning |
|-------|---------|
| `win_start` | Byte offset in `tail_file` where the in-memory window begins |
| `tail_offset` | Byte offset in `tail_file` where the unloaded tail begins (0 = fully loaded) |
| `tail_file[]` | Filename of the current backing file (original or swap) |

**Paging forward:** `gb_load_more(LOAD_CHUNK)` reads the next 4096 bytes from
`tail_file` at `tail_offset`. If the buffer is full, `gb_discard_head(n)`
removes n bytes from the beginning, advancing `win_start` and adjusting
`cur_pos`/`top_pos`. The discard is capped at `cur_pos` so the cursor is
never lost.

**Paging backward:** `gb_reload_from(offset)` uses BDOS 33 (Read Random) to
seek directly to `offset` in `tail_file` and refills the buffer from there.
No sequential scan is required.

**Edit preservation across window shifts:** Before any `gb_reload_from()` call
that navigates to a new window, if `ed.modified` is set, `gb_reload_from`
automatically calls `gb_save("HVISWP.TMP")` to flush all in-memory edits to
the swap file. `ed.tail_file` is then updated to `HVISWP.TMP`, which contains
the complete, up-to-date file content. Subsequent reads and reloads come from
the swap file, ensuring that edits made anywhere in the file are never silently
discarded when the window shifts.

**Buffer overflow during editing:** When `gb_insert()` fails because the
buffer is full (gap < `GAP_MIN`), `gb_make_room()` is called. It saves the
complete file to `HVISWP.TMP`, then calls `gb_reload_from()` with a window
centred on `ed.cur_pos`. The cursor position is restored by scanning the new
buffer. Editing can then continue without interruption.

**Line navigation in large files:** `gb_goto_line(n)` finds line N by
sequentially scanning `tail_file` from byte 0, counting LF characters until
line N−1 is located. It then positions the window half a `LOAD_CHUNK` before
that byte offset and computes the local buffer line number with a second short
scan. `nG` calls `gb_goto_line(n)` for all cases where the target is not
already in the buffer starting at byte 0.

**File size probe:** `hvi_fsize(name)` uses an exponential expansion followed
by binary search with BDOS 33 probes to determine the sector-rounded file size
in O(log N) seeks (~21 for a 1 MB file). This avoids BDOS 35 (Compute File
Size), which is not implemented on some CP/M emulators.

**Save:** `gb_save(filename)` reconstructs the full file by calling
`gb_write_head()` (bytes 0..`win_start`−1 from `tail_file`), `gb_write_buf()`
(the in-memory buffer, LF→CR+LF), and `gb_write_tail()` (bytes
`tail_offset`..end from `tail_file`). When saving to the same file that holds
the tail, `HVITMP.TMP` is used as an intermediate to prevent reading and
writing the same file simultaneously.

**Temporary files:**

| File | Purpose |
|------|---------|
| `HVISWP.TMP` | Swap file written before any window shift or buffer overflow recovery |
| `HVITMP.TMP` | Intermediate used when saving back to the current tail source file |

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
- `search_wrapped`: set to 1 when the match required crossing the true
  end-of-file (or start-of-file) boundary; cleared otherwise
- Search is a case-insensitive plain substring scan (no regex). In a small file
  (fully loaded), it wraps around the buffer. In a large file it searches the
  entire file in three phases — see §6.4a.

### 6.4a Large-File Search

`do_search_full(start_pos)` in `emove.c` orchestrates search across the entire
file:

**Phase 1 — Buffer scan:** `do_search_from(start_pos)` searches the in-memory
gap buffer with wrap-around. It sets `ed.search_wrapped` if the match position
is on the "other side" of the start position (i.e., required wrapping within
the buffer). A match found without wrap is returned immediately.

**Phase 2 — File section scan:** If Phase 1 found no match, or found one only
after wrapping within the buffer, `scan_file_for_match()` reads unloaded file
sections directly via `hvi_fopen`/`hvi_fgetc`:
- For `SEARCH_FWD`: scans the unloaded **tail** first (bytes from
  `tail_offset` to EOF — after the buffer in file order, not a wrap), then the
  unloaded **head** (bytes 0 to `win_start` — before the buffer, a true wrap).
- For `SEARCH_BWD`: scans the unloaded **head** first (not a wrap), then the
  **tail** (a true wrap).

`ed.search_wrapped` is set to 1 only when the file match came from the
"wrapped" section (head for FWD, tail for BWD). A match in the non-wrapped
section (tail for FWD, head for BWD) is genuinely ahead/behind the cursor in
file order and does not set the wrap flag.

**Window reload:** When a match is found in a file section, `gb_reload_from()`
centres a new window around the match position and Phase 1 re-runs to locate the
exact buffer offset.

`scan_file_for_match(from_off, to_off, dir)` scans `[from_off, to_off)` in the
backing file:
- `SEARCH_FWD`: returns the file offset of the first match.
- `SEARCH_BWD`: scans forward through the range but returns the file offset of
  the **last** match (the one nearest `to_off`, i.e., closest to the buffer edge
  going backward).

### 6.9 Screen Model
- The text area occupies rows `0` through `scr_rows - 2`.
- Row `scr_rows - 1` is the status/command line.
- **Visual rows**: lines wider than `scr_cols` wrap to additional terminal rows. The unit of vertical measurement is one terminal line of content. `top_pos` may point to the middle of a logical line (start of any visual row).
- `TAB_STOP = 8`: tab characters expand to the next multiple of 8.
- Lines past the end of the file are shown as `~` in column 0 (except row 0).
- Status line shows: filename, `[+]` if modified, current line number, or a transient message (search result, error, mode indicator).
- `line_cnt_cached` in the `Editor` struct caches the total line count computed by `scr_line_count()`. Set to 0 (invalid) by `gb_insert()` and `gb_delete()` whenever buffer content changes; recomputed on the next status bar update. This avoids an O(buffer) scan on every cursor movement.

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

### 7.2 Eliminating `stdio` and `malloc` (cpmio.c)

**Problem (resolved in v2.0):** HI-TECH C's `fopen()` allocates a
`BUFSIZ`-sized I/O buffer from the heap for each file it opens. `gb_init()`
allocates nearly the entire heap for the gap buffer, leaving no room for
`fopen()` buffers. Additionally, `malloc`/`free` on CP/M do not reliably
recycle memory, making repeated allocations unpredictable.

**Solution:** `cpmio.c` replaces the entire HI-TECH C buffered-I/O library
with direct CP/M BDOS calls. It provides:

- `HFILE` — a file handle struct with its own 128-byte sector buffer (no heap
  allocation needed per open)
- `hvi_fopen` / `hvi_fclose` — BDOS 15 (Open), 16 (Close), 22 (Make)
- `hvi_fgetc` / `hvi_fputc` — BDOS 20 (Read Sequential), 21 (Write Sequential)
- `hvi_fseek` — BDOS 33 (Read Random) for direct sector seek
- `hvi_ftell` — returns the cached `pos` field
- `hvi_remove` / `hvi_rename` — BDOS 19 (Erase), 23 (Rename)
- `hvi_fsize` — exponential+binary search via BDOS 33
- `hvi_malloc` / `hvi_free` — single-shot allocator from the free TPA between
  BSS end and the BDOS base (set by `cstart.as`)

A static pool of `MAX_HFILES = 3` HFILE slots serves all concurrent file
opens. At most two handles are ever open simultaneously (one write, one read)
in the save path. No heap is consumed by file I/O.

**CP/M BDOS compatibility fixes in `cpmio.c`:**

*Skip BDOS 16 (Close File) for read-only handles.* On some CP/M emulators,
calling BDOS 16 on a file opened for reading writes the FCB's `CR`/`EX` fields
back to the directory entry. The next BDOS 15 (Open File) then loads a
corrupted extent record, causing BDOS 33 (Read Random) to return sector-0 data
for all subsequent random reads — the symptom is the first sector of the file
appearing to repeat. `hvi_fclose()` skips BDOS 16 entirely for handles opened
in read mode (`mode == 1`).

*Explicitly set FCB byte 32 (CR) before every BDOS 33 call.* The CP/M 2.2
standard uses FCB bytes 33–35 (`r0`/`r1`/`r2`) as the 24-bit random record
number for BDOS 33. Some emulators also use byte 32 (`CR`) as a position hint.
`hvi_fgetc()` and `hvi_fseek()` now write the sector number into both the
`r0`/`r1`/`r2` fields (bytes 33–35) and into `CR` (byte 32) before each BDOS 33
call, ensuring correct sector addressing on both standard-conforming and
non-conforming implementations.

`util.c` provides string utilities (`hvi_strlen`, `hvi_strcpy`, `hvi_strcmp`,
`hvi_strncpy`, `hvi_sprintf`) so that `<string.h>` and the stdio `sprintf` are
not needed.

`cstart.as` (custom startup) reads the BDOS base address from page zero and
stores it in `bdos_base`; it also sets `heap_base` to the first byte after BSS
(`__Hbss`). Both are used by `hvi_malloc` to bound the single-shot allocation.
The stack is initialised to `bdos_base` (growing downward) so the full free
TPA between BSS and BDOS is available for the gap buffer.

### 7.3 IX Frame Elimination (Binary Size Optimisation)

HI-TECH C V3.09 generates an IX-register stack frame in every function that has at least one `auto` (stack-allocated) local variable. The prologue/epilogue sequence (`PUSH IX` / `LD IX,0` / `ADD IX,SP` / one `DEC SP` per local byte / `LD SP,IX` / `POP IX`) costs 12–16 bytes per function. For a codebase with many small functions this overhead is significant.

**Solution:** Declare local variables as `static` instead of `auto`. File-level `static` variables and function-scope `static` variables are placed in BSS (fixed addresses) and accessed without IX. If **all** locals in a function are `static` (or if the function has no locals), the compiler emits no IX frame at all.

**Constraints:**
- Only safe for non-recursive, single-threaded functions (all functions in HVI satisfy this — CP/M is single-process, single-thread).
- Inner-block declarations (inside `switch` cases, `if` blocks) also contribute to the IX frame and must be hoisted to function-scope `static`.
- Functions with `static` locals are not re-entrant. This is intentional and safe here.

**Affected functions (v1.3):**

| File | Functions |
|------|-----------|
| `screen.c` | `next_vrow`, `vrow_start_of`, `scr_vrow_col`, `scr_pos_line`, `scr_cur_line`, `scr_pos_col`, `scr_line_start`, `scr_line_count`, `draw_row_at`, `scr_show_status`, `scr_update_after_move` |
| `edit.c` | `get_count`, `do_find`, `insert_key` |
| `emove.c` | `undo_save_delete`, `pos_at_col`, `mv_bnb`, `mv_eol`, `mv_right`, `mv_up`, `mv_down`, `mv_word_fwd`, `mv_word_back`, `mv_word_end`, `apply_op`, `read_pattern`, `do_search_from` |

**Additional dead code removed (v1.3):**
- `gb_load_fp()` — merged into `gb_load(filename, fp)` via an optional `FILE*` parameter. When `fp` is non-NULL it is used directly; otherwise `fopen()` is called internally. Eliminates the entire function body and its IX frame.
- `scr_redraw_cur_vrow()` — single-visual-row redraw used only by `r` (replace character). Replaced with `scr_redraw_cur_line()`, which redraws the full logical line. The result is identical for single-width lines and correct for wrapped lines. Eliminates ~60 bytes.
- `scr_line_end()` — declared in `hvi.h` and defined in `screen.c` but never called anywhere. Eliminated as dead code (~40 bytes).

### 7.4 HI-TECH C Compiler Limits

#### 7.4.1 Optimizer Memory Limit: "optim: Out of memory"
**Problem:** HI-TECH C's optimizer has a fixed memory budget for processing a single function. Functions beyond a certain complexity cause the compiler to abort with `optim: Out of memory`.

**Solution:** Keep all functions small. When a function grows too large (typically beyond ~80–120 lines with many local variables and branches), split it into multiple smaller static helper functions.

**Affected functions in this codebase:**
- `gap.c::gb_save()` — split into `gb_write_head()`, `gb_write_buf()`, `gb_write_tail()`
- `gap.c::gb_load()` — split out `gb_record_tail()`
- `gap.c::gb_load_more()` — split out `gb_discard_head()`
- `edit.c::normal_cmd()` — split into `normal_edit_cmd()`, `normal_delchg_cmd()`, `normal_misc_cmd()`, `normal_find_cmd()`, `normal_page_cmd()`

#### 7.4.2 Per-File Label Limit: "Too many temporary labels"
**Problem:** HI-TECH C generates an assembly label for every `if`, `while`, `for`, `&&`, `||`, `switch` case, and ternary expression. There is a hard per-file limit on the total number of these temporary labels. When a `.C` file generates too many labels across all its functions combined, the assembler phase aborts with `Too many temporary labels`.

This is distinct from the optimizer memory limit: splitting a large function into smaller functions within the **same** `.C` file does not help, because the label count is per-file, not per-function.

**Solution:** Move code to a **new source file**. Choose a logically cohesive cluster of functions and extract them into a separate `.C` file that is compiled and linked independently.

**Affected in this codebase:**
- `erepeat.c` was created to hold `dot_ins_position()`, `dot_replay_c()`, and `dot_replay()`, which were extracted from `edit.c` to reduce `edit.c`'s total label count.

### 7.5 Variable Declaration Rules (K&R C)
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

### 7.6 No `memmove()`
HI-TECH C V3.09 does not include `memmove()` in its library. The gap buffer requires an overlapping-safe memory copy for gap movement. A private implementation named `gb_memmove()` is provided in `gap.c`.

### 7.7 Integer Width
On Z80, `int` is 2 bytes (range −32768 to 32767). Buffer positions and sizes are stored as `int`, which limits the effective buffer to 32 KB. This is acceptable given CP/M TPA constraints. File offsets (e.g., `tail_offset`, `win_start`) are stored as `long` (4 bytes) to support files larger than 32 KB.

### 7.8 Terminal Input
CP/M does not have a `termios`-style raw mode API. HI-TECH C's `getch()` function reads a single character without echo using the BIOS CONIN call directly. No setup or teardown is required for raw mode.

### 7.9 Terminal Size Query and BIOS CONIN
**Problem:** The ANSI CPR response (`ESC[rows;colsR`) is fragmented on CP/M emulators running under a Unix host with canonical (line-buffered) terminal mode. The ESC byte arrives immediately, but the remaining bytes are buffered until Enter is pressed.

**Solution:** `term_getsize()` uses a two-phase approach:
1. **Phase 1:** Poll BIOS CONST (`bios(2,0,0)`) up to 30,000 times waiting for any byte.
2. **Phase 2:** Read the full CPR sequence using BIOS CONIN (`bios(3,0,0)`) directly, bypassing BDOS buffering. No BDOS console output is performed between CONIN reads.

All debug output from the size query is deferred until after Phase 2 completes, because BDOS console-write calls between reads can consume buffered response bytes on some CP/M implementations.

If the terminal does not respond, `DEF_ROWS = 24` and `DEF_COLS = 80` are used.

### 7.10 File I/O Handle Count
`cpmio.c` provides a static pool of `MAX_HFILES = 3` HFILE slots. Each HFILE
carries its own 128-byte sector buffer; no heap is involved. The maximum
concurrent open handles in any code path is two (one write handle for the
output file, one read handle for the head or tail source). Three slots provide
one spare. All file operations follow an open–use–close pattern within a single
function; no handle is held open across function call boundaries.

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
| `BUF_MAX` | 24000 | Target maximum content size for gap buffer |
| `LOAD_CHUNK` | 4096 | Bytes loaded per `gb_load_more()` call and per navigation step |
| `UNDO_MAX` | 1024 | Maximum bytes saved in a single undo record |
| `DOT_TEXT_MAX` | 128 | Maximum bytes of inserted text stored for dot-repeat |
| `YANK_MAX` | 1024 | Maximum bytes in the yank buffer |
| `TAB_STOP` | 8 | Tab stop width in columns |
| `SEARCH_MAX` | 64 | Maximum search pattern length |
| `PATH_MAX` | 64 | Maximum filename length |
| `STATUS_MAX` | 128 | Maximum status message length |
| `CMD_MAX` | 128 | Maximum ex command-line length |
| `HEOF` | −1 | End-of-file sentinel returned by `hvi_fgetc()` (replaces `stdio` EOF) |
| `MAX_HFILES` | 3 | Number of HFILE slots in the static pool in `cpmio.c` |

---

## 10. Supported Commands

### 10.1 Normal Mode — Movement

| Key | Action |
|-----|--------|
| `h` | Move left one character (within line) |
| `l` | Move right one character (within line) |
| `j` | Move down one line (preserves want_col) |
| `k` | Move up one line (preserves want_col) |
| `Enter` | Move to first non-blank character of the next line |
| `w` | Forward to start of next word |
| `b` | Backward to start of previous word |
| `e` | Forward to end of word |
| `0` | Move to beginning of line |
| `^` | Move to first non-blank of line |
| `$` | Move to end of line |
| `G` | Go to last line (or line N with count prefix) |
| `gg` | Go to first line (or line N: `5gg`) |
| `Ctrl-F` | Scroll forward one page; cursor lands at the middle row of the new page (first non-blank). No-op if the last line of the file is already visible. |
| `Ctrl-B` | Scroll backward one page; cursor lands at the middle row of the new page (first non-blank). No-op if `top_pos == 0` (file beginning already displayed). |
| `Ctrl-D` | Scroll forward half page |
| `Ctrl-U` | Scroll backward half page |

Vertical movement maintains a "wanted column" (`want_col`) so that `j`/`k` through short lines returns to the original column when a longer line is reached.

`j` triggers `gb_load_more()` when the cursor approaches the end of loaded content and a tail exists.

`Enter` behaves identically to `j` for screen update purposes but lands on the first non-blank character of the destination line (same as `^` after `j`). Accepts a count prefix.

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

Search is a case-insensitive plain substring match (no regular expressions).
In a large file, all unloaded sections are also searched — see §6.4a. The wrap
message is shown only when the search crossed a true file boundary.

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
| `:e filename` | Abandon current buffer and edit named file (fails if unsaved changes) |
| `:e! filename` | Abandon current buffer (discarding changes) and edit named file |
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
- The default status: `"filename" [+]` (filename in quotes, optional `[+]` if the buffer has unsaved changes), also in reverse video.

No line number or line count is displayed. Scanning the full file to produce an
accurate total would be too slow for large files on a real CP/M machine, and
a buffer-local count would be misleading when the window is mid-file.

Transient messages are cleared on the next keypress in normal mode, or replaced
with `-- INSERT --` in insert mode.

### 11.4 Rendering Tiers (Performance)

All screen output is sized to the minimum needed for the operation. From cheapest to most expensive:

| Tier | Function | When used |
|------|----------|-----------|
| Cursor move only | `scr_update_cursor()` | `j`, `k`, `Enter`, `w`, `b`, `e`, `/`, `?`, `n`, `N`, `gg`, `nG` — viewport unchanged; terminal cursor repositioned (~10 bytes) |
| Cursor move only | `scr_show_status()` | `h`, `l`, `0`, `^`, `$`, `f`, `F`, `;`, `,` — text unchanged, cursor stays on-screen |
| Terminal scroll + 1 row | `scr_update_after_move(old_top)` | `j`, `k`, `Enter`, `w`, `b`, `e`, `/`, `?`, `n`, `N`, `gg`, `nG` — viewport shifts by exactly ±1 visual row (~53 bytes vs ~1200 for full refresh) |
| Current logical line [+ 1 extra] | `scr_redraw_cur_line()` | `r`, `x`, `X`, `D`, `~`, `s`, `S`, `C`, insert-mode Ctrl-U, Ctrl-W (within line) |
| Cursor row to bottom | `scr_redraw_from_cur()` | `J`, `o`, `O`, `p`, `P`, `u`, `dw`/`dd`/`cw`/etc., insert-mode Enter/BS-over-newline/Ctrl-W-across-newline, dot-repeat of insert/join — content above cursor unchanged |
| Full screen | `scr_refresh()` | `G`, Ctrl-F/B/D/U, `:` commands, large file tail load, viewport shift after any operation |

The `scr_redraw_from_cur()` tier (added in v1.1) is the key level: for any operation that modifies content at or below the cursor without moving `top_pos`, it skips all rows above the cursor. On a 24-row terminal with the cursor at the middle, this halves the terminal output compared to a full refresh.

`scr_update_after_move()` no longer updates the status bar for the no-scroll and ±1-scroll cases (it calls `scr_update_cursor()` instead). This eliminates ~58 bytes of status bar I/O per `j`/`k`/`Enter` keypress (~10 ms at 56K baud). The status bar refreshes on the next edit, search, full-screen command, or `Ctrl-L`.

**O(N) row rendering:** All multi-row drawing loops (`scr_refresh()`, `scr_redraw_from_cur()`, `scr_redraw_cur_line()`) use `draw_row_at(row, pos)`, a static helper that draws a row at a known buffer position. The calling loop advances `pos` with one `next_vrow()` call per row. The public `scr_redraw_line(row)` wrapper (used for single-row redraws) still scans from `top_pos`, but multi-row callers no longer pay the O(N²) rescan cost (previously, row N required N `next_vrow()` walks from `top_pos`, so a 23-row refresh cost 0+1+…+22 = 253 extra walks). The threaded approach reduces this to 23 walks total.

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
2. **`nG` scans sequentially from byte 0.** Navigating to line 1000 reads roughly 1000 lines from disk; navigating to line 29000 in a 30K-line file reads most of the file. The scan uses sequential BDOS 20 reads, each requiring a BDOS call.
3. **Dot-repeat text truncated at 128 bytes.** Long insertions are truncated silently.
4. **No visual/block selection mode.**
5. **No macro recording or playback.**
6. **No window splitting.**
7. **No regex search** — plain substring match only.
8. **`~` (toggle case) is single-level undo per character.** With a count, only the last character's toggle is undoable.
9. **No `:s` (substitute) command.**
10. **Terminal size query may require Enter on some CP/M setups** if the BIOS CONIN is still line-buffered.
11. **Filenames limited to `PATH_MAX = 64` characters**, which exceeds CP/M's 8.3 limit but may be relevant on cross-platform use.
12. **Swap file not cleaned up on abnormal exit.** `HVISWP.TMP` and `HVITMP.TMP` are left on disk if HVI is terminated abnormally (e.g., power loss or warm boot). They can be deleted manually.
13. **Cross-boundary pattern miss.** If a search pattern spans the boundary between the loaded buffer and an unloaded file section (e.g., the first half of the pattern is the last bytes of the buffer and the second half is the first bytes of the tail), `scan_file_for_match` will not find it — it starts scanning from `tail_offset` and the partial match start is in the buffer. This edge case is rare in practice and is a known limitation of the sequential-scan approach.

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
| `scr_redraw_from_cur()` rendering tier | Skips rows above the cursor for operations that only change content at/below cursor; halves terminal output in the common case |
| `old_top` check before `scr_redraw_from_cur()` | Saved `top_pos` before `scr_scroll_to_cursor()` determines whether the viewport actually shifted; if not, the cheaper tier is used; if yes, full refresh |
| Search uses `scr_update_after_move()` | `/`, `?`, `n`, `N` move the cursor but don't change text; the terminal-scroll tier is sufficient for ±1-row jumps |
| `scr_update_after_move()` skips status bar for small moves | The `j`/`k`/`Enter` hot path calls `scr_update_cursor()` instead of `scr_show_status()` for no-scroll and ±1-scroll cases, saving ~58 bytes of terminal output (~10 ms at 56K baud) per keypress |
| `line_cnt_cached` avoids O(buffer) scan on every keypress | `scr_line_count()` scans the entire buffer to count newlines; caching the result and invalidating on `gb_insert()`/`gb_delete()` reduces this to O(1) on consecutive movement keystrokes |
| `draw_row_at(row, pos)` threads position through multi-row loops | Multi-row render loops (full refresh, cursor-to-bottom) previously re-walked from `top_pos` for each row — O(N²) total. Threading `pos` through reduces to O(N). Critical for page refresh responsiveness at 20 MHz Z80 |
| `nG` does not load the tail | With a count prefix, `G` jumps to a specific line number. Loading the full file tail (as bare `G` requires) is unnecessary; gating the tail-load loop by `!had_count` avoids the overhead and lets `scr_update_after_move()` use the cursor-only tier when the target line is already visible |
| Ctrl-F/B cursor in middle of new page | Standard vi places the cursor at the top of the new page; placing it in the middle gives a more balanced editing context and matches the feel of modern editors |
| Ctrl-F/B no-op at file boundaries | If already at the top (`top_pos == 0`) or the bottom (new top would not advance), no redraw is issued — avoids a visible flash/flicker for no-effect keypresses |
| `:e` resets gap buffer in-place | Rather than `gb_free()` + `gb_init()`, the buffer is emptied by resetting `gstart=0, gend=size`. This avoids `malloc`/`free` churn and the CP/M heap fragmentation risk |
| `ex_execute()` does not call `scr_refresh()` | All screen updates for ex commands are done by `cmdline_mode()` after `ex_execute()` returns, ensuring exactly one refresh per command regardless of which ex command ran |
| `mv_eol()` returns immediately on `\n` | When the cursor is already on a newline (empty line), `mv_eol()` previously walked forward past the `\n` onto the next line's content, causing `A` and `$` to land on the wrong line. An early return when `gb_char_at(cur_pos) == '\n'` corrects this |
| `G` uses `scr_refresh()` instead of `scr_update_after_move()` | After `gb_reload_from(0L)` resets `ed.top_pos` to 0, the saved `old_top` is also 0. `scr_update_after_move(0)` sees no viewport change and only calls `scr_update_cursor()`, leaving the screen blank. Using `scr_refresh()` unconditionally after `G` fixes this and is always correct since `G` is a large-distance jump |
| Static locals eliminate IX frames | HI-TECH C generates a 12–16 byte PUSH/POP IX stack frame for every function with at least one `auto` local. Declaring all locals `static` moves them to BSS and eliminates the frame. Safe because all HVI functions are non-recursive and CP/M is single-threaded. See Section 7.3 |
| `gb_load_fp()` merged into `gb_load()` | Both functions were nearly identical. Adding a `FILE *fp` parameter to `gb_load()` (NULL = fopen internally) eliminates the entire `gb_load_fp()` function body and its IX frame |
| `scr_redraw_cur_vrow()` removed | Used only by `r` (replace). `scr_redraw_cur_line()` is a correct superset: it redraws the full logical line, which is identical for single-width lines and also handles wrapped lines. Removes ~60 bytes |
| `scr_line_end()` removed as dead code | Declared in `hvi.h`, defined in `screen.c`, but never called from anywhere in the codebase. Removing dead declarations and definitions reduces binary size without any functional change |
| `cpmio.c` replaces all stdio and malloc | HI-TECH C's `fopen()` allocates a heap buffer per handle; after `gb_init()` consumes the TPA there is no heap left. Direct BDOS calls with a static 3-slot HFILE pool eliminate the problem entirely and reduce binary size by avoiding buffered-I/O library code |
| `hvi_fseek` uses long shifts for sector number | The sector number for BDOS 33 is `offset >> 7`. Casting `offset` to `unsigned int` before shifting truncates files > 32 KB to the wrong sector. All shifts are performed on the raw `long` value; only then are the three FCB bytes extracted |
| `hvi_fsize` uses exponential+binary search | BDOS 35 (Compute File Size) is not implemented by all CP/M emulators. Doubling the probe offset until BDOS 33 fails, then binary-searching the final interval, finds the sector-rounded file size in O(log N) seeks (~21 for 1 MB) without relying on BDOS 35 |
| `gb_reload_from` flushes edits before discarding buffer | Without the flush, navigating to a new window in a large file silently discards all in-memory edits. `gb_reload_from` calls `gb_save("HVISWP.TMP")` when `ed.modified` is set, making `tail_file` point to a complete, up-to-date snapshot before the buffer is cleared |
| `s_reload_skip_flush` flag prevents double save | `gb_make_room()` saves to the swap file itself before calling `gb_reload_from()`. Setting `s_reload_skip_flush = 1` before the call prevents `gb_reload_from` from saving a second time (which would overwrite the complete swap with only the partial in-memory window) |
| `gb_goto_line` for `nG` in large files | The previous `nG` implementation reloaded from byte 0 and clamped to `scr_line_count()`, which is the buffer-local line count. For a large file mid-scroll this always landed at the end of the loaded buffer. `gb_goto_line(n)` scans `tail_file` from byte 0 to find the exact byte offset of line N, positions the window there, and uses a second short scan to find the local buffer line |
| `gb_count_lines` uses `hvi_fseek` + sequential read | After a BDOS 33 random seek, BDOS 20 continues sequentially from the next record. This allows `gb_count_lines` to seek to `win_off` with one BDOS 33 call and then read to `target_off` with BDOS 20 calls, avoiding a full scan from byte 0 for the local line number |
| Skip BDOS 16 for read-mode HFILE handles | Some CP/M emulators write the FCB's `CR`/`EX` fields back to the directory on BDOS 16, corrupting the extent record. The next BDOS 15 (open) then loads a wrong allocation map, causing BDOS 33 to return sector-0 data for all subsequent random reads. Skipping BDOS 16 for `mode == 1` handles eliminates the corruption entirely |
| Explicit FCB CR field write before BDOS 33 | CP/M 2.2 specifies `r0`/`r1`/`r2` (FCB bytes 33–35) as the random record number for BDOS 33. Some emulators also key off byte 32 (`CR`). Writing the sector number into byte 32 alongside the standard `r0`/`r1`/`r2` fields ensures correct operation on both conforming and non-conforming implementations |
| `do_search_full` for whole-file search | `do_search_from` only searches the in-memory buffer. To find matches in unloaded sections, `do_search_full` adds a Phase 2 that calls `scan_file_for_match` on the unloaded tail (FWD) or head (BWD), then reloads the window if a file match is found. This preserves the fast in-buffer path for the common case while enabling whole-file search |
| `dsf_file_wrapped` flag for correct wrap reporting | A match found in the file's tail section (FWD search) is genuinely ahead of the cursor in file order and is not a wrap, but a match in the head section is. Tracking which `scan_file_for_match` call succeeded allows `do_search_full` to set `ed.search_wrapped` correctly — tail match → no wrap message; head match → "search hit BOTTOM" message |
| `scan_file_for_match` uses sequential BDOS reads | Reading the file section byte-by-byte via `hvi_fgetc` after a single `hvi_fseek` is simple and correct. Random access (BDOS 33) has emulator compatibility issues (CR field); sequential BDOS 20 reads after an initial seek are more reliable and avoid repeated sector-address calculations |
| `do_search_from` uses `pos <= sp` for wrap detection | The last iteration of the scan loop (i == size) sets `pos == sp` (the start position). The old `pos < sp` check missed this case, leaving `search_wrapped = 0` when the cursor sat exactly on the only match. `do_search_full` then saw a "found without wrap" result and returned immediately instead of scanning the unloaded file sections. The `<=` change ensures Phase 2 is triggered whenever the result required a full buffer traversal |
| Ctrl-F load trigger uses `top_line + n + text_rows > total` | The old condition (`top_line + n >= total - 1`) triggered a load only when the new top row hit the last buffer line, but the new viewport still needed `text_rows` more lines below it, causing a screen of `~`. The new condition fires whenever the bottom of the new viewport would fall past the buffer end |
| Ctrl-F recomputes `top_line` after `gb_load_more` | `gb_load_more` calls `gb_discard_head` which adjusts `ed.top_pos`. Using the pre-load `top_line` for `scr_line_start(new_top)` after a discard produced an offset into stale line numbering. Recomputing `top_line = scr_pos_line(ed.top_pos)` after the load uses the correct adjusted value |
| No line count in status bar | Computing an accurate total requires scanning the entire file (O(N) BDOS reads), which is too slow on a real CP/M machine. A buffer-local count is misleading when the window is mid-file. The status bar shows only the filename and modified flag |
| Bitwise arithmetic for `TAB_STOP` | Tab-stop alignments historically use compiler-injected modulo/division code. We switched this block `((col / TAB_STOP + 1) * TAB_STOP)` to a hardware bitwise OR block `((col \| (TAB_STOP - 1)) + 1)` which compiles drastically smaller since `TAB_STOP` is 8 (a power of 2). |
| `begin_hmove`/`end_hmove` abstraction | Horizontal motion commands abstractly bracket updates to `ed.cur_pos`. The `end_hmove` block natively catches if a visual boundary crossing occurs on the physical screen (line wrap text) and correctly invalidates `ed.cur_vrow` to snap the UI rendering context back without redundant checks scattered. |
| Extract newline scans to `find_bol`/`find_eol` | 16+ inline while-loops repeating newline scanning logic across the code base were refactored into `gap.c` exports to cut binary block duplication via central CALL jumps. |
| Encapsulate `mv_find` inline command | The `f/F/;/;` character-find routines were moved from `edit.c` to `emove.c` (`mv_find()`) so they could operate under the secure caching brackets of `begin_hmove`, stopping UI freezing. We removed the "f_" status prompt to replicate pure vi mechanics. |

---

## 16. License

MIT License. Copyright (c) Juan Orlandini.
