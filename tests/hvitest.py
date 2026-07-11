#!/usr/bin/env python3
"""End-to-end functional tests for HVI running inside RunCPM.

Each test: write input file(s) into the CP/M drive, start HVI, send
keystrokes, quit (default :wq), then compare the saved file against the
expected content.  A pyte VT100 screen tracks display output so tests can
also assert on what the user would see.

Key strings may embed:
  \x1b  ESC   (a 50 ms pause is inserted automatically after ESC)
  \r    Enter
Control chars are sent as-is.
"""
import sys, os, time, re
sys.path.insert(0, "/tmp/hvi-test")
from cpm import CPM
import pyte

DRIVE = "/tmp/hvi-test/A/0"

def put_file(name, content):
    """Write a CP/M text file (LF -> CRLF, ^Z terminator)."""
    data = content.replace("\r\n", "\n").replace("\n", "\r\n").encode("latin1")
    open(os.path.join(DRIVE, name), "wb").write(data + b"\x1a")

def get_file(name):
    """Read a CP/M text file back (strip ^Z padding, CRLF -> LF)."""
    p = os.path.join(DRIVE, name)
    if not os.path.exists(p):
        return None
    d = open(p, "rb").read()
    d = d.rstrip(b"\x1a")
    return d.replace(b"\r\n", b"\n").decode("latin1")

def rm_file(name):
    p = os.path.join(DRIVE, name)
    if os.path.exists(p):
        os.remove(p)

class Session:
    """One RunCPM session; runs HVI repeatedly."""
    def __init__(self):
        self.c = CPM()
        self.c.expect_prompt()
        time.sleep(0.3)
        self.c.drain()
        self.screen = pyte.Screen(80, 24)
        self.stream = pyte.ByteStream(self.screen)
        self.scr_from = 0

    def feed_screen(self):
        """Feed any new output into the pyte screen."""
        self.c._pump(0.02)
        new = self.c.buf[self.scr_from:]
        if new:
            self.stream.feed(new)
            self.scr_from = len(self.c.buf)

    def screen_lines(self):
        self.feed_screen()
        return [row.rstrip() for row in self.screen.display]

    def start_hvi(self, arg):
        self.screen.reset()
        self.c.send("HVI %s\r" % arg if arg else "HVI\r", delay=0.002)
        # wait for initial paint to go quiet
        self.c.drain(quiet=0.35, maxwait=15)
        self.feed_screen()

    def keys(self, s, delay=0.004):
        data = s.encode("latin1") if isinstance(s, str) else s
        i = 0
        while i < len(data):
            b = data[i]
            if b == 0x1b and i + 2 < len(data) and data[i+1:i+2] == b"[":
                # ANSI arrow sequence: deliver all 3 bytes atomically
                os.write(self.c.master, data[i:i+3])
                i += 3
                self.c._pump(0.001)
                time.sleep(delay)
                continue
            os.write(self.c.master, bytes([b]))
            i += 1
            self.c._pump(0.001)
            time.sleep(delay)
            if b == 0x1b:
                time.sleep(0.05)   # let HVI's ESC-sequence poll expire
        self.c.drain(quiet=0.25, maxwait=10)
        self.feed_screen()

    def quit_expect_prompt(self, seq=":wq\r", timeout=30):
        self.keys(seq)
        end = time.time() + timeout
        prompt = re.compile(rb"[A-P]\d?>\s*$")
        while time.time() < end:
            self.c._pump(0.05)
            if prompt.search(self.c.buf[-8:]):
                self.c.mark = len(self.c.buf)
                self.feed_screen()
                return True
        return False

    def alive_prompt(self):
        """True if the CCP prompt is at end of buffer."""
        self.c._pump(0.1)
        return re.search(rb"[A-P]\d?>\s*$", self.c.buf[-8:]) is not None

    def close(self):
        self.c.close()

# ---------------------------------------------------------------- tests

RESULTS = []

def check(name, cond, detail=""):
    RESULTS.append((name, bool(cond), detail))
    print("%s %s %s" % ("PASS" if cond else "FAIL", name,
                        ("- " + detail) if (detail and not cond) else ""))

def norm(s):
    return s if s is None else s.rstrip("\n") + "\n" if s else s

