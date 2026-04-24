/*
 * ex.c - Ex command processing for HVI
 * Author: Juan Orlandini
 * License: MIT
 *
 * Handles :q :q! :w :wq :wq! :x :x! :r and :N (go to line).
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
 * Sets ed.status for feedback and ed.quit to terminate the editor.
 */
void ex_execute(cmd)
char *cmd;
{
    char *p;
    int   force;
    int   do_write;
    int   do_quit;

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
        scr_refresh();
        return;
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
        scr_refresh();
        return;
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
            return;
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
            return;
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
        scr_refresh();
        return;
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
            return;
        }
        ok = gb_save(dest);
        if (!ok) {
            sprintf(ed.status, "Cannot write: %s", dest);
            return;
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
            return;
        }
        ed.quit = 1;
        return;
    }

    if (!do_write && !do_quit) {
        sprintf(ed.status, "Unknown command: %s", cmd);
    }
}
