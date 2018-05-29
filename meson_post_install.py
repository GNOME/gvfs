#!/usr/bin/env python3

import os
import subprocess
import sys

if not os.environ.get('DESTDIR'):
  print('Compiling gsettings schemas...')
  subprocess.call(['glib-compile-schemas', sys.argv[1]])

  print('GIO module cache creation...')
  subprocess.call(['gio-querymodules-64', sys.argv[2]])
