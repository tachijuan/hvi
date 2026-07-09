/*
 * gap.c - Gap buffer implementation for HVI
 * Author: Juan Orlandini
 * License: MIT
 *
 * The gap buffer stores file content as:
 *   [text before gap][GAP bytes][text after gap]
 * Insertions move the gap to the cursor and fill from the front.
 * Deletions simply expand the gap.
 *
 * File I/O uses cpmio.c (hvi_fopen/hvi_fgetc/...) directly; no stdio
 * library is linked.  Memory allocation uses hvi_malloc from cpmio.c
 * which carves a single block from the CP/M TPA.
 */

#include "hvi.h"

extern Editor ed;
extern int bdos_disk();

/* Overlap-safe block move; LDIR/LDDR assembly in cstart.as (~7x faster
 * than the compiled loop -- this runs on every gap move). */
extern void gb_memmove(/* char *dst, char *src, int len */);

/* CPIR newline counter over a raw memory range; assembly in cstart.as
 * (~8x faster than a gb_char_at loop -- backs gb_count_nl below). */
extern int gb_cntnl(/* char *p, int len */);

/* Temp-file names: one copy of each literal, passed around directly. */
static char swp_name[] = "HVISWP.TMP";
static char tmp_name[] = "HVITMP.TMP";

/*
 * Initialise an empty gap buffer.
 *
 * Allocate the largest contiguous block the TPA can provide, stepping
 * down by 2048 bytes until hvi_malloc succeeds.
 *
 * Returns 1 on success, 0 on failure.
 */
int gb_init()
{
    int   alloc;
    char *p;

    p = (char *)0;
    for (alloc = BUF_MAX + GAP_MIN; alloc >= 4096; alloc -= 2048) {
        p = (char *)hvi_malloc(alloc);
        if (p) break;
    }
    if (!p) return 0;

    ed.gb.buf    = p;
    ed.gb.size   = alloc;
    ed.gb.gstart = 0;
    ed.gb.gend   = alloc;
    return 1;
}

