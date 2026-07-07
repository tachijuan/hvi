/*
 * erepeat.c - Dot-repeat (.) command support for HVI
 * Author: Juan Orlandini
 * License: MIT
 *
 * Split out of edit.c to keep the per-file temporary-label count under
 * the HI-TECH C V3.09 compiler limit.
 *
 * Contains:
 *   dot_ins_position()  - reposition cursor for insert-mode replay
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

/* ------------------------------------------------------------------ */

/*
 * Reposition the cursor to the correct location before replaying an
 * insert-mode command stored in ed.dot_cmd.
 */
void dot_ins_position()
{
    int sz, eol;
    switch (ed.dot_cmd) {
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
    linewise = 0;
    if (ed.dot_cmd == 'C' || ed.dot_motion == 'c') {
        from = find_bol(ed.cur_pos);
        to = find_eol(from);
    } else {
        if (ed.dot_motion == 'w') me_cw = 1;
        endpoint = motion_endpoint(ed.dot_motion, n, &linewise);
        if (endpoint < 0) return;
        from = (ed.cur_pos < endpoint) ? ed.cur_pos : endpoint;
        to   = (ed.cur_pos < endpoint) ? endpoint   : ed.cur_pos;
    }
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
        if (ed.cur_pos > 0 && gb_char_at(ed.cur_pos - 1) != '\n')
            ed.cur_pos--;
        if (ed.cur_pos < 0) ed.cur_pos = 0;
    }
    scr_after_edit();
}

/*
 * Replay the last change command at the current cursor position.
 * count == 0 means use the stored dot_count; otherwise use count.
 */
void dot_replay(count)
int count;
{
    int  n, sz, k, linewise, endpoint;
    int  from, to, ins_pos, ch, eol;
    int  start_line, end_line, total;
    int  has_nl, ki;
    char tmp_c[1];
    char sp;

    if (!ed.dot_cmd) return;
    n  = (count > 0) ? count : ed.dot_count;
    sp = ' ';

    switch (ed.dot_cmd) {

    case 'x':
        k = n;
        while (k-- > 0 && ed.cur_pos < gb_content_len() &&
               gb_char_at(ed.cur_pos) != '\n') {
            undo_save_delete(ed.cur_pos, 1);
            gb_delete(ed.cur_pos, 1);
            ed.modified = 1;
        }
        sz = gb_content_len();
        if (ed.cur_pos >= sz) ed.cur_pos = (sz > 0) ? sz - 1 : 0;
        if (ed.cur_pos > 0 && gb_char_at(ed.cur_pos) == '\n' &&
            gb_char_at(ed.cur_pos - 1) != '\n')
            ed.cur_pos--;
        scr_redraw_cur_line();
        break;

    case 'X':
        k = n;
        while (k-- > 0 && ed.cur_pos > 0 &&
               gb_char_at(ed.cur_pos - 1) != '\n') {
            ed.cur_pos--;
            undo_save_delete(ed.cur_pos, 1);
            gb_delete(ed.cur_pos, 1);
            ed.modified = 1;
        }
        scr_redraw_cur_line();
        break;

    case 'r':
        sz = gb_content_len();
        if (ed.cur_pos < sz) {
            undo_save_delete(ed.cur_pos, 1);
            gb_delete(ed.cur_pos, 1);
            tmp_c[0] = (char)ed.dot_arg;
            gb_insert(ed.cur_pos, tmp_c, 1);
            ed.modified = 1;
            if (ed.dot_arg == '\n') scr_refresh();
            else scr_redraw_cur_line();
        }
        break;

    case 'D':
        sz = gb_content_len();
        to = find_eol(ed.cur_pos);
        if (to > ed.cur_pos) {
            undo_save_delete(ed.cur_pos, to - ed.cur_pos);
            gb_delete(ed.cur_pos, to - ed.cur_pos);
            ed.modified = 1;
            sz = gb_content_len();
            if (ed.cur_pos > 0 && (ed.cur_pos >= sz ||
                gb_char_at(ed.cur_pos) == '\n'))
                if (gb_char_at(ed.cur_pos - 1) != '\n')
                    ed.cur_pos--;
        }
        scr_redraw_cur_line();
        break;

    case '~':
        sz = gb_content_len();
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
        scr_redraw_cur_line();
        break;

    case 'J':
        k = (n > 1) ? n - 1 : 1;
        while (k-- > 0) {
            sz = gb_content_len();
            eol = find_eol(ed.cur_pos);
            if (eol >= sz) break;
            undo_save_delete(eol, 1);
            gb_delete(eol, 1);
            sz = gb_content_len();
            if (eol < sz && gb_char_at(eol) != ' ')
                gb_insert(eol, &sp, 1);
            ed.modified = 1;
        }
        scr_redraw_from_cur();
        scr_show_status(ed.status);
        break;

    case 'd':
        if (ed.dot_motion == 'd') {
            start_line = scr_pos_line(ed.cur_pos);
            end_line = start_line + ed.dot_count - 1;
            total = scr_line_count();
            if (end_line >= total) end_line = total - 1;
            from = scr_line_start(start_line);
            to = (end_line + 1 < total)
                 ? scr_line_start(end_line + 1) : gb_content_len();
            apply_op('d', from, to, 1);
        } else {
            linewise = 0;
            endpoint = motion_endpoint(ed.dot_motion, ed.dot_count, &linewise);
            if (endpoint >= 0)
                apply_op('d', ed.cur_pos, endpoint, linewise);
        }
        break;

    case 'c':
    case 'C':
        dot_replay_c(n);
        break;

    default:
        if (ed.dot_len > 0) {
            dot_ins_position();
            ins_pos = ed.cur_pos;
            undo_save_insert(ins_pos, ed.dot_len);
            gb_insert(ins_pos, ed.dot_text, ed.dot_len);
            ed.cur_pos = ins_pos + ed.dot_len;
            if (ed.cur_pos > 0 && gb_char_at(ed.cur_pos - 1) != '\n')
                ed.cur_pos--;
            ed.modified = 1;
            /* Check if inserted text crosses a line boundary. */
            has_nl = 0;
            for (ki = 0; ki < ed.dot_len; ki++)
                if (ed.dot_text[ki] == '\n') { has_nl = 1; break; }
            if (has_nl) {
                /* Multi-line insert: redraw from insertion point. */
                ed.cur_pos = ins_pos;
                scr_adj();
                ed.cur_pos = ins_pos + ed.dot_len;
                if (ed.cur_pos > 0 && gb_char_at(ed.cur_pos - 1) != '\n')
                    ed.cur_pos--;
            } else {
                scr_adj();
            }
            scr_show_status(ed.status);
        }
        break;
    }
}
