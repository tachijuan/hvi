/*
 * cpmio.c - CP/M BDOS direct file I/O and heap allocation for HVI
 * Author: Juan Orlandini
 * License: MIT
 *
 * Replaces <stdio.h> (fopen/fclose/fgetc/fputc/ftell/fseek/remove/rename)
 * and stdlib.h (malloc/free) with direct CP/M BDOS calls, eliminating the
 * entire HI-TECH C buffered-I/O library.
 *
 * Each HFILE has its own 128-byte sector buffer that is set as the BDOS
 * DMA address before every I/O call.  Only the operations used by HVI are
 * implemented:
 *   - Sequential read  (hvi_fgetc)
 *   - Sequential write (hvi_fputc)
 *   - Random seek      (hvi_fseek / hvi_ftell)  -- large-file tail only
 *   - Delete / rename  (hvi_remove / hvi_rename)
 *   - Single-block allocation from a static BSS array (hvi_malloc / hvi_free)
 *
 * BDOS functions used:
 *   15  Open File (sequential read)
 *   16  Close File
 *   19  Erase File
 *   20  Read Sequential (128-byte sector)
 *   21  Write Sequential (128-byte sector)
 *   22  Make File (create for write)
 *   23  Rename File
 *   26  Set DMA Address
 *   33  Read Random (sector addressed by FCB r0/r1/r2)
 */

#include <cpm.h>
#include "hvi.h"

/*
 * bdos_disk(fn, arg) -- disk-safe BDOS wrapper defined in cstart.as.
 * Identical calling convention to bdos_disk(), but saves/restores IX around
 * CALL 5 so that disk BDOS functions cannot corrupt the caller's frame.
 * Use for all disk BDOS functions (15,16,19,20,21,22,23,26,33).
 */
extern int bdos_disk();

/* ------------------------------------------------------------------ */
/*  Heap allocation -- static BSS array                                */
/* ------------------------------------------------------------------ */

/*
 * Single-shot allocator using the free TPA between end-of-BSS and BDOS.
 *
 * heap_base is set by cstart.as to __Hbss (first byte after BSS) before
 * main() runs.  bdos_base is the top of TPA.  The heap grows up, the stack
 * (initialised to BDOS by cstart.as) grows down; they never collide as long
 * as their combined size is less than the free TPA.
 *
 * HVI calls hvi_malloc exactly once (for the gap buffer); hvi_free is a no-op.
 */
static int  s_heap_used;

/*
 * Both set by cstart.as before main() is called.
 * Reading page-zero directly in C generates absolute relocation records that
 * the linker rejects (address < 0x0100 file base); cstart.as reads/computes
 * these values safely and stores them here via normal BSS variables.
 */
unsigned int bdos_base;
unsigned int heap_base;   /* set to __Hbss by cstart.as */

char *hvi_malloc(size)
int size;
{
    unsigned int free_end;

    if (s_heap_used) return (char *)0;

    free_end = bdos_base - 512;     /* 512-byte stack reserve below BDOS */

    if ((unsigned int)size > free_end - heap_base)
        return (char *)0;

    s_heap_used = 1;
    return (char *)heap_base;   /* heap starts right after BSS */
}

void hvi_free(p)
char *p;
{
    /* Single-shot allocator: nothing to release. */
}

/* ------------------------------------------------------------------ */
/*  FCB helpers                                                         */
/* ------------------------------------------------------------------ */

#define SECTOR_SZ  128

/*
 * Initialise a 36-byte FCB from a CP/M filename string (8.3, no path).
 * Drive defaults to 0 (current).  Name and extension are upper-cased
 * and space-padded.  All other FCB fields are zeroed.
 */
