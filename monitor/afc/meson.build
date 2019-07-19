sources = files(
  'afcvolumemonitordaemon.c',
  'afcvolume.c',
  'afcvolumemonitor.c',
)

deps = [
  glib_dep,
  gobject_dep,
  libimobiledevice_dep,
  libplist_dep,
  libgvfsproxyvolumemonitordaemon_noin_dep,
]

cflags = [
  '-DG_LOG_DOMAIN="GVFS-AFC"',
  '-DGIO_MODULE_DIR="@0@"'.format(gio_giomoduledir),
  '-DGVFS_LOCALEDIR="@0@"'.format(gvfs_prefix / gvfs_localedir),
]

executable(
  'gvfs-afc-volume-monitor',
  sources,
  include_directories: top_inc,
  dependencies: deps,
  c_args: cflags,
  install: true,
  install_dir: gvfs_libexecdir,
)
