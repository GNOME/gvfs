install_data(
  'mtp.monitor',
  install_dir: gvfs_remote_volume_monitors_dir
)

service = gvfs_namespace + '.MTPVolumeMonitor.service'

configure_file(
  input: service + '.in',
  output: service,
  install: true,
  install_dir: gvfs_dbus_service_dir,
  configuration: service_conf
)

if have_systemd_user_unit
  service = 'gvfs-mtp-volume-monitor.service'

  configure_file(
    input: service + '.in',
    output: service,
    install: true,
    install_dir: systemd_user_dir,
    configuration: service_conf
  )
endif

sources = files(
  'mtp-volume-monitor-daemon.c',
  'gmtpvolume.c',
  'gmtpvolumemonitor.c'
)

deps = glib_deps + [
  gudev_dep,
  libgvfscommon_dep,
  libgvfscommon_gphoto2_dep,
  libgvfsproxyvolumemonitordaemon_noin_dep
]

cflags = [
  '-DG_LOG_DOMAIN="GVFS-MTP"',
  '-DGIO_MODULE_DIR="@0@"'.format(gio_module_dir),
  '-DGVFS_LOCALEDIR="@0@"'.format(gvfs_localedir)
]

executable(
  'gvfs-mtp-volume-monitor',
  sources,
  include_directories: top_inc,
  dependencies: deps,
  c_args: cflags,
  install: true,
  install_rpath: gvfs_rpath,
  install_dir: gvfs_libexecdir
)
