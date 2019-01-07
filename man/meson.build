xsltproc = find_program('xsltproc', required: false)
assert(xsltproc.found(), 'xsltproc is required for man pages generation')

xsltproc_cmd = [
  xsltproc,
  '--output', '@OUTPUT@',
  '--nonet',
  '--stringparam', 'man.output.quietly', '1',
  '--stringparam', 'funcsynopsis.style', 'ansi',
  '--stringparam', 'man.th.extra1.suppress', '1',
  '--stringparam', 'man.authors.section.enabled', '0',
  '--stringparam', 'man.copyright.section.enabled', '0',
  'http://docbook.sourceforge.net/release/xsl/current/manpages/docbook.xsl',
  '@INPUT@',
]

mans = [
  'gvfsd',
  'gvfsd-fuse',
  'gvfsd-metadata',
]

foreach man: mans
  xml = man + '.xml'
  output = man + '.1'

  custom_target(
    output,
    input: xml,
    output: output,
    command: xsltproc_cmd,
    install: true,
    install_dir: gvfs_mandir / 'man1',
  )
endforeach

man = 'gvfs'
output = man + '.7'

custom_target(
  output,
  input: man + '.xml',
  output: output,
  command: xsltproc_cmd,
  install: true,
  install_dir: gvfs_mandir / 'man7',
)
