/*
 * erepeat.c - Dot-repeat (.) command support for HVI
 * Author: Juan Orlandini
 * License: MIT
 *
 * Split out of edit.c to keep the per-file temporary-label count under
 * the HI-TECH C V3.09 compiler limit.
 *
 * Contains:
 *   ins_position()      - reposition cursor for insert-entry commands
 *   dot_replay_c()      - replay c/C change commands
 *   dot_replay()        - replay the last change (the '.' command)
 */

#include "hvi.h"

extern Editor ed;

/* Forward declarations for functions not exposed in hvi.h */
void undo_save_delete();
void undo_save_insert();
void mv_bnb();
void mv_eol();
int  motion_endpoint();
extern int me_cw;
void apply_op();
void status_show();
int  line_span();
extern int ls_from;

/* CPIR newline counter over raw memory (cstart.as). */
extern int gb_cntnl();

/* ------------------------------------------------------------------ */

/* Pull the cursor back off a trailing newline (shared post-edit fixup;
 * also used by the insert-mode ESC handler in edit.c). */
void cur_back_nl()
{
    if (ed.cur_pos > 0 && gb_char_at(ed.cur_pos - 1) != '\n')
        ed.cur_pos--;
}

/*
 * Reposition the cursor to the insertion point for insert-entry command
 * cmd.  Shared by the normal-mode a/I/A handlers in edit.c and the
 * dot-replay of all insert commands below.
 */
void ins_position(cmd)
int cmd;
{
    int sz, eol;
    switch (cmd) {
    case 'a':
        sz = gb_content_len();
        if (sz > 0 && ed.cur_pos < sz && gb_char_at(ed.cur_pos) != '\n')
            ed.cur_pos++;
        break;
    case 'A':
        mv_eol();
        sz = gb_content_len();
        if (sz > 0 && ed.cur_pos < sz && gb_char_at(ed.cur_pos) != '\n')
            ed.cur_pos++;
        break;
    case 'I':
        mv_bnb();
        break;
    case 'o':
        sz = gb_content_len();
        eol = find_eol(ed.cur_pos);
        if (eol < sz) eol++;
        ed.cur_pos = eol;
        break;
    case 'O':
        ed.cur_pos = find_bol(ed.cur_pos);
        break;
    default: break;
    }
}

/* Replay c/C: delete the range then re-insert stored text. */
void dot_replay_c(n)
int n;
{
    int from, to, ins_pos, linewise, endpoint;
    int light;
    linewise = 0;
    if (ed.dot_motion == 'c') {          /* cc / S: whole-line change */
        from = find_bol(ed.cur_pos);
        to = find_eol(from);
    } else {
        if (ed.dot_motion == 'w') me_cw = 1;
        endpoint = motion_endpoint(ed.dot_motion, n, &linewise);
        if (endpoint < 0) return;
        from = (ed.cur_pos < endpoint) ? ed.cur_pos : endpoint;
        to   = (ed.cur_pos < endpoint) ? endpoint   : ed.cur_pos;
    }
    /* One-row repaint when the whole change stays on a single-row line. */
    light = !linewise && scr_line_is_1row(from) &&
            gb_count_nl(from, to - from) == 0 &&
            gb_cntnl(ed.dot_text, ed.dot_len) == 0;
    ins_pos = from;
    if (to > from) {
        undo_save_delete(from, to - from);
        gb_delete(from, to - from);
        ed.cur_pos = from;
        ed.modified = 1;
    }
    if (ed.dot_len > 0) {
        undo_save_insert(ins_pos, ed.dot_len);
        gb_insert(ins_pos, ed.dot_text, ed.dot_len);
        ed.cur_pos = ins_pos + ed.dot_len;
        cur_back_nl();
    }
    scr_edit_end(light);
}

/*
 * Replay the last change command at the current cursor position.
 * count == 0 means use the stored dot_count; otherwise use count.
 */
static char dr_sp[1] = { ' ' };   /* J's separating space (1 data byte) */

