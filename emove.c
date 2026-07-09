/*
 * emove.c - Movement, operator application, and search helpers for HVI
 * Author: Juan Orlandini
 * License: MIT
 *
 * The modulo wrapping in do_search_from() is replaced with a single
 * conditional subtraction per iteration, avoiding the Z80 division
 * library routines.  No standard library headers are included.
 */

#include "hvi.h"

extern Editor ed;

int motion_endpoint();   /* fwd decl: used by the word motions below */

/* ------------------------------------------------------------------ */
/*  Character classification                                            */
/* ------------------------------------------------------------------ */

int iswordch(c)
int c;
{
    return (c >= 'a' && c <= 'z') ||
           (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') ||
           c == '_';
}

int isspacech(c)
int c;
{
    return c == ' ' || c == '\t' || c == '\n';
}

/* ------------------------------------------------------------------ */
/*  Undo helpers                                                        */
/* ------------------------------------------------------------------ */

void undo_save_delete(pos, len)
int pos, len;
{
    static int save;
    save = (len > UNDO_MAX) ? UNDO_MAX : len;
    ed.undo.type      = UNDO_DELETE;
    ed.undo.pos       = pos;
    ed.undo.len       = len;
    ed.undo.was_clean = !ed.modified;
    gb_copy_out(ed.undo.text, pos, save);
    if (save < UNDO_MAX)
        ed.undo.text[save] = '\0';
}

void undo_save_insert(pos, len)
int pos, len;
{
    ed.undo.type      = UNDO_INSERT;
    ed.undo.pos       = pos;
    ed.undo.len       = len;
    ed.undo.was_clean = !ed.modified;
    ed.undo.text[0]   = '\0';
}

/* ------------------------------------------------------------------ */
/*  Movement helpers (do NOT update want_col unless it makes sense)    */
/* ------------------------------------------------------------------ */

static int h_vstart;
static void begin_hmove()
{
    h_vstart = (ed.cur_vrow >= 0) ? vrow_start_of(ed.cur_pos) : -1;
}

static void end_hmove()
{
    if (h_vstart >= 0 && vrow_start_of(ed.cur_pos) != h_vstart)
        ed.cur_vrow = -1;
}

/* Place cursor at column wantcol on line starting at lstart. */
int pos_at_col(lstart, wantcol)
int lstart, wantcol;
{
    static int pos, col, size, c, nc;
    pos  = lstart;
    col  = 0;
    size = gb_content_len();
    while (pos < size && gb_char_at(pos) != '\n') {
        c  = gb_char_at(pos);
        nc = (c == '\t') ? (col | (TAB_STOP - 1)) + 1 : col + 1;
        if (nc > wantcol) break;
        col = nc;
        pos++;
    }
    if (pos > lstart && (pos >= size || gb_char_at(pos) == '\n'))
        pos--;
    return pos;
}

void mv_bol()
{
    begin_hmove();
    ed.cur_pos = find_bol(ed.cur_pos);
    end_hmove();
}

void mv_bnb()
{
    static int size, c;
    size = gb_content_len();
    begin_hmove();
    ed.cur_pos = find_bol(ed.cur_pos);
    while (ed.cur_pos < size) {
        c = gb_char_at(ed.cur_pos);
        if (c != ' ' && c != '\t') break;
        ed.cur_pos++;
    }
    end_hmove();
}

void mv_eol()
{
    static int size;
    size = gb_content_len();
    begin_hmove();
    if (size == 0) return;
    if (gb_char_at(ed.cur_pos) == '\n') return;
    while (ed.cur_pos < size - 1) {
        if (gb_char_at(ed.cur_pos + 1) == '\n') break;
        ed.cur_pos++;
    }
    if (ed.cur_pos < size && gb_char_at(ed.cur_pos) == '\n'
        && ed.cur_pos > 0 && gb_char_at(ed.cur_pos - 1) != '\n')
        ed.cur_pos--;
    end_hmove();
}

void mv_left(n)
int n;
{
    begin_hmove();
    while (n-- > 0) {
        if (ed.cur_pos == 0) break;
        if (gb_char_at(ed.cur_pos - 1) == '\n') break;
        ed.cur_pos--;
    }
    ed.want_col = scr_pos_col(ed.cur_pos);
    end_hmove();
}

void mv_right(n)
int n;
{
    static int size;
    size = gb_content_len();
    begin_hmove();
    while (n-- > 0) {
        if (ed.cur_pos >= size) break;
        if (gb_char_at(ed.cur_pos) == '\n') break;
        if (ed.cur_pos + 1 >= size) break;
        if (gb_char_at(ed.cur_pos + 1) == '\n') break;
        ed.cur_pos++;
    }
    ed.want_col = scr_pos_col(ed.cur_pos);
    end_hmove();
}

void mv_up(n)
int n;
{
    static int pos, prev_lstart, target, curr_vstart, target_vstart;
    while (n-- > 0) {
        pos = find_bol(ed.cur_pos);
        if (pos == 0) break;
        pos--;
        prev_lstart = find_bol(pos);
        target = pos_at_col(prev_lstart, ed.want_col);

        if (ed.cur_vrow >= 0) {
            curr_vstart = vrow_start_of(ed.cur_pos);
            target_vstart = vrow_start_of(target);
            while (target_vstart < curr_vstart) {
                target_vstart = next_vrow(target_vstart);
                ed.cur_vrow--;
            }
        }
        ed.cur_pos = target;
    }
}

void mv_down(n)
int n;
{
    static int pos, size, target, curr_vstart, target_vstart;
    size = gb_content_len();
    while (n-- > 0) {
        pos = find_eol(ed.cur_pos);
        if (pos >= size) break;
        pos++;
        target = pos_at_col(pos, ed.want_col);

        if (ed.cur_vrow >= 0) {
            curr_vstart = vrow_start_of(ed.cur_pos);
            target_vstart = vrow_start_of(target);
            while (curr_vstart < target_vstart) {
                curr_vstart = next_vrow(curr_vstart);
                ed.cur_vrow++;
            }
        }
        ed.cur_pos = target;
    }
}

/*
 * Word motions (w/b/e) delegate to motion_endpoint(), which implements
 * the exact same scans for the d/c/y operators -- one copy of the
 * logic.  'e' returns an exclusive endpoint, hence the -1.
 */
static int mw_lw;   /* dummy linewise out-param */

void mv_word(c, n)
int c, n;
{
    begin_hmove();
    ed.cur_pos = motion_endpoint(c, n, &mw_lw);
    if (c == 'e') {
        ed.cur_pos--;
        if (ed.cur_pos < 0) ed.cur_pos = 0;
    }
    end_hmove();
}

/* ------------------------------------------------------------------ */
/*  In-line find motion (f, F, ;, ,)                                    */
/* ------------------------------------------------------------------ */
void mv_find(ch, dir, count)
int ch, dir, count;
{
    static int p, sz, found;
    if (!ch) return;
    sz = gb_content_len();
    begin_hmove();
    while (count-- > 0) {
        found = -1;
        if (dir > 0) {
            p = ed.cur_pos + 1;
            while (p < sz && gb_char_at(p) != '\n') {
                if (gb_char_at(p) == ch) { found = p; break; }
                p++;
            }
        } else {
            p = ed.cur_pos - 1;
            while (p >= 0 && gb_char_at(p) != '\n') {
                if (gb_char_at(p) == ch) { found = p; break; }
                p--;
            }
        }
        if (found < 0) break;
        ed.cur_pos = found;
    }
    ed.want_col = scr_pos_col(ed.cur_pos);
    end_hmove();
}

/* ------------------------------------------------------------------ */
/*  Range helpers for operator+motion                                   */
/* ------------------------------------------------------------------ */

/* Set to 1 before motion_endpoint('w') so the endpoint stops at the end
 * of the last word instead of consuming its trailing blanks: vi's cw
 * changes the word only, while dw deletes through the following spaces. */
int me_cw;

int motion_endpoint(ch, count, linewise)
int  ch, count;
int *linewise;
{
    int pos  = ed.cur_pos;
    int size = gb_content_len();
    int n;
    int cw;

    cw = me_cw;
    me_cw = 0;

    *linewise = 0;

    switch (ch) {
    case 'l':
        n = count;
        while (n-- > 0 && pos < size && gb_char_at(pos) != '\n') pos++;
        return pos;

    case 'h':
        n = count;
        while (n-- > 0 && pos > 0 && gb_char_at(pos-1) != '\n') pos--;
        return pos;

    case 'w':
        n = count;
        while (n-- > 0) {
            int type = iswordch(gb_char_at(pos)) ? 1 :
                       isspacech(gb_char_at(pos)) ? 0 : 2;
            while (pos < size) {
                int t2 = iswordch(gb_char_at(pos)) ? 1 :
                         isspacech(gb_char_at(pos)) ? 0 : 2;
                if (t2 != type) break;
                pos++;
            }
            if (!cw || n > 0)
                while (pos < size && (gb_char_at(pos) == ' ' ||
                                      gb_char_at(pos) == '\t')) pos++;
        }
        return pos;

    case 'b':
        n = count;
        {
            while (n-- > 0) {
                if (pos == 0) break;
                pos--;
                while (pos > 0 && isspacech(gb_char_at(pos))) pos--;
                if (pos == 0) break;
                {
                    int type = iswordch(gb_char_at(pos)) ? 1 : 2;
                    while (pos > 0) {
                        int t2 = iswordch(gb_char_at(pos-1)) ? 1 :
                                 isspacech(gb_char_at(pos-1)) ? 0 : 2;
                        if (t2 != type) break;
                        pos--;
                    }
                }
            }
        }
        return pos;

    case 'e':
        n = count;
        while (n-- > 0) {
            int type;
            if (pos >= size - 1) break;
            pos++;
            while (pos < size && isspacech(gb_char_at(pos))) pos++;
            type = iswordch(gb_char_at(pos)) ? 1 : 2;
            while (pos < size - 1) {
                int t2 = iswordch(gb_char_at(pos+1)) ? 1 :
                         isspacech(gb_char_at(pos+1)) ? 0 : 2;
                if (t2 != type) break;
                pos++;
            }
        }
        return pos + 1;

    case '$':
        while (pos < size && gb_char_at(pos) != '\n') pos++;
        return pos;

    case '0':
        while (pos > 0 && gb_char_at(pos-1) != '\n') pos--;
        return pos;

    case '^':
        {
            int sol = pos;
            while (sol > 0 && gb_char_at(sol-1) != '\n') sol--;
            while (sol < size && (gb_char_at(sol)==' '||gb_char_at(sol)=='\t')) sol++;
            return sol;
        }

    /* Linewise motions walk with find_bol/find_eol -- O(range covered)
     * instead of the old O(buffer) scr_pos_line/scr_line_start scans. */
    case 'j':
        *linewise = 1;
        {
            int to = pos;
            n = count + 1;      /* this line plus count lines below */
            while (n-- > 0) {
                to = find_eol(to);
                if (to >= size) break;
                to++;           /* include the newline */
            }
            ed.cur_pos = find_bol(pos);
            return to;
        }

    case 'k':
        *linewise = 1;
        {
            int from = find_bol(pos);
            int to   = find_eol(pos);
            if (to < size) to++;
            n = count;
            while (n-- > 0 && from > 0)
                from = find_bol(from - 1);
            ed.cur_pos = from;
            return to;
        }

    case 'G':
        *linewise = 1;
        ed.cur_pos = find_bol(pos);
        return size;

    default:
        return -1;
    }
}

/* ------------------------------------------------------------------ */
/*  Operator application                                                */
/* ------------------------------------------------------------------ */

/* Copy [from, from+len) into the yank buffer (clamped); returns count. */
static int yr_save;

static int yank_range(from, len, linewise)
int from, len, linewise;
{
    yr_save = (len >= YANK_MAX) ? YANK_MAX - 1 : len;
    gb_copy_out(ed.yank, from, yr_save);
    ed.yank[yr_save] = '\0';
    ed.yank_len  = yr_save;
    ed.yank_line = linewise;
    return yr_save;
}

void apply_op(op, from, to, linewise)
int op, from, to, linewise;
{
    static int len, save, t, size, light;

    if (from > to) { t = from; from = to; to = t; }
    len = to - from;
    if (len <= 0 && op != 'c') return;

    /* Charwise edit confined to a single-row line: repaint one row
     * instead of cursor-to-bottom (checked before the delete). */
    light = !linewise && scr_line_is_1row(from) &&
            gb_count_nl(from, len) == 0;

    if (op == 'y') {
        save = yank_range(from, len, linewise);
        ed.cur_pos  = from;
        ed.cur_vrow = -1;
        hvi_sprintf(ed.status, STATUS_MAX, "%d char%s yanked",
                    save, (int)(save == 1 ? "" : "s"));
        scr_show_status(ed.status);
        return;
    }

    if (len > 0) {
        undo_save_delete(from, len);
        yank_range(from, len, linewise);
        gb_delete(from, len);
        ed.modified = 1;
    }

    size = gb_content_len();
    ed.cur_pos = from;
    /* 'd' pulls the cursor back off a trailing newline; 'c' must insert
     * exactly where the text was removed (before the newline). */
    if (op != 'c') {
        if (ed.cur_pos >= size) ed.cur_pos = size > 0 ? size - 1 : 0;
        if (ed.cur_pos > 0 && gb_char_at(ed.cur_pos) == '\n')
            if (gb_char_at(ed.cur_pos - 1) != '\n')
                ed.cur_pos--;
    }

    scr_edit_end(light);

    if (op == 'c') {
        undo_save_insert(ed.cur_pos, 0);
        ed.mode = MODE_INSERT;
        scr_show_status(msg_insert);
    }
}

/* ------------------------------------------------------------------ */
/*  Search                                                              */
/* ------------------------------------------------------------------ */

int read_pattern(prompt)
int prompt;
{
    static int c, len;
    static char *pat;
    pat = ed.search;

    scr_status_invalidate();
    term_goto(ed.scr_rows - 1, 0);
    term_clreol();
    term_putch(prompt);
    len = 0;

    for (;;) {
        c = term_getch();
        if (c == KEY_ESC) return 0;
        if (c == KEY_CR || c == KEY_LF) {
            pat[len] = '\0';
            return (len > 0) ? 1 : 0;
        }
        if ((c == KEY_BS || c == KEY_DEL) && len > 0) {
            len--;
            term_putch(KEY_BS); term_putch(' '); term_putch(KEY_BS);
            continue;
        }
        if (c >= 0x20 && c < 0x7F && len < SEARCH_MAX - 1) {
            /* Store pre-lowered: the search is case-insensitive, so the
             * inner loops need no per-comparison pattern lowering. */
            pat[len++] = (char)((c >= 'A' && c <= 'Z') ? c + 32 : c);
            term_putch(c);
        }
    }
}

/*
 * Case-insensitive substring search from start_pos in direction ed.search_dir.
 * Wrap-around uses a single conditional subtraction per step rather than
 * the % operator, avoiding the Z80 division library.
 * Sets ed.search_wrapped if the match is on the other side of start_pos.
 * Returns the matching position, or -1 if not found.
 */
int do_search_from(start_pos)
int start_pos;
{
    static int size, plen, i, j, match, dir, pos, sp, mc;
    size = gb_content_len();
    plen = hvi_strlen(ed.search);
    dir  = ed.search_dir;
    sp   = start_pos;
    ed.search_wrapped = 0;

    if (plen == 0 || size == 0) return -1;

    for (i = 1; i <= size; i++) {
        if (dir == SEARCH_FWD) {
            pos = sp + i;
            if (pos >= size) pos -= size;
        } else {
            pos = sp - i + size;
            if (pos >= size) pos -= size;
        }
        /* Pattern is stored pre-lowered (read_pattern); lower the buffer
         * character inline -- no function calls in the compare. */
        match = 1;
        for (j = 0; j < plen; j++) {
            if (pos + j >= size) { match = 0; break; }
            mc = gb_char_at(pos + j);
            if (mc >= 'A' && mc <= 'Z') mc += 32;
            if (mc != (int)(unsigned char)ed.search[j]) { match = 0; break; }
        }
        if (match) {
            /* pos == sp means we went all the way around (i == size on the
             * last iteration); treat it as wrapped just like pos < sp. */
            if (dir == SEARCH_FWD && pos <= sp) ed.search_wrapped = 1;
            if (dir != SEARCH_FWD && pos >= sp) ed.search_wrapped = 1;
            return pos;
        }
    }
    return -1;
}

/* ------------------------------------------------------------------ */
/*  Large-file search: scan unloaded file sections                      */
/* ------------------------------------------------------------------ */

/*
 * Scan tail_file bytes [from_off, to_off) for the current search pattern.
 * Reads sequentially using hvi_fgetc; CR (0x0D) bytes are skipped.
 * SEARCH_FWD: returns file offset of the first match.
 * SEARCH_BWD: scans forward but returns offset of the last match found
 *             (i.e. the one closest to to_off when going backward).
 * Returns -1L if the pattern is not found in the range.
 * All locals static: hvi_fopen / hvi_fseek / hvi_fgetc may corrupt
 * IX-relative auto vars under HI-TECH C -O register allocation.
 */
static HFILE *sif_fp;
static long   sif_from;
static long   sif_to;
static long   sif_pos;
static long   sif_ms;      /* byte offset of current partial-match start */
static long   sif_last;    /* last completed match (SEARCH_BWD accumulator) */
static int    sif_dir;
static int    sif_plen;
static int    sif_j;
static int    sif_c;
static int    sif_cl;      /* current char, lowered */

static long scan_file_for_match(from_off, to_off, dir)
long from_off, to_off;
int  dir;
{
    sif_from = from_off;
    sif_to   = to_off;
    sif_dir  = dir;
    sif_plen = hvi_strlen(ed.search);

    if (sif_plen == 0 || !ed.tail_file[0]) return -1L;
    if (sif_from < 0L)        sif_from = 0L;
    if (sif_from >= sif_to)   return -1L;

    sif_fp = hvi_fopen(ed.tail_file, "rb");
    if (!sif_fp) return -1L;
    if (hvi_fseek(sif_fp, sif_from, 0) != 0) {
        hvi_fclose(sif_fp);
        return -1L;
    }

    sif_last = -1L;
    sif_pos  = sif_from;
    sif_j    = 0;
    sif_ms   = sif_from;

    while (sif_pos < sif_to) {
        sif_c = hvi_fgetc(sif_fp);
        if (sif_c == HEOF || sif_c == 0x1A) break;
        if (sif_c == 0x0D) { sif_pos++; continue; }   /* skip bare CR */

        /* Pattern is stored pre-lowered (read_pattern). */
        sif_cl = sif_c;
        if (sif_cl >= 'A' && sif_cl <= 'Z') sif_cl += 32;

        if (sif_cl == (int)(unsigned char)ed.search[sif_j]) {
            if (sif_j == 0) sif_ms = sif_pos;         /* start of match */
            sif_j++;
            if (sif_j >= sif_plen) {                   /* full match */
                if (sif_dir == SEARCH_FWD) {
                    hvi_fclose(sif_fp);
                    return sif_ms;                     /* return immediately */
                }
                sif_last = sif_ms;                     /* save; keep scanning */
                sif_j = 0;
            }
        } else {
            if (sif_j > 0) {
                sif_j = 0;                             /* restart pattern */
                /* Retry this character as a potential new match start */
                if (sif_cl == (int)(unsigned char)ed.search[0]) {
                    sif_ms = sif_pos;
                    sif_j  = 1;
                }
            }
        }
        sif_pos++;
    }

    hvi_fclose(sif_fp);
    return sif_last;
}

/*
 * Full-file search from start_pos in ed.search_dir.
 *
 * Phase 1 – searches the in-memory gap buffer (do_search_from).
 * Phase 2 – if the buffer result required a wrap or was not found,
 *            scans the unloaded head [0, win_start) and/or tail
 *            [tail_offset, EOF) sections by reading the file directly.
 *
 * When a match is found outside the buffer, loads a window around it
 * via gb_reload_from() and returns the match's position in the new buffer.
 * Sets ed.search_wrapped if the result is past a file-order wrap point.
 * Returns -1 if the pattern is not found anywhere in the file.
 *
 * All locals static: gb_reload_from / hvi_fopen calls may corrupt
 * IX-relative auto vars under HI-TECH C -O register allocation.
 */
static int    dsf_sp;
static int    dsf_buf_result;
static int    dsf_buf_wrapped;
static long   dsf_file_match;
static long   dsf_load_from;
static int    dsf_new_result;
static int    dsf_file_wrapped; /* 1 if file match is on the "wrap" side */

int do_search_full(start_pos)
int start_pos;
{
    dsf_sp = start_pos;

    /* Phase 1: search current buffer */
    dsf_buf_result  = do_search_from(dsf_sp);
    dsf_buf_wrapped = ed.search_wrapped;

    /* No unloaded sections -- return buffer result as-is */
    if (!ed.tail_file[0] || (ed.win_start == 0L && ed.tail_offset == 0L))
        return dsf_buf_result;

    /* Found in buffer without wrap: that is the nearest match in file order */
    if (dsf_buf_result >= 0 && !dsf_buf_wrapped)
        return dsf_buf_result;

    /* Phase 2: scan unloaded file sections.
     * File order for FWD from cursor: tail (after buffer) → head (before buffer,
     * wrapped) → buffer wrap.  Tail hits are NOT a file-level wrap; head hits are.
     * For BWD: head (before buffer) → tail (after buffer, wrapped). */
    dsf_file_match  = -1L;
    dsf_file_wrapped = 0;

    if (ed.search_dir == SEARCH_FWD) {
        /* First: unloaded tail (after the buffer) — not a wrap */
        if (ed.tail_offset > 0L)
            dsf_file_match =
                scan_file_for_match(ed.tail_offset, 0x7FFFFFFFL, SEARCH_FWD);
        /* Second: unloaded head (before the buffer) — IS a wrap */
        if (dsf_file_match < 0L && ed.win_start > 0L) {
            dsf_file_match =
                scan_file_for_match(0L, ed.win_start, SEARCH_FWD);
            if (dsf_file_match >= 0L) dsf_file_wrapped = 1;
        }
    } else {
        /* First: unloaded head (before the buffer) — not a wrap */
        if (ed.win_start > 0L)
            dsf_file_match =
                scan_file_for_match(0L, ed.win_start, SEARCH_BWD);
        /* Second: unloaded tail (after the buffer) — IS a wrap */
        if (dsf_file_match < 0L && ed.tail_offset > 0L) {
            dsf_file_match =
                scan_file_for_match(ed.tail_offset, 0x7FFFFFFFL, SEARCH_BWD);
            if (dsf_file_match >= 0L) dsf_file_wrapped = 1;
        }
    }

    if (dsf_file_match < 0L) {
        /* Not found in file sections; fall back to buffer result */
        ed.search_wrapped = dsf_buf_wrapped;
        return dsf_buf_result;
    }

    /* Found in file: load a window centred on the match */
    dsf_load_from = dsf_file_match - (long)(LOAD_CHUNK >> 1);
    if (dsf_load_from < 0L) dsf_load_from = 0L;
    gb_reload_from(dsf_load_from);

    /*
     * Search the freshly loaded buffer.
     * FWD: start from the last buffer position so the scan wraps to 0
     *      and finds the first (and correct) occurrence.
     * BWD: start from position 0 so the scan goes backward to the end,
     *      finding the last occurrence (nearest to the buffer start).
     */
    if (ed.search_dir == SEARCH_FWD)
        dsf_new_result = do_search_from(gb_content_len() - 1);
    else
        dsf_new_result = do_search_from(0);

    /* Wrap flag: true only when the match required going past file boundaries */
    ed.search_wrapped = dsf_file_wrapped;
    return dsf_new_result;
}
