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

mans = {
  'gvfs': '7',
  'gvfsd': '1',
  'gvfsd-fuse': '1',
  'gvfsd-metadata': '1',
}

foreach program, section: mans
  custom_target(
    program + section,
    input: program + '.xml',
    output: '@BASENAME@.' + section,
    command: xsltproc_cmd,
    install: true,
    install_dir: gvfs_mandir / ('man' + section),
  )
endforeach
