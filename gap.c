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

/* CPIR/CPDR byte scanners over a raw memory range; assembly in
 * cstart.as.  Return the first/last index of c in p[0..len), or -1.
 * They back find_eol/find_bol below, which every line motion uses. */
extern int gb_memchr(/* char *p, int len, int c */);
extern int gb_memrchr(/* char *p, int len, int c */);

/* Temp-file names: one copy of each literal, passed around directly. */
static char swp_name[] = "HVISWP.TMP";
static char tmp_name[] = "HVITMP.TMP";

/* ------------------------------------------------------------------ */
/*  Marks (m / ` commands)                                              */
/* ------------------------------------------------------------------ */

/* Invalidate all marks: buffer positions no longer describe the same
 * text (fresh load, or the large-file window moved). */
static int mkc_i;

static void mk_clear()
{
    for (mkc_i = 0; mkc_i < NMARKS; mkc_i++)
        ed.marks[mkc_i] = -1;
}

/*
 * Keep marks pointing at the same characters across an edit at pos.
 * del=0: len bytes inserted at pos; del=1: len bytes deleted at pos.
 * A mark inside a deleted range is cleared (its text is gone) -- this
 * also drops marks that gb_discard_head slides out of the window.
 * The -1 sentinel is naturally skipped by the mark < pos test.
 */
static int mka_i, mka_m;

static void mk_adjust(pos, len, del)
int pos, len, del;
{
    for (mka_i = 0; mka_i < NMARKS; mka_i++) {
        mka_m = ed.marks[mka_i];
        if (mka_m < pos) continue;
        if (!del)
            ed.marks[mka_i] = mka_m + len;
        else if (mka_m >= pos + len)
            ed.marks[mka_i] = mka_m - len;
        else
            ed.marks[mka_i] = -1;
    }
}

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
    mk_clear();     /* ed is BSS-zeroed; 0 would be a valid mark */
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
 * gb_char_at(pos) -- the character at logical position pos, or -1 out
 * of range -- lives in cstart.as: it is the hottest function in the
 * editor (every scanner calls it once per character), so it is written
 * frameless in assembly reading the GapBuf fields directly from ed.
 */

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
static int gbi_nl_added;

int gb_insert(pos, text, len)
int   pos;
char *text;
int   len;
{
    if (len <= 0) return 1;
    if (ed.gb.gend - ed.gb.gstart < len)
        return 0;   /* buffer is pre-allocated; cannot grow */

    gbi_nl_added = gb_cntnl(text, len);

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

    mk_adjust(pos, len, 0);

    gb_move_gap(pos);
    gb_memmove(ed.gb.buf + ed.gb.gstart, text, len);
    ed.gb.gstart += len;
    return 1;
}

/*
 * Split the logical range [pos, pos+len) -- clamped to the content --
 * into its raw buffer segments: sp1/sl1 below the gap, sp2/sl2 above
 * it (lengths 0 when a side is empty).  One copy of the clamp/split
 * arithmetic shared by gb_copy_out and gb_count_nl.
 */
static char *sp1, *sp2;
static int   sl1, sl2, spl_cl;

static void gb_split(pos, len)
int pos, len;
{
    sl1 = 0;
    sl2 = 0;
    spl_cl = gb_content_len();
    if (pos < 0) { len += pos; pos = 0; }
    if (pos >= spl_cl || len <= 0) return;
    if (pos + len > spl_cl) len = spl_cl - pos;

    if (pos < ed.gb.gstart) {
        sl1 = ed.gb.gstart - pos;
        if (sl1 > len) sl1 = len;
        sp1 = ed.gb.buf + pos;
        pos += sl1;
        len -= sl1;
    }
    if (len > 0) {
        sp2 = ed.gb.buf + (ed.gb.gend - ed.gb.gstart) + pos;
        sl2 = len;
    }
}

/*
 * Copy len bytes of logical content starting at pos into dst.
 * Bulk replacement for per-character gb_char_at loops (yank, undo and
 * dot-text capture): at most two LDIR block copies, one per gap side.
 */
void gb_copy_out(dst, pos, len)
char *dst;
int   pos, len;
{
    gb_split(pos, len);
    if (sl1 > 0) { gb_memmove(dst, sp1, sl1); dst += sl1; }
    if (sl2 > 0) gb_memmove(dst, sp2, sl2);
}

/*
 * Count '\n' characters in the logical range [pos, pos+len) with the
 * CPIR scanner from cstart.as, one run per raw segment.
 */
static int cnl_n;

int gb_count_nl(pos, len)
int pos, len;
{
    gb_split(pos, len);
    cnl_n = (sl1 > 0) ? gb_cntnl(sp1, sl1) : 0;
    if (sl2 > 0) cnl_n += gb_cntnl(sp2, sl2);
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

    mk_adjust(pos, len, 1);

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
    term_status_row();
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
    mk_clear();
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
    mk_clear();          /* window moved: old positions are meaningless */
    /* tail_file unchanged -- same source file */
    return gb_fill(grf_f, (char *)0);
}