BASIC = "alpha bravo charlie\ndelta echo foxtrot\ngolf hotel india\njuliet kilo lima\nmike november oscar\n"

def file_test(sess, name, content, keys, expected, quit_seq=":wq\r",
              fname="T.TXT"):
    """Run one keystroke test; verify saved file equals expected."""
    rm_file(fname)
    if content is not None:
        put_file(fname, content)
    sess.start_hvi(fname)
    if keys:
        sess.keys(keys)
    ok = sess.quit_expect_prompt(quit_seq)
    if not ok:
        check(name, False, "editor did not exit cleanly")
        return False
    got = get_file(fname)
    good = (got == expected)
    check(name, good, "expected %r got %r" % (expected, got))
    return good

def main():
    sess = Session()

    # ---------- movement (marker technique: rX marks the cursor) ----------
    file_test(sess, "move h/l", "abcdef\n", "4l2hrX", "abXdef\n")
    file_test(sess, "move j", "one\ntwo\nthree\n", "jrX", "one\nXwo\nthree\n")
    file_test(sess, "move k", "one\ntwo\nthree\n", "jjkrX", "one\nXwo\nthree\n")
    file_test(sess, "move w", "foo bar baz\n", "wrX", "foo Xar baz\n")
    file_test(sess, "move 2w", "foo bar baz\n", "2wrX", "foo bar Xaz\n")
    file_test(sess, "move b", "foo bar baz\n", "$brX", "foo bar Xaz\n")
    file_test(sess, "move e", "foo bar baz\n", "erX", "foX bar baz\n")
    file_test(sess, "move 0", "  indented line\n", "$0rX", "X indented line\n")
    file_test(sess, "move ^", "  indented line\n", "$^rX", "  Xndented line\n")
    file_test(sess, "move $", "abc def\n", "$rX", "abc deX\n")
    file_test(sess, "move Enter", "one\n   two\nthree\n", "\rrX",
              "one\n   Xwo\nthree\n")
    file_test(sess, "move G", BASIC, "GrX",
              BASIC.replace("mike", "Xike"))
    file_test(sess, "move gg", BASIC, "GggrX",
              BASIC.replace("alpha", "Xlpha"))
    file_test(sess, "move 3gg", BASIC, "3ggrX",
              BASIC.replace("golf", "Xolf"))
    file_test(sess, "move 3G", BASIC, "3GrX",
              BASIC.replace("golf", "Xolf"))
    file_test(sess, "find f", "abcabc\n", "fcrX", "abXabc\n")
    file_test(sess, "find ;", "abcabc\n", "fc;rX", "abcabX\n")
    file_test(sess, "find ,", "abcabc\n", "fc;,rX", "abXabc\n")
    file_test(sess, "find F", "abcabc\n", "$FarX", "abcXbc\n")

    # arrow keys (ANSI sequences)
    file_test(sess, "arrow right+down", "one\ntwo\n",
              "\x1b[C\x1b[BrX", "one\ntXo\n")
    sess.close(); sess = Session()

    # ---------- insert family ----------
    file_test(sess, "insert i", "world\n", "ihello \x1b", "hello world\n")
    file_test(sess, "insert a", "world\n", "aX\x1b", "wXorld\n")
    file_test(sess, "insert I", "  foo\n", "Ibar \x1b", "  bar foo\n")
    file_test(sess, "insert A", "foo\n", "A bar\x1b", "foo bar\n")
    file_test(sess, "insert o", "one\nthree\n", "otwo\x1b", "one\ntwo\nthree\n")
    file_test(sess, "insert O", "one\nthree\n", "jOtwo\x1b", "one\ntwo\nthree\n")
    file_test(sess, "subst s", "abc\n", "sX\x1b", "Xbc\n")
    file_test(sess, "subst 2s", "abcdef\n", "2sXY\x1b", "XYcdef\n")
    file_test(sess, "subst S", "foo\nbar\n", "Snew\x1b", "new\nbar\n")
    file_test(sess, "insert Enter", "ab\n", "aX\rY\x1b", "aX\nYb\n")
    file_test(sess, "insert BS", "xyz\n", "iabc\x08\x08\x1b", "axyz\n")
    file_test(sess, "insert ^W", "z\n", "ifoo bar\x17baz\x1b", "foo bazz\n")
    file_test(sess, "insert ^U", "q\n", "iabc\x15xy\x1b", "xyq\n")
    sess.close(); sess = Session()

    # ---------- delete / change ----------
    file_test(sess, "del x", "abc\n", "x", "bc\n")
    file_test(sess, "del 3x", "abcdef\n", "3x", "def\n")
    file_test(sess, "del X", "abc\n", "$X", "ac\n")
    file_test(sess, "del dd", "one\ntwo\nthree\n", "jdd", "one\nthree\n")
    file_test(sess, "del 2dd", "a\nb\nc\nd\n", "2dd", "c\nd\n")
    file_test(sess, "del dw", "foo bar\n", "dw", "bar\n")
    file_test(sess, "del db", "foo bar\n", "$db", "foo r\n")
    file_test(sess, "del d$", "abcdef\n", "2ld$", "ab\n")
    file_test(sess, "del d0", "abcdef\n", "3ld0", "def\n")
    file_test(sess, "del dG", "one\ntwo\nthree\n", "jdG", "one\n")
    file_test(sess, "del D", "abcdef\n", "2lD", "ab\n")
    file_test(sess, "chg cc", "foo\nbar\n", "ccnew\x1b", "new\nbar\n")
    file_test(sess, "chg cw", "foo bar\n", "cwqux\x1b", "qux bar\n")
    file_test(sess, "chg c$", "abcdef\n", "2lc$XY\x1b", "abXY\n")
    file_test(sess, "chg C", "abcdef\n", "2lCXY\x1b", "abXY\n")
    file_test(sess, "repl r", "abc\n", "rZ", "Zbc\n")
    file_test(sess, "repl r Enter", "abcd\n", "lr\r", "a\ncd\n")
    file_test(sess, "join J", "one\ntwo\n", "J", "one two\n")
    file_test(sess, "join 3J", "a\nb\nc\nd\n", "3J", "a b c\nd\n")
    file_test(sess, "tilde ~", "abc\n", "~~", "ABc\n")
    sess.close(); sess = Session()

    # ---------- yank / put ----------
    file_test(sess, "yank yy p", "one\ntwo\n", "yyp", "one\none\ntwo\n")
    file_test(sess, "yank yy P", "one\ntwo\n", "jyyP", "one\ntwo\ntwo\n")
    file_test(sess, "yank Y p", "one\ntwo\n", "Yp", "one\none\ntwo\n")
    file_test(sess, "yank yw p", "foo bar\n", "yw$p", "foo barfoo \n")
    file_test(sess, "yank y$ p", "abc\n", "y$0p", "aabcbc\n")
    file_test(sess, "del then p", "one\ntwo\n", "ddjp".replace("j",""),
              "two\none\n")  # dd then p puts the deleted line below

    # ---------- search ----------
    file_test(sess, "search /", BASIC, "/echo\rrX",
              BASIC.replace("echo", "Xcho"))
    file_test(sess, "search / case-insens", BASIC, "/ECHO\rrX",
              BASIC.replace("echo", "Xcho"))
    file_test(sess, "search ?", BASIC, "G?bravo\rrX",
              BASIC.replace("bravo", "Xravo"))
    file_test(sess, "search n wrap", "aaa\nbbb\naaa x\n", "/aaa\rnrX",
              "Xaa\nbbb\naaa x\n")
    file_test(sess, "search N", "aaa\nbbb\naaa x\n", "/aaa\rnNrX",
              "aaa\nbbb\nXaa x\n")
    sess.close(); sess = Session()

    # ---------- dot repeat ----------
    file_test(sess, "dot x.", "abcdef\n", "x..", "def\n")
    file_test(sess, "dot dd.", "a\nb\nc\nd\n", "dd.", "c\nd\n")
    file_test(sess, "dot dw.", "aa bb cc dd\n", "dw.", "cc dd\n")
    file_test(sess, "dot i.", "xyz\n", "iab\x1bl.", "ababxyz\n")
    file_test(sess, "dot cw.", "foo bar\n", "cwZZ\x1bw.", "ZZ ZZ\n")

    # ---------- undo ----------
    file_test(sess, "undo x", "abc\n", "xu", "abc\n")
    file_test(sess, "undo dd", "a\nb\n", "ddu", "a\nb\n")
    file_test(sess, "undo insert", "abc\n", "iZZ\x1bu", "abc\n")
    file_test(sess, "undo p", "one\ntwo\n", "yypu", "one\ntwo\n")
    # undo restores clean state; :q must exit without complaint
    rm_file("T.TXT"); put_file("T.TXT", "abc\n")
    sess.start_hvi("T.TXT")
    sess.keys("xu")
    ok = sess.quit_expect_prompt(":q\r", timeout=10)
    check("undo restores clean (:q exits)", ok)
    if not ok:
        sess.quit_expect_prompt(":q!\r")
    sess.close(); sess = Session()

    # ---------- ex commands ----------
    # :w then :q! -- only first change saved
    rm_file("T.TXT"); put_file("T.TXT", "abcd\n")
    sess.start_hvi("T.TXT")
    sess.keys("x:w\r")
    sess.keys("x")
    ok = sess.quit_expect_prompt(":q!\r")
    check("ex :w / :q!", ok and get_file("T.TXT") == "bcd\n",
          "got %r" % get_file("T.TXT"))

    # :w NEWFILE
    rm_file("T.TXT"); rm_file("T2.TXT"); put_file("T.TXT", "hello\n")
    sess.start_hvi("T.TXT")
    ok = sess.quit_expect_prompt(":w T2.TXT\r:q\r")
    check("ex :w newfile", ok and get_file("T2.TXT") == "hello\n",
          "got %r" % get_file("T2.TXT"))

    # :x
    file_test(sess, "ex :x", "abcd\n", "x", "bcd\n", quit_seq=":x\r")

    # :q on modified buffer refuses, :q! then quits
    rm_file("T.TXT"); put_file("T.TXT", "abc\n")
    sess.start_hvi("T.TXT")
    sess.keys("x:q\r")
    still_alive = not sess.alive_prompt()
    lines = sess.screen_lines()
    warned = any("Modified buffer" in (l or "") for l in lines)
    ok = sess.quit_expect_prompt(":q!\r")
    check("ex :q refuses on modified", still_alive and warned and ok,
          "alive=%s warned=%s exited=%s" % (still_alive, warned, ok))

    # :e other file
    rm_file("T.TXT"); rm_file("T2.TXT")
    put_file("T.TXT", "first\n"); put_file("T2.TXT", "second\n")
    sess.start_hvi("T.TXT")
    sess.keys(":e T2.TXT\r")
    sess.keys("A!\x1b")
    ok = sess.quit_expect_prompt(":wq\r")
    check("ex :e other file", ok and get_file("T2.TXT") == "second!\n"
          and get_file("T.TXT") == "first\n",
          "T2=%r T=%r" % (get_file("T2.TXT"), get_file("T.TXT")))

    # :e! discards changes
    rm_file("T.TXT"); rm_file("T2.TXT")
    put_file("T.TXT", "first\n"); put_file("T2.TXT", "second\n")
    sess.start_hvi("T.TXT")
    sess.keys("x")           # modify
    sess.keys(":e T2.TXT\r") # should refuse
    lines = sess.screen_lines()
    refused = any("Modified buffer" in (l or "") for l in lines)
    sess.keys(":e! T2.TXT\r")
    ok = sess.quit_expect_prompt(":q\r")
    check("ex :e refuses / :e! discards", refused and ok
          and get_file("T.TXT") == "first\n",
          "refused=%s exited=%s T=%r" % (refused, ok, get_file("T.TXT")))

    # :r file
    rm_file("T.TXT"); rm_file("T2.TXT")
    put_file("T.TXT", "one\nthree\n"); put_file("T2.TXT", "two\n")
    sess.start_hvi("T.TXT")
    sess.keys(":r T2.TXT\r")
    ok = sess.quit_expect_prompt(":wq\r")
    check("ex :r file", ok and get_file("T.TXT") == "one\ntwo\nthree\n",
          "got %r" % get_file("T.TXT"))

    # :N goto line
    file_test(sess, "ex :N goto line", BASIC, ":4\rrX",
              BASIC.replace("juliet", "Xuliet"))
    # :$ last line
    file_test(sess, "ex :$ last line", BASIC, ":$\rrX",
              BASIC.replace("mike", "Xike"))

    # new (nonexistent) file
    rm_file("T3.TXT")
    sess.start_hvi("T3.TXT")
    sess.keys("ihello new\x1b")
    ok = sess.quit_expect_prompt(":wq\r")
    check("new file create", ok and get_file("T3.TXT") == "hello new\n",
          "got %r" % get_file("T3.TXT"))
    rm_file("T3.TXT")

    # no filename; :w NAME
    rm_file("T4.TXT")
    sess.start_hvi("")
    sess.keys("inoname\x1b")
    ok = sess.quit_expect_prompt(":w T4.TXT\r:q\r")
    check("no-name :w file", ok and get_file("T4.TXT") == "noname\n",
          "got %r" % get_file("T4.TXT"))
    rm_file("T4.TXT")
    sess.close(); sess = Session()

    # ---------- put/undo regressions ----------
    # linewise p below a last line that lacks its trailing newline must
    # start a new line, not glue onto the last one
    file_test(sess, "ddp at EOF no trailing NL", "one\ntwo\nthree", "jddp",
              "one\nthree\ntwo\n")
    file_test(sess, "ddp with trailing NL", "one\ntwo\nthree\n", "jddp",
              "one\nthree\ntwo\n")
    # undo of a put must remove exactly the pasted bytes
    file_test(sess, "undo linewise p", "one\ntwo\nthree\n", "jddpu",
              "one\nthree\n")
    file_test(sess, "undo charwise p", "abcdef\n", "2ylpu", "abcdef\n")

    # ---------- marks (m / `) ----------
    # `a returns to the exact position (line AND column) of the mark
    file_test(sess, "mark jump exact", "alpha bravo\ndelta echo\ngolf hotel\n",
              "jllmagg`arX", "alpha bravo\ndeXta echo\ngolf hotel\n")
    # `` returns to the position before the last jump (here: G)
    file_test(sess, "mark backtick return", BASIC, "G``rX",
              BASIC.replace("alpha", "Xlpha"))
    # `` again toggles back to where the first `` came from
    file_test(sess, "mark backtick toggle", BASIC, "G````rX",
              BASIC.replace("mike", "Xike"))
    # a search records the previous position for ``
    file_test(sess, "mark after search", BASIC, "/mike\r``rX",
              BASIC.replace("alpha", "Xlpha"))
    # :N records the previous position for ``
    file_test(sess, "mark after :N", BASIC, ":4\r``rX",
              BASIC.replace("alpha", "Xlpha"))
    # marks shift with edits made before them
    file_test(sess, "mark tracks delete", "one\ntwo\nthree\n",
              "jjmaggdd`arX", "two\nXhree\n")
    file_test(sess, "mark tracks insert", "one\ntwo\n",
              "jmaggOnew\x1b`arX", "new\none\nXwo\n")

    # ---------- screen checks ----------
    rm_file("T.TXT"); put_file("T.TXT", BASIC)
    sess.start_hvi("T.TXT")
    lines = sess.screen_lines()
    check("screen shows text", lines[0] == "alpha bravo charlie",
          "row0=%r" % lines[0])
    check("screen tildes", all((lines[r] == "~") for r in range(6, 22)),
          "rows6-21=%r" % lines[6:10])
    check("screen status has filename", "T.TXT" in (lines[23] or ""),
          "row23=%r" % lines[23])
    sess.keys("i")
    lines = sess.screen_lines()
    check("screen INSERT mode", "-- INSERT --" in (lines[23] or ""),
          "row23=%r" % lines[23])
    sess.keys("\x1b")
    sess.quit_expect_prompt(":q!\r")

    # tab rendering
    rm_file("T.TXT"); put_file("T.TXT", "a\tb\n")
    sess.start_hvi("T.TXT")
    lines = sess.screen_lines()
    check("screen tab expands to col 8", lines[0] == "a" + " " * 7 + "b",
          "row0=%r" % lines[0])
    sess.quit_expect_prompt(":q\r")

    # long line wraps
    rm_file("T.TXT"); put_file("T.TXT", "x" * 100 + "\n" + "second\n")
    sess.start_hvi("T.TXT")
    lines = sess.screen_lines()
    check("screen long line wraps",
          lines[0] == "x" * 80 and lines[1] == "x" * 20
          and lines[2] == "second",
          "rows=%r" % lines[:3])
    sess.quit_expect_prompt(":q\r")
    sess.close()

    # ---------- screen checks: minimal-redraw paths ----------
    sess = Session()

    # x / u / dw / P / r / ~ repaint only the edited row (or cell)
    rm_file("T.TXT"); put_file("T.TXT", BASIC)
    sess.start_hvi("T.TXT")
    sess.keys("jwx")                       # delete 'e' of 'echo' on row 1
    lines = sess.screen_lines()
    check("screen x one row",
          lines[1] == "delta cho foxtrot" and
          lines[0] == "alpha bravo charlie" and
          lines[2] == "golf hotel india",
          "rows=%r" % lines[:3])
    sess.keys("u")
    lines = sess.screen_lines()
    check("screen undo x", lines[1] == "delta echo foxtrot",
          "row1=%r" % lines[1])
    sess.keys("dw")                        # delete 'echo '
    lines = sess.screen_lines()
    check("screen dw", lines[1] == "delta foxtrot", "row1=%r" % lines[1])
    sess.keys("u")                         # restore 'echo '
    sess.keys("ywP")                       # yank + put: duplicate word
    lines = sess.screen_lines()
    check("screen P charwise", lines[1] == "delta echo echo foxtrot",
          "row1=%r" % lines[1])
    sess.keys("rE")                        # in-place single-cell replace
    lines = sess.screen_lines()
    check("screen r in place", lines[1] == "delta Echo echo foxtrot",
          "row1=%r" % lines[1])
    sess.keys("3~")                        # in-place span toggle: Ech -> eCH
    lines = sess.screen_lines()
    check("screen ~ span", lines[1] == "delta eCHo echo foxtrot",
          "row1=%r" % lines[1])
    sess.keys("A tail\x1b")                # append + fast insert-exit
    lines = sess.screen_lines()
    check("screen insert-exit row", lines[1].endswith("foxtrot tail"),
          "row1=%r" % lines[1])
    check("screen insert-exit status", "T.TXT" in (lines[23] or ""),
          "row23=%r" % lines[23])
    sess.quit_expect_prompt(":q!\r")

    # :N must place the terminal cursor on the target row
    rm_file("T.TXT"); put_file("T.TXT", BASIC)
    sess.start_hvi("T.TXT")
    sess.keys(":4\r")
    sess.feed_screen()
    check("screen :N cursor row", sess.screen.cursor.y == 3,
          "cursor.y=%d" % sess.screen.cursor.y)
    sess.quit_expect_prompt(":q\r")

    # J and dd repaint the shifted rows below the cursor
    rm_file("T.TXT"); put_file("T.TXT", "one\ntwo\nthree\n")
    sess.start_hvi("T.TXT")
    sess.keys("J")
    lines = sess.screen_lines()
    check("screen J", lines[0] == "one two" and lines[1] == "three"
          and lines[2] == "~", "rows=%r" % lines[:3])
    sess.keys("dd")
    lines = sess.screen_lines()
    check("screen dd", lines[0] == "three" and lines[1] == "~",
          "rows=%r" % lines[:2])
    sess.quit_expect_prompt(":q!\r")

    # Ctrl-D / Ctrl-U scroll the region instead of repainting everything
    rm_file("T.TXT")
    put_file("T.TXT", "".join("line %02d\n" % i for i in range(1, 41)))
    sess.start_hvi("T.TXT")
    sess.keys("\x04\x04\x04")              # 3 half-pages down: top -> line 12
    lines = sess.screen_lines()
    check("screen ^D scroll", lines[0] == "line 12" and lines[22] == "line 34",
          "row0=%r row22=%r" % (lines[0], lines[22]))
    sess.keys("\x15")                      # back: cursor stays in view
    lines = sess.screen_lines()
    check("screen ^U stable", lines[0] == "line 12", "row0=%r" % lines[0])
    sess.keys("\x15\x15")                  # scrolls back to the top
    lines = sess.screen_lines()
    check("screen ^U scroll", lines[0] == "line 01" and lines[22] == "line 23",
          "row0=%r row22=%r" % (lines[0], lines[22]))
    sess.quit_expect_prompt(":q\r")

    # growing a line across the wrap boundary during insert
    rm_file("T.TXT"); put_file("T.TXT", "b" * 79 + "\nsecond\n")
    sess.start_hvi("T.TXT")
    sess.keys("AXY\x1b")                   # 79 -> 81 chars: line wraps
    lines = sess.screen_lines()
    check("screen wrap grow",
          lines[0] == "b" * 79 + "X" and lines[1] == "Y"
          and lines[2] == "second",
          "rows=%r" % [lines[0][-3:], lines[1], lines[2]])
    sess.quit_expect_prompt(":q!\r")

    # a blank line typed between text lines must stay blank (stray ~)
    rm_file("T.TXT")
    sess.start_hvi("T.TXT")
    sess.keys("iaaa\rbbb\r\rccc\x1b")
    lines = sess.screen_lines()
    check("screen typed blank line",
          lines[0] == "aaa" and lines[1] == "bbb" and lines[2] == "" and
          lines[3] == "ccc" and lines[4] == "~",
          "rows=%r" % lines[:5])
    sess.quit_expect_prompt(":q!\r")

    # mid-line Enter: the row above must lose the tail that moved down
    rm_file("T.TXT"); put_file("T.TXT", "ab\n")
    sess.start_hvi("T.TXT")
    sess.keys("aX\rY\x1b")
    lines = sess.screen_lines()
    check("screen mid-line Enter", lines[0] == "aX" and lines[1] == "Yb",
          "rows=%r" % lines[:2])
    sess.quit_expect_prompt(":q!\r")

    # append at the true end of buffer (no trailing newline): the cursor
    # must sit after the last char, and typed text must append in place
    rm_file("T.TXT"); put_file("T.TXT", "one\ntwo\nthree")
    sess.start_hvi("T.TXT")
    sess.keys("G$a")
    sess.feed_screen()
    check("screen EOF append cursor",
          sess.screen.cursor.y == 2 and sess.screen.cursor.x == 5,
          "cursor=(%d,%d)" % (sess.screen.cursor.y, sess.screen.cursor.x))
    sess.keys("XY\x1b")
    lines = sess.screen_lines()
    check("screen EOF append text", lines[2] == "threeXY",
          "row2=%r" % lines[2])
    ok = sess.quit_expect_prompt(":wq\r")
    check("file EOF append", ok and get_file("T.TXT") == "one\ntwo\nthreeXY\n",
          "got %r" % get_file("T.TXT"))

    # a longer-than-STATUS_MAX message must not overflow ed.status into
    # the adjacent editor state (hvi_sprintf caps each %s expansion).
    # Pre-fix, the overflow corrupted want_col, so the trailing j landed
    # at the wrong column instead of column 0.
    rm_file("T.TXT"); put_file("T.TXT", "one\ntwo\n")
    sess.start_hvi("T.TXT")
    # (the >80-char echo and message wrap the emulated screen, so the
    #  message may land on any row -- search them all)
    sess.keys(":" + "z" * 120 + "\r")      # "Unknown command: zzz..."
    lines = sess.screen_lines()
    check("screen long ex command",
          any("Unknown command:" in (l or "") for l in lines),
          "rows=%r" % lines[20:])
    sess.keys(":r " + "y" * 120 + "\r")    # "Cannot open: yyy..."
    lines = sess.screen_lines()
    check("screen long :r filename",
          any("Cannot open:" in (l or "") for l in lines),
          "rows=%r" % lines[20:])
    sess.keys("jrX")                       # want_col must still be 0
    ok = sess.quit_expect_prompt(":wq\r")
    check("file after long ex command",
          ok and get_file("T.TXT") == "one\nXwo\n",
          "got %r" % get_file("T.TXT"))

    # marks: jumping to a deleted or never-set mark reports an error
    rm_file("T.TXT"); put_file("T.TXT", "one\ntwo\nthree\n")
    sess.start_hvi("T.TXT")
    sess.keys("jmagg2dd`a")                # 2dd removes the marked text
    lines = sess.screen_lines()
    check("screen mark deleted", "Mark not set" in (lines[23] or ""),
          "row23=%r" % lines[23])
    sess.keys("`z")                        # never set
    lines = sess.screen_lines()
    check("screen mark unset", "Mark not set" in (lines[23] or ""),
          "row23=%r" % lines[23])
    sess.quit_expect_prompt(":q!\r")

    # shrinking a line back across the wrap boundary (dirty-flag path)
    rm_file("T.TXT"); put_file("T.TXT", "c" * 81 + "\nsecond\n")
    sess.start_hvi("T.TXT")
    sess.keys("A\x08\x08\x1b")             # 81 -> 79 chars: line unwraps
    lines = sess.screen_lines()
    check("screen wrap shrink",
          lines[0] == "c" * 79 and lines[1] == "second",
          "rows=%r" % [lines[0][-3:], lines[1]])
    sess.quit_expect_prompt(":q!\r")

    # ---------- drive / user-area prefixes (du:) ----------
    # RunCPM maps user areas to subdirectories: A/1, B/0, B/5, ...
    U1 = "/tmp/hvi-test/A/1"
    B0 = "/tmp/hvi-test/B/0"
    B5 = "/tmp/hvi-test/B/5"
    for d in (U1, B0, B5):
        os.makedirs(d, exist_ok=True)

    def put_at(dirp, name, content):
        data = content.replace("\n", "\r\n").encode("latin1")
        open(os.path.join(dirp, name), "wb").write(data + b"\x1a")

    def get_at(dirp, name):
        p = os.path.join(dirp, name)
        if not os.path.exists(p):
            return None
        d = open(p, "rb").read().rstrip(b"\x1a")
        return d.replace(b"\r\n", b"\n").decode("latin1")

    def rm_at(dirp, name):
        p = os.path.join(dirp, name)
        if os.path.exists(p):
            os.remove(p)

    # edit a file living in user area 1, from user 0
    rm_file("U.TXT"); rm_at(U1, "U.TXT")
    put_at(U1, "U.TXT", "user one file\n")
    sess.start_hvi("1:U.TXT")
    sess.keys("rX")
    sess.quit_expect_prompt(":wq\r")
    check("user edit 1:file", get_at(U1, "U.TXT") == "Xser one file\n",
          "got %r" % get_at(U1, "U.TXT"))
    check("user edit no stray copy", get_file("U.TXT") is None,
          "file leaked into user 0")

    # :w into another user area
    rm_at(U1, "OUT.TXT")
    file_test(sess, "user :w 1:name", "cross user\n", ":w 1:OUT.TXT\r",
              "cross user\n", quit_seq=":q\r")
    check("user :w 1:name content", get_at(U1, "OUT.TXT") == "cross user\n",
          "got %r" % get_at(U1, "OUT.TXT"))

    # :r from another user area
    put_at(U1, "INC.TXT", "included\n")
    file_test(sess, "user :r 1:name", "top\n", ":r 1:INC.TXT\r",
              "top\nincluded\n")

    # :w with a drive prefix, and with drive+user combined
    rm_at(B0, "BOUT.TXT"); rm_at(B5, "BU.TXT")
    file_test(sess, "drive :w B:name", "on drive b\n", ":w B:BOUT.TXT\r",
              "on drive b\n", quit_seq=":q\r")
    check("drive :w B:name content", get_at(B0, "BOUT.TXT") == "on drive b\n",
          "got %r" % get_at(B0, "BOUT.TXT"))
    file_test(sess, "du :w B5:name", "drive b user 5\n", ":w B5:BU.TXT\r",
              "drive b user 5\n", quit_seq=":q\r")
    check("du :w B5:name content", get_at(B5, "BU.TXT") == "drive b user 5\n",
          "got %r" % get_at(B5, "BU.TXT"))

    # :e across user areas, then back to an unprefixed file
    put_at(U1, "E.TXT", "edit me\n")
    rm_file("PLAIN.TXT"); put_file("PLAIN.TXT", "plain\n")
    sess.start_hvi("PLAIN.TXT")
    sess.keys(":e 1:E.TXT\r")
    sess.keys("rY")
    sess.quit_expect_prompt(":wq\r")
    check("user :e 1:name", get_at(U1, "E.TXT") == "Ydit me\n",
          "got %r" % get_at(U1, "E.TXT"))
    check("user :e keeps plain intact", get_file("PLAIN.TXT") == "plain\n",
          "got %r" % get_file("PLAIN.TXT"))
    sess.close()

    # ---------- summary ----------
    print("\n---- %d tests, %d failed ----" %
          (len(RESULTS), sum(1 for _, ok, _ in RESULTS if not ok)))
    for n, ok, d in RESULTS:
        if not ok:
            print("FAILED: %s %s" % (n, d))

if __name__ == "__main__":
    main()
