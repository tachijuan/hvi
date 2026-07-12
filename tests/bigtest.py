#!/usr/bin/env python3
"""Large-file (sliding window) and paging tests for HVI."""
import sys, os, time
sys.path.insert(0, "/tmp/hvi-test")
from hvitest import Session, put_file, get_file, rm_file, check, RESULTS, file_test

def make_big():
    # ~62 KB, 2600 lines -- far beyond the 24 KB in-memory window
    lines = []
    for i in range(1, 2601):
        if i == 10:
            lines.append("line %04d HEADMARK" % i)
        elif i == 2500:
            lines.append("line %04d ZZTOP" % i)
        else:
            lines.append("line %04d abcdefghij" % i)
    return "\n".join(lines) + "\n"

def main():
    big = make_big()
    big_lines = big.split("\n")

    sess = Session()

    # --- paging on an in-memory 100-line file ---
    small = "".join("L%03d\n" % i for i in range(1, 101))
    file_test(sess, "page ^F", small, "\x06rX",
              small.replace("L033", "X033"))
    file_test(sess, "page ^F^B", small, "\x06\x02rX",
              small.replace("L012", "X012"))
    file_test(sess, "page ^D", small, "\x04rX",
              small.replace("L012", "X012"))
    file_test(sess, "page ^D^U", small, "\x04\x15rX",
              small.replace("L001", "X001"))
    sess.close(); sess = Session()

    # --- large file: open shows partial status ---
    rm_file("BIG.TXT"); put_file("BIG.TXT", big)
    t0 = time.time()
    sess.start_hvi("BIG.TXT")
    lines = sess.screen_lines()
    check("big: opens (%.1fs)" % (time.time()-t0),
          lines[0].startswith("line 0001"), "row0=%r" % lines[0])
    check("big: partial status", "Partial" in (lines[23] or ""),
          "row23=%r" % lines[23])

    # --- :wq with no edits must round-trip the whole file ---
    ok = sess.quit_expect_prompt(":wq\r", timeout=120)
    got = get_file("BIG.TXT")
    check("big: unmodified save round-trips", ok and got == big,
          "len got=%s want=%s" % (len(got or ""), len(big)))

    # --- G jumps to last line; mark it; save; verify everything ---
    rm_file("BIG.TXT"); put_file("BIG.TXT", big)
    sess.start_hvi("BIG.TXT")
    t0 = time.time()
    sess.keys("G")
    sess.c.drain(quiet=0.4, maxwait=60)
    lines = sess.screen_lines()
    gtime = time.time() - t0
    sess.keys("rX")
    ok = sess.quit_expect_prompt(":wq\r", timeout=120)
    want = big.replace("line 2600", "Xine 2600")
    got = get_file("BIG.TXT")
    check("big: G to EOF (%.1fs)" % gtime, ok and got == want,
          "tail got=%r want=%r" % ((got or "")[-40:], want[-40:]))

    # --- gg back to start from the end window ---
    rm_file("BIG.TXT"); put_file("BIG.TXT", big)
    sess.start_hvi("BIG.TXT")
    sess.keys("G")
    sess.c.drain(quiet=0.4, maxwait=60)
    sess.keys("gg")
    sess.c.drain(quiet=0.4, maxwait=60)
    sess.keys("rX")
    ok = sess.quit_expect_prompt(":wq\r", timeout=120)
    want = big.replace("line 0001", "Xine 0001")
    got = get_file("BIG.TXT")
    check("big: G then gg", ok and got == want,
          "head got=%r" % (got or "")[:30])

    # --- 2000gg goes to line 2000 across the window ---
    rm_file("BIG.TXT"); put_file("BIG.TXT", big)
    sess.start_hvi("BIG.TXT")
    t0 = time.time()
    sess.keys("2000gg")
    sess.c.drain(quiet=0.4, maxwait=60)
    ggtime = time.time() - t0
    sess.keys("rX")
    ok = sess.quit_expect_prompt(":wq\r", timeout=120)
    want = big.replace("line 2000", "Xine 2000")
    got = get_file("BIG.TXT")
    check("big: 2000gg (%.1fs)" % ggtime, ok and got == want,
          "line2000 got=%r" % ([l for l in (got or "").split("\n")
                                if "2000" in l][:1]))

    # --- search into unloaded tail ---
    rm_file("BIG.TXT"); put_file("BIG.TXT", big)
    sess.start_hvi("BIG.TXT")
    t0 = time.time()
    sess.keys("/ZZTOP\r")
    sess.c.drain(quiet=0.4, maxwait=120)
    stime = time.time() - t0
    sess.keys("rX")
    ok = sess.quit_expect_prompt(":wq\r", timeout=120)
    want = big.replace("ZZTOP", "XZTOP")
    got = get_file("BIG.TXT")
    check("big: / into unloaded tail (%.1fs)" % stime, ok and got == want,
          "match got=%r" % ([l for l in (got or "").split("\n")
                             if "ZTOP" in l][:1]))

    # --- backward search into unloaded head from EOF ---
    rm_file("BIG.TXT"); put_file("BIG.TXT", big)
    sess.start_hvi("BIG.TXT")
    sess.keys("G")
    sess.c.drain(quiet=0.4, maxwait=60)
    sess.keys("?HEADMARK\r")
    sess.c.drain(quiet=0.4, maxwait=120)
    sess.keys("rX")
    ok = sess.quit_expect_prompt(":wq\r", timeout=120)
    want = big.replace("HEADMARK", "XEADMARK")
    got = get_file("BIG.TXT")
    check("big: ? into unloaded head", ok and got == want,
          "match got=%r" % ([l for l in (got or "").split("\n")
                             if "EADMARK" in l][:1]))

    # --- edit at top, jump to end (window shift), edit, save ---
    rm_file("BIG.TXT"); put_file("BIG.TXT", big)
    sess.start_hvi("BIG.TXT")
    sess.keys("rA")            # mark line 1 (modifies buffer)
    sess.keys("G")             # window shift must flush the edit
    sess.c.drain(quiet=0.4, maxwait=60)
    sess.keys("rB")            # mark last line
    ok = sess.quit_expect_prompt(":wq\r", timeout=120)
    want = big.replace("line 0001", "Aine 0001").replace("line 2600",
                                                         "Bine 2600")
    got = get_file("BIG.TXT")
    check("big: edits survive window shift", ok and got == want,
          "head=%r tail=%r" % ((got or "")[:15], (got or "")[-30:]))

    # --- put larger than the free gap (v2.6.1 regression) ---
    # A freshly loaded window keeps only GAP_MIN (256) bytes of gap; a
    # put bigger than that must swap the window out (gb_insert_room),
    # not silently drop the yank.
    off20 = sum(len(l) + 1 for l in big_lines[:20])   # start of line 21
    off25 = sum(len(l) + 1 for l in big_lines[:25])   # start of line 26
    off1  = len(big_lines[0]) + 1                     # start of line 2

    rm_file("BIG.TXT"); put_file("BIG.TXT", big)
    sess.start_hvi("BIG.TXT")
    sess.keys("ma20jy`ap")     # ~440-byte charwise yank, put at pos 1
    ok = sess.quit_expect_prompt(":wq\r", timeout=120)
    want = big[0] + big[0:off20] + big[1:]
    got = get_file("BIG.TXT")
    check("big: put > gap charwise (y`a p)", ok and got == want,
          "lens got=%s want=%s" % (len(got or ""), len(want)))

    rm_file("BIG.TXT"); put_file("BIG.TXT", big)
    sess.start_hvi("BIG.TXT")
    sess.keys("25Yp")          # ~520-byte linewise yank, put below line 1
    ok = sess.quit_expect_prompt(":wq\r", timeout=120)
    want = big[:off1] + big[:off25] + big[off1:]
    got = get_file("BIG.TXT")
    check("big: put > gap linewise (25Y p)", ok and got == want,
          "lens got=%s want=%s" % (len(got or ""), len(want)))

    # the window swap invalidated the undo record: u must be a no-op
    rm_file("BIG.TXT"); put_file("BIG.TXT", big)
    sess.start_hvi("BIG.TXT")
    sess.keys("ma20jy`apu")
    ok = sess.quit_expect_prompt(":wq\r", timeout=120)
    want = big[0] + big[0:off20] + big[1:]
    got = get_file("BIG.TXT")
    check("big: put > gap then u (undo inert)", ok and got == want,
          "lens got=%s want=%s" % (len(got or ""), len(want)))

    # --- o at a zero-byte gap (v2.6.1 regression) ---
    # Typing exactly the free gap (256 bytes) leaves gap 0; 'o' must then
    # swap the window out for its newline, not silently stay on the line.
    rm_file("BIG.TXT"); put_file("BIG.TXT", big)
    sess.start_hvi("BIG.TXT")
    sess.keys("i" + "Q" * 256 + "\x1b")   # fill the gap exactly
    sess.keys("oNEW\x1b")                 # needs room for its newline
    ok = sess.quit_expect_prompt(":wq\r", timeout=120)
    want = "Q" * 256 + big[:off1] + "NEW\n" + big[off1:]
    got = get_file("BIG.TXT")
    check("big: o at zero gap", ok and got == want,
          "lens got=%s want=%s" % (len(got or ""), len(want)))

    # --- dot replay larger than the gap (v2.6.1 regression) ---
    # 128-byte insert leaves gap 128; the first '.' fits exactly (gap 0),
    # the second must swap the window out instead of dropping the text.
    off2 = off1 + len(big_lines[1]) + 1               # start of line 3
    rm_file("BIG.TXT"); put_file("BIG.TXT", big)
    sess.start_hvi("BIG.TXT")
    sess.keys("i" + "Q" * 128 + "\x1b")
    sess.keys("j0.")                      # replay fits exactly: gap -> 0
    sess.keys("j0.")                      # replay must make room
    ok = sess.quit_expect_prompt(":wq\r", timeout=120)
    Q = "Q" * 128
    want = Q + big[:off1] + Q + big[off1:off2] + Q + big[off2:]
    got = get_file("BIG.TXT")
    check("big: dot replay > gap", ok and got == want,
          "lens got=%s want=%s" % (len(got or ""), len(want)))

    sess.close()
    print("\n---- %d tests, %d failed ----" %
          (len(RESULTS), sum(1 for _, ok, _ in RESULTS if not ok)))
    for n, ok, d in RESULTS:
        if not ok:
            print("FAILED: %s %s" % (n, d))

if __name__ == "__main__":
    main()
