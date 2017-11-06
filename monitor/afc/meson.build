afc_monitor = files('afc.monitor')

install_data(
  afc_monitor,
  install_dir: gvfs_remote_volume_monitors_dir
)

dbus_service = gvfs_namespace + '.AfcVolumeMonitor'
dbus_exec = 'gvfs-afc-volume-monitor'
dbus_systemd_service = ''

if install_systemd_user_unit_dir
  service = dbus_exec + '.service'

  configure_file(
    input: service + '.in',
    output: service,
    install: true,
    install_dir: systemd_user_unit_dir,
    configuration: service_conf
  )

  dbus_systemd_service = 'SystemdService=' + service
endif

dbus_service_conf = configuration_data()
dbus_service_conf.set('service', dbus_service)
dbus_service_conf.set('exec', join_paths(gvfs_libexecdir, dbus_exec))
dbus_service_conf.set('systemd_service', dbus_systemd_service)

afc_service = configure_file(
  input: dbus_service_in,
  output: dbus_service + '.service',
  install: true,
  install_dir: dbus_service_dir,
  configuration: dbus_service_conf
)

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
