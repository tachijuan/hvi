/*
 * ex.c - Ex command processing for HVI
 * Author: Juan Orlandini
 * License: MIT
 *
 * Handles :q :q! :w :wq :wq! :x :x! :r :e :e! and :N (go to line).
 */

#include "hvi.h"

extern Editor ed;

/* Skip leading whitespace in s; returns pointer to first non-space. */
static char *skip_space(s)
char *s;
{
    while (*s == ' ' || *s == '\t') s++;
    return s;
}

/*
 * Execute an ex command string (without the leading colon).
 * Returns 1 if the viewport was changed (requiring scr_refresh), 0 otherwise.
 * Sets ed.status for feedback and ed.quit to terminate the editor.
 */
int ex_execute(cmd)
char *cmd;
{
    char *p;
    int   force;
    int   do_write;
    int   do_quit;
    int   rc;

    p = skip_space(cmd);

    /* :N  -- go to line N */
    if (*p >= '0' && *p <= '9') {
        int lnum = 0;
        int old_top;
        while (*p >= '0' && *p <= '9')
            lnum = lnum * 10 + (*p++ - '0');
        if (lnum < 1) lnum = 1;
        lnum--;   /* 0-based */
        {
            int total = scr_line_count();
            if (lnum >= total) lnum = total - 1;
            ed.cur_pos = scr_line_start(lnum);
            ed.want_col = 0;
        }
        ed.cur_vrow = -1;   /* line jump: cached cursor row is stale */
        old_top = ed.top_pos;
        scr_scroll_to_cursor();
        if (ed.top_pos == old_top)
            return 0;       /* viewport unchanged: no text redraw needed */
        return 1;
    }

    /* :$ -- go to last line (loads entire tail for large files) */
    if (*p == '$') {
        int clen;
        int old_top;
        while (ed.tail_offset > 0L) {
            clen = gb_content_len();
            if (clen > 0) ed.cur_pos = clen - 1;
            if (gb_load_more(LOAD_CHUNK) == 0) break;
        }
        ed.cur_pos = scr_last_line_start();
        ed.want_col = 0;
        ed.cur_vrow = -1;   /* line jump: cached cursor row is stale */
        old_top = ed.top_pos;
        scr_scroll_to_cursor();
        if (ed.top_pos == old_top && ed.win_start == 0L && ed.tail_offset == 0L)
            return 0;       /* viewport unchanged: no text redraw needed */
        return 1;
    }

    do_write = 0;
    do_quit  = 0;
    force    = 0;

    /* :r filename -- read file and insert after cursor line */
    if (*p == 'r' && (p[1] == ' ' || p[1] == '\t' || p[1] == '\0')) {
        char  *fname;
        HFILE *f;
        int    c;
        char   tmp[2];
        int    ins_pos, old_len, new_len;

        p++;
        fname = skip_space(p);
        if (*fname == '\0') {
            hvi_strcpy(ed.status, "Usage: :r filename");
            return 0;
        }

        /* Find end of current line, insert after newline */
        ins_pos = ed.cur_pos;
        {
            int size = gb_content_len();
            while (ins_pos < size && gb_char_at(ins_pos) != '\n')
                ins_pos++;
            if (ins_pos < size)
                ins_pos++;   /* skip the newline */
        }

        f = hvi_fopen(fname, "rb");
        if (!f) {
            hvi_sprintf(ed.status, "Cannot open: %s", (int)fname, 0);
            return 0;
        }
        old_len = gb_content_len();
        while ((c = hvi_fgetc(f)) != HEOF) {
            if (c == 0x0D) continue;
            if (c == 0x1A) break;   /* CP/M EOF marker */
            tmp[0] = (char)c;
            gb_insert(ins_pos++, tmp, 1);
        }
        hvi_fclose(f);
        new_len = gb_content_len();
        ed.modified = 1;
        hvi_sprintf(ed.status, "\"%s\" %d chars", (int)fname, new_len - old_len);
        return 1;
    }

    /* :e[!] filename -- abandon current buffer and edit a new file */
    if (*p == 'e' && (p[1] == ' ' || p[1] == '\t' || p[1] == '!' || p[1] == '\0')) {
        char *fname;
        p++;
        force = (*p == '!') ? (p++, 1) : 0;
        fname = skip_space(p);
        if (*fname == '\0') {
            hvi_strcpy(ed.status, "Usage: :e[!] filename");
            return 0;
        }
        if (ed.modified && !force) {
            hvi_sprintf(ed.status, "Modified buffer (use :%c! to discard)",
                        'e', 0);
            return 0;
        }

        /* Reset gap buffer to empty without reallocating. */
        ed.gb.gstart = 0;
        ed.gb.gend   = ed.gb.size;

        /* Reset all cursor, viewport, and edit state. */
        ed.cur_pos         = 0;
        ed.top_pos         = 0;
        ed.modified        = 0;
        ed.win_start       = 0L;
        ed.tail_offset     = 0L;
        ed.tail_file[0]    = '\0';
        ed.cur_line        = 0;
        ed.cur_line_pos    = -1;
        ed.line_cnt_cached = 0;
        ed.want_col        = 0;
        ed.count           = 0;
        ed.undo.type       = UNDO_NONE;
        ed.yank_len        = 0;
        ed.yank_line       = 0;
        ed.dot_cmd         = 0;
        ed.status[0]       = '\0';

        hvi_strncpy(ed.filename, fname, PATH_MAX - 1);
        ed.filename[PATH_MAX - 1] = '\0';

        rc = gb_load(fname, (HFILE *)0);
        if (rc == 0)
            hvi_sprintf(ed.status, "\"%s\" [New File]", (int)fname, 0);
        else if (rc == 2)
            hvi_sprintf(ed.status, "\"%s\" (partial load)", (int)fname, 0);
        else
            hvi_sprintf(ed.status, "\"%s\" loaded", (int)fname, 0);
        return 1;
    }

    /* Parse w / q / x / wq combinations with optional ! */
    while (*p == 'w' || *p == 'q' || *p == 'x') {
        if (*p == 'w') do_write = 1;
        if (*p == 'q') do_quit  = 1;
        if (*p == 'x') { do_write = 1; do_quit = 1; }
        p++;
    }
    if (*p == '!') { force = 1; p++; }

    /* Optional filename argument for :w */
    p = skip_space(p);

    if (do_write) {
        char *dest;
        int   ok;
        dest = (*p) ? p : ed.filename;
        if (!dest || !dest[0]) {
            hvi_strcpy(ed.status, "No filename: use :w filename");
            return 0;
        }
        ok = gb_save(dest);
        if (!ok) {
            hvi_sprintf(ed.status, "Cannot write: %s", (int)dest, 0);
            return 0;
        }
        /* If saved to a new name, record it */
        if (*p)
            hvi_strncpy(ed.filename, p, PATH_MAX - 1);
        ed.modified = 0;
        hvi_sprintf(ed.status, "\"%s\" written", (int)ed.filename, 0);
    }

    if (do_quit) {
        if (ed.modified && !force && !do_write) {
            hvi_sprintf(ed.status, "Modified buffer (use :%c! to discard)",
                        'q', 0);
            return 0;
        }
        ed.quit = 1;
        return 0;
    }

    if (!do_write && !do_quit) {
        hvi_sprintf(ed.status, "Unknown command: %s", (int)cmd, 0);
    }
    return 0;
}
