install_data(
  'goa.monitor',
  install_dir: gvfs_remote_volume_monitors_dir
)

service = gvfs_namespace + '.GoaVolumeMonitor.service'

configure_file(
  input: service + '.in',
  output: service,
  install: true,
  install_dir: dbus_service_dir,
  configuration: service_conf
)

if install_systemd_user_unit_dir
  service = 'gvfs-goa-volume-monitor.service'

  configure_file(
    input: service + '.in',
    output: service,
    install: true,
    install_dir: systemd_user_unit_dir,
    configuration: service_conf
  )
endif

sources = files(
  'goavolumemonitordaemon.c',
  'goavolume.c',
  'goavolumemonitor.c'
)

deps = glib_deps + [
  goa_dep,
  libgvfscommon_dep,
  libgvfsproxyvolumemonitordaemon_noin_dep
]

cflags = [
  '-DG_LOG_DOMAIN="GVFS-GOA"',
  '-DGIO_MODULE_DIR="@0@"'.format(gio_module_dir),
  '-DGVFS_LOCALEDIR="@0@"'.format(gvfs_localedir)
]

executable(
  'gvfs-goa-volume-monitor',
  sources,
  include_directories: top_inc,
  dependencies: deps,
  c_args: cflags,
  install: true,
  install_rpath: gvfs_rpath,
  install_dir: gvfs_libexecdir
)
