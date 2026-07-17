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
extern char s_rb[];      /* "rb" fopen mode, defined in gap.c */

int  motion_endpoint();  /* fwd decl: used by the word motions below */
void enter_insert();     /* edit.c: insert-mode entry for the 'c' op */

/* ------------------------------------------------------------------ */
/*  Shared status-line helpers                                          */
/* ------------------------------------------------------------------ */
/* status_show() lives in screen.c (v2.9 size pass): screen.c's own
 * refresh tails call it too, and the single-pass linker only resolves
 * calls into already-linked modules -- screen links before emove. */

/* Set ed.status to a fixed message and show it. */
void status_msg(s)
char *s;
{
    hvi_strcpy(ed.status, s);
    status_show();
}

/* Remember the cursor's column for subsequent vertical movement. */
void set_wcol()
{
    ed.want_col = scr_pos_col(ed.cur_pos);
}

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
    while (pos < size && !gb_is_nl(pos)) {
        c  = gb_char_at(pos);
        nc = (c == '\t') ? (col | (TAB_STOP - 1)) + 1 : col + 1;
        if (nc > wantcol) break;
        col = nc;
        pos++;
    }
    if (pos > lstart && (pos >= size || gb_is_nl(pos)))
        pos--;
    return pos;
}

void mv_bol()
{
    begin_hmove();
    ed.cur_pos = find_bol(ed.cur_pos);
    end_hmove();
}

/* First non-blank position of the line containing pos (shared by
 * mv_bnb and the '^' operator motion). */
static int bnb_p, bnb_size, bnb_c;

static int bnb_of(pos)
int pos;
{
    bnb_size = gb_content_len();
    bnb_p = find_bol(pos);
    while (bnb_p < bnb_size) {
        bnb_c = gb_char_at(bnb_p);
        if (bnb_c != ' ' && bnb_c != '\t') break;
        bnb_p++;
    }
    return bnb_p;
}

void mv_bnb()
{
    begin_hmove();
    ed.cur_pos = bnb_of(ed.cur_pos);
    end_hmove();
}

void mv_eol()
{
    static int p;
    begin_hmove();
    /* find_eol lands on the '\n' (or buffer end); the cursor sits on
     * the last character before it.  p == cur_pos when the line is
     * empty or the cursor is already on its '\n'. */
    p = find_eol(ed.cur_pos);
    if (p > ed.cur_pos) ed.cur_pos = p - 1;
    end_hmove();
}

void mv_left(n)
int n;
{
    begin_hmove();
    while (n-- > 0) {
        if (ed.cur_pos == 0) break;
        if (gb_is_nl(ed.cur_pos - 1)) break;
        ed.cur_pos--;
    }
    set_wcol();
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
        if (gb_is_nl(ed.cur_pos)) break;
        if (ed.cur_pos + 1 >= size) break;
        if (gb_is_nl(ed.cur_pos + 1)) break;
        ed.cur_pos++;
    }
    set_wcol();
    end_hmove();
}

/* Count vrow starts stepped walking from a up to b (a, b are vrow
 * starts; 0 when a >= b).  Shared cur_vrow bookkeeping for mv_up/mv_down. */
static int vst_n;

static int vsteps(a, b)
int a, b;
{
    vst_n = 0;
    while (a < b) { a = next_vrow(a); vst_n++; }
    return vst_n;
}

void mv_up(n)
int n;
{
    static int pos, target;
    while (n-- > 0) {
        pos = find_bol(ed.cur_pos);
        if (pos == 0) break;
        pos--;
        target = pos_at_col(find_bol(pos), ed.want_col);

        if (ed.cur_vrow >= 0)
            ed.cur_vrow -= vsteps(vrow_start_of(target),
                                  vrow_start_of(ed.cur_pos));
        ed.cur_pos = target;
    }
}

void mv_down(n)
int n;
{
    static int pos, size, target;
    size = gb_content_len();
    while (n-- > 0) {
        pos = find_eol(ed.cur_pos);
        if (pos >= size) break;
        pos++;
        target = pos_at_col(pos, ed.want_col);

        if (ed.cur_vrow >= 0)
            ed.cur_vrow += vsteps(vrow_start_of(ed.cur_pos),
                                  vrow_start_of(target));
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
    set_wcol();     /* w/b/e establish a new column for later j/k (issue #6) */
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
            while (p < sz && !gb_is_nl(p)) {
                if (gb_char_at(p) == ch) { found = p; break; }
                p++;
            }
        } else {
            p = ed.cur_pos - 1;
            while (p >= 0 && !gb_is_nl(p)) {
                if (gb_char_at(p) == ch) { found = p; break; }
                p--;
            }
        }
        if (found < 0) break;
        ed.cur_pos = found;
    }
    set_wcol();
    end_hmove();
}

