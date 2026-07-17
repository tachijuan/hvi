/*
 * ex.c - Ex command processing for HVI
 * Author: Juan Orlandini
 * License: MIT
 *
 * Handles :q :q! :w :wq :wq! :x :x! :r :e :e! :N (go to line) and
 * :[range]s/old/new/[g] (plain-text, case-sensitive substitute).
 */

#include "hvi.h"

extern Editor ed;

/* Shared with hvi.c (one copy of each literal). */
extern char fmt_newfile[];   /* "\"%s\" [New File]" */
extern char fmt_chars[];     /* "\"%s\" %d chars"   */

/* One copy each of the formats used twice below (the compiler does not
 * pool identical string literals). */
static char msg_modified[] = "Modified buffer (use :%c! to discard)";
static char msg_usage[]    = "Usage: :%c filename";

/* Shared status literals defined elsewhere (one copy each). */
extern char s_full[];    /* "Buffer full"       (edit.c) */
extern char s_nopat[];   /* "Pattern not found" (edit.c) */

/* Mark resolution shared with the operators (emove.c): me_mkc holds
 * the mark char, motion_endpoint('`') validates and reports. */
extern int me_mkc;
int  motion_endpoint();
int  apply_shift();     /* emove.c: the >>/<< engine, shared by :> :< */
extern int sh_op, sh_from, sh_to;   /* its arguments (globals) */

/* Skip leading whitespace in s; returns pointer to first non-space. */
static char *skip_space(s)
char *s;
{
    while (*s == ' ' || *s == '\t') s++;
    return s;
}

/* Shared tail for the :N and :$ line jumps: normalise the cursor state,
 * scroll, and return 1 when the viewport moved (text redraw needed). */
static int ejd_top;

static int ex_jump_done()
{
    ed.want_col = 0;
    ed.cur_vrow = -1;   /* line jump: cached cursor row is stale */
    ejd_top = ed.top_pos;
    scr_scroll_to_cursor();
    return ed.top_pos != ejd_top;
}

/* ------------------------------------------------------------------ */
/*  :[range]s/old/new/[g] -- substitute (plain text, case-sensitive)    */
/* ------------------------------------------------------------------ */

/* Range-address parser cursor (also shared by the :N handler). */
static char *xs_p;
static int   xs_n, xs_t;
static char *xs_s;

/* Length of the field at xs_p, delimited by '/' or the end of the
 * command; leaves xs_p at the delimiter. */
static int xs_len()
{
    xs_s = xs_p;
    while (*xs_p && *xs_p != '/') xs_p++;
    return xs_p - xs_s;
}

/* Record s as the buffer's filename (shared by :e and :w name). */
static void set_fname(s)
char *s;
{
    hvi_strncpy(ed.filename, s, PATH_MAX - 1);
    ed.filename[PATH_MAX - 1] = '\0';
}

/*
 * Parse one range address at xs_p: a line number (clamped to the
 * buffer), '.' (the cursor's line), '$' (the last line), or '{a-z}
 * (the mark's line; slot = char - 0x60 as in motion_endpoint).
 * Returns the line-start position; -1 when xs_p holds no address
 * (xs_p unmoved); -2 on a bad or unset mark ("Mark not set" set).
 */
static int xs_addr()
{
    if (*xs_p == '.') {
        xs_p++;
        return find_bol(ed.cur_pos);
    }
    if (*xs_p == '$') {
        xs_p++;
        return scr_last_line_start();
    }
    if (*xs_p == '\'') {
        /* motion_endpoint('`') resolves and validates the mark -- the
         * same code the operators use ("Mark not set" shown there;
         * xs_t is a dummy linewise out-param). */
        me_mkc = xs_p[1];
        xs_n = motion_endpoint('`', 1, &xs_t);
        if (xs_n < 0) return -2;
        xs_p += 2;
        return find_bol(xs_n);
    }
    if (*xs_p >= '0' && *xs_p <= '9') {
        xs_n = 0;
        while (*xs_p >= '0' && *xs_p <= '9')
            xs_n = xs_n * 10 + (*xs_p++ - '0');
        if (xs_n < 1) xs_n = 1;
        xs_t = scr_line_count();
        if (xs_n > xs_t) xs_n = xs_t;
        return scr_line_start(xs_n - 1);
    }
    return -1;
}

/*
 * :[range]s/old/new/[g] at p.  Plain-text and case-sensitive (no
 * regular expressions); 'g' replaces every occurrence in a line
 * instead of the first; the default range is the cursor's line.
 * The single-slot undo record cannot describe the scattered edits,
 * so a substitute is not undoable (the record is invalidated).
 * Returns -1 when p is not a substitute command (the caller falls
 * through to the other ex commands), else the ex_execute contract.
 */
