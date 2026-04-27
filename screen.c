/*
 * screen.c - Screen rendering for HVI
 * Author: Juan Orlandini
 * License: MIT
 *
 * Manages the text viewport and status line.
 * The text area occupies rows 0..(scr_rows-2).
 * Row (scr_rows-1) is the status/command line.
 *
 * Lines wider than the terminal wrap to the next screen row.  The unit
 * of vertical measurement throughout this file is the "visual row": one
 * terminal line's worth of content (scr_cols display columns, or fewer
 * when a newline ends the logical line sooner).  top_pos may point to
 * the middle of a long logical line (the start of any visual row).
 *
 * Performance notes (9600 baud, 4 MHz Z80):
 *   scr_update_after_move() uses term_scroll_up/dn() to repaint just
 *   one new line (~53 bytes) instead of a full screen (~1200 bytes)
 *   when the viewport shifts by exactly 1 visual row.
 *
 *   scr_redraw_from_cur() redraws only the rows from the cursor to the
 *   bottom of the text area.  Used by editing commands (J, o, O, d, c,
 *   p, u, Enter) where content above the cursor is unchanged.  Saves
 *   all rows above the cursor vs a full scr_refresh().
 *
 *   scr_cur_line() maintains an incremental cache of the current line
 *   number so that the status bar update and movement routines avoid
 *   the O(buffer) scan of scr_pos_line(cur_pos) on every keystroke.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "hvi.h"

extern Editor ed;

/* ------------------------------------------------------------------ */
/*  Visual-row helpers                                                  */
/* ------------------------------------------------------------------ */

static int nvr_col, nvr_c, nvr_nc, nvr_size; /* next_vrow statics */
static int nvs_p, nvs_next;                  /* vrow_start_of statics */

int next_vrow(from)
int from;
{
    nvr_size = gb_content_len();
    nvr_col  = 0;
    while (from < nvr_size) {
        nvr_c  = gb_char_at(from);
        if (nvr_c == '\n') return from + 1;
        nvr_nc = (nvr_c == '\t') ? (nvr_col / TAB_STOP + 1) * TAB_STOP : nvr_col + 1;
        if (nvr_nc > ed.scr_cols) return from;
        nvr_col = nvr_nc;
        from++;
    }
    return from;
}

int vrow_start_of(pos)
int pos;
{
    nvs_p = pos;
    while (nvs_p > 0 && gb_char_at(nvs_p - 1) != '\n')
        nvs_p--;
    for (;;) {
        nvs_next = next_vrow(nvs_p);
        if (nvs_next > pos || nvs_next <= nvs_p) break;
        nvs_p = nvs_next;
    }
    return nvs_p;
}

static int svc_p, svc_vstart, svc_col, svc_c, svc_nc, svc_size; /* scr_vrow_col */

int scr_vrow_col(pos)
int pos;
{
    svc_size   = gb_content_len();
    svc_vstart = vrow_start_of(pos);
    svc_col    = 0;
    svc_p      = svc_vstart;
    while (svc_p < pos && svc_p < svc_size) {
        svc_c  = gb_char_at(svc_p);
        svc_nc = (svc_c == '\t') ? (svc_col / TAB_STOP + 1) * TAB_STOP : svc_col + 1;
        svc_col = svc_nc;
        svc_p++;
    }
    return svc_col;
}

/* ------------------------------------------------------------------ */
/*  Logical-position helpers                                            */
/* ------------------------------------------------------------------ */

/*
 * Return the line number (0-based) of buffer position pos.
 * O(pos) scan — use scr_cur_line() for the current cursor position.
 */
static int spl_i, spl_line; /* scr_pos_line statics */

int scr_pos_line(pos)
int pos;
{
    spl_line = 0;
    for (spl_i = 0; spl_i < pos; spl_i++)
        if (gb_char_at(spl_i) == '\n')
            spl_line++;
    return spl_line;
}

/*
 * Cached, incremental line number for ed.cur_pos.
 *
 * On the first call after initialisation (cur_line_pos == -1) a full
 * scr_pos_line() scan is done once.  Subsequent calls update the cache
 * incrementally: for a forward move scan only [old_pos, new_pos); for a
 * backward move scan [new_pos, old_pos) and subtract.  For typical
 * single-line movement (j/k) this is O(line_length) instead of O(buffer).
 *
 * The cache is invalidated (cur_line_pos set to -1) by any operation
 * that changes cur_pos non-incrementally (G, gg, search, undo, put, …).
 * Those callers call scr_cur_line() once afterwards to rebuild the cache.
 */
