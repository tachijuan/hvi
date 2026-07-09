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
 *
 * No standard library headers are included; hvi_sprintf from util.c
 * is used for status-bar formatting.
 */

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
        nvr_nc = (nvr_c == '\t') ? (nvr_col | (TAB_STOP - 1)) + 1 : nvr_col + 1;
        if (nvr_nc > ed.scr_cols) return from;
        nvr_col = nvr_nc;
        from++;
    }
    return from;
}

int vrow_start_of(pos)
int pos;
{
    nvs_p = find_bol(pos);
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
        svc_nc = (svc_c == '\t') ? (svc_col | (TAB_STOP - 1)) + 1 : svc_col + 1;
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
 * O(pos), but via the CPIR scanner (gb_count_nl) rather than one
 * gb_char_at call per byte -- use scr_cur_line() for the cursor.
 */
int scr_pos_line(pos)
int pos;
{
    return gb_count_nl(0, pos);
}

/*
 * Cached, incremental line number for ed.cur_pos.
 */
static int scl_pos, scl_old_pos, scl_old_line; /* scr_cur_line statics */

int scr_cur_line()
{
    scl_pos = ed.cur_pos;
    if (ed.cur_line_pos == scl_pos) return ed.cur_line;
    scl_old_pos  = ed.cur_line_pos;
    scl_old_line = ed.cur_line;
    if (scl_old_pos >= 0 && scl_old_line >= 0) {
        if (scl_pos > scl_old_pos) {
            scl_old_line += gb_count_nl(scl_old_pos, scl_pos - scl_old_pos);
        } else {
            scl_old_line -= gb_count_nl(scl_pos, scl_old_pos - scl_pos);
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
    spc_start = find_bol(pos);
    spc_col = 0;
    for (spc_i = spc_start; spc_i < pos; spc_i++) {
        spc_c = gb_char_at(spc_i);
        if (spc_c == '\t')
            spc_col = (spc_col | (TAB_STOP - 1)) + 1;
        else
            spc_col++;
    }
    return spc_col;
}

/* Buffer position of the first character of the last line. */
int scr_last_line_start()
{
    return scr_line_start(scr_line_count() - 1);
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
 * Result is cached in ed.line_cnt_cached.
 */
static int slc_size, slc_lines; /* scr_line_count statics */

int scr_line_count()
{
    if (ed.line_cnt_cached > 0) return ed.line_cnt_cached;
    slc_size = gb_content_len();
    if (slc_size == 0) { ed.line_cnt_cached = 1; return 1; }
    slc_lines = gb_count_nl(0, slc_size);
    if (gb_char_at(slc_size - 1) != '\n')
        slc_lines++;
    slc_lines = (slc_lines > 0) ? slc_lines : 1;
    ed.line_cnt_cached = slc_lines;
    return slc_lines;
}

/* ------------------------------------------------------------------ */
/*  Scroll / cursor placement                                           */
/* ------------------------------------------------------------------ */

/* statics for scr_scroll_to_cursor -- draw_row_at may trigger term_flush
 * which calls bdos(2,...); disk bdos calls may not preserve IX on CP/M. */
static int stc_p, stc_rows, stc_advance, stc_text_rows, stc_next;

void scr_scroll_to_cursor()
{
    stc_text_rows = ed.scr_rows - 1;

    /* Cursor above viewport: reset top_pos to cursor's visual row start. */
    if (ed.cur_pos < ed.top_pos) {
        ed.top_pos  = vrow_start_of(ed.cur_pos);
        ed.cur_vrow = 0;
        return;
    }

    /* Fast path: cur_vrow valid and cursor already in the viewport. */
    if (ed.cur_vrow >= 0 && ed.cur_vrow < stc_text_rows)
        return;

    /* Fast path 2: cur_vrow valid but scrolled down */
    if (ed.cur_vrow >= stc_text_rows) {
        stc_advance = ed.cur_vrow - stc_text_rows + 1;
        stc_p = ed.top_pos;
        while (stc_advance-- > 0) {
            stc_next = next_vrow(stc_p);
            if (stc_next <= stc_p) break;
            stc_p = stc_next;
        }
        ed.top_pos = stc_p;
        ed.cur_vrow = stc_text_rows - 1;
        return;
    }

    /* cur_vrow == -1: count visual rows from top_pos to find the cursor. */
    stc_p    = ed.top_pos;
    stc_rows = 0;
    while (stc_p < ed.cur_pos) {
        stc_next = next_vrow(stc_p);
        if (stc_next <= stc_p) break;
        if (stc_next > ed.cur_pos) break;
        stc_p = stc_next;
        stc_rows++;
    }

    if (stc_rows >= stc_text_rows) {
        stc_advance = stc_rows - stc_text_rows + 1;
        stc_p = ed.top_pos;
        while (stc_advance-- > 0) {
            stc_next = next_vrow(stc_p);
            if (stc_next <= stc_p) break;
            stc_p = stc_next;
        }
        ed.top_pos = stc_p;
        stc_rows = stc_text_rows - 1;
    }
    ed.cur_vrow = stc_rows;
}

static int suc_p, suc_vstart, suc_scr_row, suc_scr_col, suc_col, suc_c, suc_nc, suc_size, suc_next;

void scr_update_cursor()
{
    suc_size   = gb_content_len();
    suc_vstart = vrow_start_of(ed.cur_pos);

    if (ed.cur_vrow >= 0) {
        suc_scr_row = ed.cur_vrow;
    } else {
        suc_p       = ed.top_pos;
        suc_scr_row = 0;
        while (suc_p < suc_vstart) {
            suc_next = next_vrow(suc_p);
            if (suc_next <= suc_p) break;
            suc_p = suc_next;
            suc_scr_row++;
        }
    }

    suc_col = 0;
    suc_p   = suc_vstart;
    while (suc_p < ed.cur_pos && suc_p < suc_size) {
        suc_c  = gb_char_at(suc_p);
        suc_nc = (suc_c == '\t') ? (suc_col | (TAB_STOP - 1)) + 1 : suc_col + 1;
        suc_col = suc_nc;
        suc_p++;
    }
    suc_scr_col = suc_col;

    if (suc_scr_col >= ed.scr_cols)      suc_scr_col = ed.scr_cols - 1;
    if (suc_scr_row < 0)                 suc_scr_row = 0;
    if (suc_scr_row >= ed.scr_rows - 1) suc_scr_row = ed.scr_rows - 2;

    term_goto(suc_scr_row, suc_scr_col);
}

/* ------------------------------------------------------------------ */
/*  Rendering                                                           */
/* ------------------------------------------------------------------ */

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
            dra_nc = (dra_col | (TAB_STOP - 1)) + 1;
            while (dra_col < dra_nc) { term_putch(' '); dra_col++; }
        } else {
            term_putch(dra_c);
            dra_col++;
        }
        pos++;
    }
}

static int rl_pos, rl_r, rl_next;

void scr_redraw_line(screen_row)
int screen_row;
{
    rl_pos = ed.top_pos;
    for (rl_r = 0; rl_r < screen_row; rl_r++) {
        rl_next = next_vrow(rl_pos);
        if (rl_next <= rl_pos) break;
        rl_pos = rl_next;
    }
    draw_row_at(screen_row, rl_pos);
}

static int sr_row, sr_text_rows, sr_pos, sr_next;

void scr_refresh()
{
    sr_text_rows = ed.scr_rows - 1;
    scr_scroll_to_cursor();
    sr_pos = ed.top_pos;
    for (sr_row = 0; sr_row < sr_text_rows; sr_row++) {
        draw_row_at(sr_row, sr_pos);
        sr_next = next_vrow(sr_pos);
        if (sr_next > sr_pos) sr_pos = sr_next;
    }
    scr_show_status(ed.status);
}

/*
 * Locate the screen row of the cursor's visual row and its buffer start.
 * Sets lcr_vstart / lcr_row.  Shared by scr_redraw_cur_line() and
 * scr_redraw_from_cur().
 */
static int lcr_vstart, lcr_row, lcr_p, lcr_next;

static void locate_cur_row()
{
    lcr_vstart = vrow_start_of(ed.cur_pos);

    if (ed.cur_vrow >= 0) {
        lcr_p   = lcr_vstart;
        lcr_row = ed.cur_vrow;
        while (lcr_p < ed.cur_pos) {
            lcr_next = next_vrow(lcr_p);
            if (lcr_next <= lcr_p || lcr_next > ed.cur_pos) break;
            lcr_p = lcr_next;
            lcr_row--;
        }
    } else {
        lcr_p   = ed.top_pos;
        lcr_row = 0;
        while (lcr_p < lcr_vstart) {
            lcr_next = next_vrow(lcr_p);
            if (lcr_next <= lcr_p) break;
            lcr_p = lcr_next;
            lcr_row++;
        }
    }
}

static int rcl_p, rcl_scr_row, rcl_text_rows, rcl_row, rcl_next, rcl_size;

void scr_redraw_cur_line()
{
    rcl_text_rows = ed.scr_rows - 1;
    rcl_size      = gb_content_len();

    locate_cur_row();
    rcl_scr_row = lcr_row;
    rcl_p   = lcr_vstart;
    rcl_row = rcl_scr_row;
    for (;;) {
        if (rcl_row >= rcl_text_rows) break;
        draw_row_at(rcl_row, rcl_p);
        rcl_row++;
        rcl_next = next_vrow(rcl_p);
        /* Stop at end of buffer or end of this logical line */
        if (rcl_next <= rcl_p || rcl_next >= rcl_size || gb_char_at(rcl_next - 1) == '\n') {
            if (rcl_row < rcl_text_rows) draw_row_at(rcl_row, rcl_next);
            break;
        }
        rcl_p = rcl_next;
    }

    ed.cur_vrow = rcl_scr_row;
    scr_update_cursor();
}

static int rfc_p, rfc_scr_row, rfc_next, rfc_text_rows, rfc_row;

void scr_redraw_from_cur()
{
    rfc_text_rows = ed.scr_rows - 1;

    locate_cur_row();
    rfc_scr_row = lcr_row;
    rfc_p       = lcr_vstart;

    /* rfc_p is now the vrow-start for rfc_scr_row; thread it through the loop. */
    for (rfc_row = rfc_scr_row; rfc_row < rfc_text_rows; rfc_row++) {
        draw_row_at(rfc_row, rfc_p);
        rfc_next = next_vrow(rfc_p);
        if (rfc_next > rfc_p) rfc_p = rfc_next;
    }

    ed.cur_vrow = rfc_scr_row;
    scr_update_cursor();
}

/* ------------------------------------------------------------------ */
/*  Single-line edit refresh                                            */
/* ------------------------------------------------------------------ */

/*
 * Non-zero when the logical line containing pos occupies one visual row.
 */
static int sl1_bol;

int scr_line_is_1row(pos)
int pos;
{
    sl1_bol = find_bol(pos);
    return next_vrow(sl1_bol) >= find_eol(pos);
}

/*
 * Refresh after an edit.  When light is non-zero the edit added or
 * removed no newlines and its line occupied a single visual row BEFORE
 * the edit (callers verify both): repaint exactly one screen row plus
 * the status bar instead of the cursor-to-bottom redraw.  Falls back
 * to the full scr_after_edit() when light is zero, the line now wraps
 * (an insert grew it), or the cursor is outside the viewport.
 */
void scr_edit_end(light)
int light;
{
    if (light && ed.cur_pos >= ed.top_pos && scr_line_is_1row(ed.cur_pos)) {
        locate_cur_row();
        if (lcr_row < ed.scr_rows - 1) {
            draw_row_at(lcr_row, lcr_vstart);
            ed.cur_vrow = lcr_row;
            scr_show_status(ed.status);
            return;
        }
    }
    scr_after_edit();
}

/*
 * Repaint after the character under the cursor was replaced (r).
 * When both old and new characters are one-column printables and the
 * cell is not in the last screen column, emit the new character in
 * place (2-4 bytes); otherwise redraw the logical line.
 */
static int sfc_col;

void scr_fix_char(oldc, newc)
int oldc, newc;
{
    if (oldc >= 0x20 && oldc != 0x7F && newc >= 0x20 && newc < 0x7F) {
        sfc_col = scr_vrow_col(ed.cur_pos);
        if (sfc_col < ed.scr_cols - 1) {
            scr_update_cursor();
            term_putch(newc);
            scr_update_cursor();
            return;
        }
    }
    scr_redraw_cur_line();
}

/*
 * Repaint after characters in [from, to) were replaced in place by
 * same-width characters (the ~ command).  Emits the span when it holds
 * no tabs and stays left of the last column of one screen row (span+2
 * bytes vs a whole-line redraw); otherwise redraws the logical line.
 */
static int sfs_i, sfs_save, sfs_ok;

void scr_fix_span(from, to)
int from, to;
{
    sfs_ok = 1;
    for (sfs_i = from; sfs_i < to; sfs_i++)
        if (gb_char_at(sfs_i) == '\t') { sfs_ok = 0; break; }
    if (sfs_ok && scr_vrow_col(from) + (to - from) <= ed.scr_cols - 1) {
        sfs_save = ed.cur_pos;
        ed.cur_pos = from;
        scr_update_cursor();
        for (sfs_i = from; sfs_i < to; sfs_i++)
            term_putch(gb_char_at(sfs_i));
        ed.cur_pos = sfs_save;
        scr_update_cursor();
    } else {
        ed.cur_vrow = -1;   /* span may have crossed a row boundary */
        scr_redraw_cur_line();
    }
}

/* ------------------------------------------------------------------ */
/*  Smart scroll after cursor movement                                  */
/* ------------------------------------------------------------------ */

/*
 * The viewport moved by uam_delta visual rows with (text_rows - delta)
 * rows still on screen: scroll the region and repaint only the rows
 * that came into view.  Used by every movement command, including
 * Ctrl-D/Ctrl-U half-page scrolls (delta ~11 rows: roughly half the
 * output of a full refresh; the status bar is left untouched).
 */
void scr_update_after_move(old_top)
int old_top;
{
    static int uam_tr, uam_p, uam_new_top, uam_delta, uam_nx, uam_i;
    static int uam_down, uam_from, uam_to;

    uam_tr      = ed.scr_rows - 1;
    uam_new_top = ed.top_pos;

    if (uam_new_top == old_top) {
        scr_update_cursor();
        return;
    }

    /* Count vrows between the nearer and farther of the two tops. */
    uam_down = (uam_new_top > old_top);
    uam_p    = uam_down ? old_top : uam_new_top;
    uam_to   = uam_down ? uam_new_top : old_top;
    uam_delta = 0;
    while (uam_p < uam_to && uam_delta < uam_tr) {
        uam_nx = next_vrow(uam_p);
        if (uam_nx <= uam_p) break;
        uam_p = uam_nx;
        uam_delta++;
    }
    if (uam_p != uam_to || uam_delta >= uam_tr) {
        scr_refresh();
        return;
    }

    /* Scroll the region, then repaint only the rows that came into
     * view: the bottom uam_delta rows going down, the top ones going up. */
    for (uam_i = 0; uam_i < uam_delta; uam_i++) {
        if (uam_down) term_scroll_up();
        else          term_scroll_dn();
    }
    uam_from = uam_down ? uam_tr - uam_delta : 0;
    uam_to   = uam_down ? uam_tr : uam_delta;
    uam_p    = uam_new_top;
    for (uam_i = 0; uam_i < uam_from; uam_i++) {
        uam_nx = next_vrow(uam_p);
        if (uam_nx <= uam_p) break;
        uam_p = uam_nx;
    }
    for (uam_i = uam_from; uam_i < uam_to; uam_i++) {
        draw_row_at(uam_i, uam_p);
        uam_nx = next_vrow(uam_p);
        if (uam_nx > uam_p) uam_p = uam_nx;
    }
    scr_update_cursor();
}

/* ------------------------------------------------------------------ */
/*  Status line                                                         */
/* ------------------------------------------------------------------ */

/*
 * Cache of the text currently on the status row.  scr_show_status()
 * skips the repaint (goto + clreol + attributes + text, 30-50 bytes)
 * when it would rewrite the identical string -- which is the common
 * case for the filename bar on every edit and the -- INSERT --
 * indicator on every insert-mode keypress.  Anything else that writes
 * to the status row (command line, search prompt, [Loading...]) must
 * call scr_status_invalidate().
 */
static char sss_last[STATUS_MAX];

void scr_status_invalidate()
{
    sss_last[0] = '\0';
}

static char *sss_p;

void scr_show_status(msg)
char *msg;
{
    static char lineno[48];

    if (msg && *msg) {
        sss_p = msg;
    } else {
        hvi_sprintf(lineno, "\"%s\"%s",
            (int)(ed.filename[0] ? ed.filename : "[No Name]"),
            (int)(ed.modified ? " [+]" : ""),
            0, 0, 0);
        sss_p = lineno;
    }

    if (hvi_strcmp(sss_p, sss_last) != 0) {
        hvi_strncpy(sss_last, sss_p, STATUS_MAX - 1);
        sss_last[STATUS_MAX - 1] = '\0';
        term_goto(ed.scr_rows - 1, 0);
        term_clreol();
        term_reverse();
        term_puts(sss_p);
        term_normal();
    }
    scr_update_cursor();
}

/* Clear the status line and return cursor to edit area. */
void scr_clear_status()
{
    ed.status[0] = '\0';
    sss_last[0] = '\0';
    term_goto(ed.scr_rows - 1, 0);
    term_clreol();
    scr_update_cursor();
}

/* ------------------------------------------------------------------ */
/*  Shared edit-refresh helpers                                         */
/* ------------------------------------------------------------------ */

static int scr_adj_t;

void scr_adj()
{
    scr_adj_t = ed.top_pos;
    scr_scroll_to_cursor();
    if (ed.top_pos == scr_adj_t) scr_redraw_from_cur();
    else scr_refresh();
}

void scr_after_edit()
{
    scr_adj();
    scr_show_status(ed.status);
}
