script_names = [
  ['mount', 'mount'],
  ['cat', 'cat'],
  ['open', 'open'],
  ['save', 'save'],
  ['ls', 'list'],
  ['tree', 'tree'],
  ['info', 'info'],
  ['set-attribute', 'set'],
  ['trash', 'trash'],
  ['rename', 'rename'],
  ['rm', 'remove'],
  ['copy', 'copy'],
  ['move', 'move'],
  ['monitor-file', 'monitor'],
  ['monitor-dir', 'monitor'],
  ['mkdir', 'mkdir'],
  ['mime', 'mime']
]

foreach name: script_names
  conf = configuration_data()
  conf.set('command', name[1])

  configure_file(
    input: 'deprecated.in',
    output: 'gvfs-' + name[0],
    install: true,
    install_dir: gvfs_bindir,
    configuration: conf
  )
endforeach

install_data(
  'gvfs-less',
  install_dir: gvfs_bindir
)
