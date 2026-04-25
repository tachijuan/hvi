/*
 * gap.c - Gap buffer implementation for HVI
 * Author: Juan Orlandini
 * License: MIT
 *
 * The gap buffer stores file content as:
 *   [text before gap][GAP bytes][text after gap]
 * Insertions move the gap to the cursor and fill from the front.
 * Deletions simply expand the gap.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "hvi.h"

extern Editor ed;

/* HI-TECH C V3.09 does not include memmove -- provide our own. */
static void gb_memmove(dst, src, len)
char *dst;
char *src;
int   len;
{
    if (dst < src) {
        while (len-- > 0) *dst++ = *src++;
    } else if (dst > src) {
        dst += len; src += len;
        while (len-- > 0) *--dst = *--src;
    }
}

/*
 * Initialise an empty gap buffer.
 *
 * Allocate the largest contiguous block the heap can provide, stepping
 * down by 2048 bytes until malloc succeeds.  We do NOT probe-then-free:
 * if HiTech-C's free() does not return memory to the free list (a common
 * CP/M runtime limitation), a probe allocation permanently consumes heap
 * space and leaves nothing for fopen().  A single direct allocation lets
 * fopen() use whatever the heap has left.
 *
 * Returns 1 on success, 0 on failure.
 */
int gb_init()
{
    int   alloc;
    char *p;

    p = (char *)0;
    for (alloc = BUF_MAX + GAP_MIN; alloc >= 4096; alloc -= 2048) {
        p = (char *)malloc(alloc);
        if (p) break;
    }
    if (!p) return 0;

    if (ed.debug)
        fprintf(stderr, "gb_init: alloc=%d\n", alloc);

    ed.gb.buf    = p;
    ed.gb.size   = alloc;
    ed.gb.gstart = 0;
    ed.gb.gend   = alloc;
    return 1;
}

void gb_free()
{
    if (ed.gb.buf) {
        free(ed.gb.buf);
        ed.gb.buf = (char *)0;
    }
}

/* Logical content length (excludes the gap). */
int gb_content_len()
{
    return ed.gb.size - (ed.gb.gend - ed.gb.gstart);
}

/*
 * Return the character at logical position pos.
 * Returns -1 if pos is out of range.
 */
int gb_char_at(pos)
int pos;
{
    int len = gb_content_len();
    if (pos < 0 || pos >= len)
        return -1;
    if (pos < ed.gb.gstart)
        return (unsigned char)ed.gb.buf[pos];
    return (unsigned char)ed.gb.buf[ed.gb.gend + (pos - ed.gb.gstart)];
}

/*
 * Move the gap so that gstart == pos.
 * This is the core operation: O(n) move of characters.
 */
static void gb_move_gap(pos)
int pos;
{
    int gap_len = ed.gb.gend - ed.gb.gstart;
    if (pos == ed.gb.gstart)
        return;
    if (pos < ed.gb.gstart) {
        /* move gap left: shift text right */
        int move = ed.gb.gstart - pos;
        gb_memmove(ed.gb.buf + ed.gb.gend - move,
                ed.gb.buf + pos,
                move);
        ed.gb.gstart = pos;
        ed.gb.gend   = pos + gap_len;
    } else {
        /* move gap right: shift text left */
        int move = pos - ed.gb.gstart;
        gb_memmove(ed.gb.buf + ed.gb.gstart,
                ed.gb.buf + ed.gb.gend,
                move);
        ed.gb.gstart = pos;
        ed.gb.gend   = pos + gap_len;
    }
}

/*
 * The gap buffer is pre-allocated to its full capacity in gb_init().
 * If gb_insert() is called when the gap is already empty the buffer is
 * full; signal failure so the caller can report "Buffer full".
 */
static int gb_ensure_gap()
{
    return 0;   /* buffer is pre-allocated; cannot grow */
}

/*
 * Insert len bytes from text at logical position pos.
 * Returns 1 on success, 0 on failure.
 */
int gb_insert(pos, text, len)
int   pos;
char *text;
int   len;
{
    int i;
    /* Any buffer change may alter line counts — invalidate both caches. */
    ed.cur_line_pos  = -1;
    ed.line_cnt_cached = 0;
    for (i = 0; i < len; i++) {
        if (ed.gb.gend - ed.gb.gstart < 1) {
            if (!gb_ensure_gap())
                return 0;
        }
        gb_move_gap(pos + i);
        ed.gb.buf[ed.gb.gstart] = text[i];
        ed.gb.gstart++;
    }
    return 1;
}

/*
 * Delete len bytes starting at logical position pos.
 * Returns 1 on success.
 */