static int scl_pos, scl_old_pos, scl_old_line, scl_i; /* scr_cur_line statics */

int scr_cur_line()
{
    scl_pos = ed.cur_pos;
    if (ed.cur_line_pos == scl_pos) return ed.cur_line;
    scl_old_pos  = ed.cur_line_pos;
    scl_old_line = ed.cur_line;
    if (scl_old_pos >= 0 && scl_old_line >= 0) {
        if (scl_pos > scl_old_pos) {
            for (scl_i = scl_old_pos; scl_i < scl_pos; scl_i++)
                if (gb_char_at(scl_i) == '\n') scl_old_line++;
        } else {
            for (scl_i = scl_pos; scl_i < scl_old_pos; scl_i++)
                if (gb_char_at(scl_i) == '\n') scl_old_line--;
            if (scl_old_line < 0) scl_old_line = 0;
        }
        ed.cur_line = scl_old_line;
    } else {
        ed.cur_line = scr_pos_line(scl_pos);
    }
    ed.cur_line_pos = scl_pos;
    return ed.cur_line;
}

/*
 * Return the display column (0-based) of pos within its logical line.
 */
static int spc_col, spc_i, spc_start, spc_c; /* scr_pos_col statics */

int scr_pos_col(pos)
int pos;
{
    spc_start = pos;
    while (spc_start > 0 && gb_char_at(spc_start - 1) != '\n')
        spc_start--;
    spc_col = 0;
    for (spc_i = spc_start; spc_i < pos; spc_i++) {
        spc_c = gb_char_at(spc_i);
        if (spc_c == '\t')
            spc_col = (spc_col / TAB_STOP + 1) * TAB_STOP;
        else
            spc_col++;
    }
    return spc_col;
}

static int sls_pos, sls_line, sls_size; /* scr_line_start statics */

int scr_line_start(linenum)
int linenum;
{
    sls_pos  = 0;
    sls_line = 0;
    sls_size = gb_content_len();
    while (sls_pos < sls_size && sls_line < linenum) {
        if (gb_char_at(sls_pos) == '\n')
            sls_line++;
        sls_pos++;
    }
    return sls_pos;
}

/*
 * Return total number of logical lines in the buffer.
 * Result is cached in ed.line_cnt_cached; invalidated to 0 by gb_insert()
 * and gb_delete() whenever the buffer content changes.
 */
static int slc_i, slc_size, slc_lines; /* scr_line_count statics */

int scr_line_count()
{
    if (ed.line_cnt_cached > 0) return ed.line_cnt_cached;
    slc_size = gb_content_len();
    if (slc_size == 0) { ed.line_cnt_cached = 1; return 1; }
    slc_lines = 0;
    for (slc_i = 0; slc_i < slc_size; slc_i++)
        if (gb_char_at(slc_i) == '\n')
            slc_lines++;
    if (gb_char_at(slc_size - 1) != '\n')
        slc_lines++;
    slc_lines = (slc_lines > 0) ? slc_lines : 1;
    ed.line_cnt_cached = slc_lines;
    return slc_lines;
}

/* ------------------------------------------------------------------ */
/*  Scroll / cursor placement                                           */
/* ------------------------------------------------------------------ */

/*
 * Ensure the cursor's visual row is within the viewport.
 * Adjusts top_pos (which may land mid-line for wrapped content).
 *
 * Uses ed.cur_vrow as a cache.  When cur_vrow >= 0 the cache holds the
 * cursor's visual row offset from top_pos and the full scan from top_pos
 * is skipped entirely — reducing per-keypress cost from
 * O(text_rows * line_len) to O(line_len) for j/k navigation.
 *
 * cur_vrow is maintained by mv_down()/mv_up().  All other code that
 * changes cur_pos non-incrementally (search, undo, put, content edits)
 * sets cur_vrow = -1 to force a full recompute here.
 */
