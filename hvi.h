/*
 * hvi.h - HVI: A VI clone for CP/M
 * Author: Juan Orlandini
 * License: MIT
 *
 * Common definitions, types, and extern declarations.
 */

#ifndef HVI_H
#define HVI_H

#define HVI_VERSION "1.0"

/* Terminal defaults */
#define DEF_COLS    80
#define DEF_ROWS    24

/* Gap buffer limits */
#define GAP_MIN     256     /* gap reserve kept after each insertion */
#define BUF_MAX     30000   /* target in-memory content (actual may be less) */
#define LOAD_CHUNK  4096    /* chars loaded per page-in from tail */

/* Undo text buffer size */
#define UNDO_MAX    1024

/* Dot-repeat text buffer size */
#define DOT_TEXT_MAX 128

/* Yank buffer size */
#define YANK_MAX    1024

/* Tab stop width */
#define TAB_STOP    8

/* Search pattern buffer */
#define SEARCH_MAX  64

/* Filename/status buffer */
#define PATH_MAX    64
#define STATUS_MAX  128
#define CMD_MAX     128

/* Key code definitions */
#define KEY_NUL     0x00
#define KEY_CTRL_A  0x01
#define KEY_CTRL_B  0x02
#define KEY_CTRL_C  0x03
#define KEY_CTRL_D  0x04
#define KEY_CTRL_E  0x05
#define KEY_CTRL_F  0x06
#define KEY_CTRL_G  0x07
#define KEY_BS      0x08
#define KEY_CTRL_H  0x08
#define KEY_TAB     0x09
#define KEY_LF      0x0A
#define KEY_CTRL_K  0x0B
#define KEY_CTRL_L  0x0C
#define KEY_CR      0x0D
#define KEY_CTRL_N  0x0E
#define KEY_CTRL_P  0x0F
#define KEY_CTRL_R  0x12
#define KEY_CTRL_U  0x15
#define KEY_CTRL_W  0x17
#define KEY_ESC     0x1B
#define KEY_DEL     0x7F

/* Editor modes */
#define MODE_NORMAL  0
#define MODE_INSERT  1
#define MODE_REPLACE 2
#define MODE_CMDLINE 3

/* Undo operation types */
#define UNDO_NONE    0
#define UNDO_INSERT  1
#define UNDO_DELETE  2

/* Search direction */
#define SEARCH_FWD   1
#define SEARCH_BWD  -1

/*
 * Gap buffer: content is split around a gap.
 * [data before gap][  GAP  ][data after gap]
 * Content length = size - (gend - gstart)
 */
typedef struct {
    char *buf;      /* allocated buffer */
    int   size;     /* total allocation including gap */
    int   gstart;   /* gap start (first gap byte) */
    int   gend;     /* gap end   (first non-gap byte after gap) */
} GapBuf;

/* Single-level undo record */
typedef struct {
    int  type;              /* UNDO_INSERT or UNDO_DELETE */
    int  pos;               /* buffer position of operation */
    int  len;               /* number of characters */
    int  was_clean;         /* non-zero if buffer was unmodified before this op */
    char text[UNDO_MAX];    /* saved text (for delete undo) */
} UndoRec;

/* Editor global state */
typedef struct {
    /* Gap buffer */
    GapBuf  gb;

    /* File */
    char    filename[PATH_MAX];
    int     modified;       /* non-zero if buffer differs from disk */

    /* Cursor: byte offset in logical buffer */
    int     cur_pos;

    /* Display */
    int     top_pos;        /* buffer pos of first visible char */
    int     scr_rows;       /* terminal height */
    int     scr_cols;       /* terminal width */

    /* Mode */
    int     mode;

    /* Undo */
    UndoRec undo;

    /* Yank buffer */
    char    yank[YANK_MAX];
    int     yank_len;
    int     yank_line;      /* non-zero if yanked whole lines */

    /* Search */
    char    search[SEARCH_MAX];
    int     search_dir;

    /* Ex command line */
    char    cmdline[CMD_MAX];
    int     cmdlen;

    /* Status message (shown until next keypress) */
    char    status[STATUS_MAX];

    /* Wanted column for vertical movement */
    int     want_col;

    /* Count prefix for commands */
    int     count;

    /* Dot-repeat: replay last change with '.' */
    int     dot_cmd;            /* 0=none, else command key */
    int     dot_motion;         /* motion for d/c operators */
    int     dot_arg;            /* extra arg (replacement char for 'r') */
    int     dot_count;          /* count when originally issued */
    int     dot_len;            /* length of text for insert replay */
    char    dot_text[DOT_TEXT_MAX]; /* inserted text for replay */

    /* Debug flag */
    int     debug;

    /* Quit flag */
    int     quit;

    /* Large-file sliding window */
    long    win_start;           /* byte offset in tail_file where buffer begins */
    long    tail_offset;         /* byte offset in tail_file where buffer ends */
    char    tail_file[PATH_MAX]; /* original source file for head and tail */
} Editor;

extern Editor ed;

/* ---- gap.c ---- */
int  gb_init();
void gb_free();
int  gb_content_len();  /* logical content size (no gap) */
int  gb_char_at(/* int pos */);
int  gb_insert(/* int pos, char *text, int len */);
int  gb_delete(/* int pos, int len */);
int  gb_load(/* char *filename */);
int  gb_load_fp(/* FILE *f, char *filename */);
int  gb_save(/* char *filename */);
int  gb_load_more(/* int n */);

/* ---- term.c ---- */
void term_init();
void term_restore();
void term_clear();
void term_clreol();
void term_goto(/* int row, int col */);
void term_putch(/* int c */);
void term_puts(/* char *s */);
int  term_getch();
void term_getsize(/* int *rows, int *cols */);
void term_bold();
void term_reverse();
void term_normal();

/* ---- screen.c ---- */
void scr_refresh();
void scr_redraw_line(/* int screen_row */);
void scr_redraw_cur_line();
void scr_update_cursor();
void scr_show_status(/* char *msg */);
void scr_clear_status();
void scr_scroll_to_cursor();
int  scr_pos_line(/* int pos */);
int  scr_pos_col(/* int pos */);
int  scr_vrow_col(/* int pos */);
int  scr_line_start(/* int linenum */);
int  scr_line_end(/* int pos */);
int  scr_line_count();

/* ---- edit.c ---- */
void edit_run();

/* ---- ex.c ---- */
void ex_execute(/* char *cmd */);

#endif /* HVI_H */
