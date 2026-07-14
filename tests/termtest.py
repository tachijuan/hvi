#!/usr/bin/env python3
"""Terminal-family output tests for HVI.

Each non-ANSI build must (a) round-trip an edit correctly like the ANSI
build, and (b) emit ONLY control codes its terminal understands -- never an
ANSI CSI (ESC[) and never the ESC[6n size query that would garble a dumb
terminal.  This drives each family's .COM through a scripted session inside
RunCPM and asserts on the raw byte stream the terminal would receive.

The core editor logic is covered by hvitest.py against the ANSI build; this
file only checks the per-terminal escape layer, so it does not need pyte.

Usage:
  python3 termtest.py            test every family .COM already on the drive
  python3 termtest.py --build    build each family first (slow, ~40s each)
  python3 termtest.py VT52 ADM3A [--build]   only the named families
"""
import sys, os, time, subprocess
sys.path.insert(0, "/tmp/hvi-test")
from cpm import CPM

DRIVE = "/tmp/hvi-test/A/0"

# family -> traits.  addr = cursor-address lead-in bytes.
#   ildl   : has hardware insert/delete-line scroll
#   icdc   : has insert/delete-char (fast mid-line edit)
#   reverse: status bar uses a reverse-video attribute
#   no_esc : the family uses no ESC (0x1b) at all (Hazeltine)
#   tilde  : cannot display '~', substitutes '^'
#   cols   : fixed screen width
FAMILIES = {
    "VT52":    dict(com="HVIVT52", addr=b"\x1bY", ildl=False, icdc=False,
                    reverse=False, no_esc=False, tilde=False, cols=80),
    "H19":     dict(com="HVIH19",  addr=b"\x1bY", ildl=True,  icdc=False,
                    reverse=True,  no_esc=False, tilde=False, cols=80),
    "ADM3A":   dict(com="HVIADM3", addr=b"\x1b=", ildl=False, icdc=False,
                    reverse=False, no_esc=False, tilde=False, cols=80),
    "TVI":     dict(com="HVITVI",  addr=b"\x1b=", ildl=True,  icdc=True,
                    reverse=False, no_esc=False, tilde=False, cols=80),
    "WYSE50":  dict(com="HVIWY50", addr=b"\x1b=", ildl=True,  icdc=True,
                    reverse=False, no_esc=False, tilde=False, cols=80),
    "HAZ1500": dict(com="HVIHZ15", addr=b"\x7e\x11", ildl=True, icdc=False,
                    reverse=False, no_esc=True,  tilde=True,  cols=80),
    "OSB1":    dict(com="HVIOSB1", addr=b"\x1b=", ildl=False, icdc=False,
                    reverse=False, no_esc=False, tilde=False, cols=52),
}

npass = nfail = 0

def check(name, cond):
    global npass, nfail
    if cond:
        npass += 1
        print("  PASS %s" % name)
    else:
        nfail += 1
        print("  FAIL %s" % name)

def put_big(name):
    lines = ["line %02d" % i for i in range(1, 41)]
    lines[1] = "second line"          # row 2: mid-line insert target
    lines[2] = "a~b tilde row"        # row 3: tilde display test
    data = ("\r\n".join(lines) + "\r\n").encode("latin1")
    open(os.path.join(DRIVE, name), "wb").write(data + b"\x1a")

def get_lines(name):
    d = open(os.path.join(DRIVE, name), "rb").read().rstrip(b"\x1a")
    return d.replace(b"\r\n", b"\n").decode("latin1").split("\n")

def send(c, s):
    for b in s.encode("latin1"):
        os.write(c.master, bytes([b]))
        c._pump(0.002)
        time.sleep(0.012)
        if b == 0x1b:
            time.sleep(0.06)

