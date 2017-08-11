#!/usr/bin/env python3

import os
import subprocess
import sys

if not os.environ.get('DESTDIR'):
  schemadir = os.path.join(sys.argv[1], 'glib-2.0', 'schemas')
  print('Compiling gsettings schemas...')
  subprocess.call(['glib-compile-schemas', schemadir])

  giomoduledir = os.path.join(sys.argv[2], 'gio', 'modules')
  print('GIO module cache creation...')
  subprocess.call(['gio-querymodules', giomoduledir])
