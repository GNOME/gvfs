#!/usr/bin/env python3

'''
FIXME

This script is used only to call gdbus-codegen and simulate the
generation of the source code and header as different targets.

Both are generated implicitly, so meson is not able to know how
many files are generated, so it does generate only one opaque
target that represents the two files.

Please see:
   https://bugzilla.gnome.org/show_bug.cgi?id=791015
   https://github.com/mesonbuild/meson/pull/2930
'''

import subprocess
import sys

subprocess.call([
  'gdbus-codegen',
  '--interface-prefix=' + sys.argv[1],
  '--generate-c-code=' + sys.argv[2],
  '--c-namespace=' + sys.argv[3],
  '--output-directory=' + sys.argv[4],
  sys.argv[5]
])
