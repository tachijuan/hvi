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

    sess.close()
    print("\n---- %d tests, %d failed ----" %
          (len(RESULTS), sum(1 for _, ok, _ in RESULTS if not ok)))
    for n, ok, d in RESULTS:
        if not ok:
            print("FAILED: %s %s" % (n, d))

if __name__ == "__main__":
    main()
