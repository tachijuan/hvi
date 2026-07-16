# HVI — Requirements and Implementation Reference

**Author:** Juan Orlandini  
**License:** MIT  
**Version:** 2.8.0  
**Date:** 2026-07-14

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
- No `memmove` in the standard library — provided in assembly (`gb_memmove`, cstart.as)
- The library's `fopen`/`malloc`/`getch`/`sprintf` exist but HVI links **none** of
  them: file I/O, allocation, console I/O, and formatting are all replaced by
  `cpmio.c`, `util.c`, `term.c`, and `cstart.as` (see §7.2).  The only libc
  symbols that survive the link are the arithmetic helpers (`imul`, the 32-bit
  shift/add/compare routines) — `imul` is always present, so `* 10` costs
  nothing extra over shift-adds
- All BDOS traffic goes through `bdos_disk` / `dsk_u` in cstart.as, which
  preserve IX across `CALL 5` (disk BDOS functions corrupt IX on some hosts)
- The compiler driver is named `C` (not `cc`); the linker is named `LINQ`
  (renamed to `L` in the build environment)
- CP/M filenames are 8.3 uppercase; HI-TECH C upcases filenames automatically

### 2.3 Terminal
- The terminal **family is selected at compile time** via `-DTERM_xxx` (see `termcfg.h` and §12); no flag builds the ANSI/VT100 default. Each build emits only the codes its terminal understands.
- **ANSI/VT100 build** (`HVI.COM`): full escape set; queries terminal size at startup via ANSI CPR (`ESC[6n`); default 80 × 24 if the terminal does not respond. Compatible with VT100, VT220, xterm, ANSI.SYS, and most emulators.
- **Non-ANSI builds** (VT52, H19, ADM-3A, Kaypro 83/84, Televideo 9xx, Wyse 50, Hazeltine 1500, Osborne 1): **fixed** geometry, default 80 × 24 (Osborne 52 × 24), overridable with `-DTERM_ROWS=` / `-DTERM_COLS=`. No size query is sent. Capabilities a terminal lacks (clear-to-EOL, hardware scroll, insert/delete-char, reverse video) are emulated in software so editing stays correct.

---

## 3. Source Files

| File | Purpose |
|------|---------|
| `cstart.as` | Custom Z80 startup (stack, BSS zeroing, `heap_base`/`bdos_base`; replaces CRTCPM.OBJ) **plus the assembly core**: fixed `csv`/`cret`/`ncsv` frame routines, IX-safe `bdos_disk`, `dsk_u` (BDOS call inside a user area), `fill_fcb` (FCB build + `du:` prefix parse), `gb_memmove` (LDIR/LDDR), `gb_char_at`, `gb_memchr`/`gb_memrchr` (CPIR/CPDR byte scans), `gb_cntnl` (CPIR newline count), `con_write` (block console output via BDOS 6) |
| `hvi.c` | Main entry point, argument parsing, startup sequence |
| `hvi.h` | Shared types, constants, and extern declarations (includes `termcfg.h`) |
| `termcfg.h` | Compile-time terminal family selection: `-DTERM_xxx` picks an addressing style, capability macros, and fixed geometry; no flag = ANSI. Consumed by `term.c` and the capability fallbacks in `screen.c`/`edit.c` |
| `gap.c` | Gap buffer: allocation, load, save, insert, delete, `find_bol`/`find_eol`, marks, large-file paging |
| `cpmio.c` | Direct BDOS file I/O, user-area plumbing, and heap allocator (replaces stdio and malloc) |
| `util.c` | String utilities and `hvi_sprintf` (replaces `<string.h>` and `<stdio.h>` sprintf) |
| `term.c` | Terminal I/O: buffered ANSI output, BDOS-6 raw input, arrow keys, size query |
| `screen.c` | Viewport rendering: line drawing, cursor placement, status bar |
| `emove.c` | Cursor movement, operator application (`apply_op`), search |
| `edit.c` | VI normal mode, insert mode, ex command-line mode |
| `erepeat.c` | Dot-repeat: `ins_position`, `dot_replay_c`, `dot_replay`, `cur_back_nl` |
| `ex.c` | Ex command execution (`:q`, `:w`, `:r`, `:N`, etc.) |

The `tests/` directory holds an end-to-end harness (not part of the CP/M
build): `build.py` drives the five compiler passes inside a RunCPM emulator,
`hvitest.py` (139 tests) and `bigtest.py` (13 tests) run HVI through a pty,
scrape the screen with a VT100 emulator, and verify saved files — including
the drive/user-area prefixes via RunCPM's user-area subdirectories.

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

Every filename — on the command line and in `:w`, `:e`, `:r` — accepts an
optional ZCPR-style `du:` drive/user prefix: `B:FILE.TXT` (drive B, current
user), `3:FILE.TXT` (current drive, user area 3), `B3:FILE.TXT` (both).
Drives A–P and user areas 0–15 are accepted; anything else (user > 15,
three or more digits, or no terminating `:`) is treated as part of the
filename.  Unprefixed names use the drive and user area HVI was started
from.  See §6.11 for the implementation.

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
- `gb_char_at(pos)` — character at logical position (handles the gap transparently).  **Assembly** in cstart.as: it is the hottest function in the editor (every per-character scanner calls it), frameless, reading the GapBuf fields at fixed offsets from `ed` — GapBuf must stay the first member of `Editor`
- `find_bol(pos)` / `find_eol(pos)` — line boundaries around pos, via backward/forward CPIR scans (`gb_memrchr`/`gb_memchr`, 21 T-states/byte) over the raw gap segments; these back every line motion
- `gb_count_nl(pos, len)` / `gb_copy_out(dst, pos, len)` — newline count and bulk copy over a logical range; both split the range at the gap with a shared clamp helper (`gb_split`) and run the CPIR counter / LDIR mover per raw segment
- `gb_insert(pos, text, len)` — insert text at logical position (also maintains the line/row caches and marks)
- `gb_delete(pos, len)` — delete len bytes at logical position (expands gap)
- `gb_load(filename, fp)` — opens and loads a file; when `fp` is non-NULL, uses that already-open handle
- `gb_save(filename)` — saves buffer to file (head + window + tail for large files)
- `gb_load_more(n)` / `gb_reload_from(off)` / `gb_load_last()` / `gb_load_prev()` — large-file paging (§6.4)
- `gb_goto_line(n)` / `gb_make_room()` — large-file line navigation and buffer-overflow recovery

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