static void fill_fcb(fcb, name)
unsigned char *fcb;
char          *name;
{
    static int  i;   /* static: bdos calls in caller must not corrupt these */
    static char c;

    for (i = 0; i < 36; i++) fcb[i] = 0;
    for (i = 1; i <= 8;  i++) fcb[i] = ' ';  /* name field  */
    for (i = 9; i <= 11; i++) fcb[i] = ' ';  /* ext field   */

    /* Copy filename (up to 8 chars) */
    i = 1;
    while (i <= 8 && *name && *name != '.') {
        c = *name++;
        if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
        fcb[i++] = (unsigned char)c;
    }
    /* Skip any extra name chars before dot */
    while (*name && *name != '.') name++;
    if (*name == '.') name++;

    /* Copy extension (up to 3 chars) */
    i = 9;
    while (i <= 11 && *name) {
        c = *name++;
        if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
        fcb[i++] = (unsigned char)c;
    }
}

/* ------------------------------------------------------------------ */
/*  HFILE pool                                                          */
/* ------------------------------------------------------------------ */

#define MAX_HFILES 3

static HFILE s_files[MAX_HFILES];

/* ------------------------------------------------------------------ */
/*  Open / close                                                        */
/* ------------------------------------------------------------------ */

/* All locals/params cached in statics before any disk bdos call:
 * disk BDOS (fn 15/19/20/22/26) may corrupt IX on CP/M hardware. */
static HFILE *s_fop_fp;
static char  *s_fop_name;
static int    s_fop_i, s_fop_rc, s_fop_write;

HFILE *hvi_fopen(name, mode)
char *name, *mode;
{
    s_fop_name  = name;    /* cache params before any bdos call */
    s_fop_write = (*mode != 'r');

    s_fop_fp = (HFILE *)0;
    for (s_fop_i = 0; s_fop_i < MAX_HFILES; s_fop_i++) {
        if (!s_files[s_fop_i].mode) { s_fop_fp = &s_files[s_fop_i]; break; }
    }
    if (!s_fop_fp) return (HFILE *)0;

    fill_fcb(s_fop_fp->fcb, s_fop_name);
    s_fop_fp->buf_pos   = 0;
    s_fop_fp->buf_valid = 0;
    s_fop_fp->pos       = 0L;
    s_fop_fp->dirty     = 0;
    s_fop_fp->at_eof    = 0;

    bdos_disk(26, (int)s_fop_fp->buf);  /* set DMA; disk bdos -- may corrupt IX */

    if (!s_fop_write) {
        s_fop_rc = bdos_disk(15, (int)s_fop_fp->fcb);   /* BDOS 15: Open File */
        if ((s_fop_rc & 0xFF) == 0xFF) return (HFILE *)0;
        s_fop_fp->fcb[0]  = 0;               /* BDOS 15 may set drive; keep current (0) */
        s_fop_fp->mode    = 1;
        s_fop_fp->fcb[32] = 0;               /* CR = 0 */
    } else {
        /* Create: erase any existing file first (may fail silently) */
        bdos_disk(19, (int)s_fop_fp->fcb);        /* BDOS 19: Erase File */
        fill_fcb(s_fop_fp->fcb, s_fop_name); /* re-init FCB after erase */
        s_fop_rc = bdos_disk(22, (int)s_fop_fp->fcb);   /* BDOS 22: Make File */
        if ((s_fop_rc & 0xFF) == 0xFF) return (HFILE *)0;
        s_fop_fp->fcb[0]  = 0;               /* BDOS 22 may set drive; keep current (0) */
        s_fop_fp->mode    = 2;
        s_fop_fp->fcb[32] = 0;               /* CR = 0 */
    }

    return s_fop_fp;
}

/* Write the current sector buffer (BDOS 21) and mark it clean.
 * s_fw_fp is static: bdos calls may corrupt IX-relative auto vars. */
static HFILE *s_fw_fp;
static int    s_fw_i;

static void wr_sector(fp)
HFILE *fp;
{
    s_fw_fp = fp;
    bdos_disk(26, (int)s_fw_fp->buf);
    bdos_disk(21, (int)s_fw_fp->fcb);   /* BDOS 21: Write Sequential */
    s_fw_fp->buf_pos = 0;
    s_fw_fp->dirty   = 0;
}