def run_family(fam, tr):
    com = tr["com"]
    if not os.path.exists(os.path.join(DRIVE, com + ".COM")):
        print("== %s: %s.COM not built (run with --build) -- SKIP" % (fam, com))
        return
    print("== %s (%s)" % (fam, com))
    put_big("TT.TXT")
    c = CPM()
    c.expect_prompt(); time.sleep(0.3); c.drain()
    start = len(c.buf)
    c.send("%s TT.TXT\r" % com, delay=0.003)
    c.drain(quiet=0.45, maxwait=15)
    init_end = len(c.buf)

    # mid-line insert on row 2: "second" -> "seXcond"
    send(c, "jll")
    mid_start = len(c.buf)
    send(c, "iX\x1b")
    mid_end = len(c.buf)

    # force scrolling: return to the top, then step down past the screen
    send(c, "1G")
    scr_start = len(c.buf)
    for _ in range(30):
        os.write(c.master, b"j"); c._pump(0.003); time.sleep(0.012)
    c.drain(quiet=0.3, maxwait=8)
    scr_end = len(c.buf)

    send(c, ":wq\r")
    c.drain(quiet=0.3, maxwait=8)
    c.close()

    whole = c.buf[start:]
    init  = c.buf[start:init_end]
    mid   = c.buf[mid_start:mid_end]
    scr   = c.buf[scr_start:scr_end]
    lines = get_lines("TT.TXT")

    # --- universal correctness ---
    check("round-trip: mid-line insert", len(lines) > 1 and lines[1] == "seXcond line")
    check("round-trip: tilde preserved on disk",
          len(lines) > 2 and lines[2] == "a~b tilde row")
    check("never emits ANSI CSI (ESC[)", b"\x1b[" not in whole)
    check("never sends ESC[6n size query", b"\x1b[6n" not in whole)
    check("emits family address lead-in %r" % tr["addr"], tr["addr"] in whole)

    # --- family-specific ---
    if tr["no_esc"]:
        check("uses no ESC at all", b"\x1b" not in whole)
    if tr["reverse"]:
        check("status uses reverse video (ESC p/q)",
              b"\x1bp" in whole and b"\x1bq" in whole)
    else:
        check("no reverse-video ESC p", b"\x1bp" not in whole)
    if tr["tilde"]:
        check("tilde shown as '^' on screen", b"a^b" in init)
    if tr["icdc"]:
        check("mid-line insert uses insert-char (ESC Q)", b"\x1bQ" in mid)
    else:
        check("mid-line insert avoids insert-char", b"\x1bQ" not in mid)
    if tr["ildl"]:
        # scroll uses insert/delete-line, not a full ANSI region newline
        il_dl = any(x in scr for x in (b"\x1bL", b"\x1bM",     # H19
                                       b"\x1bE", b"\x1bR",     # TVI/Wyse
                                       b"\x7e\x1a", b"\x7e\x13"))  # Haz
        check("scroll uses insert/delete-line", il_dl)
    else:
        check("no insert/delete-line (full repaint scroll)",
              not any(x in scr for x in (b"\x1bL", b"\x1bE", b"\x1bR")))

    # geometry: no cursor address beyond the compiled width (offset builds)
    if tr["addr"] == b"\x1b=":
        ok = True; i = 0
        while True:
            i = whole.find(b"\x1b=", i)
            if i < 0 or i + 3 >= len(whole): break
            r, col = whole[i+2], whole[i+3]
            if col > 32 + tr["cols"] - 1 or r > 32 + 24: ok = False; break
            i += 4
        check("all ESC= coords within %dx%d" % (25, tr["cols"]), ok)

def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    do_build = "--build" in sys.argv
    fams = [a.upper() for a in args] if args else list(FAMILIES)
    for fam in fams:
        if fam not in FAMILIES:
            print("unknown family %r; choose from %s" % (fam, ", ".join(FAMILIES)))
            continue
        if do_build:
            print("== building %s ..." % fam)
            r = subprocess.run(["python3", "build.py", fam],
                               cwd="/tmp/hvi-test",
                               stdout=subprocess.DEVNULL)
            if r.returncode != 0:
                print("   build FAILED")
        run_family(fam, FAMILIES[fam])
    print("\n---- termtest: %d passed, %d failed ----" % (npass, nfail))
    sys.exit(1 if nfail else 0)

if __name__ == "__main__":
    main()