int gb_delete(pos, len)
int pos;
int len;
{
    int clen;
    ed.cur_line_pos  = -1;
    ed.line_cnt_cached = 0;
    clen = gb_content_len();
    if (pos < 0 || pos >= clen)
        return 0;
    if (pos + len > clen)
        len = clen - pos;
    gb_move_gap(pos);
    /* Expand gap right to consume deleted chars */
    ed.gb.gend += len;
    return 1;
}

/* Record the tail start position when the buffer fills during load. */
static void gb_record_tail(f, filename, c, prev_cr)
FILE *f;
char *filename;
int   c, prev_cr;
{
    long pos;
    pos = ftell(f) - 1L;
    if (c == 0x0A && prev_cr)
        pos -= 1L;
    ed.tail_offset = pos;
    strncpy(ed.tail_file, filename, PATH_MAX - 1);
    ed.tail_file[PATH_MAX - 1] = '\0';
}

/*
 * Load a file into the gap buffer.
 * Strips bare CR characters to normalise line endings.
 * When the file exceeds the buffer capacity, records the byte offset where
 * loading stopped so gb_save() can append the tail on write.
 * Returns 1 on success, 0 on open error, 2 on partial load (file too large).
 */
int gb_load(filename)
char *filename;
{
    FILE *f;
    int   c, prev_cr, full;
    char  tmp[1];

    /* Open the file first, before touching any struct fields,
     * to rule out struct-write side-effects on the C library state. */
    if (ed.debug)
        fprintf(stderr, "gb_load: file='%s' gbsize=%d\n",
                filename, ed.gb.size);
    f = fopen(filename, "rb");
    if (ed.debug)
        fprintf(stderr, "gb_load: fopen=%s\n", f ? "ok" : "FAILED");
    if (!f)
        return 0;

    ed.gb.gstart    = 0;
    ed.gb.gend      = ed.gb.size;
    ed.tail_offset  = 0L;
    ed.tail_file[0] = '\0';

    prev_cr = 0;
    while ((c = fgetc(f)) != EOF) {
        if (c == 0x0D) { prev_cr = 1; continue; }
        if (c == 0x1A) break;
        full = (gb_content_len() >= ed.gb.size - GAP_MIN);
        if (full) {
            gb_record_tail(f, filename, c, prev_cr);
            fclose(f);
            return 2;
        }
        prev_cr = 0;
        tmp[0] = (char)c;
        gb_insert(gb_content_len(), tmp, 1);
    }
    fclose(f);
    return 1;
}

/*
 * Load from an already-open file handle (pre-opened before gb_init()).
 * Identical to gb_load() but skips fopen() so that the caller can open
 * the file while the heap is still free, before gb_init() consumes it.
 * Closes f on return.
 * Returns 1 on full load, 2 on partial load.
 */
int gb_load_fp(f, filename)
FILE *f;
char *filename;
{
    int   c, prev_cr, full;
    char  tmp[1];

    if (ed.debug)
        fprintf(stderr, "gb_load_fp: file='%s' gbsize=%d\n",
                filename, ed.gb.size);

    ed.gb.gstart    = 0;
    ed.gb.gend      = ed.gb.size;
    ed.tail_offset  = 0L;
    ed.tail_file[0] = '\0';

    prev_cr = 0;
    while ((c = fgetc(f)) != EOF) {
        if (c == 0x0D) { prev_cr = 1; continue; }
        if (c == 0x1A) break;
        full = (gb_content_len() >= ed.gb.size - GAP_MIN);
        if (full) {
            gb_record_tail(f, filename, c, prev_cr);
            fclose(f);
            return 2;
        }
        prev_cr = 0;
        tmp[0] = (char)c;
        gb_insert(gb_content_len(), tmp, 1);
    }
    fclose(f);
    return 1;
}

/*
 * Discard n characters from the head of the buffer, advancing win_start
 * to track the file-byte offset.  Adjusts cur_pos and top_pos.
 * The discard is capped so the cursor position is never dropped.
 * Returns the number of characters actually discarded.
 */
static int gb_discard_head(n)
int n;
{
    int i, c, discard;
    long new_win;

    if (n <= 0) return 0;
    discard = n;
    if (discard > ed.cur_pos) discard = ed.cur_pos;
    if (discard <= 0) return 0;

    /* Each '\n' in the buffer was '\r\n' in the file, so count extra bytes */
    new_win = ed.win_start;
    for (i = 0; i < discard; i++) {
        c = gb_char_at(i);
        if (c == '\n') new_win++;
        new_win++;
    }
    gb_delete(0, discard);
    ed.win_start = new_win;
    ed.cur_pos  -= discard;
    ed.top_pos  -= discard;
    if (ed.top_pos < 0) ed.top_pos = 0;
    if (ed.cur_pos < 0) ed.cur_pos = 0;
    return discard;
}

