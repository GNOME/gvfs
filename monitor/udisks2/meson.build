sources = files(
  'udisks2volumemonitordaemon.c',
  'gvfsudisks2drive.c',
  'gvfsudisks2mount.c',
  'gvfsudisks2utils.c',
  'gvfsudisks2volume.c',
  'gvfsudisks2volumemonitor.c',
)

deps = [
  gio_unix_dep,
  glib_dep,
  gobject_dep,
  gudev_dep,
  libgvfscommon_monitor_dep,
  libgvfsproxyvolumemonitordaemon_noin_dep,
  udisks2_dep,
  gsettings_desktop_schemas_dep,
]

if enable_logind
  deps += logind_dep
endif

if enable_keyring
  deps += libsecret_dep
endif

cflags = [
  '-DG_LOG_DOMAIN="GVFS-UDisks2"',
  '-DG_DISABLE_DEPRECATED',
  '-DGIO_MODULE_DIR="@0@"'.format(gio_giomoduledir),
  '-DGVFS_LOCALEDIR="@0@"'.format(gvfs_prefix / gvfs_localedir),
  '-DUDISKS_API_IS_SUBJECT_TO_CHANGE',
]

executable(
  'gvfs-udisks2-volume-monitor',
  sources,
  include_directories: top_inc,
  dependencies: deps,
  c_args: cflags,
  install: true,
  install_dir: gvfs_libexecdir,
)
