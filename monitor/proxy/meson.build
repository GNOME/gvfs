dbus_sources = gnome.gdbus_codegen(
  'gvfsvolumemonitordbus',
  'dbus-interfaces.xml',
  interface_prefix: 'org.gtk.Private.',
  namespace: 'GVfs',
)

sources = files(
  'remote-volume-monitor-module.c',
  'gproxydrive.c',
  'gproxymount.c',
  'gproxymountoperation.c',
  'gproxyshadowmount.c',
  'gproxyvolume.c',
  'gproxyvolumemonitor.c',
)

deps = [
  gio_unix_dep,
  libgvfscommon_dep,
]

cflags = [
  '-DG_LOG_DOMAIN="GVFS-RemoteVolumeMonitor"',
  '-DGIO_MODULE_DIR="@0@"'.format(gio_giomoduledir),
  '-DGVFS_LOCALEDIR="@0@"'.format(gvfs_prefix / gvfs_localedir),
  '-DREMOTE_VOLUME_MONITORS_DIR="@0@"'.format(gvfs_prefix / gvfs_remote_volume_monitors_dir),
]

symbol_map = meson.current_source_dir() / 'symbol.map'

ldflags = []
if have_version_script
  ldflags += '@0@,@1@'.format(version_script_ldflag, symbol_map)
endif

libgioremote_volume_monitor = shared_module(
  'gioremote-volume-monitor',
  sources: sources + [dbus_sources],
  include_directories: top_inc,
  dependencies: deps,
  c_args: cflags,
  link_args: ldflags,
  link_depends: symbol_map,
  install: true,
  install_rpath: gvfs_rpath,
  install_dir: gio_giomoduledir,
)

sources = files('gvfsproxyvolumemonitordaemon.c')

deps = [
  gio_dep,
  gio_unix_dep,
  glib_dep,
]

cflags = [
  '-DG_LOG_DOMAIN="GVFS-RemoteVolumeMonitorDaemon"',
  '-DGVFS_LOCALEDIR="@0@"'.format(gvfs_prefix / gvfs_localedir),
]

libgvfsproxyvolumemonitordaemon_noin = static_library(
  'gvfsproxyvolumemonitordaemon-noin',
  sources: sources + [dbus_sources],
  include_directories: top_inc,
  dependencies: deps,
  c_args: cflags,
)

libgvfsproxyvolumemonitordaemon_noin_dep = declare_dependency(
  include_directories: '.',
  dependencies: gio_dep,
  link_with: libgvfsproxyvolumemonitordaemon_noin,
)
