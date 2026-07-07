#!/usr/bin/env python3
"""Supplemental checks for the size-reduction refactor."""
import sys
sys.path.insert(0, "/tmp/hvi-test")
from hvitest import Session, file_test, RESULTS

sess = Session()
# ~ now honors a count (was: toggled one char regardless)
file_test(sess, "tilde 3~", "abcd\n", "3~rX", "ABCX\n")
# ~ running past end of line leaves cursor on last char
file_test(sess, "tilde at EOL", "ab\n", "9~rX", "AX\n")
# word motions still exact after delegation to motion_endpoint
file_test(sess, "w mid-punct", "foo.bar baz\n", "wrX", "fooXbar baz\n")
file_test(sess, "e twice", "ab cd\n", "eerX", "ab cX\n")
file_test(sess, "b from mid-word", "foo bar\n", "$hbrX", "foo Xar\n")
# 2dw across words (operator uses same scan)
file_test(sess, "2dw", "aa bb cc\n", "2dw", "cc\n")
# J joins with exactly one space (vi rules)
file_test(sess, "J basic", "one\ntwo\n", "J", "one two\n")
file_test(sess, "J trailing space", "one \ntwo\n", "J", "one two\n")
file_test(sess, "J leading blanks", "one\n   two\n", "J", "one two\n")
file_test(sess, "J next line empty", "one\n\nx\n", "J", "one\nx\n")
file_test(sess, "J this line empty", "\ntwo\n", "J", "two\n")
file_test(sess, "J dot repeat", "a\nb\nc\n", "J.", "a b c\n")
sess.close()

print("\n---- %d tests, %d failed ----" %
      (len(RESULTS), sum(1 for _, ok, _ in RESULTS if not ok)))
for n, ok, d in RESULTS:
    if not ok:
        print("FAILED: %s %s" % (n, d))