**Block inserts larger than the gap (v2.6.1):** a freshly loaded window
keeps only `GAP_MIN` bytes of gap, and `gb_make_room()`'s reload restores
no more than that — so a put larger than the gap cannot be satisfied by a
single retry. `gb_insert_room()` (assembly, cstart.as; state declared in
gap.c) inserts in gap-sized chunks, calling `gb_make_room()` between
chunks. Its arguments live in globals (`gir_pos`/`gir_text`/`gir_len`):
`gir_pos` doubles as input and output so consecutive inserts chain, and a
failure (-1, disk full) passes through the chain so the caller checks once
after the last call. It sets `gb_roomed` when a swap happened — the reload
invalidated marks and the undo record, so the caller skips the undo save
and repaints the screen from scratch. All block-insert paths go through
it: `put_yank()` (`p`/`P`), `nl_room()` (the `o`/`O` newline, reachable
with a zero-byte gap after insert-mode typing exactly fills the window),
and `dot_ins()` (the `.` replay of insert/change text, up to 128 bytes).
Before v2.6.1 these called `gb_insert()` unchecked and an insert larger
than the free gap was silently dropped (put also set the modified flag
and a bogus undo record).

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

**Save:** `gb_save(filename)` reconstructs the full file: `gb_copy_file()`
copies the head (bytes 0..`win_start`−1 from `tail_file`), `gb_write_buf()`
writes the in-memory buffer (LF→CR+LF, walking the two raw gap segments
with a pointer), and `gb_copy_file()` copies the unloaded tail (bytes
`tail_offset`..end).  When saving to the same file that holds the tail,
`HVITMP.TMP` — built with the destination's `du:` prefix — is used as an
intermediate to prevent reading and writing the same file simultaneously.

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
- Status line shows: filename, `[+]` if modified, or a transient message (search result, error, mode indicator).  No line numbers (see §11.3).
- Three incremental caches in `Editor` keep per-keystroke work O(1):
  - `line_cnt_cached` — total line count; **adjusted** (not invalidated) by `gb_insert()`/`gb_delete()` using their newline tallies; 0 = recompute.
  - `cur_line`/`cur_line_pos` — line number of the cursor, updated by `scr_cur_line()` with a CPIR count over just the moved-over span.
  - `cur_vrow` — the cursor's visual row within the viewport, maintained by `mv_up`/`mv_down` and invalidated (−1) whenever an edit or jump may have moved the cursor across a visual-row boundary.  Staleness here is the classic screen-corruption trap: anything that moves the cursor across a vrow boundary must set `cur_vrow = -1`.

### 6.10 Marks

`ed.marks[NMARKS]` holds 27 buffer positions: slot 0 (`MARK_PREV`) for
`` ``` `` — the position before the last jump — and slots 1–26 for
`` `a``–`` `z`` (set with `m{a-z}`).  The layout mirrors ASCII
(`slot = mark char − 0x60`, `` ` `` = 0x60), so `motion_endpoint('`')`
resolves any mark char with one subtraction and one unsigned range test.
−1 means "not set".  Every `gb_insert`/`gb_delete`
runs `mk_adjust()` so marks keep pointing at the same characters; a mark
inside a deleted range is cleared.  All marks are cleared when the
large-file window slides or a new file is loaded (buffer positions no
longer describe the same text).  `G`, `gg`, `:N`, `:$`, searches, and
`` `x`` jumps record `MARK_PREV` first, so `` ``` `` returns to the origin.