/*
 * Load up to n characters from the tail file into the end of the buffer.
 * If the buffer is already full, discards an equal number from the head first.
 * Returns the number of characters loaded, or 0 if the tail is exhausted.
 */
int gb_load_more(n)
int n;
{
    FILE *f;
    int   c, loaded, need;
    char  tmp[1];

    if (ed.tail_offset == 0L || !ed.tail_file[0]) return 0;

    /* Make room: if content + n would exceed capacity, discard from head */
    need = gb_content_len() - (ed.gb.size - GAP_MIN - n);
    if (need > 0)
        if (gb_discard_head(need) == 0) return 0;

    f = fopen(ed.tail_file, "rb");
    if (!f) return 0;
    fseek(f, ed.tail_offset, 0);

    loaded = 0;
    while (loaded < n && gb_content_len() < ed.gb.size - GAP_MIN) {
        c = fgetc(f);
        if (c == EOF || c == 0x1A) { ed.tail_offset = 0L; break; }
        if (c == 0x0D) continue;
        tmp[0] = (char)c;
        if (!gb_insert(gb_content_len(), tmp, 1)) break;
        loaded++;
    }
    if (ed.tail_offset != 0L)
        ed.tail_offset = ftell(f);

    fclose(f);
    return loaded;
}

/*
 * Save the buffer to filename.
 * Writes LF as CR+LF (CP/M convention) and terminates with ^Z.
 * If the file was partially loaded (tail_offset > 0), the unloaded tail
 * is appended from tail_file after the in-memory content, so no data is
 * lost even when editing files larger than BUF_MAX.
 *
 * When saving to the same file that holds the tail, we write to a temp
 * file (HVITMP.TMP) first, then replace the original with the temp so
 * that we are never reading from and writing to the same file at once.
 *
 * After a successful save, tail_file and tail_offset are updated to
 * point to the correct location in the newly written file so that
 * repeated saves remain correct.
 *
 * Returns 1 on success, 0 on error.
 */
/* Write the original-file head (bytes before the buffer window) to f. */
static void gb_write_head(f)
FILE *f;
{
    FILE *tf;
    long  pos;
    int   c;
    if (ed.win_start <= 0L || !ed.tail_file[0]) return;
    tf = fopen(ed.tail_file, "rb");
    if (!tf) return;
    pos = 0L;
    while (pos < ed.win_start) {
        c = fgetc(tf);
        if (c == EOF || c == 0x1A) break;
        fputc(c, f);
        pos++;
    }
    fclose(tf);
}

/* Write in-memory buffer to f (LF -> CR+LF); return byte count written. */
static long gb_write_buf(f)
FILE *f;
{
    long  written;
    int   i, len, c;
    written = ed.win_start;
    len = gb_content_len();
    for (i = 0; i < len; i++) {
        c = gb_char_at(i);
        if (c == '\n') { fputc(0x0D, f); written++; }
        fputc(c, f);
        written++;
    }
    return written;
}

/* Append the unloaded tail from the original source file to f. */
static void gb_write_tail(f)
FILE *f;
{
    FILE *tf;
    int   c;
    if (ed.tail_offset <= 0L || !ed.tail_file[0]) return;
    tf = fopen(ed.tail_file, "rb");
    if (!tf) return;
    fseek(tf, ed.tail_offset, 0);
    while ((c = fgetc(tf)) != EOF && c != 0x1A)
        fputc(c, f);
    fclose(tf);
}

/*
 * Save the buffer to filename.
 * For large files, writes head + buffer + tail so no data is lost.
 * Uses a temp file when saving back to the same file that holds the tail.
 * Returns 1 on success, 0 on error.
 */
int gb_save(filename)
char *filename;
{
    FILE *f;
    int   using_tmp;
    long  new_tail;
    char  tmp_name[16];

    using_tmp = 0;
    if (ed.tail_file[0] && (ed.win_start > 0L || ed.tail_offset > 0L) &&
        strcmp(filename, ed.tail_file) == 0) {
        strcpy(tmp_name, "HVITMP.TMP");
        f = fopen(tmp_name, "wb");
        using_tmp = 1;
    } else {
        f = fopen(filename, "wb");
    }
    if (!f) return 0;

    gb_write_head(f);
    new_tail = gb_write_buf(f);
    gb_write_tail(f);
    fputc(0x1A, f);
    fclose(f);

    if (using_tmp) { remove(filename); rename(tmp_name, filename); }

    if (ed.tail_file[0] && (ed.win_start > 0L || ed.tail_offset > 0L)) {
        strncpy(ed.tail_file, filename, PATH_MAX - 1);
        ed.tail_file[PATH_MAX - 1] = '\0';
        ed.tail_offset = new_tail;
    }
    return 1;
}