/* ------------------------------------------------------------------ */
/*  Range helpers for operator+motion                                   */
/* ------------------------------------------------------------------ */

/* Set to 1 before motion_endpoint('w') so the endpoint stops at the end
 * of the last word instead of consuming its trailing blanks: vi's cw
 * changes the word only, while dw deletes through the following spaces. */
int me_cw;

/* Mark char for the '`' motion.  The caller sets it before invoking
 * motion_endpoint('`', ...): edit.c stores the char it read from the
 * keyboard, erepeat.c replays ed.dot_arg. */
int me_mkc;

/*
 * Word-character class of the char at pos: 1 = word, 0 = blank/newline,
 * 2 = other punctuation (past-the-end reads as 2, matching the old
 * iswordch(-1)?1:2 fallbacks).  One gb_char_at call per character
 * instead of the two the inline classifiers cost.
 */
static int cht_c;

static int chtype(pos)
int pos;
{
    cht_c = gb_char_at(pos);
    if (iswordch(cht_c))  return 1;
    if (isspacech(cht_c)) return 0;
    return 2;
}

/* Mark-resolve temp: static (3-byte absolute stores) beats an auto
 * (6-byte IX-relative spills) under HI-TECH C. */
static int me_mk_n;

static char s_nomark[] = "Mark not set";

int motion_endpoint(ch, count, linewise)
int  ch, count;
int *linewise;
{
    static int pos, size, n, cw;

    pos  = ed.cur_pos;
    size = gb_content_len();

    cw = me_cw;
    me_cw = 0;

    *linewise = 0;

    switch (ch) {
    case 'l':
        n = count;
        while (n-- > 0 && pos < size && !gb_is_nl(pos)) pos++;
        return pos;

    case 'h':
        n = count;
        while (n-- > 0 && pos > 0 && !gb_is_nl(pos-1)) pos--;
        return pos;

    case 'w':
        n = count;
        while (n-- > 0) {
            static int type;
            type = chtype(pos);
            while (pos < size) {
                if (chtype(pos) != type) break;
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
                    static int type;
                    type = chtype(pos);
                    while (pos > 0) {
                        if (chtype(pos - 1) != type) break;
                        pos--;
                    }
                }
            }
        }
        return pos;

    case 'e':
        n = count;
        while (n-- > 0) {
            static int type;
            if (pos >= size - 1) break;
            pos++;
            while (pos < size && isspacech(gb_char_at(pos))) pos++;
            type = chtype(pos);
            while (pos < size - 1) {
                if (chtype(pos + 1) != type) break;
                pos++;
            }
        }
        return pos + 1;

    case '$':
        return find_eol(pos);

    case '0':
        return find_bol(pos);

    case '^':
        return bnb_of(pos);

    /* Linewise motions walk with find_bol/find_eol -- O(range covered)
     * instead of the old O(buffer) scr_pos_line/scr_line_start scans. */
    case 'j':
        *linewise = 1;
        {
            static int to;
            to = pos;
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
            static int from, to;
            from = find_bol(pos);
            to   = find_eol(pos);
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

    /* `{a-z} / ``: exclusive charwise motion to a mark (vi: d`a deletes
     * from the cursor up to, not including, the mark).  Serves both the
     * d/c/y operators and the standalone jump in edit.c.  A bad char
     * aborts silently; an unset mark reports "Mark not set". */
    case '`':
        me_mk_n = me_mkc - '`'; /* '`' -> MARK_PREV (0), a-z -> 1-26 */
        if ((unsigned)me_mk_n >= NMARKS)
            return -1;          /* not a mark char: abort silently */
        me_mk_n = ed.marks[me_mk_n];
        /* unsigned test: catches unset (-1) and past-the-end in one */
        if ((unsigned)me_mk_n > (unsigned)size) {
            status_msg(s_nomark);
            return -1;
        }
        return me_mk_n;

    default:
        /* Message shown here so every caller can just test for < 0. */
        status_fmt("Unknown motion: %c", ch, 0);
        status_show();
        return -1;
    }
}

/* ------------------------------------------------------------------ */
/*  Operator application                                                */
/* ------------------------------------------------------------------ */

/*
 * Whole-line span for the doubled operators (dd/cc/yy) and their '.'
 * replay: from the start of the cursor's line, walk count lines with
 * find_bol/find_eol (O(range)).  Sets ls_from to the span start and
 * returns the end (just past the count-th newline, or buffer end).
 */
int ls_from;
static int lsp_k, lsp_size, lsp_to;