/* Pad and flush the current write sector then mark buffer clean. */
static void flush_write(fp)
HFILE *fp;
{
    s_fw_fp = fp;
    if (!s_fw_fp->dirty) return;
    for (s_fw_i = s_fw_fp->buf_pos; s_fw_i < SECTOR_SZ; s_fw_i++)
        s_fw_fp->buf[s_fw_i] = 0x1A;
    wr_sector(s_fw_fp);
}

/*
 * Load 128-byte sector `sect` of fp into its buffer via BDOS 33
 * (Read Random).  Sets the FCB random-record fields r0/r1/r2 and CR
 * (some emulators track position via CR rather than r0/r1/r2).
 * Shared by hvi_fgetc refills and hvi_fseek.
 * Returns the BDOS result (0 = ok).
 * All statics: disk BDOS calls may corrupt IX-relative auto vars.
 */
static HFILE *s_rs_fp;
static long   s_rs_sect;

static int rd_sector(fp, sect)
HFILE *fp;
long   sect;
{
    s_rs_fp   = fp;        /* cache params before any bdos call */
    s_rs_sect = sect;
    s_rs_fp->fcb[32] = (unsigned char)(s_rs_sect & 0x7FL);
    s_rs_fp->fcb[33] = (unsigned char)( s_rs_sect        & 0xFFL);
    s_rs_fp->fcb[34] = (unsigned char)((s_rs_sect >>  8) & 0xFFL);
    s_rs_fp->fcb[35] = (unsigned char)((s_rs_sect >> 16) & 0x7FL);
    bdos_disk(26, (int)s_rs_fp->buf);
    return bdos_disk(33, (int)s_rs_fp->fcb);   /* BDOS 33: Read Random */
}

/* s_fcl_fp is static: bdos calls may corrupt IX-relative auto vars. */
static HFILE *s_fcl_fp;

void hvi_fclose(fp)
HFILE *fp;
{
    s_fcl_fp = fp;
    if (!s_fcl_fp || !s_fcl_fp->mode) return;
    if (s_fcl_fp->mode == 2) {
        flush_write(s_fcl_fp);
        bdos_disk(26, (int)s_fcl_fp->buf);
        bdos_disk(16, (int)s_fcl_fp->fcb);   /* BDOS 16: Close File */
    }
    /* Skip BDOS 16 for read-only files: some emulators write back the FCB's
     * CR/EX fields to the directory on close, which corrupts the on-disk
     * extent record.  The next BDOS 15 (Open) then loads the wrong allocation
     * map, causing BDOS 33 to return stale sector-0 data for any subsequent
     * sector.  Read-only files never need directory update on close. */
    s_fcl_fp->mode = 0;
}

/* ------------------------------------------------------------------ */
/*  Sequential character I/O                                            */
/* ------------------------------------------------------------------ */

/*
 * hvi_fgetc: use BDOS 33 (Read Random) for every sector refill instead of
 * BDOS 20 (Read Sequential).
 *
 * BDOS 33 does not update CR/EX/S2 after a random read on CP/M 2.2, so
 * sequential BDOS 20 calls re-read the same sector.  Setting CR/EX/S2
 * manually in hvi_fseek should fix this but some implementations ignore
 * the manual update or reset CR internally.  The bulletproof approach:
 * derive the next sector number from fp->pos (which we track ourselves)
 * and always issue an explicit BDOS 33, eliminating all dependence on
 * CR/EX/S2 state.
 *
 * All statics: disk BDOS calls may corrupt IX-relative auto vars.
 */
static HFILE *s_fgc_fp;
static int    s_fgc_rc;