void gb_free()
{
    if (ed.gb.buf) {
        hvi_free(ed.gb.buf);
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
 *
 * This is the hottest function in the editor -- every scanner calls it
 * once per character -- so it must not call gb_content_len(): the bounds
 * check is done inline on the raw buffer index instead (pos >= content
 * length is equivalent to the gap-adjusted index reaching ed.gb.size).
 */
int gb_char_at(pos)
int pos;
{
    if (pos < 0)
        return -1;
    if (pos < ed.gb.gstart)
        return (unsigned char)ed.gb.buf[pos];
    pos += ed.gb.gend - ed.gb.gstart;
    if (pos >= ed.gb.size)
        return -1;
    return (unsigned char)ed.gb.buf[pos];
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
 * Insert len bytes from text at logical position pos.
 * Returns 1 on success, 0 if the buffer is full (gap too small);
 * on failure nothing is inserted (all-or-nothing).
 *
 * Bulk operation: one gap move plus one LDIR block copy, instead of a
 * gb_move_gap call per character (a 1 KB paste previously made ~3000
 * function calls).
 *
 * All locals static: gb_move_gap is a function call that may corrupt
 * IX-relative auto variables under HI-TECH C -O.
 */
static int gbi_i;
static int gbi_nl_added;

int gb_insert(pos, text, len)
int   pos;
char *text;
int   len;
{
    if (len <= 0) return 1;
    if (ed.gb.gend - ed.gb.gstart < len)
        return 0;   /* buffer is pre-allocated; cannot grow */

    gbi_nl_added = 0;
    for (gbi_i = 0; gbi_i < len; gbi_i++) {
        if (text[gbi_i] == '\n') gbi_nl_added++;
    }

    if (ed.line_cnt_cached > 0) ed.line_cnt_cached += gbi_nl_added;

    if (ed.cur_line_pos >= 0) {
        if (pos <= ed.cur_line_pos) {
            ed.cur_line_pos += len;
            ed.cur_line += gbi_nl_added;
        }
    }

    if (pos != ed.cur_pos || gbi_nl_added > 0) {
        ed.cur_vrow = -1;
    }

    gb_move_gap(pos);
    gb_memmove(ed.gb.buf + ed.gb.gstart, text, len);
    ed.gb.gstart += len;
    return 1;
}

/*
 * Copy len bytes of logical content starting at pos into dst.
 * Bulk replacement for per-character gb_char_at loops (yank, undo and
 * dot-text capture): at most two LDIR block copies, one per gap side.
 * The range is clamped to the buffer content.
 */
static int gco_seg;

void gb_copy_out(dst, pos, len)
char *dst;
int   pos, len;
{
    gco_seg = gb_content_len();
    if (pos < 0 || pos >= gco_seg || len <= 0) return;
    if (pos + len > gco_seg) len = gco_seg - pos;

    if (pos < ed.gb.gstart) {
        gco_seg = ed.gb.gstart - pos;
        if (gco_seg > len) gco_seg = len;
        gb_memmove(dst, ed.gb.buf + pos, gco_seg);
        dst += gco_seg;
        pos += gco_seg;
        len -= gco_seg;
    }
    if (len > 0)
        gb_memmove(dst, ed.gb.buf + (ed.gb.gend - ed.gb.gstart) + pos, len);
}

/*
 * Count '\n' characters in the logical range [pos, pos+len).
 * Splits the range at the gap and runs the CPIR scanner from cstart.as
 * on each raw segment.  The range is clamped to the buffer content.
 */
static int cnl_n, cnl_seg;

int gb_count_nl(pos, len)
int pos, len;
{
    cnl_n = gb_content_len();
    if (pos < 0) { len += pos; pos = 0; }
    if (pos >= cnl_n || len <= 0) return 0;
    if (pos + len > cnl_n) len = cnl_n - pos;

    cnl_n = 0;
    if (pos < ed.gb.gstart) {
        cnl_seg = ed.gb.gstart - pos;
        if (cnl_seg > len) cnl_seg = len;
        cnl_n = gb_cntnl(ed.gb.buf + pos, cnl_seg);
        pos += cnl_seg;
        len -= cnl_seg;
    }
    if (len > 0)
        cnl_n += gb_cntnl(ed.gb.buf + (ed.gb.gend - ed.gb.gstart) + pos, len);
    return cnl_n;
}

/*
 * Delete len bytes starting at logical position pos.
 * Returns 1 on success.
 *
 * All locals static: gb_count_nl/gb_move_gap are function calls that may
 * corrupt IX-relative auto variables under HI-TECH C -O.
 */
static int gbd_clen;
static int gbd_nl_del;

int gb_delete(pos, len)
int pos;
int len;
{
    gbd_clen = gb_content_len();
    if (pos < 0 || pos >= gbd_clen)
        return 0;
    if (pos + len > gbd_clen)
        len = gbd_clen - pos;

    gbd_nl_del = gb_count_nl(pos, len);

    if (ed.line_cnt_cached > 0) ed.line_cnt_cached -= gbd_nl_del;

    if (ed.cur_line_pos >= 0) {
        if (pos + len <= ed.cur_line_pos) {
            ed.cur_line_pos -= len;
            ed.cur_line -= gbd_nl_del;
        } else if (pos <= ed.cur_line_pos) {
            ed.cur_line_pos = -1;
        }
    }

    if (pos != ed.cur_pos || gbd_nl_del > 0) {
        ed.cur_vrow = -1;
    }

    gb_move_gap(pos);
    /* Expand gap right to consume deleted chars */
    ed.gb.gend += len;
    return 1;
}

/*
 * Show the [Loading...] indicator on the status line.
 * The caller's subsequent scr_refresh() will overwrite it.
 */
static void show_loading()
{
    scr_status_invalidate();
    term_goto(ed.scr_rows - 1, 0);
    term_clreol();
    term_reverse();
    term_puts("[Loading...]");
    term_normal();
    term_flush();
}

/*
 * Inner read loop shared by gb_load and gb_reload_from.
 * Reads chars from f into the gap buffer until HEOF/^Z or the buffer fills.
 * On partial fill: records tail offset; if filename != NULL also sets
 * tail_file.  Always closes f before returning.
 * Returns 1 (complete) or 2 (partial -- buffer filled before EOF).
 */
/* All locals static so IX-relative frame is never used after a BDOS disk call
 * (BDOS functions 15/20/26 do not guarantee IX preservation on CP/M). */
static HFILE *s_gbf_f;
static char  *s_gbf_fn;
static int    gbf_c, gbf_prev_cr, gbf_limit;
static long   gbf_pos;

static int gb_fill(f, filename)
HFILE *f;
char  *filename;
{
    s_gbf_f  = f;
    s_gbf_fn = filename;

    /* Direct append: both callers reset the buffer first, so the gap
     * spans [gstart, size) throughout the fill and content length equals
     * gstart.  Each byte is stored with one array write instead of a
     * gb_insert call per character (which cost ~6 function calls/byte).
     * The display caches gb_insert would have maintained are simply
     * invalidated up front. */
    ed.cur_vrow        = -1;
    ed.cur_line_pos    = -1;
    ed.line_cnt_cached = 0;
    gbf_limit = ed.gb.size - GAP_MIN;

    gbf_prev_cr = 0;
    while ((gbf_c = hvi_fgetc(s_gbf_f)) != HEOF) {
        if (gbf_c == 0x0D) { gbf_prev_cr = 1; continue; }
        if (gbf_c == 0x1A) break;
        if (ed.gb.gstart >= gbf_limit) {
            gbf_pos = hvi_ftell(s_gbf_f) - 1L;
            if (gbf_c == 0x0A && gbf_prev_cr) gbf_pos -= 1L;
            ed.tail_offset = gbf_pos;
            if (s_gbf_fn) {
                hvi_strncpy(ed.tail_file, s_gbf_fn, PATH_MAX - 1);
                ed.tail_file[PATH_MAX - 1] = '\0';
            }
            hvi_fclose(s_gbf_f);
            return 2;
        }
        gbf_prev_cr = 0;
        ed.gb.buf[ed.gb.gstart++] = (char)gbf_c;
    }
    hvi_fclose(s_gbf_f);
    return 1;
}

/*
 * Load a file into the gap buffer.
 * If fp != NULL use that already-open handle; otherwise hvi_fopen filename.
 * Strips bare CR characters to normalise line endings.
 * Returns 1 on success, 0 on open error, 2 on partial load (file too large).
 */
/* Cache fp and filename before any bdos call -- hvi_fopen calls disk BDOS
 * which may corrupt IX; parameters at IX+N would then read wrong values. */
static HFILE *s_gbl_fp;
static char  *s_gbl_fn;
static int    s_gbl_rc;   /* return value of gb_fill -- static so IX not needed */

int gb_load(filename, fp)
char  *filename;
HFILE *fp;
{
    s_gbl_fn = filename;   /* cache before any bdos call */
    s_gbl_fp = fp;
    if (!s_gbl_fp) {
        s_gbl_fp = hvi_fopen(s_gbl_fn, "rb");
        if (!s_gbl_fp) return 0;
    }
    ed.gb.gstart    = 0;
    ed.gb.gend      = ed.gb.size;
    ed.win_start    = 0L;
    ed.tail_offset  = 0L;
    ed.tail_file[0] = '\0';
    s_gbl_rc = gb_fill(s_gbl_fp, s_gbl_fn);
    return s_gbl_rc;
}

/*
 * Discard n characters from the head of the buffer, advancing win_start
 * to track the file-byte offset.  Adjusts cur_pos and top_pos.
 * The discard is capped so the cursor position is never dropped.
 * Returns the number of characters actually discarded.
 *
 * All locals static: gb_char_at/gb_delete are function calls and may
 * corrupt IX-relative auto variables under HI-TECH C -O.
 */
static int  gdh_discard;
static long gdh_new_win;

static int gb_discard_head(n)
int n;
{
    if (n <= 0) return 0;
    gdh_discard = n;
    if (gdh_discard > ed.cur_pos) gdh_discard = ed.cur_pos;
    if (gdh_discard <= 0) return 0;

    /* Each '\n' in the buffer was '\r\n' in the file, so count extra bytes */
    gdh_new_win = ed.win_start;
    gdh_new_win += (long)gdh_discard;
    gdh_new_win += (long)gb_count_nl(0, gdh_discard);
    gb_delete(0, gdh_discard);
    ed.win_start = gdh_new_win;
    ed.cur_pos  -= gdh_discard;
    ed.top_pos  -= gdh_discard;
    if (ed.top_pos < 0) ed.top_pos = 0;
    if (ed.cur_pos < 0) ed.cur_pos = 0;
    return gdh_discard;
}

/*
 * Load up to n characters from the tail file into the end of the buffer.
 * If the buffer is already full, discards an equal number from the head first.
 * Returns the number of characters loaded, or 0 if the tail is exhausted.
 *
 * All locals static: every sub-call (hvi_fopen, hvi_fgetc, gb_insert, ...)
 * is a function call that may trash IX-relative auto vars under HI-TECH C -O.
 */
static HFILE *glm_f;
static int    glm_n;       /* cache param: BDOS trashes HL which may hold n */
static int    glm_c;
static int    glm_loaded;
static int    glm_need;
static int    glm_limit;
static int    glm_nl;

int gb_load_more(n)
int n;
{
    glm_n = n;             /* cache before any BDOS call */
    if (ed.tail_offset == 0L || !ed.tail_file[0]) return 0;

    /* Make room: if content + n would exceed capacity, discard from head */
    glm_need = gb_content_len() - (ed.gb.size - GAP_MIN - glm_n);
    if (glm_need > 0)
        if (gb_discard_head(glm_need) == 0) return 0;

    show_loading();

    glm_f = hvi_fopen(ed.tail_file, "rb");
    if (!glm_f) return 0;
    if (hvi_fseek(glm_f, ed.tail_offset, 0) != 0) { hvi_fclose(glm_f); return 0; }

    /* Move the gap to the content end once, then append bytes directly
     * (content length == gstart while the gap sits at the end); newlines
     * are tallied so the line-count cache stays valid. */
    gb_move_gap(gb_content_len());
    glm_limit  = ed.gb.size - GAP_MIN;
    glm_loaded = 0;
    glm_nl     = 0;
    while (glm_loaded < glm_n && ed.gb.gstart < glm_limit) {
        glm_c = hvi_fgetc(glm_f);
        if (glm_c == HEOF || glm_c == 0x1A) { ed.tail_offset = 0L; break; }
        if (glm_c == 0x0D) continue;
        if (glm_c == '\n') glm_nl++;
        ed.gb.buf[ed.gb.gstart++] = (char)glm_c;
        glm_loaded++;
    }
    if (glm_loaded > 0) {
        if (ed.line_cnt_cached > 0) ed.line_cnt_cached += glm_nl;
        ed.cur_vrow = -1;
    }
    if (ed.tail_offset != 0L)
        ed.tail_offset = hvi_ftell(glm_f);

    hvi_fclose(glm_f);
    return glm_loaded;
}

/*
 * Reload the buffer starting from a given byte offset in tail_file.
 * Clears all buffer content and loads fresh content from that offset.
 * Resets cur_pos, top_pos, win_start, tail_offset, and display caches.
 * Shows [Loading...] on the status line while reading.
 * Returns 1 on success, 0 on failure.
 *
 * Local f is static: hvi_fopen/hvi_fseek may corrupt auto vars under -O.
 *
 * If ed.modified is set, the in-memory buffer is flushed to HVISWP.TMP
 * before reloading so that edits are not lost when the window shifts.
 * gb_make_room sets s_reload_skip_flush before calling here to suppress
 * the flush (it already saved).
 */
static int   s_reload_skip_flush;
static HFILE *grf_f;
static long   grf_offset;   /* cache param: BDOS (fopen/save) trashes HL:DE */

int gb_reload_from(offset)
long offset;
{
    grf_offset = offset;   /* cache before any BDOS call */
    if (!ed.tail_file[0]) return 0;
    if (grf_offset < 0L) grf_offset = 0L;
    grf_offset &= ~127L;   /* ensure sector alignment */

    /* Flush unsaved edits before discarding the buffer. */
    if (ed.modified && !s_reload_skip_flush) {
        if (!gb_save(swp_name)) { s_reload_skip_flush = 0; return 0; }
    }
    s_reload_skip_flush = 0;

    show_loading();   /* console I/O before any disk access */

#ifdef HVI_DEBUG
    {
        /* Print "RELOAD off=WWXXYYZZ\r\n" (grf_offset as 4 hex bytes). */
        static unsigned char dbg_ww, dbg_xx, dbg_yy, dbg_zz;
        dbg_ww = (unsigned char)((grf_offset >> 24) & 0xFFL);
        dbg_xx = (unsigned char)((grf_offset >> 16) & 0xFFL);
        dbg_yy = (unsigned char)((grf_offset >>  8) & 0xFFL);
        dbg_zz = (unsigned char)( grf_offset        & 0xFFL);
        bdos_disk(2,'R'); bdos_disk(2,'E'); bdos_disk(2,'L');
        bdos_disk(2,'O'); bdos_disk(2,'A'); bdos_disk(2,'D');
        bdos_disk(2,' '); bdos_disk(2,'o'); bdos_disk(2,'f');
        bdos_disk(2,'f'); bdos_disk(2,'=');
        bdos_disk(2, (dbg_ww>>4)<10 ? '0'+(dbg_ww>>4) : 'A'+(dbg_ww>>4)-10);
        bdos_disk(2, (dbg_ww&0xF)<10 ? '0'+(dbg_ww&0xF) : 'A'+(dbg_ww&0xF)-10);
        bdos_disk(2, (dbg_xx>>4)<10 ? '0'+(dbg_xx>>4) : 'A'+(dbg_xx>>4)-10);
        bdos_disk(2, (dbg_xx&0xF)<10 ? '0'+(dbg_xx&0xF) : 'A'+(dbg_xx&0xF)-10);
        bdos_disk(2, (dbg_yy>>4)<10 ? '0'+(dbg_yy>>4) : 'A'+(dbg_yy>>4)-10);
        bdos_disk(2, (dbg_yy&0xF)<10 ? '0'+(dbg_yy&0xF) : 'A'+(dbg_yy&0xF)-10);
        bdos_disk(2, (dbg_zz>>4)<10 ? '0'+(dbg_zz>>4) : 'A'+(dbg_zz>>4)-10);
        bdos_disk(2, (dbg_zz&0xF)<10 ? '0'+(dbg_zz&0xF) : 'A'+(dbg_zz&0xF)-10);
        bdos_disk(2,'\r'); bdos_disk(2,'\n');
    }
#endif

    grf_f = hvi_fopen(ed.tail_file, "rb");
    if (!grf_f) return 0;

    /* Position without a BDOS 33 pre-load.  grf_offset is sector-aligned so
     * buf_pos is always 0.  hvi_fgetc issues BDOS 33 for the first sector on
     * demand -- same code path as gb_load's initial load, which works. */
    grf_f->pos = grf_offset;

    /* Clear buffer and reset all window and display tracking. */
    ed.gb.gstart       = 0;
    ed.gb.gend         = ed.gb.size;
    ed.win_start       = grf_offset;
    ed.tail_offset     = 0L;
    ed.cur_pos         = 0;
    ed.top_pos         = 0;
    ed.cur_vrow        = -1;
    ed.cur_line_pos    = -1;
    ed.line_cnt_cached = 0;
    /* tail_file unchanged -- same source file */
    return gb_fill(grf_f, (char *)0);
}

/*
 * Write the original-file head (bytes before the buffer window) to f.
 * All locals static: hvi_fopen/hvi_fgetc/hvi_fputc are function calls.
 */
static HFILE *gwh_f;       /* cache param: BDOS trashes HL which may hold f */
static HFILE *gwh_tf;
static long   gwh_pos;
static int    gwh_c;

static void gb_write_head(f)
HFILE *f;
{
    gwh_f = f;             /* cache before any BDOS call */
    if (ed.win_start <= 0L || !ed.tail_file[0]) return;
    gwh_tf = hvi_fopen(ed.tail_file, "rb");
    if (!gwh_tf) return;
    gwh_pos = 0L;
    while (gwh_pos < ed.win_start) {
        gwh_c = hvi_fgetc(gwh_tf);
        if (gwh_c == HEOF || gwh_c == 0x1A) break;
        hvi_fputc(gwh_c, gwh_f);
        gwh_pos++;
    }
    hvi_fclose(gwh_tf);
}

/*
 * Write in-memory buffer to f (LF -> CR+LF); return byte count written.
 * Walks the two raw gap-buffer segments with a pointer instead of one
 * gb_char_at call per byte -- halves the per-byte call count on :w.
 * All locals static: hvi_fputc is a function call (BDOS inside).
 */
static HFILE *gwb_f;       /* cache param: BDOS trashes HL which may hold f */
static long   gwb_written;
static int    gwb_c;
static char  *gwb_p, *gwb_end;

static void gwb_seg(p, end)
char *p, *end;
{
    gwb_p   = p;           /* cache params before any BDOS call */
    gwb_end = end;
    while (gwb_p < gwb_end) {
        gwb_c = (int)(unsigned char)*gwb_p;
        if (gwb_c == '\n') { hvi_fputc(0x0D, gwb_f); gwb_written++; }
        hvi_fputc(gwb_c, gwb_f);
        gwb_written++;
        gwb_p++;
    }
}

static long gb_write_buf(f)
HFILE *f;
{
    gwb_f = f;             /* cache before any BDOS call */
    gwb_written = ed.win_start;
    gwb_seg(ed.gb.buf, ed.gb.buf + ed.gb.gstart);
    gwb_seg(ed.gb.buf + ed.gb.gend, ed.gb.buf + ed.gb.size);
    return gwb_written;
}

/*
 * Append the unloaded tail from the original source file to f.
 * All locals static: hvi_fopen/hvi_fgetc/hvi_fputc are function calls.
 */
static HFILE *gwt_f;       /* cache param: BDOS trashes HL which may hold f */
static HFILE *gwt_tf;
static int    gwt_c;

static void gb_write_tail(f)
HFILE *f;
{
    gwt_f = f;             /* cache before any BDOS call */
    if (ed.tail_offset <= 0L || !ed.tail_file[0]) return;
    gwt_tf = hvi_fopen(ed.tail_file, "rb");
    if (!gwt_tf) return;
    if (hvi_fseek(gwt_tf, ed.tail_offset, 0) != 0) { hvi_fclose(gwt_tf); return; }
    while ((gwt_c = hvi_fgetc(gwt_tf)) != HEOF && gwt_c != 0x1A)
        hvi_fputc(gwt_c, gwt_f);
    hvi_fclose(gwt_tf);
}

/*
 * Save the buffer to filename.
 * For large files, writes head + buffer + tail so no data is lost.
 * Uses a temp file when saving back to the same file that holds the tail.
 * Returns 1 on success, 0 on error.
 *
 * All locals static: every sub-call (hvi_fopen, gb_write_head, ...) is a
 * function call and may corrupt IX-relative auto vars under HI-TECH C -O.
 */
static char  *gsv_fn;          /* cache param: BDOS trashes HL which may hold filename */
static HFILE *gsv_f;
static int    gsv_using_tmp;
static int    gsv_len;
static long   gsv_new_tail;
static char   gsv_nl[1];

int gb_save(filename)
char *filename;
{
    gsv_fn = filename;     /* cache before any BDOS call */
    /* vi convention: a text file ends with a newline.  Append one to the
     * buffer when the buffer holds the end of the file and lacks it. */
    if (ed.tail_offset == 0L) {
        gsv_len = gb_content_len();
        if (gsv_len > 0 && gb_char_at(gsv_len - 1) != '\n') {
            gsv_nl[0] = '\n';
            gb_insert(gsv_len, gsv_nl, 1);
        }
    }
    gsv_using_tmp = 0;
    if (ed.tail_file[0] && (ed.win_start > 0L || ed.tail_offset > 0L) &&
        hvi_strcmp(gsv_fn, ed.tail_file) == 0) {
        gsv_f = hvi_fopen(tmp_name, "wb");
        gsv_using_tmp = 1;
    } else {
        gsv_f = hvi_fopen(gsv_fn, "wb");
    }
    if (!gsv_f) return 0;

    gb_write_head(gsv_f);
    gsv_new_tail = gb_write_buf(gsv_f);
    gb_write_tail(gsv_f);
    hvi_fputc(0x1A, gsv_f);
    hvi_fclose(gsv_f);

    if (gsv_using_tmp) {
        hvi_remove(gsv_fn);
        hvi_rename(tmp_name, gsv_fn);
    }

    if (ed.tail_file[0] && (ed.win_start > 0L || ed.tail_offset > 0L)) {
        hvi_strncpy(ed.tail_file, gsv_fn, PATH_MAX - 1);
        ed.tail_file[PATH_MAX - 1] = '\0';
        ed.tail_offset = gsv_new_tail;
    }
    return 1;
}

/* ------------------------------------------------------------------ */
/*  Buffer overflow recovery                                           */
/* ------------------------------------------------------------------ */

/*
 * Compute the absolute byte position of ed.cur_pos in the on-disk file.
 * Each '\n' in the buffer corresponds to '\r\n' in the file (two bytes).
 * All locals static to survive BDOS/function calls under HI-TECH C -O.
 */
static long gmr_cursor_fp;

static long gb_cursor_file_pos()
{
    gmr_cursor_fp = ed.win_start;
    gmr_cursor_fp += (long)ed.cur_pos;
    gmr_cursor_fp += (long)gb_count_nl(0, ed.cur_pos);
    return gmr_cursor_fp;
}

/*
 * Make room in the gap buffer when it fills during editing.
 * Saves the complete file (head + buffer + tail) to HVISWP.TMP, then
 * reloads a window around the cursor.  On return ed.cur_pos is restored
 * to the cursor's logical position within the new buffer window.
 * Returns 1 on success, 0 on failure (disk full, I/O error).
 *
 * All locals static: gb_save/gb_reload_from issue BDOS calls that may
 * corrupt IX-relative auto variables under HI-TECH C -O.
 */
static long gmr_cfp;
static long gmr_load_from;
static long gmr_target;
static long gmr_counted;
static int  gmr_p;
static int  gmr_len;
static int  gmr_c2;

int gb_make_room()
{
    gmr_cfp = gb_cursor_file_pos();

    /* Save the complete file to the swap. */
    if (!gb_save(swp_name)) return 0;

    /* Make the swap the source for subsequent head/tail reads. */
    hvi_strcpy(ed.tail_file, swp_name);
    ed.win_start   = 0L;
    ed.tail_offset = 0L;

    /* Reload a window that starts before the cursor. */
    gmr_load_from = gmr_cfp - (long)(LOAD_CHUNK >> 1);
    if (gmr_load_from < 0L) gmr_load_from = 0L;
    gmr_load_from &= ~127L;   /* sector-align for gb_reload_from */

    s_reload_skip_flush = 1;  /* gb_save above already flushed */
    if (!gb_reload_from(gmr_load_from)) return 0;

    /* Invalidate undo -- buffer positions shifted after the reload. */
    ed.undo.type = UNDO_NONE;

    /* Scan the new buffer to find the logical position of the cursor.
     * File position of buffer[p] = load_from + p + newlines_before_p.
     * We advance until that equals gmr_cfp. */
    gmr_target  = gmr_cfp - gmr_load_from;
    gmr_counted = 0L;
    gmr_p       = 0;
    gmr_len     = gb_content_len();
    while (gmr_p < gmr_len && gmr_counted < gmr_target) {
        gmr_c2 = gb_char_at(gmr_p);
        if (gmr_c2 == '\n') gmr_counted++;
        gmr_counted++;
        gmr_p++;
    }
    ed.cur_pos = (gmr_p <= gmr_len) ? gmr_p : gmr_len;
    return 1;
}

/* ------------------------------------------------------------------ */
/*  Large-file line navigation                                          */
/* ------------------------------------------------------------------ */

/*
 * Scan ed.tail_file from byte 0, counting LF (0x0A) characters.
 * Returns the byte offset of the first byte of line n (0-indexed) in
 * the CP/M file (which uses CR+LF, so each line end costs 2 bytes).
 * Returns -1L if the file contains fewer than n lines.
 * All locals static to survive BDOS calls under HI-TECH C -O.
 */
static HFILE *gfl_f;
static long   gfl_off;
static int    gfl_n;       /* cache param: BDOS trashes HL which may hold n */
static int    gfl_line;
static int    gfl_c;

static long gb_find_line_offset(n)
int n;
{
    gfl_n = n;             /* cache before any BDOS call */
    if (gfl_n <= 0) return 0L;
    if (!ed.tail_file[0]) return -1L;
    gfl_f = hvi_fopen(ed.tail_file, "rb");
    if (!gfl_f) return -1L;
    gfl_line = 0;
    gfl_off  = 0L;
    while (gfl_line < gfl_n) {
        gfl_c = hvi_fgetc(gfl_f);
        if (gfl_c == HEOF || gfl_c == 0x1A) { hvi_fclose(gfl_f); return -1L; }
        gfl_off++;
        if (gfl_c == '\n') gfl_line++;
    }
    hvi_fclose(gfl_f);
    return gfl_off;
}

/*
 * Count LF characters in ed.tail_file between byte offsets [from_off, to_off).
 * Seeks to from_off with BDOS 33 then reads sequentially.
 * Returns the count; 0 on error or if from_off >= to_off.
 * All locals static to survive BDOS calls under HI-TECH C -O.
 */
static HFILE *gcl_f;
static long   gcl_pos;
static long   gcl_from;    /* cache param: BDOS trashes HL:DE */
static long   gcl_to;      /* cache param: used in loop condition after BDOS */
static int    gcl_cnt;
static int    gcl_c2;

static int gb_count_lines(from_off, to_off)
long from_off, to_off;
{
    gcl_from = from_off;   /* cache before any BDOS call */
    gcl_to   = to_off;
    if (gcl_from >= gcl_to || !ed.tail_file[0]) return 0;
    gcl_f = hvi_fopen(ed.tail_file, "rb");
    if (!gcl_f) return 0;
    if (hvi_fseek(gcl_f, gcl_from, 0) != 0) { hvi_fclose(gcl_f); return 0; }
    gcl_cnt = 0;
    gcl_pos = gcl_from;
    while (gcl_pos < gcl_to) {
        gcl_c2 = hvi_fgetc(gcl_f);
        if (gcl_c2 == HEOF || gcl_c2 == 0x1A) break;
        gcl_pos++;
        if (gcl_c2 == '\n') gcl_cnt++;
    }
    hvi_fclose(gcl_f);
    return gcl_cnt;
}

/*
 * Navigate to line n (1-indexed) in the file, handling the large-file
 * sliding window.
 *
 * Fast path: if the buffer starts at byte 0 and line n is already loaded,
 * navigate directly without any file I/O.
 *
 * Large-file path: scan ed.tail_file from byte 0 to find the byte offset
 * of line n-1 (0-indexed), position the window to start just before that
 * offset, count newlines in the short window prefix to obtain the local
 * buffer line number, then call gb_reload_from (which flushes edits first).
 *
 * Returns 1 on success; 0 if n is past end of file (clamped to last line).
 * All locals static to survive BDOS calls under HI-TECH C -O.
 */
static long gg_target_off;
static long gg_win_off;
static int  gg_local_line;
static int  gg_buf_lines;

int gb_goto_line(n)
int n;
{
    if (n <= 0) n = 1;
    gg_buf_lines = scr_line_count();

    /* Fast path: buffer starts at file byte 0 and the target line is loaded. */
    if (ed.win_start == 0L && (!ed.tail_file[0] || n - 1 < gg_buf_lines)) {
        gg_local_line = n - 1;
        if (gg_local_line >= gg_buf_lines) gg_local_line = gg_buf_lines - 1;
        if (gg_local_line < 0) gg_local_line = 0;
        ed.cur_pos = scr_line_start(gg_local_line);
        return 1;
    }

    /* Scan the file to locate line n-1. */
    gg_target_off = gb_find_line_offset(n - 1);

    if (gg_target_off < 0L) {
        /* Line n doesn't exist -- jump to last line instead. */
        gg_win_off = hvi_fsize(ed.tail_file);
        if (gg_win_off > (long)LOAD_CHUNK)
            gg_win_off -= (long)LOAD_CHUNK;
        else
            gg_win_off = 0L;
        gb_reload_from(gg_win_off);
        ed.cur_pos = scr_last_line_start();
        return 0;
    }

    /* Position the window to start half a chunk before the target line.
     * Sector-align so BDOS 33 can seek directly to the sector. */
    gg_win_off = gg_target_off;
    if (gg_win_off > (long)(LOAD_CHUNK >> 1))
        gg_win_off -= (long)(LOAD_CHUNK >> 1);
    else
        gg_win_off = 0L;
    gg_win_off &= ~127L;

    /* Count newlines from gg_win_off to gg_target_off to find the local
     * buffer line number of the target.  Must be done before gb_reload_from
     * because gb_reload_from may update ed.tail_file. */
    gg_local_line = gb_count_lines(gg_win_off, gg_target_off);

    /* Reload (flushes any unsaved edits first via s_reload_skip_flush). */
    gb_reload_from(gg_win_off);

    if (gg_local_line >= scr_line_count()) gg_local_line = scr_line_count() - 1;
    if (gg_local_line < 0) gg_local_line = 0;
    ed.cur_pos = scr_line_start(gg_local_line);
    return 1;
}

/*
 * Find beginning of line logically containing pos.
 * Static locals survive gb_char_at calls under HI-TECH C -O register allocation.
 */
static int fbol_p;
int find_bol(pos)
int pos;
{
    fbol_p = pos;
    while (fbol_p > 0 && gb_char_at(fbol_p - 1) != '\n') fbol_p--;
    return fbol_p;
}

/*
 * Find end of line logically containing pos.
 * Static locals survive gb_char_at calls under HI-TECH C -O register allocation.
 */
static int feol_p, feol_size;
int find_eol(pos)
int pos;
{
    feol_p    = pos;
    feol_size = gb_content_len();
    while (feol_p < feol_size && gb_char_at(feol_p) != '\n') feol_p++;
    return feol_p;
}