int line_span(count)
int count;
{
    lsp_size = gb_content_len();
    ls_from  = find_bol(ed.cur_pos);
    lsp_to   = ls_from;
    for (lsp_k = count; lsp_k > 0; lsp_k--) {
        lsp_to = find_eol(lsp_to);
        if (lsp_to >= lsp_size) break;
        lsp_to++;       /* include the newline */
    }
    return lsp_to;
}

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

/*
 * Shift every line touched by [from, to) one TAB_STOP right ('>') or
 * left ('<').  Always linewise (vi): the range expands to whole lines;
 * an exclusive endpoint in column 0 leaves that line out.  '>' inserts
 * one tab (empty lines are skipped, and gb_insert_room recovers when
 * the gap is exhausted); '<' removes up to TAB_STOP columns of leading
 * blanks.  Lines are walked bottom-up, so each line-start position is
 * unaffected by the edits already made below it -- this also survives
 * a window swap (the count, not positions, drives the loop).
 * The cursor lands on the first non-blank of the topmost shifted line
 * (vi).  A single-line shift is undoable; more are scattered edits, so
 * the undo record is invalidated (like :s).  Returns non-zero when
 * exactly one line was shifted (the caller's light-repaint hint).
 */
extern char  gb_roomed;

static char sh_tab[1] = { '\t' };
static int  sh_nl, sh_i, sh_n, sh_c, sh_ch, sh_ls;
static char sh_did;

/* edit.c: 1-byte insert (gir_pos/gir_text) via gb_insert_room. */
int room1();
extern int   gir_pos;
extern char *gir_text;

/* Arguments in globals (set by the caller, like gir_*): a paramless
 * function compiles frameless, and the loop re-reads sh_op with 3-byte
 * absolute loads instead of 6-byte IX-relative ones. */
int sh_op, sh_from, sh_to;

int apply_shift()
{
    gb_roomed = 0;
    sh_did = 0;
    if (sh_from > sh_to) { sh_i = sh_from; sh_from = sh_to; sh_to = sh_i; }
    if (sh_to != sh_from) sh_to--;  /* last char (to >= from: equality) */
    sh_ls   = find_bol(sh_to);      /* bottom line first */
    sh_from = find_bol(sh_from);
    sh_nl = gb_count_nl(sh_from, sh_ls - sh_from) + 1;
    sh_i  = sh_nl;                  /* >= 1 always */
    for (;;) {
        if (sh_op == '>') {
            sh_ch = gb_char_at(sh_ls);   /* -1 at buffer end */
            if (sh_ch >= 0 && sh_ch != '\n') {
                gir_pos = sh_ls; gir_text = sh_tab;
                sh_n = room1();
                if (sh_n < 0) break;
                sh_ls = sh_n - 1;       /* window may have shifted */
                undo_save_insert(sh_ls, 1);
                sh_did = 1;
            }
        } else {
            /* consume leading blanks up to one TAB_STOP of columns */
            sh_n = sh_ls;
            sh_c = 0;
            while (sh_c < TAB_STOP) {
                sh_ch = gb_char_at(sh_n);
                if (sh_ch == ' ')       sh_c++;
                else if (sh_ch == '\t') sh_c = TAB_STOP;  /* sh_c < 8:
                                           (sh_c|7)+1 is always 8 */
                else break;
                sh_n++;
            }
            if (sh_n != sh_ls) {    /* sh_n >= sh_ls: cheap equality */
                undo_save_delete(sh_ls, sh_n - sh_ls);
                gb_delete(sh_ls, sh_n - sh_ls);
                sh_did = 1;
            }
        }
        if (--sh_i == 0) break;
        sh_ls = find_bol(sh_ls - 1);
    }
    if (sh_nl != 1 || gb_roomed)    /* sh_nl >= 1: != 1 means > 1 */
        ed.undo.type = UNDO_NONE;   /* scattered edits: not undoable */
    ed.cur_pos  = bnb_of(sh_ls);    /* topmost line, first non-blank */
    ed.cur_vrow = -1;
    set_wcol();
    if (sh_did) ed.modified = 1;
    return sh_nl == 1;
}

