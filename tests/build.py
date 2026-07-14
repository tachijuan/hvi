#!/usr/bin/env python3
"""Build HVI.COM inside RunCPM, running compiler passes manually
(RunCPM's internal CCP cannot exec the driver's $EXEC chain loader).

Pass chain for `C -CPM -O -C X.C` (recovered from $$EXEC.$$$):
  CPP -DCPM -DHI_TECH_C -Dz80 -I X.C $CTMP1.$$$
  P1 T1.TMP T2.TMP T3.TMP
  CGEN T2.TMP T1.TMP
  OPTIM T1.TMP T2.TMP
  ZAS -J -N -oX.OBJ $CTMP2.$$$

Terminal family (compile-time, see termcfg.h):
  python3 build.py            -> ANSI/VT100, output HVI.COM (default)
  python3 build.py VT52       -> -DTERM_VT52, output HVIVT52.COM
  python3 build.py OSB1 TERM_ROWS=24   -> extra -D defines after the family
"""
import sys, time, os, glob
sys.path.insert(0, "/tmp/hvi-test")
from cpm import CPM

SRCS = ["hvi", "gap", "term", "screen", "emove", "edit",
        "erepeat", "ex", "util", "cpmio"]

# terminal token -> (compile define, 8.3 output .COM name)
TERMS = {
    "VT52":    ("TERM_VT52",    "HVIVT52.COM"),
    "H19":     ("TERM_H19",     "HVIH19.COM"),
    "ADM3A":   ("TERM_ADM3A",   "HVIADM3.COM"),
    "TVI":     ("TERM_TVI",     "HVITVI.COM"),
    "WYSE50":  ("TERM_WYSE50",  "HVIWY50.COM"),
    "HAZ1500": ("TERM_HAZ1500", "HVIHZ15.COM"),
    "OSB1":    ("TERM_OSB1",    "HVIOSB1.COM"),
}

# extra -D flags for the CPP pass, and the target .COM name, from argv
def term_config():
    defs = ""
    comname = "HVI.COM"
    args = sys.argv[1:]
    if args:
        tok = args[0].upper()
        if tok not in TERMS:
            print("unknown terminal %r; choose from %s or none (ANSI)"
                  % (args[0], ", ".join(sorted(TERMS))))
            sys.exit(2)
        d, comname = TERMS[tok]
        defs = " -D" + d
        for extra in args[1:]:           # e.g. TERM_ROWS=24
            defs += " -D" + extra
    return defs, comname

def clean_temps():
    for p in set(glob.glob("/tmp/hvi-test/A/0/$*") +
                 glob.glob("/tmp/hvi-test/A/0/*.$$$") +
                 glob.glob("/tmp/hvi-test/A/0/T?.TMP")):
        if p.endswith("$EXEC.COM"):
            continue
        try:
            os.remove(p)
        except FileNotFoundError:
            pass

def run(c, cmdline, timeout=600):
    out = c.cmd(cmdline, timeout=timeout).decode("latin1")
    return out

def main():
    termdefs, comname = term_config()
    if termdefs:
        print("== terminal build:%s -> %s" % (termdefs, comname))
    clean_temps()
    log = open("/tmp/hvi-test/build.log", "wb")
    c = CPM(log=log)
    c.expect_prompt()
    time.sleep(0.5)
    c.drain()

    print("== preparing LX.LIB")
    print(run(c, "PIP LX.LIB=LIBC.LIB", 60)[-120:])
    print(run(c, "LIBR d LX.LIB csv.obj", 60)[-200:])

    print("== assembling cstart.as")
    print(run(c, "ZAS -J -N -oCSTART.OBJ CSTART.AS", 300)[-300:])

    failed = []
    for s in SRCS:
        u = s.upper()
        print("== compiling %s.c" % s)
        t0 = time.time()
        o1 = run(c, "CPP -DCPM -DHI_TECH_C -Dz80%s -I %s.C T1.TMP" % (termdefs, u))
        o2 = run(c, "P1 T1.TMP T2.TMP T3.TMP")
        o3 = run(c, "CGEN T2.TMP T1.TMP")
        o4 = run(c, "OPTIM T1.TMP T2.TMP")
        o5 = run(c, "ZAS -J -N -o%s.OBJ T2.TMP" % u)
        for name, o in zip(["CPP","P1","CGEN","OPTIM","ZAS"], [o1,o2,o3,o4,o5]):
            # strip the command echo line, show any diagnostics
            body = "\n".join(o.split("\r\n")[1:-1]).strip()
            if body:
                print("  [%s] %s" % (name, body[:400]))
        obj = "/tmp/hvi-test/A/0/%s.OBJ" % u
        if os.path.exists(obj) and os.path.getsize(obj) > 0:
            print("  -> %s.OBJ %d bytes (%.0fs)" % (u, os.path.getsize(obj), time.time()-t0))
        else:
            print("  !! %s.OBJ missing" % u)
            failed.append(s)

    if failed:
        print("FAILED:", failed)
        c.close()
        sys.exit(1)

    print("== linking")
    c.send("L\r")
    c.expect("link> ", timeout=60)
    c.send("-Ptext=100H,data,bss -C100H -oh.com CSTART.OBJ CPMIO.OBJ UTIL.OBJ \\\r", delay=0.002)
    c.expect("link> ", timeout=60)
    out = run(c, "GAP.OBJ TERM.OBJ SCREEN.OBJ EMOVE.OBJ EREPEAT.OBJ EX.OBJ EDIT.OBJ HVI.OBJ LX.LIB", 300)
    print(out)

    run(c, "ERA %s" % comname)
    print(run(c, "REN %s=H.COM" % comname))
    print(run(c, "STAT %s" % comname))
    c.close()

if __name__ == "__main__":
    main()