/*
 * Copy bytes [from, to) of ed.tail_file to f (stops early at EOF/^Z).
 * Serves both save-time copies: the head (bytes before the window,
 * [0, win_start)) and the unloaded tail ([tail_offset, end), passed
 * with to = 0x7FFFFFFF).
 * All locals static: hvi_fopen/hvi_fgetc/hvi_fputc are function calls.
 */
static HFILE *gcp_f;       /* cache param: BDOS trashes HL which may hold f */
static HFILE *gcp_tf;
static long   gcp_pos;
static long   gcp_to;
static int    gcp_c;

static void gb_copy_file(f, from, to)
HFILE *f;
long from, to;
{
    gcp_f  = f;            /* cache before any BDOS call */
    gcp_to = to;
    if (from >= gcp_to || !ed.tail_file[0]) return;
    gcp_tf = hvi_fopen(ed.tail_file, "rb");
    if (!gcp_tf) return;
    if (from > 0L && hvi_fseek(gcp_tf, from, 0) != 0) {
        hvi_fclose(gcp_tf);
        return;
    }
    gcp_pos = from;
    while (gcp_pos < gcp_to) {
        gcp_c = hvi_fgetc(gcp_tf);
        if (gcp_c == HEOF || gcp_c == 0x1A) break;
        hvi_fputc(gcp_c, gcp_f);
        gcp_pos++;
    }
    hvi_fclose(gcp_tf);
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
static char   gsv_nl[1] = { '\n' };   /* 1 data byte beats a store per call */
static char   gsv_tmp[PATH_MAX];      /* tmp_name with the dest's du: prefix */
static int    gsv_i, gsv_j;

/* Build the temp-file name on the same drive/user as gsv_fn: copy its
 * "du:" prefix (at most "B15:", 4 chars) if present, then tmp_name.
 * BDOS rename cannot cross a drive or user area, so the temp file must
 * be created where the destination will live. */
static void gsv_mk_tmp()
{
    gsv_j = 0;
    for (gsv_i = 0; gsv_i < 4 && gsv_fn[gsv_i]; gsv_i++) {
        if (gsv_fn[gsv_i] == ':') {
            while (gsv_j <= gsv_i) { gsv_tmp[gsv_j] = gsv_fn[gsv_j]; gsv_j++; }
            break;
        }
    }
    hvi_strcpy(gsv_tmp + gsv_j, tmp_name);
}

int gb_save(filename)
char *filename;
{
    gsv_fn = filename;     /* cache before any BDOS call */
    /* vi convention: a text file ends with a newline.  Append one to the
     * buffer when the buffer holds the end of the file and lacks it. */
    if (ed.tail_offset == 0L) {
        gsv_len = gb_content_len();
        if (gsv_len > 0 && gb_char_at(gsv_len - 1) != '\n')
            gb_insert(gsv_len, gsv_nl, 1);
    }
    gsv_using_tmp = 0;
    if (ed.tail_file[0] && (ed.win_start > 0L || ed.tail_offset > 0L) &&
        hvi_strcmp(gsv_fn, ed.tail_file) == 0) {
        gsv_mk_tmp();
        gsv_f = hvi_fopen(gsv_tmp, "wb");
        gsv_using_tmp = 1;
    } else {
        gsv_f = hvi_fopen(gsv_fn, "wb");
    }
    if (!gsv_f) return 0;

    gb_copy_file(gsv_f, 0L, ed.win_start);       /* head before window */
    gsv_new_tail = gb_write_buf(gsv_f);
    if (ed.tail_offset > 0L)                     /* unloaded tail      */
        gb_copy_file(gsv_f, ed.tail_offset, 0x7FFFFFFFL);
    hvi_fputc(0x1A, gsv_f);
    hvi_fclose(gsv_f);

    if (gsv_using_tmp) {
        hvi_remove(gsv_fn);
        hvi_rename(gsv_tmp, gsv_fn);
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
 * Shared window-jump helpers: one copy of the 32-bit clamp arithmetic
 * (each inlined copy costs ~50 bytes of long-library calls).
 */
static long gll_off;

/* Jump the window to the last LOAD_CHUNK of the tail file and park the
 * cursor on the last line's start (G, and gb_goto_line past-EOF). */
void gb_load_last()
{
    gll_off = hvi_fsize(ed.tail_file);
    if (gll_off > (long)LOAD_CHUNK)
        gll_off -= (long)LOAD_CHUNK;
    else
        gll_off = 0L;
    gb_reload_from(gll_off);
    ed.cur_pos = scr_last_line_start();
}

/* Slide the window back one LOAD_CHUNK from win_start (^B, k at top). */
void gb_load_prev()
{
    gll_off = ed.win_start - (long)LOAD_CHUNK;
    if (gll_off < 0L) gll_off = 0L;
    gb_reload_from(gll_off);
}

/*
 * Scan ed.tail_file, tracking LF (0x0A) characters in [from, to).
 * stop_n >= 0: return the byte offset just past the stop_n-th LF (the
 *              first byte of 0-indexed line stop_n), or -1L when the
 *              file ends first (fewer than stop_n lines).
 * stop_n < 0:  return the LF count seen before reaching to (or EOF);
 *              -1L on open/seek error (callers clamp negatives to 0).
 * One scanner serves both the line-offset lookup and the range count
 * that gb_goto_line needs.
 * All locals static to survive BDOS calls under HI-TECH C -O.
 */
static HFILE *gsl_f;
static long   gsl_off;
static long   gsl_from;    /* cache params: BDOS trashes HL:DE */
static long   gsl_to;
static int    gsl_n;
static int    gsl_cnt;
static int    gsl_c;

static long gb_scan_lines(from, to, stop_n)
long from, to;
int  stop_n;
{
    gsl_from = from;       /* cache before any BDOS call */
    gsl_to   = to;
    gsl_n    = stop_n;
    if (!ed.tail_file[0]) return -1L;
    gsl_f = hvi_fopen(ed.tail_file, "rb");
    if (!gsl_f) return -1L;
    if (gsl_from > 0L && hvi_fseek(gsl_f, gsl_from, 0) != 0) {
        hvi_fclose(gsl_f);
        return -1L;
    }
    gsl_cnt = 0;
    gsl_off = gsl_from;
    while (gsl_off < gsl_to && gsl_cnt != gsl_n) {
        gsl_c = hvi_fgetc(gsl_f);
        if (gsl_c == HEOF || gsl_c == 0x1A) {
            hvi_fclose(gsl_f);
            return (gsl_n >= 0) ? -1L : (long)gsl_cnt;
        }
        gsl_off++;
        if (gsl_c == '\n') gsl_cnt++;
    }
    hvi_fclose(gsl_f);
    return (gsl_n >= 0) ? gsl_off : (long)gsl_cnt;
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
    gg_target_off = gb_scan_lines(0L, 0x7FFFFFFFL, n - 1);

    if (gg_target_off < 0L) {
        /* Line n doesn't exist -- jump to last line instead. */
        gb_load_last();
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
     * buffer line number of the target (clamped below: a scan error
     * returns -1).  Must be done before gb_reload_from because
     * gb_reload_from may update ed.tail_file. */
    gg_local_line = (int)gb_scan_lines(gg_win_off, gg_target_off, -1);

    /* Reload (flushes any unsaved edits first via s_reload_skip_flush). */
    gb_reload_from(gg_win_off);

    if (gg_local_line >= scr_line_count()) gg_local_line = scr_line_count() - 1;
    if (gg_local_line < 0) gg_local_line = 0;
    ed.cur_pos = scr_line_start(gg_local_line);
    return 1;
}

/*
 * Find beginning of line logically containing pos: one past the last
 * '\n' before pos, or 0.  Backward CPDR scan (gb_memrchr) over the raw
 * gap-buffer segments -- at most one per gap side -- instead of one
 * gb_char_at call per character.
 */
static int fbol_r;
int find_bol(pos)
int pos;
{
    if (pos <= 0) return pos;
    if (pos > ed.gb.gstart) {
        /* logical [gstart, pos) lives at raw buf + gend */
        fbol_r = gb_memrchr(ed.gb.buf + ed.gb.gend, pos - ed.gb.gstart, '\n');
        if (fbol_r >= 0) return ed.gb.gstart + fbol_r + 1;
        pos = ed.gb.gstart;
    }
    return gb_memrchr(ed.gb.buf, pos, '\n') + 1;   /* -1 (absent) -> 0 */
}

/*
 * Find end of line logically containing pos: the '\n' at or after pos,
 * or the content length.  Forward CPIR scan (gb_memchr), split at the
 * gap like find_bol.
 */
static int feol_r, feol_size;
int find_eol(pos)
int pos;
{
    feol_size = gb_content_len();
    if (pos >= feol_size) return pos;
    if (pos < 0) pos = 0;
    if (pos < ed.gb.gstart) {
        feol_r = gb_memchr(ed.gb.buf + pos, ed.gb.gstart - pos, '\n');
        if (feol_r >= 0) return pos + feol_r;
        pos = ed.gb.gstart;
    }
    feol_r = gb_memchr(ed.gb.buf + (ed.gb.gend - ed.gb.gstart) + pos,
                       feol_size - pos, '\n');
    return (feol_r >= 0) ? pos + feol_r : feol_size;
}
