sources = files(
  'mtp-volume-monitor-daemon.c',
  'gmtpvolume.c',
  'gmtpvolumemonitor.c',
)

deps = [
  gio_unix_dep,
  gobject_dep,
  libgvfscommon_gphoto2_dep,
  libgvfsproxyvolumemonitordaemon_noin_dep,
]

cflags = [
  '-DG_LOG_DOMAIN="GVFS-MTP"',
  '-DGIO_MODULE_DIR="@0@"'.format(gio_giomoduledir),
  '-DGVFS_LOCALEDIR="@0@"'.format(gvfs_prefix / gvfs_localedir),
]

executable(
  'gvfs-mtp-volume-monitor',
  sources,
  include_directories: top_inc,
  dependencies: deps,
  c_args: cflags,
  install: true,
  install_dir: gvfs_libexecdir,
)
