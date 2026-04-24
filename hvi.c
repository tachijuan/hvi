/*
 * hvi.c - Main entry point for HVI (VI clone for CP/M)
 * Author: Juan Orlandini
 * License: MIT
 *
 * Usage: hvi [filename]
 *        hvi -d [filename]   (enable debug output to stderr)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "hvi.h"

/* Global editor state -- defined here, externed everywhere else. */
Editor ed;

static void usage()
{
    fprintf(stderr, "Usage: hvi [-d] [filename]\n");
    exit(1);
}

main(argc, argv)
int   argc;
char *argv[];
{
    int   i;
    int   file_arg;
    int   partial;
    FILE *preopen;

    /* --- Zero-initialise editor state --- */
    memset(&ed, 0, sizeof(ed));
    ed.scr_rows    = DEF_ROWS;
    ed.scr_cols    = DEF_COLS;
    ed.search_dir  = SEARCH_FWD;
    ed.undo.type   = UNDO_NONE;

    file_arg = -1;

    /* --- Parse arguments --- */
    for (i = 1; i < argc; i++) {
        if (argv[i][0] == '-') {
            if (argv[i][1] == 'd' || argv[i][1] == 'D') {
                ed.debug = 1;
            } else {
                usage();
            }
        } else {
            if (file_arg >= 0) usage();
            file_arg = i;
        }
    }

    /*
     * Open the file BEFORE gb_init().  gb_init() allocates nearly the
     * entire heap, leaving no room for fopen()'s internal I/O buffer.
     * Opening first guarantees fopen() has the full heap available.
     */
    preopen = (FILE *)0;
    if (file_arg >= 0) {
        strncpy(ed.filename, argv[file_arg], PATH_MAX - 1);
        ed.filename[PATH_MAX - 1] = '\0';
        preopen = fopen(ed.filename, "rb");
        if (ed.debug)
            fprintf(stderr, "pre-open '%s': %s\n",
                    ed.filename, preopen ? "ok" : "new file");
    }

    /* --- Initialise gap buffer (takes most of the heap) --- */
    if (!gb_init()) {
        if (preopen) fclose(preopen);
        fprintf(stderr, "hvi: out of memory\n");
        exit(1);
    }

    /* --- Load file using the pre-opened handle --- */
    if (file_arg >= 0) {
        if (preopen) {
            /* gb_load_fp closes preopen when done */
            partial = gb_load_fp(preopen, ed.filename);
            if (partial == 2) {
                sprintf(ed.status,
                        "\"%s\" [Partial: %d chars, tail preserved]",
                        ed.filename, gb_content_len());
            } else {
                sprintf(ed.status, "\"%s\" %d chars",
                        ed.filename, gb_content_len());
            }
        } else {
            /* File does not exist -- start with empty buffer */
            sprintf(ed.status, "\"%s\" [New File]", ed.filename);
        }
        ed.modified = 0;
    } else {
        strcpy(ed.status, "[No Name]");
    }

    /* --- Initialise terminal and run editor --- */
    term_init();
    edit_run();
    term_restore();

    gb_free();
    return 0;
}