void scr_scroll_to_cursor()
{
    int p, rows, advance, text_rows, next;

    text_rows = ed.scr_rows - 1;

    /* Cursor above viewport: reset top_pos to cursor's visual row start. */
    if (ed.cur_pos < ed.top_pos) {
        ed.top_pos  = vrow_start_of(ed.cur_pos);
        ed.cur_vrow = 0;
        return;
    }

    /* Fast path: cur_vrow valid and cursor already in the viewport. */
    if (ed.cur_vrow >= 0 && ed.cur_vrow < text_rows)
        return;

    /* Fast path 2: cur_vrow valid but scrolled down */
    if (ed.cur_vrow >= text_rows) {
        advance = ed.cur_vrow - text_rows + 1;
        p = ed.top_pos;
        while (advance-- > 0) {
            next = next_vrow(p);
            if (next <= p) break;
            p = next;
        }
        ed.top_pos = p;
        ed.cur_vrow = text_rows - 1;
        return;
    }

    /* cur_vrow == -1: count visual rows from top_pos to find the cursor. */
    p    = ed.top_pos;
    rows = 0;
    while (p < ed.cur_pos) {
        next = next_vrow(p);
        if (next <= p) break;
        if (next > ed.cur_pos) break;
        p = next;
        rows++;
    }

    if (rows >= text_rows) {
        advance = rows - text_rows + 1;
        p = ed.top_pos;
        while (advance-- > 0) {
            next = next_vrow(p);
            if (next <= p) break;
            p = next;
        }
        ed.top_pos = p;
        rows = text_rows - 1;
    }
    ed.cur_vrow = rows;
}

/*
 * Move the terminal cursor to match the editor cursor position.
 * Uses ed.cur_vrow when valid (set by scr_scroll_to_cursor or the redraw
 * functions below) to skip the O(text_rows) scan from top_pos.
 */
void scr_update_cursor()
{
    int p, vstart, scr_row, scr_col, col, c, nc, size, next;

    size   = gb_content_len();
    vstart = vrow_start_of(ed.cur_pos);

    if (ed.cur_vrow >= 0) {
        scr_row = ed.cur_vrow;
    } else {
        p       = ed.top_pos;
        scr_row = 0;
        while (p < vstart) {
            next = next_vrow(p);
            if (next <= p) break;
            p = next;
            scr_row++;
        }
    }

    col = 0;
    p   = vstart;
    while (p < ed.cur_pos && p < size) {
        c  = gb_char_at(p);
        nc = (c == '\t') ? (col / TAB_STOP + 1) * TAB_STOP : col + 1;
        col = nc;
        p++;
    }
    scr_col = col;

    if (scr_col >= ed.scr_cols)      scr_col = ed.scr_cols - 1;
    if (scr_row < 0)                 scr_row = 0;
    if (scr_row >= ed.scr_rows - 1) scr_row = ed.scr_rows - 2;

    term_goto(scr_row, scr_col);
}

/* ------------------------------------------------------------------ */
/*  Rendering                                                           */
/* ------------------------------------------------------------------ */

/*
 * Draw terminal row screen_row using buffer content starting at pos.
 * pos must already be the correct vrow start — no scanning from top_pos.
 * This is the inner drawing kernel; all multi-row loops use this directly
 * to avoid the O(N^2) rescan that arises when every row re-walks from
 * top_pos to find its start position.
 */
static int dra_col, dra_size, dra_c, dra_nc; /* draw_row_at statics */

static void draw_row_at(screen_row, pos)
int screen_row, pos;
{
    dra_size = gb_content_len();
    term_goto(screen_row, 0);
    term_clreol();
    if (pos >= dra_size) {
        if (screen_row > 0) term_putch('~');
        return;
    }
    dra_col = 0;
    while (pos < dra_size && dra_col < ed.scr_cols) {
        dra_c = gb_char_at(pos);
        if (dra_c == '\n') break;
        if (dra_c == '\t') {
            dra_nc = (dra_col / TAB_STOP + 1) * TAB_STOP;
            if (dra_nc > ed.scr_cols) break;
            while (dra_col < dra_nc) { term_putch(' '); dra_col++; }
        } else {
            term_putch(dra_c);
            dra_col++;
        }
        pos++;
    }
}