void apply_op(op0, from0, to0, linewise)
int op0, from0, to0, linewise;
{
    /* Params copied to statics: 3-byte absolute accesses (also fewer
     * T-states than IX-relative) -- apply_op never nests. */
    static int op, from, to;
    static int len, save, t, size, light;

    op = op0; from = from0; to = to0;

    if (op == '>' || op == '<') {
        sh_op = op; sh_from = from; sh_to = to;
        t = apply_shift();
        if (gb_roomed) scr_refresh();
        else           scr_edit_end(t);
        return;
    }

    if (from > to) { t = from; from = to; to = t; }
    len = to - from;
    if (len == 0 && op != 'c') return;  /* len >= 0 after the swap */

    /* Charwise edit confined to a single-row line: repaint one row
     * instead of cursor-to-bottom (checked before the delete). */
    light = !linewise && scr_line_is_1row(from) &&
            gb_count_nl(from, len) == 0;

#ifdef TERM_HAS_SCROLL
    /* Linewise 'd': size the doomed range in visual rows while it is
     * still in the buffer -- scr_del_rows shifts the rows below up with
     * the hardware scroll instead of repainting them (issue #8).  0 =
     * range starts above the viewport or is taller than the text area
     * (scr_del_rows then falls back to the ordinary repaint). */
    sdr_n = 0;
    if (op == 'd' && linewise && from >= ed.top_pos) {
        vcr_from = from;
        vcr_to   = to;
        scr_count_rows();       /* capped; 0 = fall back */
        sdr_n = vcr_n;
    }
#endif

    if (op == 'y') {
        save = yank_range(from, len, linewise);
        ed.cur_pos  = from;
        ed.cur_vrow = -1;
        status_fmt("%d char%s yanked",
                    save, (int)(save == 1 ? "" : "s"));
        status_show();
        return;
    }

    if (len != 0) {     /* len >= 0 after the swap: cheap equality */
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
        if (ed.cur_pos != 0 && gb_is_nl(ed.cur_pos))
            if (!gb_is_nl(ed.cur_pos - 1))
                ed.cur_pos--;
    }

#ifdef TERM_HAS_SCROLL
    if (op == 'd' && linewise) {
        if (size == 0) sdr_n = 0;   /* emptied buffer: row 0 blank rule */
        sdr_pos = from;
        scr_del_rows();
        status_show();
    } else {
        scr_edit_end(light);
    }
#else
    scr_edit_end(light);
#endif

    if (op == 'c')
        enter_insert('c');   /* edit.c: undo arm + insert-mode entry */
}

/* ------------------------------------------------------------------ */
/*  Search                                                              */
/* ------------------------------------------------------------------ */

/*
 * Read a line of input on the status row after echoing prompt.
 * Stores up to max-1 chars (plus NUL) into buf; lower != 0 folds A-Z to
 * a-z as stored (the echo keeps the typed case).  Returns the length,
 * or -1 on ESC.  Shared by the ex ':' command line and the / ? prompts.
 * All locals static: term_getch's bios call may corrupt IX-relative autos.
 */
static int   rl_c, rl_len, rl_max, rl_lower;
static char *rl_buf;

int read_line(prompt, buf, max, lower)
int   prompt;
char *buf;
int   max, lower;
{
    rl_buf   = buf;
    rl_max   = max;
    rl_lower = lower;

    scr_status_invalidate();
    term_status_row();
    term_putch(prompt);
    rl_len = 0;

    for (;;) {
        rl_c = term_getch();
        if (rl_c == KEY_ESC) return -1;
        if (rl_c == KEY_CR || rl_c == KEY_LF) {
            rl_buf[rl_len] = '\0';
            return rl_len;
        }
        if ((rl_c == KEY_BS || rl_c == KEY_DEL) && rl_len > 0) {
            rl_len--;
            term_putch(KEY_BS); term_putch(' '); term_putch(KEY_BS);
            continue;
        }
        if (rl_c >= 0x20 && rl_c < 0x7F && rl_len < rl_max - 1) {
            rl_buf[rl_len++] = (char)((rl_lower && rl_c >= 'A' && rl_c <= 'Z')
                                      ? rl_c + 32 : rl_c);
            term_putch(rl_c);
        }
    }
}

/* Search patterns are stored pre-lowered: the search is case-insensitive,
 * so the inner compare loops need no per-comparison pattern lowering. */
int read_pattern(prompt)
int prompt;
{
    return read_line(prompt, ed.search, SEARCH_MAX, 1) > 0;
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

    sif_fp = gb_open_tail();
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
        if (ed.tail_offset != 0L)
            dsf_file_match =
                scan_file_for_match(ed.tail_offset, 0x7FFFFFFFL, SEARCH_FWD);
        /* Second: unloaded head (before the buffer) — IS a wrap */
        if (dsf_file_match < 0L && ed.win_start != 0L) {
            dsf_file_match =
                scan_file_for_match(0L, ed.win_start, SEARCH_FWD);
            if (dsf_file_match >= 0L) dsf_file_wrapped = 1;
        }
    } else {
        /* First: unloaded head (before the buffer) — not a wrap */
        if (ed.win_start != 0L)
            dsf_file_match =
                scan_file_for_match(0L, ed.win_start, SEARCH_BWD);
        /* Second: unloaded tail (after the buffer) — IS a wrap */
        if (dsf_file_match < 0L && ed.tail_offset != 0L) {
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