static int   sb_p1, sb_p2, sb_end, sb_cnt, sb_pos;
static int   sb_olen, sb_nlen, sb_g, sb_t, sb_last, sb_c0;
static char *sb_old, *sb_new;
static char  sb_tmp[CMD_MAX];   /* staged match candidate (BSS: free) */

static int ex_subst(p)
char *p;
{
    xs_p = p;
    if (*xs_p == '%') {
        /* '%' is the whole-buffer range, an alias for '1,$'. */
        xs_p++;
        sb_p1 = scr_line_start(0);
        sb_p2 = scr_last_line_start();
    } else {
        sb_p1 = xs_addr();
        if (sb_p1 == -2) return 0;
        sb_p2 = sb_p1;
        if (sb_p1 >= 0 && *xs_p == ',') {
            xs_p++;
            sb_p2 = xs_addr();
            if (sb_p2 == -2) return 0;
            if (sb_p2 < 0) return -1;   /* "N,junk" is not a substitute */
        }
        if (sb_p1 < 0)
            sb_p1 = sb_p2 = find_bol(ed.cur_pos);   /* default: this line */
    }
    if (sb_p1 > sb_p2) { sb_t = sb_p1; sb_p1 = sb_p2; sb_p2 = sb_t; }

    /* :[range]> and :[range]< -- shift lines (the >>/<< engine) */
    if (*xs_p == '>' || *xs_p == '<') {
        sh_op   = *xs_p;
        sh_from = sb_p1;
        sh_to   = find_eol(sb_p2);
        apply_shift();
        return 1;
    }

    if (xs_p[0] != 's' || xs_p[1] != '/')
        return -1;

    /* Split old/new/flags on '/' in place (no escapes: '/' cannot
     * appear in either text).  Missing delimiters mean empty fields.
     * old is NUL-terminated in the command buffer so the match test
     * below is one hvi_strcmp. */
    xs_p += 2;
    sb_old = xs_p;
    sb_olen = xs_len();
    if (*xs_p) *xs_p++ = '\0';          /* terminate old, skip the '/' */
    sb_new = xs_p;
    sb_nlen = xs_len();
    sb_g = (xs_p[0] == '/' && xs_p[1] == 'g');

    /* One flat scan over [p1, end of p2's line): old cannot contain a
     * newline (read_line stores printable chars only), so a match can
     * never cross a line boundary -- no per-line loop is needed.  The
     * 'g' flag only decides whether a match skips to the next line. */
    sb_cnt = 0;
    if (sb_olen != 0) {  /* olen >= 0: cheap equality */
        sb_c0  = (int)(unsigned char)sb_old[0];
        sb_tmp[sb_olen] = '\0';         /* stage terminator, set once */
        sb_pos = sb_p1;
        sb_end = find_eol(sb_p2);       /* maintained across edits */
        for (;;) {
            /* CPIR to the next candidate first char; stage the
             * candidate (two LDIRs) and compare -- case-sensitive
             * exact match, no per-character gb_char_at calls. */
            sb_pos = gb_find_ch(sb_pos, sb_c0);
            if (sb_pos + sb_olen > sb_end) break;
            gb_copy_out(sb_tmp, sb_pos, sb_olen);
            if (hvi_strcmp(sb_tmp, sb_old)) { sb_pos++; continue; }
            gb_delete(sb_pos, sb_olen);
            if (sb_nlen && !gb_insert(sb_pos, sb_new, sb_nlen)) {
                /* gap exhausted: keep what was substituted */
                hvi_strcpy(ed.status, s_full);
                break;
            }
            sb_end += sb_nlen - sb_olen;
            sb_last = sb_pos;
            sb_cnt++;
            sb_pos += sb_nlen;          /* never rescan the new text */
            if (!sb_g)                  /* first match per line only */
                sb_pos = find_eol(sb_pos) + 1;
        }
    }
    if (sb_cnt == 0) {
        hvi_strcpy(ed.status, s_nopat);
        return 0;
    }
    ed.modified  = 1;
    ed.undo.type = UNDO_NONE;
    ed.cur_pos   = find_bol(sb_last);   /* last substitution's line */
    ex_jump_done();     /* want_col/cur_vrow reset (scroll is redone) */
    return 1;   /* full redraw; scr_refresh also shows any status */
}

/*
 * Execute an ex command string (without the leading colon).
 * Returns 1 if the viewport was changed (requiring scr_refresh), 0 otherwise.
 * Sets ed.status for feedback and ed.quit to terminate the editor.
 */
