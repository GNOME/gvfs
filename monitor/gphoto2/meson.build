gphoto2_monitor = files('gphoto2.monitor')

install_data(
  gphoto2_monitor,
  install_dir: gvfs_remote_volume_monitors_dir
)

service = gvfs_namespace + '.GPhoto2VolumeMonitor.service'

gphoto2_service = configure_file(
  input: service + '.in',
  output: service,
  install: true,
  install_dir: dbus_service_dir,
  configuration: service_conf
)

if install_systemd_user_unit_dir
  service = 'gvfs-gphoto2-volume-monitor.service'

  configure_file(
    input: service + '.in',
    output: service,
    install: true,
    install_dir: systemd_user_unit_dir,
    configuration: service_conf
  )
endif

sources = files(
  'gphoto2-volume-monitor-daemon.c',
  'ggphoto2volume.c',
  'ggphoto2volumemonitor.c'
)

deps = glib_deps + [
  libgphoto2_dep,
  gudev_dep,
  libgvfscommon_dep,
  libgvfscommon_gphoto2_dep,
  libgvfsproxyvolumemonitordaemon_noin_dep
]

cflags = [
  '-DG_LOG_DOMAIN="GVFS-GPhoto2"',
  '-DGIO_MODULE_DIR="@0@"'.format(gio_module_dir),
  '-DGVFS_LOCALEDIR="@0@"'.format(gvfs_localedir)
]

executable(
  'gvfs-gphoto2-volume-monitor',
  sources,
  include_directories: top_inc,
  dependencies: deps,
  c_args: cflags,
  install: true,
  install_rpath: gvfs_rpath,
  install_dir: gvfs_libexecdir
)
