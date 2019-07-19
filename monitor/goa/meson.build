sources = files(
  'goavolumemonitordaemon.c',
  'goavolume.c',
  'goavolumemonitor.c',
)

deps = [
  glib_dep,
  goa_dep,
  gobject_dep,
  libgvfsproxyvolumemonitordaemon_noin_dep,
]

cflags = [
  '-DG_LOG_DOMAIN="GVFS-GOA"',
  '-DGIO_MODULE_DIR="@0@"'.format(gio_giomoduledir),
  '-DGVFS_LOCALEDIR="@0@"'.format(gvfs_prefix / gvfs_localedir),
]

executable(
  'gvfs-goa-volume-monitor',
  sources,
  include_directories: top_inc,
  dependencies: deps,
  c_args: cflags,
  install: true,
  install_dir: gvfs_libexecdir,
)