/*
 * Draw one terminal row by index, scanning from top_pos to find its start.
 * O(screen_row) buffer walk — only use for single-row redraws where the
 * starting position is not already known.  Multi-row callers use draw_row_at
 * directly and thread pos through the loop.
 */
void scr_redraw_line(screen_row)
int screen_row;
{
    int pos, r, next;
    pos = ed.top_pos;
    for (r = 0; r < screen_row; r++) {
        next = next_vrow(pos);
        if (next <= pos) break;
        pos = next;
    }
    draw_row_at(screen_row, pos);
}

/*
 * Full screen refresh: redraws all text rows and the status line.
 * Threads pos through the row loop (O(N) total, not O(N^2)).
 * Sequential rows use \r\n (2 bytes) instead of a full ESC[R;CH goto
 * (7-10 bytes) — term_goto() handles this automatically via cursor
 * tracking.
 */
void scr_refresh()
{
    int row, text_rows, pos, next;
    text_rows = ed.scr_rows - 1;
    scr_scroll_to_cursor();
    pos = ed.top_pos;
    for (row = 0; row < text_rows; row++) {
        draw_row_at(row, pos);
        next = next_vrow(pos);
        if (next > pos) pos = next;
    }
    scr_show_status(ed.status);
}

/*
 * Redraw only the visual rows that belong to the current logical line,
 * plus one extra row after the line ends (to clear any row freed by a
 * deletion that shortened the line).  Much cheaper than redrawing the
 * entire area below the cursor for single-line edits (x, X, D, ~, …).
 * Uses draw_row_at with a threaded pos to avoid rescanning from top_pos.
 */
void scr_redraw_cur_line()
{
    int p, vstart, scr_row, text_rows, row, next, size;

    vstart    = vrow_start_of(ed.cur_pos);
    text_rows = ed.scr_rows - 1;
    size      = gb_content_len();

    if (ed.cur_vrow >= 0) {
        p = vstart;
        scr_row = ed.cur_vrow;
        while (p < ed.cur_pos) {
            next = next_vrow(p);
            if (next <= p || next > ed.cur_pos) break;
            p = next;
            scr_row--;
        }
    } else {
        p = ed.top_pos;
        scr_row = 0;
        while (p < vstart) {
            next = next_vrow(p);
            if (next <= p) break;
            p = next;
            scr_row++;
        }
    }

    p   = vstart;
    row = scr_row;
    for (;;) {
        if (row >= text_rows) break;
        draw_row_at(row, p);
        row++;
        next = next_vrow(p);
        /* Stop at end of buffer or end of this logical line */
        if (next <= p || next >= size || gb_char_at(next - 1) == '\n') {
            if (row < text_rows) draw_row_at(row, next); /* 1 extra: clears freed row */
            break;
        }
        p = next;
    }

    ed.cur_vrow = scr_row;
    scr_update_cursor();
}

/*
 * Redraw all visual rows from the cursor's screen row to the bottom of
 * the text area.  Use this when content at or below the cursor changed
 * but content ABOVE the cursor is unmodified — saves every row above.
 * Caller is responsible for calling scr_scroll_to_cursor() first and
 * for calling scr_show_status() afterward.
 */
void scr_redraw_from_cur()
{
    int vstart, p, scr_row, next, text_rows, row;

    text_rows = ed.scr_rows - 1;
    vstart    = vrow_start_of(ed.cur_pos);

    if (ed.cur_vrow >= 0) {
        p = vstart;
        scr_row = ed.cur_vrow;
        while (p < ed.cur_pos) {
            next = next_vrow(p);
            if (next <= p || next > ed.cur_pos) break;
            p = next;
            scr_row--;
        }
        p = vstart;
    } else {
        p = ed.top_pos;
        scr_row = 0;
        while (p < vstart) {
            next = next_vrow(p);
            if (next <= p) break;
            p = next;
            scr_row++;
        }
    }

    /* p is now the vrow-start for scr_row; thread it through the loop. */
    for (row = scr_row; row < text_rows; row++) {
        draw_row_at(row, p);
        next = next_vrow(p);
        if (next > p) p = next;
    }

    ed.cur_vrow = scr_row;
    scr_update_cursor();
}

