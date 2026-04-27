/*
 * ex.c - Ex command processing for HVI
 * Author: Juan Orlandini
 * License: MIT
 *
 * Handles :q :q! :w :wq :wq! :x :x! :r :e :e! and :N (go to line).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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
        scr_scroll_to_cursor();
        return 1;
    }

    /* :$ -- go to last line (loads entire tail for large files) */
    if (*p == '$') {
        int total, clen;
        while (ed.tail_offset > 0L) {
            clen = gb_content_len();
            if (clen > 0) ed.cur_pos = clen - 1;
            if (gb_load_more(LOAD_CHUNK) == 0) break;
        }
        total = scr_line_count();
        ed.cur_pos = scr_line_start(total - 1);
        ed.want_col = 0;
        scr_scroll_to_cursor();
        return 1;
    }

    do_write = 0;
    do_quit  = 0;
    force    = 0;

    /* :r filename -- read file and insert after cursor line */
    if (*p == 'r' && (p[1] == ' ' || p[1] == '\t' || p[1] == '\0')) {
        char *fname;
        FILE *f;
        int   c;
        char  tmp[2];
        int   ins_pos, old_len, new_len;

        p++;
        fname = skip_space(p);
        if (*fname == '\0') {
            strcpy(ed.status, "Usage: :r filename");
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

        if (ed.debug)
            fprintf(stderr, "ex :r '%s'\n", fname);
        f = fopen(fname, "rb");
        if (ed.debug)
            fprintf(stderr, "ex :r %s\n", f ? "ok" : "FAILED");
        if (!f) {
            sprintf(ed.status, "Cannot open: %s", fname);
            return 0;
        }
        old_len = gb_content_len();
        while ((c = fgetc(f)) != EOF) {
            if (c == 0x0D) continue;
            if (c == 0x1A) break;   /* CP/M EOF marker */
            tmp[0] = (char)c;
            gb_insert(ins_pos++, tmp, 1);
        }
        fclose(f);
        new_len = gb_content_len();
        ed.modified = 1;
        sprintf(ed.status, "\"%s\" %d chars", fname, new_len - old_len);
        return 1;
    }

    /* :e[!] filename -- abandon current buffer and edit a new file */
    if (*p == 'e' && (p[1] == ' ' || p[1] == '\t' || p[1] == '!' || p[1] == '\0')) {
        char *fname;
        p++;
        force = (*p == '!') ? (p++, 1) : 0;
        fname = skip_space(p);
        if (*fname == '\0') {
            strcpy(ed.status, "Usage: :e[!] filename");
            return 0;
        }
        if (ed.modified && !force) {
            strcpy(ed.status, "Modified buffer (use :e! to discard)");
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

        strncpy(ed.filename, fname, PATH_MAX - 1);
        ed.filename[PATH_MAX - 1] = '\0';

        rc = gb_load(fname, (FILE *)0);
        if (rc == 0)
            sprintf(ed.status, "\"%s\" [New File]", fname);
        else if (rc == 2)
            sprintf(ed.status, "\"%s\" (partial load)", fname);
        else
            sprintf(ed.status, "\"%s\" loaded", fname);
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
            strcpy(ed.status, "No filename: use :w filename");
            return 0;
        }
        ok = gb_save(dest);
        if (!ok) {
            sprintf(ed.status, "Cannot write: %s", dest);
            return 0;
        }
        /* If saved to a new name, record it */
        if (*p)
            strncpy(ed.filename, p, PATH_MAX - 1);
        ed.modified = 0;
        sprintf(ed.status, "\"%s\" written", ed.filename);
    }

    if (do_quit) {
        if (ed.modified && !force && !do_write) {
            strcpy(ed.status, "Modified buffer (use :q! to discard)");
            return 0;
        }
        ed.quit = 1;
        return 0;
    }

    if (!do_write && !do_quit) {
        sprintf(ed.status, "Unknown command: %s", cmd);
    }
    return 0;
}