void dot_replay(count)
int count;
{
    int  n, sz, k, linewise, endpoint;
    int  from, to, ins_pos, ch, eol, del;
    int  has_nl;
    char tmp_c[1];

    if (!ed.dot_cmd) return;
    n  = (count > 0) ? count : ed.dot_count;

    /* Note: x, X, D, C, s, S never appear as dot_cmd -- they expand
     * through op_motion() and replay as 'd' or 'c' with a motion. */
    switch (ed.dot_cmd) {

    case 'r':
        sz = gb_content_len();
        if (ed.cur_pos < sz) {
            ch = gb_char_at(ed.cur_pos);
            undo_save_delete(ed.cur_pos, 1);
            gb_delete(ed.cur_pos, 1);
            tmp_c[0] = (char)ed.dot_arg;
            gb_insert(ed.cur_pos, tmp_c, 1);
            ed.modified = 1;
            if (ed.dot_arg == '\n') scr_refresh();
            else scr_fix_char(ch, ed.dot_arg);
        }
        break;

    case '~':
        sz = gb_content_len();
        from = ed.cur_pos;
        for (k = 0; k < n; k++) {
            if (ed.cur_pos >= sz || gb_char_at(ed.cur_pos) == '\n') break;
            ch = gb_char_at(ed.cur_pos);
            if (ch >= 'a' && ch <= 'z') ch = ch - 'a' + 'A';
            else if (ch >= 'A' && ch <= 'Z') ch = ch - 'A' + 'a';
            tmp_c[0] = (char)ch;
            undo_save_delete(ed.cur_pos, 1);
            gb_delete(ed.cur_pos, 1);
            gb_insert(ed.cur_pos, tmp_c, 1);
            ed.cur_pos++;
            ed.modified = 1;
        }
        to = ed.cur_pos;
        /* don't leave the cursor on the newline / past the end */
        if (ed.cur_pos > 0 &&
            (ed.cur_pos >= sz || gb_char_at(ed.cur_pos) == '\n') &&
            gb_char_at(ed.cur_pos - 1) != '\n')
            ed.cur_pos--;
        scr_fix_span(from, to);   /* in-place emit, or line redraw */
        break;

    case 'J':
        k = (n > 1) ? n - 1 : 1;
        while (k-- > 0) {
            sz = gb_content_len();
            eol = find_eol(ed.cur_pos);
            if (eol >= sz) break;
            /* remove the newline plus the joined line's leading blanks */
            del = 1;
            while ((ch = gb_char_at(eol + del)) == ' ' || ch == '\t')
                del++;
            undo_save_delete(eol, del);
            gb_delete(eol, del);
            sz = gb_content_len();
            /* single separating space -- unless this line already ends in
             * a blank, or either side of the join is empty (vi rules) */
            ch = (eol > 0) ? gb_char_at(eol - 1) : '\n';
            if (eol < sz && gb_char_at(eol) != '\n' &&
                ch != ' ' && ch != '\t' && ch != '\n')
                gb_insert(eol, dr_sp, 1);
            ed.modified = 1;
        }
        scr_redraw_from_cur();
        status_show();
        break;

    case 'd':
        if (ed.dot_motion == 'd') {
            /* Same line_span walk as dd in edit.c -- O(range). */
            to = line_span(ed.dot_count);
            apply_op('d', ls_from, to, 1);
        } else {
            linewise = 0;
            endpoint = motion_endpoint(ed.dot_motion, ed.dot_count, &linewise);
            if (endpoint >= 0)
                apply_op('d', ed.cur_pos, endpoint, linewise);
        }
        break;

    case 'c':
        dot_replay_c(n);
        break;

    default:
        if (ed.dot_len > 0) {
            ins_position(ed.dot_cmd);
            ins_pos = ed.cur_pos;
            /* Check if inserted text crosses a line boundary. */
            has_nl = gb_cntnl(ed.dot_text, ed.dot_len) != 0;
            del = !has_nl && scr_line_is_1row(ins_pos);   /* light path */
            undo_save_insert(ins_pos, ed.dot_len);
            gb_insert(ins_pos, ed.dot_text, ed.dot_len);
            ed.cur_pos = ins_pos + ed.dot_len;
            cur_back_nl();
            ed.modified = 1;
            if (has_nl) {
                /* Multi-line insert: redraw from insertion point. */
                ed.cur_pos = ins_pos;
                scr_adj();
                ed.cur_pos = ins_pos + ed.dot_len;
                cur_back_nl();
                status_show();
            } else if (del) {
                scr_edit_end(1);
            } else {
                scr_adj();
                status_show();
            }
        }
        break;
    }
}
