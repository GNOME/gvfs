udisks2_monitor = files('udisks2.monitor')

install_data(
  udisks2_monitor,
  install_dir: gvfs_remote_volume_monitors_dir
)

dbus_service = gvfs_namespace + '.UDisks2VolumeMonitor'
dbus_exec = 'gvfs-udisks2-volume-monitor'
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

udisks2_service = configure_file(
  input: dbus_service_in,
  output: dbus_service + '.service',
  install: true,
  install_dir: dbus_service_dir,
  configuration: dbus_service_conf
)

sources = files(
  'udisks2volumemonitordaemon.c',
  'gvfsudisks2drive.c',
  'gvfsudisks2mount.c',
  'gvfsudisks2utils.c',
  'gvfsudisks2volume.c',
  'gvfsudisks2volumemonitor.c'
)

deps = glib_deps + [
  udisks2_dep,
  gudev_dep,
  libgvfscommon_dep,
  libgvfscommon_monitor_dep,
  libgvfsproxyvolumemonitordaemon_noin_dep
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
  '-DGIO_MODULE_DIR="@0@"'.format(gio_module_dir),
  '-DGVFS_LOCALEDIR="@0@"'.format(gvfs_localedir),
  '-DUDISKS_API_IS_SUBJECT_TO_CHANGE'
]

executable(
  'gvfs-udisks2-volume-monitor',
  sources,
  include_directories: top_inc,
  dependencies: deps,
  c_args: cflags,
  install: true,
  install_rpath: gvfs_rpath,
  install_dir: gvfs_libexecdir
)