#ifdef HVI_DEBUG
/* Print a 4-bit nibble as a hex digit via BDOS 2. */
static void dbg_nib(n)
int n;
{
    n &= 0xF;
    bdos_disk(2, n < 10 ? '0' + n : 'A' + n - 10);
}
/* Print a byte as two hex digits. */
static void dbg_byte(b)
int b;
{
    dbg_nib(b >> 4);
    dbg_nib(b);
}
/* Print a long as 8 hex digits (sector numbers are small so only low 16 shown). */
static void dbg_long(v)
long v;
{
    dbg_byte((int)((v >> 8) & 0xFF));
    dbg_byte((int)( v       & 0xFF));
}
/* Print a NUL-terminated string via BDOS 2. */
static void dbg_str(s)
char *s;
{
    while (*s) bdos_disk(2, (int)(unsigned char)*s++);
}
#endif /* HVI_DEBUG */

int hvi_fgetc(fp)
HFILE *fp;
{
    s_fgc_fp = fp;

    if (!s_fgc_fp || s_fgc_fp->mode != 1) return HEOF;
    if (s_fgc_fp->at_eof)                  return HEOF;

    if (s_fgc_fp->buf_pos >= s_fgc_fp->buf_valid) {
        /* Refill: sector = current byte position >> 7 (fp->pos tracks it). */
        s_fgc_rc = rd_sector(s_fgc_fp, s_fgc_fp->pos >> 7);
        if (s_fgc_rc != 0) { s_fgc_fp->at_eof = 1; return HEOF; }
        s_fgc_fp->buf_pos   = 0;
        s_fgc_fp->buf_valid = SECTOR_SZ;
#ifdef HVI_DEBUG
        /* Print: "R sect=NNNN rc=R b0=HH b1=HH b2=HH b3=HH\r\n" */
        dbg_str("R sect=");
        dbg_long(s_fgc_sect);
        dbg_str(" rc=");
        dbg_byte(s_fgc_rc);
        dbg_str(" b0=");
        dbg_byte((int)(unsigned char)s_fgc_fp->buf[0]);
        dbg_str(" b1=");
        dbg_byte((int)(unsigned char)s_fgc_fp->buf[1]);
        dbg_str(" b2=");
        dbg_byte((int)(unsigned char)s_fgc_fp->buf[2]);
        dbg_str(" b3=");
        dbg_byte((int)(unsigned char)s_fgc_fp->buf[3]);
        dbg_str("\r\n");
#endif
    }

    s_fgc_fp->pos++;
    return (int)(unsigned char)s_fgc_fp->buf[s_fgc_fp->buf_pos++];
}

int hvi_fputc(c, fp)
int    c;
HFILE *fp;
{
    if (!fp || fp->mode != 2) return HEOF;

    fp->buf[fp->buf_pos++] = (unsigned char)c;
    fp->pos++;
    fp->dirty = 1;

    if (fp->buf_pos >= SECTOR_SZ)
        wr_sector(fp);
    return c;
}

/* ------------------------------------------------------------------ */
/*  Random seek / position                                              */
/* ------------------------------------------------------------------ */

long hvi_ftell(fp)
HFILE *fp;
{
    if (!fp || !fp->mode) return -1L;
    return fp->pos;
}

/*
 * Seek to an absolute byte offset in a read-mode file (whence == 0).
 * Sector = offset >> 7 (avoids division library).
 * Sets FCB random-record fields r0/r1/r2 and calls BDOS 33 (Read Random)
 * to load that sector, then positions buf_pos within it.
 * After this call, sequential hvi_fgetc() will continue from that point.
 *
 * BUG FIX: The sector must be computed from the full 32-bit offset.
 * The previous code cast offset to unsigned int (16-bit) BEFORE shifting,
 * truncating the sector number for any offset >= 32768 bytes.
 * Fix: shift the long first, then extract each FCB byte separately.
 *
 * s_fsk_byte_off is static: bdos_disk may corrupt IX-relative auto vars.
 */
static HFILE *s_fsk_fp;
static long   s_fsk_offset;
static int    s_fsk_byte_off;
static int    s_fsk_rc;