int ex_execute(cmd)
char *cmd;
{
    static char *p;
    static int   force;
    static int   do_write;
    static int   do_quit;
    static int   rc;

    p = skip_space(cmd);

    /* :[range]s/old/new/[g] -- substitute (must run before :N: a
     * range can start with digits) */
    rc = ex_subst(p);
    if (rc >= 0) return rc;

    /* :N  -- go to line N (xs_addr clamps and finds the line start) */
    if (*p >= '0' && *p <= '9') {
        xs_p = p;
        ed.marks[MARK_PREV] = ed.cur_pos;   /* `` returns here */
        ed.cur_pos = xs_addr();
        /* return 0 when the viewport is unchanged: no text redraw needed */
        return ex_jump_done();
    }

    /* :$ -- go to last line (loads entire tail for large files) */
    if (*p == '$') {
        static int clen;
        ed.marks[MARK_PREV] = ed.cur_pos;   /* `` returns here */
        while (ed.tail_offset > 0L) {
            clen = gb_content_len();
            if (clen > 0) ed.cur_pos = clen - 1;
            if (gb_load_more(LOAD_CHUNK) == 0) break;
        }
        ed.cur_pos = scr_last_line_start();
        if (!ex_jump_done() && ed.win_start == 0L && ed.tail_offset == 0L)
            return 0;       /* viewport unchanged: no text redraw needed */
        return 1;
    }

    do_write = 0;
    do_quit  = 0;
    force    = 0;

    /* :r filename -- read file and insert after cursor line */
    if (*p == 'r' && (p[1] == ' ' || p[1] == '\t' || p[1] == '\0')) {
        /* 128-byte staging chunk: one gb_insert (gap move + mark sweep)
         * per sector instead of per character. */
        static char rbuf[128];
        static char  *fname;
        static HFILE *f;
        static int    c;
        static int    rn;
        static int    ins_pos, old_len, new_len;

        p++;
        fname = skip_space(p);
        if (*fname == '\0') {
            hvi_sprintf(ed.status, msg_usage, 'r', 0);
            return 0;
        }
        hvi_fname_clean(fname);

        /* Find end of current line, insert after its newline */
        ins_pos = find_eol(ed.cur_pos);
        if (ins_pos < gb_content_len())
            ins_pos++;

        f = hvi_fopen(fname, "rb");
        if (!f) {
            hvi_sprintf(ed.status, "Cannot open: %s", (int)fname, 0);
            return 0;
        }
        old_len = gb_content_len();
        rn = 0;
        while ((c = hvi_fgetc(f)) != HEOF) {
            if (c == 0x0D) continue;
            if (c == 0x1A) break;   /* CP/M EOF marker */
            rbuf[rn++] = (char)c;
            if (rn == 128) {
                if (!gb_insert(ins_pos, rbuf, rn)) { rn = 0; break; }
                ins_pos += rn;
                rn = 0;
            }
        }
        if (rn != 0)
            gb_insert(ins_pos, rbuf, rn);
        hvi_fclose(f);
        new_len = gb_content_len();
        ed.modified = 1;
        hvi_sprintf(ed.status, fmt_chars, (int)fname, new_len - old_len);
        return 1;
    }

    /* :e[!] filename -- abandon current buffer and edit a new file */
    if (*p == 'e' && (p[1] == ' ' || p[1] == '\t' || p[1] == '!' || p[1] == '\0')) {
        static char *fname;
        p++;
        force = (*p == '!') ? (p++, 1) : 0;
        fname = skip_space(p);
        if (*fname == '\0') {
            hvi_sprintf(ed.status, msg_usage, 'e', 0);
            return 0;
        }
        hvi_fname_clean(fname);
        if (ed.modified && !force) {
            hvi_sprintf(ed.status, msg_modified, 'e', 0);
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
        ed.undo.type       = UNDO_NONE;
        ed.yank_len        = 0;
        ed.yank_line       = 0;
        ed.dot_cmd         = 0;
        ed.status[0]       = '\0';

        set_fname(fname);

        rc = gb_load(fname, (HFILE *)0);
        if (rc == 0)
            hvi_sprintf(ed.status, fmt_newfile, (int)fname, 0);
        else
            hvi_sprintf(ed.status, "\"%s\" %s", (int)fname,
                        (int)(rc == 2 ? "(partial load)" : "loaded"));
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
        static char *dest;
        static int   ok;
        if (*p) hvi_fname_clean(p);   /* match ed.tail_file's case/blanks */
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
            set_fname(p);
        ed.modified = 0;
        hvi_sprintf(ed.status, "\"%s\" written", (int)ed.filename, 0);
    }

    if (do_quit) {
        if (ed.modified && !force && !do_write) {
            hvi_sprintf(ed.status, msg_modified, 'q', 0);
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
