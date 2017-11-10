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
  '@INPUT@'
]

mans = [
  ['gvfs-cat', 'gio cat'],
  ['gvfs-copy', 'gio copy'],
  ['gvfs-info', 'gio info'],
  ['gvfs-less', ''],
  ['gvfs-ls', 'gio list'],
  ['gvfs-mime', 'gio mime'],
  ['gvfs-mkdir', 'gio mkdir'],
  ['gvfs-monitor-dir', 'gio monitor'],
  ['gvfs-monitor-file', 'gio monitor'],
  ['gvfs-mount', 'gio mount'],
  ['gvfs-move', 'gio move'],
  ['gvfs-open', 'gio open'],
  ['gvfs-rename', 'gio rename'],
  ['gvfs-rm', 'gio remove'],
  ['gvfs-save', 'gio save'],
  ['gvfs-set-attribute', 'gio set'],
  ['gvfs-trash', 'gio trash'],
  ['gvfs-tree', 'gio tree'],
  ['gvfsd', ''],
  ['gvfsd-fuse', ''],
  ['gvfsd-metadata', '']
]

foreach man: mans
  if man[1].strip() == ''
    xml = man[0] + '.xml'
  else
    conf = configuration_data()
    conf.set('original', man[0])
    conf.set('replacement', man[1])

    xml = configure_file(
      input: 'deprecated.xml.in',
      output: man[0] + '.xml',
      configuration: conf
    )
  endif

  output = man[0] + '.1'

  custom_target(
    output,
    input: xml,
    output: output,
    command: xsltproc_cmd,
    install: true,
    install_dir: join_paths(gvfs_mandir, 'man1')
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
  install_dir: join_paths(gvfs_mandir, 'man7')
)
