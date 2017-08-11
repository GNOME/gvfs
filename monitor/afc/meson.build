afc_monitor = files('afc.monitor')

install_data(
  afc_monitor,
  install_dir: gvfs_remote_volume_monitors_dir
)

service = gvfs_namespace + '.AfcVolumeMonitor.service'

afc_service = configure_file(
  input: service + '.in',
  output: service,
  install: true,
  install_dir: gvfs_dbus_service_dir,
  configuration: service_conf
)

if have_systemd_user_unit
  service = 'gvfs-afc-volume-monitor.service'

  configure_file(
    input: service + '.in',
    output: service,
    install: true,
    install_dir: systemd_user_dir,
    configuration: service_conf
  )
endif

sources = files(
  'afcvolumemonitordaemon.c',
  'afcvolume.c',
  'afcvolumemonitor.c'
)

deps = glib_deps + [
  libimobiledevice_dep,
  libplist_dep,
  libgvfscommon_dep,
  libgvfsproxyvolumemonitordaemon_noin_dep
]

cflags = [
  '-DG_LOG_DOMAIN="GVFS-AFC"',
  '-DGIO_MODULE_DIR="@0@"'.format(gio_module_dir),
  '-DGVFS_LOCALEDIR="@0@"'.format(gvfs_localedir)
]

executable(
  'gvfs-afc-volume-monitor',
  sources,
  include_directories: top_inc,
  dependencies: deps,
  c_args: cflags,
  install: true,
  install_rpath: gvfs_rpath,
  install_dir: gvfs_libexecdir
)
