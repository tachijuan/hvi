/*
 * hvi.c - Main entry point for HVI (VI clone for CP/M)
 * Author: Juan Orlandini
 * License: MIT
 *
 * Usage: hvi [filename]
 *
 * No standard library headers are included.  All string and I/O operations
 * use the custom routines in util.c and cpmio.c.
 */

#include "hvi.h"

extern int bdos_disk();   /* IX-safe BDOS wrapper (cstart.as) */

/* Global editor state -- defined here, externed everywhere else. */
Editor ed;
char msg_insert[] = "-- INSERT --";

/* Status formats shared with ex.c (one copy of each literal). */
char fmt_newfile[] = "\"%s\" [New File]";
char fmt_chars[]   = "\"%s\" %d chars";

/*
 * cstart.as reads the CP/M command tail from page zero (0x0080/0x0081)
 * in assembly -- page-zero pointer casts in C generate relocation records
 * the linker rejects -- and passes the values as normal arguments.
 */
main(cmdlen, cmdtail)
int   cmdlen;
char *cmdtail;
{
    char *cmdp;
    int   i, has_file;
    int   partial;        /* auto is safe: bdos_disk preserves IX throughout */

    /* ed is zero-initialised by cstart.as */
    ed.scr_rows     = DEF_ROWS;
    ed.scr_cols     = DEF_COLS;
    ed.search_dir   = SEARCH_FWD;
    ed.undo.type    = UNDO_NONE;
    ed.cur_line     = 0;
    ed.cur_line_pos = -1;  /* force full scan on first scr_cur_line() call */
    ed.cur_vrow     = -1;  /* force full scan on first scr_scroll_to_cursor() */

    /* Parse filename from CP/M command tail (supplied by cstart.as).
     * CP/M implementations differ on whether cmdlen counts the leading
     * space separator: some include it, some do not.  We skip leading
     * whitespace by scanning content (not by decrementing cmdlen), then
     * read until we hit another space, CR, or NUL.  The bound
     * (cmdp - cmdtail) <= cmdlen works for both conventions:
     *   - cmdlen includes space (e.g. 11 for " MYFILE.TXT"): CR stops us first.
     *   - cmdlen excludes space (e.g. 10 for " MYFILE.TXT"): bound allows
     *     reading up through offset 10 from cmdtail, which is the last char. */
    has_file = 0;
    cmdp = cmdtail;
    while (*cmdp == ' ' || *cmdp == '\t') cmdp++;
    if (*cmdp && *cmdp != '\r') {
        if (*cmdp == '-') {
            bdos_puts("Usage: hvi [filename]\r\n");
            return 1;
        }
        i = 0;
        while (i < PATH_MAX - 1 && (cmdp - cmdtail) <= cmdlen
               && *cmdp && *cmdp != ' ' && *cmdp != '\t' && *cmdp != '\r') {
            ed.filename[i++] = *cmdp++;
        }
        ed.filename[i] = '\0';
        if (i > 0) {
            /* CP/M filesystems are uppercase-only; fold the name now. */
            for (i = 0; ed.filename[i]; i++)
                if (ed.filename[i] >= 'a' && ed.filename[i] <= 'z')
                    ed.filename[i] = ed.filename[i] - 'a' + 'A';
            has_file = 1;
        }
    }

    /* --- Initialise gap buffer (takes most of the TPA heap) --- */
    if (!gb_init()) {
        bdos_puts("hvi: out of memory\r\n");
        return 1;
    }
    /* --- Load file --- */
    if (has_file) {
        bdos_puts("HVI " HVI_VERSION " - Loading file...\r\n");
        partial = gb_load(ed.filename, (HFILE *)0);
        if (partial == 2) {
            hvi_sprintf(ed.status,
                        "\"%s\" [Partial: %d chars, tail preserved]",
                        (int)ed.filename, gb_content_len());
        } else if (partial == 1) {
            hvi_sprintf(ed.status, fmt_chars,
                        (int)ed.filename, gb_content_len());
        } else {
            /* File does not exist -- start with empty buffer */
            hvi_sprintf(ed.status, fmt_newfile,
                        (int)ed.filename, 0);
        }
        ed.modified = 0;
    } else {
        hvi_strcpy(ed.status, "[No Name]");
    }

    /* --- Initialise terminal and run editor --- */
    term_init();
    edit_run();
    term_restore();

    gb_free();
    return 0;
}
