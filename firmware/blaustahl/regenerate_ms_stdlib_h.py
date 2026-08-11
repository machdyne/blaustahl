#!/usr/bin/env python3
# Regenerates ms_stdlib.h from the real ms_stdlib.l that ships with the
# ms submodule. Run this after updating the ms submodule to a newer
# upstream commit, in case ms_stdlib.l changed.
#
# Usage: python3 regenerate_ms_stdlib_h.py path/to/ms_stdlib.l
# (writes ms_stdlib.h into the current directory)

import sys

if len(sys.argv) != 2:
    print("usage: regenerate_ms_stdlib_h.py path/to/ms_stdlib.l")
    sys.exit(1)

with open(sys.argv[1]) as f:
    content = f.read()

escaped = (content
    .replace('\\', '\\\\')
    .replace('"', '\\"')
    .replace('\n', '\\n"\n"'))

with open("ms_stdlib.h", "w") as out:
    out.write("/* generated from the real machdyne/ms ms_stdlib.l -- see\n")
    out.write(" * this directory's regenerate_ms_stdlib_h.py to regenerate. */\n\n")
    out.write('static const char ms_stdlib_l[] =\n"' + escaped + '";\n')

print("wrote ms_stdlib.h")
