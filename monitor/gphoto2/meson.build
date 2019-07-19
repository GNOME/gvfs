sources = files(
  'gphoto2-volume-monitor-daemon.c',
  'ggphoto2volume.c',
  'ggphoto2volumemonitor.c',
)

deps = [
  gio_unix_dep,
  gobject_dep,
  libgphoto2_dep,
  libgvfscommon_gphoto2_dep,
  libgvfsproxyvolumemonitordaemon_noin_dep,
]

cflags = [
  '-DG_LOG_DOMAIN="GVFS-GPhoto2"',
  '-DGIO_MODULE_DIR="@0@"'.format(gio_giomoduledir),
  '-DGVFS_LOCALEDIR="@0@"'.format(gvfs_prefix / gvfs_localedir),
]

executable(
  'gvfs-gphoto2-volume-monitor',
  sources,
  include_directories: top_inc,
  dependencies: deps,
  c_args: cflags,
  install: true,
  install_dir: gvfs_libexecdir,
)