/* ------------------------------------------------------------------ */
/*  Smart scroll after cursor movement                                  */
/* ------------------------------------------------------------------ */

/*
 * After a movement command that may have changed top_pos, decide the
 * cheapest way to update the display.
 *
 *   No scroll    — top_pos unchanged: only reposition the terminal cursor.
 *   ±1 vrow      — use terminal scroll (term_scroll_up/dn) + redraw one
 *                  new line.  ~53 bytes vs ~1200 for a full refresh.
 *   Larger jump  — fall back to full scr_refresh().
 *
 * Pass the value of top_pos that was saved BEFORE calling
 * scr_scroll_to_cursor().
 */
void scr_update_after_move(old_top)
int old_top;
{
    static int uam_tr, uam_p, uam_new_top, uam_delta, uam_nx;

    uam_tr      = ed.scr_rows - 1;
    uam_new_top = ed.top_pos;

    if (uam_new_top == old_top) {
        scr_update_cursor();
        return;
    }

    uam_delta = 0;
    if (uam_new_top > old_top) {
        uam_p = old_top;
        while (uam_p < uam_new_top && uam_delta < 2) {
            uam_nx = next_vrow(uam_p);
            if (uam_nx <= uam_p) break;
            uam_p = uam_nx;
            uam_delta++;
        }
        if (uam_delta == 1 && uam_p == uam_new_top) {
            term_scroll_up();
            draw_row_at(uam_tr - 1, vrow_start_of(ed.cur_pos));
            scr_update_cursor();
            return;
        }
    } else {
        uam_p = uam_new_top;
        while (uam_p < old_top && uam_delta < 2) {
            uam_nx = next_vrow(uam_p);
            if (uam_nx <= uam_p) break;
            uam_p = uam_nx;
            uam_delta++;
        }
        if (uam_delta == 1 && uam_p == old_top) {
            term_scroll_dn();
            scr_redraw_line(0);
            scr_update_cursor();
            return;
        }
    }

    scr_refresh();
}

/* ------------------------------------------------------------------ */
/*  Status line                                                         */
/* ------------------------------------------------------------------ */

/*
 * Display msg in the status line (row scr_rows-1).
 * If msg is empty or NULL, show the default:
 *   "filename" [+] L<cur>/<total>
 * When the file is larger than the buffer (tail_offset > 0), the total
 * is the count of lines currently in memory followed by '+' to indicate
 * that more content exists beyond what has been loaded.
 * Uses scr_cur_line() (O(1) when cache is warm) for the line number.
 */
void scr_show_status(msg)
char *msg;
{
    static char lineno[48];
    static int  cur, total;

    term_goto(ed.scr_rows - 1, 0);
    term_clreol();

    if (msg && *msg) {
        term_reverse();
        term_puts(msg);
        term_normal();
    } else {
        cur   = scr_cur_line() + 1;
        total = scr_line_count();
        sprintf(lineno, "\"%s\"%s L%d/%d%s",
            ed.filename[0] ? ed.filename : "[No Name]",
            ed.modified ? " [+]" : "",
            cur, total,
            ed.tail_offset > 0L ? "+" : "");
        term_reverse();
        term_puts(lineno);
        term_normal();
    }
    scr_update_cursor();
}

/* Clear the status line and return cursor to edit area. */
void scr_clear_status()
{
    ed.status[0] = '\0';
    term_goto(ed.scr_rows - 1, 0);
    term_clreol();
    scr_update_cursor();
}

/* ------------------------------------------------------------------ */
/*  Shared edit-refresh helpers                                         */
/* ------------------------------------------------------------------ */

static int scr_adj_t; /* avoids a local variable and its IX-frame overhead */

/*
 * After an edit: scroll viewport if needed, then redraw from the cursor
 * down (if viewport unchanged) or do a full refresh (if it shifted).
 * Does NOT update the status line — caller chooses what to show there.
 */
void scr_adj()
{
    scr_adj_t = ed.top_pos;
    scr_scroll_to_cursor();
    if (ed.top_pos == scr_adj_t) scr_redraw_from_cur();
    else scr_refresh();
}

/* Like scr_adj(), then also refresh the status line from ed.status. */
void scr_after_edit()
{
    scr_adj();
    scr_show_status(ed.status);
}