int hvi_fseek(fp, offset, whence)
HFILE *fp;
long   offset;
int    whence;
{
    s_fsk_fp     = fp;       /* cache params before any bdos call */
    s_fsk_offset = offset;

    if (!s_fsk_fp || s_fsk_fp->mode != 1) return -1;

    /* Low 7 bits = byte position within the 128-byte sector. */
    s_fsk_byte_off = (int)(s_fsk_offset & 0x7FL);

    /* Sector number = offset >> 7 as a long shift: a 16-bit cast before
     * shifting would truncate for offsets >= 32 KB. */
    s_fsk_rc = rd_sector(s_fsk_fp, s_fsk_offset >> 7);
    if (s_fsk_rc != 0) return -1;

    s_fsk_fp->buf_pos   = s_fsk_byte_off;
    s_fsk_fp->buf_valid = SECTOR_SZ;
    s_fsk_fp->pos       = s_fsk_offset;
    s_fsk_fp->at_eof    = 0;
    return 0;
}

/*
 * Return the approximate file size in bytes by probing the last readable
 * 128-byte sector with BDOS 33 (Read Random).
 *
 * Uses exponential expansion followed by binary search so only O(log N)
 * seeks are needed (~21 seeks for a 1 MB file).  Works on any CP/M that
 * supports BDOS 33 (which hvi_fseek already relies on), without requiring
 * BDOS 35 (Compute File Size) which some emulators do not implement.
 *
 * Returns the byte position of the first byte PAST the last valid sector
 * (i.e., the sector-rounded file size).  The caller should subtract the
 * desired window and pass the result to gb_reload_from().
 *
 * All locals static: every sub-call may corrupt IX-relative auto vars.
 */
static HFILE *s_fsz_f;
static long   s_fsz_lo;
static long   s_fsz_hi;
static long   s_fsz_mid;

long hvi_fsize(name)
char *name;
{
    s_fsz_f = hvi_fopen(name, "rb");
    if (!s_fsz_f) return 0L;

    /* Exponential expansion: double the probe offset until seek fails. */
    s_fsz_lo = 0L;
    s_fsz_hi = (long)LOAD_CHUNK;
    while (hvi_fseek(s_fsz_f, s_fsz_hi, 0) == 0) {
        s_fsz_lo = s_fsz_hi;
        s_fsz_hi <<= 1;
        if (s_fsz_hi > 8388480L) break;   /* cap at ~8 MB (CP/M max) */
    }
    /* s_fsz_lo = last successful seek; s_fsz_hi = first failed (or cap). */

    /* Binary search to find the last readable 128-byte sector. */
    while (s_fsz_hi - s_fsz_lo > 128L) {
        s_fsz_mid = ((s_fsz_lo + s_fsz_hi) >> 1) & ~127L;
        if (hvi_fseek(s_fsz_f, s_fsz_mid, 0) == 0)
            s_fsz_lo = s_fsz_mid;
        else
            s_fsz_hi = s_fsz_mid;
    }

    hvi_fclose(s_fsz_f);
    return s_fsz_lo + 128L;  /* first byte past the last valid sector */
}

/* ------------------------------------------------------------------ */
/*  File delete and rename                                              */
/* ------------------------------------------------------------------ */

void hvi_remove(name)
char *name;
{
    unsigned char fcb[36];
    fill_fcb(fcb, name);
    bdos_disk(19, (int)fcb);                /* BDOS 19: Erase File */
}

/*
 * Rename oldname to newname.
 * BDOS 23 (Rename) takes a 32-byte buffer: bytes 0-15 = old FCB name,
 * bytes 16-31 = new FCB name (first 16 bytes of each FCB).
 */
void hvi_rename(oldname, newname)
char *oldname, *newname;
{
    unsigned char ren[32], tmp[36];
    int i;

    fill_fcb(tmp, oldname);
    for (i = 0; i < 16; i++) ren[i]      = tmp[i];
    fill_fcb(tmp, newname);
    for (i = 0; i < 16; i++) ren[i + 16] = tmp[i];

    bdos_disk(23, (int)ren);                /* BDOS 23: Rename File */
}