Marks are also operator motions (v2.6): `` d`x ``, `` c`x `` and `` y`x ``
apply the operator to the exclusive character range between the cursor and
the mark (`apply_op` orders the endpoints), and `` d`` `` uses
`MARK_PREV`.  Resolution and validation live in the `motion_endpoint()`
'`' case, shared by the operators, the `.` replay (the mark char is
recorded in `dot_arg` and re-resolved at repeat time via `me_mkc`), and
the standalone `` `x`` jump in edit.c.

### 6.11 Drive and User-Area File Access (v2.5)

Every file operation accepts the `du:` prefix described in §5.  The
implementation is concentrated in cstart.as and `cpmio.c`:

- **Parsing** happens inside `fill_fcb` (assembly, cstart.as) while the FCB
  is built: the drive code (0 = current, 1 = A …) lands in FCB byte 0 and
  is also left in the C global `fcb_drive`; the user area (−1 = current,
  0–15) lands in `fcb_user`.  An invalid prefix leaves the whole string as
  the filename.
- **Drive** needs nothing further — CP/M's FCB carries it.
- **User area** has no FCB field.  Each `HFILE` records its file's user
  area in a `user` member (−1 for unprefixed names), and every directory or
  data BDOS call on that file (open 15, close 16, erase 19, write 21,
  make 22, rename 23, read-random 33) goes through `dsk_u(fn, arg, user)`
  in cstart.as, which brackets the operation with BDOS 32 set-user calls
  and restores the process user afterwards.  The process user number is
  fetched lazily with one BDOS 32 get on the first prefixed operation
  (`du_cur`, a data-psect byte initialised to 80h = unknown, because BSS
  zeroing would read as "user 0").
- **Zero overhead when unused:** `user < 0` makes `dsk_u` call straight
  through — an unprefixed session never issues a single BDOS 32.
- **Rename constraint:** BDOS 23 renames within one drive and user area
  only.  `gb_save` therefore builds its `HVITMP.TMP` staging name with the
  *destination's* `du:` prefix (`gsv_mk_tmp` in gap.c), so the final
  erase + rename happens entirely inside the target drive/user area.
- `hvi_fopen` re-asserts `fcb_drive` into FCB byte 0 after open — some
  BDOS implementations overwrite the drive byte.

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
  allocation needed per open) and the file's user area (§6.11)
- `hvi_fopen` / `hvi_fclose` — BDOS 15 (Open), 16 (Close), 22 (Make)
- `hvi_fgetc` — refills its sector buffer with BDOS 33 (Read Random), never
  BDOS 20: the sector number is always derived from the self-tracked `pos`
  field (`pos >> 7`), eliminating all dependence on the FCB's CR/EX/S2
  state, which BDOS 33 does not reliably update across implementations
- `hvi_fputc` — BDOS 21 (Write Sequential)
- `hvi_fseek` — BDOS 33 (Read Random) for direct sector seek
- `hvi_ftell` — returns the cached `pos` field
- `hvi_remove` / `hvi_rename` — BDOS 19 (Erase), 23 (Rename)
- `hvi_fsize` — exponential+binary search via BDOS 33
- `hvi_malloc` / `hvi_free` — single-shot allocator from the free TPA between
  BSS end and the BDOS base (set by `cstart.as`)
- `fill_fcb` — FCB builder + `du:` prefix parser, in assembly (cstart.as)

The FCB random-record fields (`CR`, `r0`–`r2`) are written from the sector
number's **little-endian bytes** (`((unsigned char *)&sect)[n]`) rather than
32-bit shift/mask expressions — each `long` shift/mask compiles to a library
call (~50 bytes) and this sits in the per-sector hot path.

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

### 7.3 Static Locals, IX Frames, and the csv History

HI-TECH C V3.09 generates an IX-register stack frame in every function that
has at least one `auto` (stack-allocated) local variable.  The
prologue/epilogue costs 12–16 bytes per function, so most HVI functions
declare their locals `static` (BSS, fixed addresses, no frame).  This is
safe because all HVI functions are non-recursive and CP/M is
single-process/single-thread; inner-block declarations are hoisted to
function scope for the same reason.

**Historical note (the "-O corrupts autos" myth, fixed in v2.1):** many
static-local conversions were originally made because auto variables
appeared to be corrupted across function calls under `-O`.  The real cause
was a bug in the custom `csv` in cstart.as: it left SP two bytes above IX
at function-body entry, so the deepest auto local sat below SP and was
clobbered by the next argument push.  The v2.1 `csv`/`ncsv`/`cret` match
the real HI-TECH frame (saves IX **and IY**, arguments at IX+6, SP == IX
at body entry), and **auto locals are fully reliable now**.  Statics are
retained purely as the size optimisation above; new code should not add
static-local "workarounds" for correctness reasons.  Statics genuinely
needed for state that must survive BDOS calls are those in cpmio.c where
IX itself could be corrupted — but `bdos_disk`/`dsk_u` also protect IX,
so this is defence in depth.

### 7.3a Assembly Core (cstart.as)

Beyond startup and the frame routines, cstart.as holds hand-written Z80 for
the hot paths and for code where compiled K&R C is ~2.5× the size:

| Routine | Purpose |
|---------|---------|
| `bdos_disk(fn, arg)` | BDOS call that saves/restores IX around `CALL 5` (disk functions corrupt IX on some hosts) |
| `dsk_u(fn, arg, user)` | `bdos_disk` bracketed by BDOS 32 user-area switches (§6.11); `user < 0` calls straight through |
| `fill_fcb(fcb, name)` | 36-byte FCB build + `du:` prefix parse; results in `fcb_user`/`fcb_drive` |
| `gb_memmove(dst, src, len)` | Overlap-safe LDIR/LDDR block move — every gap move |
| `gb_char_at(pos)` | Frameless logical byte fetch through the gap; reads GapBuf fields at `_ed+0/2/4/6` |
| `gb_memchr` / `gb_memrchr` | CPIR/CPDR first/last index-of over a raw range (21 T-states/byte) — back `find_eol`/`find_bol` |
| `gb_cntnl(p, len)` | CPIR newline counter — backs `gb_count_nl`, line caches, yank/undo newline checks |
| `con_write(buf, len)` | Block console output via BDOS 6 — one call per flushed output buffer |

All are frameless or save IX themselves, and none touch IY.  `gb_char_at`
depends on `GapBuf` being the **first member** of `Editor` (offsets are
`EQU`s at the top of cstart.as).

### 7.4 HI-TECH C Compiler Limits

#### 7.4.1 Optimizer Memory Limit: "optim: Out of memory"
**Problem:** HI-TECH C's optimizer has a fixed memory budget for processing a single function. Functions beyond a certain complexity cause the compiler to abort with `optim: Out of memory`.

**Solution:** Keep all functions small. When a function grows too large (typically beyond ~80–120 lines with many local variables and branches), split it into multiple smaller static helper functions.

**Affected functions in this codebase:**
- `gap.c::gb_save()` — split into `gb_copy_file()` (head/tail) and `gb_write_buf()`/`gwb_seg()`
- `gap.c::gb_load()` / `gb_reload_from()` — share the split-out read loop `gb_fill()`
- `gap.c::gb_load_more()` — split out `gb_discard_head()`
- `edit.c::normal_cmd()` — split into `normal_edit_cmd()`, `normal_delchg_cmd()`, `normal_misc_cmd()`, `normal_find_cmd()`, `normal_page_cmd()`

#### 7.4.2 Per-File Label Limit: "Too many temporary labels"
**Problem:** HI-TECH C generates an assembly label for every `if`, `while`, `for`, `&&`, `||`, `switch` case, and ternary expression. There is a hard per-file limit on the total number of these temporary labels. When a `.C` file generates too many labels across all its functions combined, the assembler phase aborts with `Too many temporary labels`.

This is distinct from the optimizer memory limit: splitting a large function into smaller functions within the **same** `.C` file does not help, because the label count is per-file, not per-function.

**Solution:** Move code to a **new source file**. Choose a logically cohesive cluster of functions and extract them into a separate `.C` file that is compiled and linked independently.

**Affected in this codebase:**
- `erepeat.c` was created to hold `ins_position()`, `dot_replay_c()`, and `dot_replay()`, which were extracted from `edit.c` to reduce `edit.c`'s total label count.

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
HI-TECH C V3.09 does not include `memmove()` in its library. The gap buffer requires an overlapping-safe memory copy for gap movement. `gb_memmove()` is provided in assembly (cstart.as) using LDIR/LDDR — 21 T-states/byte versus well over 100 for a compiled loop; this runs on every gap move.

### 7.7 Integer Width
On Z80, `int` is 2 bytes (range −32768 to 32767). Buffer positions and sizes are stored as `int`, which limits the effective buffer to 32 KB. This is acceptable given CP/M TPA constraints. File offsets (e.g., `tail_offset`, `win_start`) are stored as `long` (4 bytes) to support files larger than 32 KB.

### 7.8 Terminal Input
CP/M does not have a `termios`-style raw mode API.  `term_getch()` reads
raw keys by spinning on **BDOS function 6** (Direct Console I/O,
E = FFh: no echo, non-blocking, returns 00h when no character is ready),
through `bdos_disk` so IX is preserved.  No BIOS calls are used anywhere.

Function 6 alone is the poll — deliberately (v2.7.1).  Earlier versions
pre-polled **BDOS 11** (Console Status) and consumed its result
unmasked.  Only register A carries the DRI-guaranteed byte result of a
BDOS call; H mirrors B, which real BDOS implementations leave as
internal junk (RunCPM returns a clean HL, which is why the emulator
never showed the bug).  On real CP/M 2.2 the junk high byte made idle
look "ready", function 6 then correctly returned 00h, and the editor
received an endless stream of NUL keystrokes (reported on a North Star
Horizon under both North Star and Lifeboat CP/M 2.2 — insert mode
filled with characters by itself, and the byte after an operator
arrived as a bogus "Unknown motion").  Two hardenings (v2.7.1):
`bdos_disk` (cstart.as) now returns A zero-extended, cleaning every
call site at once — including the `dsk_u` file-operation results — and
the status pre-poll is gone entirely, which also sidesteps BIOSes
whose CONST strays from the specified 00h/FFh.  The inherent
trade-off: a real `^@` keystroke is indistinguishable from idle and is
ignored (vi binds nothing to NUL).

A bare ESC is disambiguated from an ANSI arrow sequence by a bounded
countdown poll (`con_wait`): if `[` follows quickly, `ESC[A/B/C/D` are
translated to synthetic key codes `KEY_UP`/`KEY_DOWN`/`KEY_LEFT`/`KEY_RIGHT`
(> 0xFF, never produced by raw input); any other quick byte is **pushed
back** as the next keystroke rather than swallowed.

### 7.9 Terminal Size Query
`term_getsize()` parks the cursor at `ESC[999;999H` (the terminal clamps to
its real size) and requests a cursor-position report with `ESC[6n`, sent as
one `con_write` block.  The `ESC[rows;colsR` reply is parsed with the same
`con_wait` countdown polling (BDOS 6); every byte is guarded by a
timeout so a non-responding terminal falls back to `DEF_ROWS = 24`,
`DEF_COLS = 80`.  Any pty harness driving HVI must answer the `ESC[6n`
query (the test harness replies `ESC[24;80R`).

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
- On load: a `CR` (0x0D) is stripped only when it is the `CR` of a `CR+LF` pair (normalized to `LF`); a bare `CR` mid-line is kept as content and round-trips on save.
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
| `PATH_MAX` | 64 | Maximum filename length (including any `du:` prefix) |
| `STATUS_MAX` | 128 | Maximum status message length |
| `CMD_MAX` | 128 | Maximum ex command-line length |
| `NMARKS` | 27 | Mark slots: `a`–`z` plus `MARK_PREV` |
| `MARK_PREV` | 0 | Slot holding the position before the last jump (`` ``` ``); a–z occupy slots 1–26 so `slot = char − 0x60` |
| `HEOF` | −1 | End-of-file sentinel returned by `hvi_fgetc()` (replaces `stdio` EOF) |
| `MAX_HFILES` | 3 | Number of HFILE slots in the static pool in `cpmio.c` |
| `OUT_BUF_SZ` | 256 | Terminal output buffer (term.c), flushed via `con_write` |
| `SPRINTF_SMAX` | 100 | Per-`%s` expansion cap in `hvi_sprintf` (keeps any status message inside `STATUS_MAX`) |

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
| `↑` `↓` `←` `→` | ANSI arrow keys, translated to `k` `j` `h` `l` |

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
| `` >> `` / `` << `` | Shift line(s) one tab stop right / left (count = lines; also `>{motion}`, `<{motion}` incl. `` >`a ``) |
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

### 10.6a Normal Mode — Marks

| Key | Action |
|-----|--------|
| `m{a-z}` | Set mark at the cursor position |
| `` `{a-z} `` | Jump to mark (exact position); "Mark not set" if unset or invalidated |
| `` `` `` | Jump to the position before the last jump (`G`, `gg`, `:N`, `:$`, search, or a mark jump) |
| `` d`{a-z} `` / `` c`{a-z} `` / `` y`{a-z} `` | Operate on the exclusive range between cursor and mark (either order); `` d`` `` etc. use the previous-jump position; "Mark not set" aborts the operator |

Marks track edits (§6.10) and are cleared when the large-file window slides
or a new file is loaded.

### 10.7 Normal Mode — Miscellaneous

| Key | Action |
|-----|--------|
| `.` | Repeat last change |
| `u` | Undo last change (single level) |
| `:` | Enter ex command mode |
| `Ctrl-L` | Redraw screen (re-establishes the scroll region) |

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
| `` d`x `` / `` c`x<text>ESC `` | Re-resolve mark `x` at its current position and re-apply (mark char kept in `dot_arg`) |
| `>>` / `<<` / `>{motion}` | Re-apply the same shift |
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
| `↑` `↓` `←` `→` | Move the cursor without leaving insert mode |
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
| `:[range]s/old/new/[g]` | Substitute (v2.7): plain-text, case-sensitive |
| `:[range]>` / `:[range]<` | Shift the range's lines one tab stop right / left (v2.7) |

All `filename` arguments accept the `du:` drive/user prefix (§5, §6.11).

### 10.11a Substitute (`:s`, v2.7)

`:[range]s/old/new/[g]` replaces `old` with `new` -- a plain-text,
**case-sensitive** match, no regular expressions.  Without `g` only the
first occurrence in each line is replaced; with `g`, every occurrence.
The trailing `/` may be omitted; an empty `new` deletes `old`; `/`
cannot appear in either text (no escape syntax).

The range is one address or `addr1,addr2` (order-independent); the
default is the cursor's line.  Addresses: a line number (clamped to the
buffer), `.` (the cursor's line), `$` (the last line), or `'{a-z}` (the
line containing the mark, resolved by the same `motion_endpoint('`')`
code the operators use -- an unset mark reports "Mark not set").

Implementation (`ex_subst`, ex.c): the parser runs before the other ex
commands (a range can start with digits) and returns "not mine" so `:N`
etc. fall through; the `:N` handler shares the same address parser.
`old` cannot contain a newline (`read_line` stores printable characters
only), so a match can never cross a line boundary -- the scan is one
flat pass over `[addr1's line start, addr2's line end)` with no
per-line loop: `gb_find_ch` CPIRs to each candidate first character,
the candidate is staged with `gb_copy_out` (two LDIRs) and compared
with one `hvi_strcmp` (no per-character `gb_char_at` calls).  The end
position is adjusted by `new-old` length per replacement; after a
replacement the scan continues past the inserted text (never rescanning
it), and without `g` it skips to the next line.  On a zero-match run
the status shows "Pattern not found"; if the gap fills mid-run
(`new` longer than `old`), completed substitutions are kept and
"Buffer full" is shown.  The cursor moves to the start of the last
substituted line (vi).  A substitute is **not undoable**: the edits are
scattered, so the single-slot undo record is invalidated.  Marks track
the edits as usual (`mk_adjust`); a mark inside a replaced `old` is
cleared.

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
| Single cell / span | `scr_fix_char()` / `scr_fix_span()` | `r` and `~` on printable, non-wrapping cells — the new character(s) are emitted in place (2–4 bytes) |
| Cursor move only | `scr_update_cursor()` | `j`, `k`, `Enter`, `w`, `b`, `e`, `/`, `?`, `n`, `N`, `gg`, `nG`, `` `x `` — viewport unchanged; terminal cursor repositioned (~10 bytes) |
| Cursor move only | `scr_show_status()` | `h`, `l`, `0`, `^`, `$`, `f`, `F`, `;`, `,` — text unchanged, cursor stays on-screen |
| Terminal scroll + N rows | `scr_update_after_move(old_top)` | Any move where the viewport shifts by fewer than a screenful of visual rows — the scroll region scrolls and only the rows that came into view are painted (Ctrl-D/U half-pages use this too) |
| One screen row | `scr_edit_end(1)` | Charwise edits confined to a single-row line with no newline change (`x`, small `p`, in-line `d`/`c`, undo of same) — repaints exactly one row plus the status bar |
| Current logical line [+ 1 extra] | `scr_redraw_cur_line()` | `r`, `~` fallbacks, insert-mode Ctrl-U, Ctrl-W (within line), tab edits, and mid-line typing/backspace when a tab lies between the cursor and end of line (`tail_has_tab()`, v2.7.2 — the one-column `ESC[@`/`ESC[P` shift would misplace the tab-aligned tail) |
| Cursor row to bottom | `scr_redraw_from_cur()` | `J`, `o`, `O`, `p`, `P`, `u`, `dw`/`dd`/`cw`/etc. (when not single-row), insert-mode Enter/BS-over-newline/Ctrl-W-across-newline, dot-repeat of insert/join — content above cursor unchanged |
| Full screen | `scr_refresh()` | `G`, Ctrl-F/B, `:` commands, large-file window reloads, viewport shift after any operation |

The `scr_redraw_from_cur()` tier (added in v1.1) is the key level: for any operation that modifies content at or below the cursor without moving `top_pos`, it skips all rows above the cursor. On a 24-row terminal with the cursor at the middle, this halves the terminal output compared to a full refresh.

`scr_update_after_move()` no longer updates the status bar for the no-scroll and ±1-scroll cases (it calls `scr_update_cursor()` instead). This eliminates ~58 bytes of status bar I/O per `j`/`k`/`Enter` keypress (~10 ms at 56K baud). The status bar refreshes on the next edit, search, full-screen command, or `Ctrl-L`.

**O(N) row rendering:** All multi-row drawing loops (`scr_refresh()`, `scr_redraw_from_cur()`, `scr_redraw_cur_line()`) use `draw_row_at(row, pos)`, a static helper that draws a row at a known buffer position. The calling loop advances `pos` with one `next_vrow()` call per row. The public `scr_redraw_line(row)` wrapper (used for single-row redraws) still scans from `top_pos`, but multi-row callers no longer pay the O(N²) rescan cost (previously, row N required N `next_vrow()` walks from `top_pos`, so a 23-row refresh cost 0+1+…+22 = 253 extra walks). The threaded approach reduces this to 23 walks total.

---

## 12. Terminal Interface (`term.c`)

All output accumulates in a 256-byte buffer (`OUT_BUF_SZ`) and is flushed
as one `con_write` block (BDOS 6 loop in cstart.as) just before blocking on
input — many BDOS calls become one, which matters when the link is
baud-rate limited.  Two further output optimisations:

- **Cursor tracking** — `s_trow`/`s_tcol` shadow the terminal cursor so
  `term_goto()` can emit `\r` (1 byte), backspaces, the family's short
  cursor-right code, or `\r`+`\n`s instead of a full re-address when the
  move is small.  Any untracked control byte invalidates the shadow, as
  does writing the last column on the non-ANSI families (`TERM_WRAP_IMMEDIATE`,
  which wrap the instant the last cell is written).
- **Scroll region** — on the ANSI build, set once at startup to the text
  area (`ESC[1;(rows-1)r`) so `term_scroll_up()`/`term_scroll_dn()` scroll
  by one visual row in 2–3 bytes; re-established by Ctrl-L after its
  `ESC[2J`.

**Terminal families** are selected at compile time (`termcfg.h`).  The public
`term_*` contract is identical across families; only the emitted bytes and a
handful of capability `#ifdef`s in `screen.c`/`edit.c` differ.  Per-family
sequences (all non-ANSI codes taken from the terminals' manuals and marked
`VERIFY` in `term.c` for confirmation against specific models):

| Operation | ANSI | VT52 / H19 | ADM-3A | Televideo / Wyse | Hazeltine 1500 | Osborne 1 |
|-----------|------|-----------|--------|------------------|----------------|-----------|
| Address cursor | `ESC[r;cH` (decimal) | `ESC Y r+32 c+32` | `ESC = r+32 c+32` | `ESC = r+32 c+32` | `~ DC1 c r` (binary, col first) | `ESC = r+32 c+32` |
| Clear + home | `ESC[2J ESC[H` | `ESC H ESC J` | `^Z` | `^Z` | `~ FS` | `^Z` |
| Clear to EOL | `ESC[K` | `ESC K` | *space-pad* | `ESC T` | `~ SI` | `ESC T` |
| Cursor right (short) | `ESC[C` | `ESC C` | `^L` | `^L` | *(none)* | `^L` |
| 1-row scroll | scroll region (`\n` / `ESC M`) | H19: `ESC L`/`ESC M`; VT52: *repaint* | *repaint* | `ESC E`/`ESC R` | `~ SUB`/`~ DC3` | *repaint* |
| Insert / delete char | `ESC[@` / `ESC[P` | *repaint* | *repaint* | `ESC Q` / `ESC W` | *repaint* | *repaint* |
| Reverse / normal | `ESC[7m` / `ESC[0m` | H19: `ESC p`/`ESC q`; VT52: *plain* | *plain* | *plain* | *plain* | *plain* |
| Size query | `ESC[999;999H`+`ESC[6n`, reply `ESC[r;cR` | *(fixed size)* | *(fixed)* | *(fixed)* | *(fixed)* | *(fixed)* |
| Arrow-key input | `ESC[A`–`D` | `ESC A`–`D` | *(none; hjkl)* | *(none; hjkl)* | *(none; hjkl)* | *(none; hjkl)* |

The **Kaypro 83/84** build (`TERM_KPRO`) is the ADM-3A column with one
change: it has hardware insert-line (`ESC E`) / delete-line (`ESC R`), so its
1-row scroll uses those instead of a repaint (the same codes as Televideo /
Wyse, reached through the shared `ESC =` offset path in `term.c`).  It keeps
the ADM-3A's `^Z` clear, space-padded clear-to-EOL, and no reverse video.
`TERM_HAS_SCROLL` (the flag the scroll fast path tests) is derived in
`termcfg.h` from `TERM_HAS_REGION` / `TERM_HAS_ILDL`, so declaring a family's
scroll mechanism is enough to enable it.

Where a cell says *repaint* / *space-pad* / *plain*, the operation is
emulated: mid-line insert/delete falls back to `scr_redraw_cur_line()`, a
missing scroll to a full text-area `scr_refresh()`, a missing clear-to-EOL
to space padding (stopping one short of the last column so an auto-wrap
terminal never scrolls from the bottom-right cell), and a missing reverse
attribute to plain status text.  The Hazeltine build also maps `~` (its
command lead-in, undisplayable) to `^` on screen via `term_putch`, leaving
file contents untouched.

Input is described in §7.8 (BDOS 6/11 via `bdos_disk`; no BIOS, no libc
`getch()`).

---

## 13. Operator-Motion Model (`emove.c`)

The `apply_op(op, from, to, linewise)` function applies an operator to a range:
- `'d'`: saves undo, saves to yank buffer, deletes range
- `'c'`: saves undo, saves to yank buffer, deletes range, calls `undo_save_insert`, enters insert mode
- `'y'`: saves to yank buffer only

`motion_endpoint(ch, count, &linewise)` computes the endpoint position for a given motion character and count. Supported motions: `h`, `l`, `w`, `b`, `e`, `$`, `0`, `^`, `j`, `k`, `G`, and `` ` `` (mark; exclusive charwise).

The `>` and `<` operators (v2.7) ride the same model: `apply_op` routes them to `apply_shift` (arguments in the `sh_op`/`sh_from`/`sh_to` globals -- frameless, like `gb_insert_room`), which shifts every line touched by the range one `TAB_STOP` right (insert one tab; empty lines skipped; `room1`/`gb_insert_room` recovers a full gap) or left (remove up to `TAB_STOP` columns of leading blanks).  Always linewise regardless of the motion; an exclusive endpoint in column 0 leaves that line out (vim's rule).  Lines are processed bottom-up so the positions of the lines still to do are unaffected by the edits below them -- this also survives a window swap, since the loop is driven by a line count.  The cursor lands on the first non-blank of the topmost shifted line.  A single-line shift is undoable; multi-line shifts invalidate the undo record (scattered edits, like `:s`).  A shift that changes nothing (e.g. `<<` on an unindented line) does not set the modified flag.  `:[range]>` and `:[range]<` (ex.c) call the same engine through the shared range parser.

For the `` ` `` motion the mark character is passed in the global `me_mkc` (edit.c reads it from the keyboard; erepeat.c replays `ed.dot_arg`): an invalid character returns −1 silently, an unset/invalidated mark shows "Mark not set" and returns −1.  The "Unknown motion: %c" message for an unrecognised `ch` is also shown inside `motion_endpoint()` (default case), so callers only test for a negative return.

---

## 14. Known Limitations

1. **Single-level undo.** Only the most recent change can be undone. `u` after `u` is a no-op.
2. **`nG` scans sequentially from byte 0.** Navigating to line 1000 reads roughly 1000 lines from disk; navigating to line 29000 in a 30K-line file reads most of the file. The scan reads byte-by-byte via `hvi_fgetc` (one BDOS 33 per 128-byte sector).
3. **Dot-repeat text truncated at 128 bytes.** Long insertions are truncated silently.
4. **No visual/block selection mode.**
5. **No macro recording or playback.**
6. **No window splitting.**
7. **No regex search** — plain substring match only.
8. **`~` (toggle case) is single-level undo per character.** With a count, only the last character's toggle is undoable.
9. **Terminal size query may require Enter on some CP/M setups** if the host console is line-buffered (the countdown polls expire and the 80×24 defaults are used; the buffered reply may then leak into the input stream).
10. **`:r` into a full window truncates the read.** The `:r` staging loop stops when `gb_insert()` fails instead of room-swapping; existing buffer content is never lost. All other insert paths (typing, put, `o`/`O`, dot replay) recover via `gb_make_room` / `gb_insert_room`.
11. **Filenames limited to `PATH_MAX = 64` characters**, which exceeds CP/M's 8.3 + `du:` prefix limit but may be relevant on cross-platform use.
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
| Size-query reads defer all other console output | BDOS console-write calls between response reads can consume buffered terminal response bytes on some hosts; the whole `ESC[6n` handshake is one `con_write` block followed only by `con_wait` polls |
| Size query exists only in the ANSI build | The `ESC[6n` handshake (and its `raw_num`/`tgs_num` decimal parser) are compiled in only when `TERM_HAS_GETSIZE` is defined (ANSI). Non-ANSI families use the compile-time fixed geometry and send no query, so a dumb terminal never receives a report request it cannot answer — and each such build is smaller |
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
| `gb_load_fp()` merged into `gb_load()` | Both functions were nearly identical. Adding an optional handle parameter to `gb_load()` (NULL = `hvi_fopen` internally; now an `HFILE *`) eliminates the entire `gb_load_fp()` function body and its IX frame |
| `scr_redraw_cur_vrow()` removed | Used only by `r` (replace). `scr_redraw_cur_line()` is a correct superset: it redraws the full logical line, which is identical for single-width lines and also handles wrapped lines. Removes ~60 bytes |
| `scr_line_end()` removed as dead code | Declared in `hvi.h`, defined in `screen.c`, but never called from anywhere in the codebase. Removing dead declarations and definitions reduces binary size without any functional change |
| `cpmio.c` replaces all stdio and malloc | HI-TECH C's `fopen()` allocates a heap buffer per handle; after `gb_init()` consumes the TPA there is no heap left. Direct BDOS calls with a static 3-slot HFILE pool eliminate the problem entirely and reduce binary size by avoiding buffered-I/O library code |
| FCB sector fields from little-endian bytes | The sector number for BDOS 33 is `offset >> 7`, a `long`. A 16-bit cast before shifting truncates files > 32 KB (a historical bug); 32-bit shift/mask expressions are correct but each compiles to a ~50-byte library call in the per-sector hot path. `rd_sector`/`hvi_fseek` shift the `long` once and then read its little-endian bytes directly via `((unsigned char *)&sect)[n]` |
| `hvi_fsize` uses exponential+binary search | BDOS 35 (Compute File Size) is not implemented by all CP/M emulators. Doubling the probe offset until BDOS 33 fails, then binary-searching the final interval, finds the sector-rounded file size in O(log N) seeks (~21 for 1 MB) without relying on BDOS 35 |
| `tail_has_tab()` gates the `ESC[@`/`ESC[P` fast paths | The one-column insert/delete-character escapes assume a fixed-width tail; a tab between the cursor and end of line renders wider or narrower as text before it changes, so each mid-line insert-mode keystroke drifted the tab-aligned tail one column (v2.7.2, reported editing tab-separated assembly). Scanning the tail for a tab and falling back to `scr_redraw_cur_line()` keeps the cheap path for the common tab-free case |
| `gb_reload_from` flushes edits before discarding buffer | Without the flush, navigating to a new window in a large file silently discards all in-memory edits. `gb_reload_from` calls `gb_save("HVISWP.TMP")` when `ed.modified` is set, making `tail_file` point to a complete, up-to-date snapshot before the buffer is cleared |
| `s_reload_skip_flush` flag prevents double save | `gb_make_room()` saves to the swap file itself before calling `gb_reload_from()`. Setting `s_reload_skip_flush = 1` before the call prevents `gb_reload_from` from saving a second time (which would overwrite the complete swap with only the partial in-memory window) |
| `gb_goto_line` for `nG` in large files | The previous `nG` implementation reloaded from byte 0 and clamped to `scr_line_count()`, which is the buffer-local line count. For a large file mid-scroll this always landed at the end of the loaded buffer. `gb_goto_line(n)` scans `tail_file` from byte 0 to find the exact byte offset of line N, positions the window there, and uses a second short scan to find the local buffer line |
| One `gb_scan_lines()` file scanner | The line-offset lookup (`stop_n >= 0`: offset just past the Nth LF) and the range line-count (`stop_n < 0`) were the same seek+`hvi_fgetc` loop with different stop conditions — merged into one scanner serving both `gb_goto_line` needs |
| Skip BDOS 16 for read-mode HFILE handles | Some CP/M emulators write the FCB's `CR`/`EX` fields back to the directory on BDOS 16, corrupting the extent record. The next BDOS 15 (open) then loads a wrong allocation map, causing BDOS 33 to return sector-0 data for all subsequent random reads. Skipping BDOS 16 for `mode == 1` handles eliminates the corruption entirely |
| Explicit FCB CR field write before BDOS 33 | CP/M 2.2 specifies `r0`/`r1`/`r2` (FCB bytes 33–35) as the random record number for BDOS 33. Some emulators also key off byte 32 (`CR`). Writing the sector number into byte 32 alongside the standard `r0`/`r1`/`r2` fields ensures correct operation on both conforming and non-conforming implementations |
| `do_search_full` for whole-file search | `do_search_from` only searches the in-memory buffer. To find matches in unloaded sections, `do_search_full` adds a Phase 2 that calls `scan_file_for_match` on the unloaded tail (FWD) or head (BWD), then reloads the window if a file match is found. This preserves the fast in-buffer path for the common case while enabling whole-file search |
| `dsf_file_wrapped` flag for correct wrap reporting | A match found in the file's tail section (FWD search) is genuinely ahead of the cursor in file order and is not a wrap, but a match in the head section is. Tracking which `scan_file_for_match` call succeeded allows `do_search_full` to set `ed.search_wrapped` correctly — tail match → no wrap message; head match → "search hit BOTTOM" message |
| `scan_file_for_match` uses sequential `hvi_fgetc` | Reading the file section byte-by-byte via `hvi_fgetc` after a single `hvi_fseek` is simple and correct; `hvi_fgetc` itself derives every sector refill from its self-tracked position (BDOS 33), so no CR/EX/S2 state is trusted |
| `do_search_from` uses `pos <= sp` for wrap detection | The last iteration of the scan loop (i == size) sets `pos == sp` (the start position). The old `pos < sp` check missed this case, leaving `search_wrapped = 0` when the cursor sat exactly on the only match. `do_search_full` then saw a "found without wrap" result and returned immediately instead of scanning the unloaded file sections. The `<=` change ensures Phase 2 is triggered whenever the result required a full buffer traversal |
| Ctrl-F load trigger uses `top_line + n + text_rows > total` | The old condition (`top_line + n >= total - 1`) triggered a load only when the new top row hit the last buffer line, but the new viewport still needed `text_rows` more lines below it, causing a screen of `~`. The new condition fires whenever the bottom of the new viewport would fall past the buffer end |
| Ctrl-F recomputes `top_line` after `gb_load_more` | `gb_load_more` calls `gb_discard_head` which adjusts `ed.top_pos`. Using the pre-load `top_line` for `scr_line_start(new_top)` after a discard produced an offset into stale line numbering. Recomputing `top_line = gb_count_nl(0, ed.top_pos)` after the load uses the correct adjusted value |
| No line count in status bar | Computing an accurate total requires scanning the entire file (O(N) BDOS reads), which is too slow on a real CP/M machine. A buffer-local count is misleading when the window is mid-file. The status bar shows only the filename and modified flag |
| Bitwise arithmetic for `TAB_STOP` | Tab-stop alignments historically use compiler-injected modulo/division code. We switched this block `((col / TAB_STOP + 1) * TAB_STOP)` to a hardware bitwise OR block `((col \| (TAB_STOP - 1)) + 1)` which compiles drastically smaller since `TAB_STOP` is 8 (a power of 2). |
| `begin_hmove`/`end_hmove` abstraction | Horizontal motion commands abstractly bracket updates to `ed.cur_pos`. The `end_hmove` block natively catches if a visual boundary crossing occurs on the physical screen (line wrap text) and correctly invalidates `ed.cur_vrow` to snap the UI rendering context back without redundant checks scattered. |
| Extract newline scans to `find_bol`/`find_eol` | 16+ inline while-loops repeating newline scanning logic across the code base were refactored into `gap.c` exports to cut binary block duplication via central CALL jumps. |
| Encapsulate `mv_find` inline command | The `f/F/;/;` character-find routines were moved from `edit.c` to `emove.c` (`mv_find()`) so they could operate under the secure caching brackets of `begin_hmove`, stopping UI freezing. We removed the "f_" status prompt to replicate pure vi mechanics. |
| Fixed `csv`/`ncsv`/`cret` frame (v2.1) | The original custom `csv` left SP two bytes above IX at body entry, silently clobbering the deepest auto local on the next argument push (the "-O corrupts autos" myth). The fixed routines match the real HI-TECH frame — save IX **and** IY, arguments at IX+6, SP == IX — making auto locals fully reliable |
| Statics over autos and params, everywhere (v2.7.1) | HI-TECH C spills auto locals and parameters to IX-relative slots: 6 bytes / 38 T-states per int access vs 3 bytes / 32 T-states for a static. Since nothing in HVI is reentrant, all remaining auto locals became function statics, and parameters of hot functions read ~4+ times are copied to statics on entry (fewer-use copies measured as net losses and were not kept). Combined with `x > 0` → `x != 0` where operands are provably non-negative (a 7-byte inline test instead of a 13-byte `wrelop` call), the pass removed 1,604 bytes — 12 records |
| `gb_char_at` in assembly (v2.5) | The hottest function in the editor — every per-character scanner calls it once per byte. Frameless assembly reading the GapBuf fields at fixed offsets from `ed` roughly halves its cost; requires GapBuf to stay `Editor`'s first member |
| `find_bol`/`find_eol` on CPIR/CPDR scanners (v2.5) | `gb_memchr`/`gb_memrchr` scan at 21 T-states/byte vs ~300 for the compiled `gb_char_at` loop. These two functions back every line motion (`j`/`k`, `dd`, `J`, `o`, `G`, `:N`, `line_span`), so line scans in a full buffer are an order of magnitude faster; `scr_line_start` hops lines via `find_eol` instead of scanning every byte |
| Newline counting funnels into `gb_cntnl` | One CPIR counter backs `gb_count_nl`, the line caches, `gb_insert`'s tally, and the yank/undo/dot-replay "does it contain a newline" checks — one copy of the loop, 8× faster than per-byte C |
| `:r` reads through a 128-byte staging chunk | One `gb_insert` per sector instead of per character; each per-character insert paid a full gap move plus the 27-slot mark sweep |
| `con_write` (BDOS 6) for all console output | One assembly loop flushes the whole output buffer; BDOS 6 also skips BDOS 2's ^S/^P polling. `bdos_puts` and the size query share it |
| Input via BDOS 6/11, never BIOS | All console I/O goes through `bdos_disk`, preserving IX; BIOS CONIN is not used anywhere (earlier versions used it and hit line-buffering and IX-corruption issues on emulated hosts) |
| `dsk_u` brackets user-area BDOS calls (v2.5) | CP/M has no per-FCB user number. One assembly routine does set-user / operation / restore-user in a single call; `user < 0` (unprefixed names) calls straight through, so normal operation never issues a BDOS 32. Six C-level bracket pairs collapsed into one routine |
| `fill_fcb` + `du:` parser in assembly (v2.5) | Compiled K&R C is ~2.5× the size of hand Z80 for this kind of byte-pushing; moving the FCB build and prefix parse to cstart.as paid for the entire user-area feature (v2.5 ships at the same 229 records as v2.4) |
| Prefixed temp name for cross-user saves (v2.5) | BDOS 23 renames within one drive and user area only; `gb_save` builds `HVITMP.TMP` with the destination's `du:` prefix so the erase + rename never crosses a boundary |
| `du_cur` is a data-psect byte, not BSS | BSS is zeroed at startup, and 0 is a valid user number; the "not yet fetched" sentinel (80h) must therefore be an initialised data byte |
| `gb_split` shared clamp/split helper | `gb_copy_out` and `gb_count_nl` duplicated ~250 bytes of range-clamp and split-at-gap arithmetic; one helper emits the two raw segments for both |
| `gb_load_last`/`gb_load_prev` window jumps | The `G`-to-EOF and `^B`/`k`-at-top window reloads each inlined ~50 bytes of 32-bit clamp arithmetic at two or three call sites; shared helpers in gap.c hold the only copies |

---

## 16. License

MIT License. Copyright (c) Juan Orlandini.
