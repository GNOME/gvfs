dbus_sources = gnome.gdbus_codegen(
  'gvfsvolumemonitordbus',
  'dbus-interfaces.xml',
  interface_prefix: 'org.gtk.Private.',
  namespace: 'GVfs'
)

sources = files(
  'remote-volume-monitor-module.c',
  'gproxydrive.c',
  'gproxymount.c',
  'gproxymountoperation.c',
  'gproxyshadowmount.c',
  'gproxyvolume.c',
  'gproxyvolumemonitor.c'
)

deps = glib_deps + [libgvfscommon_dep]

cflags = [
  '-DG_LOG_DOMAIN="GVFS-RemoteVolumeMonitor"',
  '-DGIO_MODULE_DIR="@0@"'.format(gio_module_dir),
  '-DGVFS_LOCALEDIR="@0@"'.format(gvfs_localedir),
  '-DREMOTE_VOLUME_MONITORS_DIR="@0@"'.format(gvfs_remote_volume_monitors_dir)
]

symbol_map = join_paths(meson.current_source_dir(), 'symbol.map')

ldflags = []
if have_version_script
  ldflags += '-Wl,--version-script,@0@'.format(symbol_map)
endif

libgioremove_volume_monitor = shared_module(
  'gioremote-volume-monitor',
  sources: sources + [dbus_sources],
  include_directories: top_inc,
  dependencies: deps,
  c_args: cflags,
  link_args: ldflags,
  link_depends: symbol_map,
  install: true,
  install_rpath: gvfs_rpath,
  install_dir: gio_module_dir
)

sources = files('gvfsproxyvolumemonitordaemon.c')

cflags = [
  '-DG_LOG_DOMAIN="GVFS-RemoteVolumeMonitorDaemon"',
  '-DGVFS_LOCALEDIR="@0@"'.format(gvfs_localedir)
]

libgvfsproxyvolumemonitordaemon_noin = static_library(
  'gvfsproxyvolumemonitordaemon-noin',
  sources: sources + [dbus_sources],
  include_directories: top_inc,
  dependencies: deps,
  c_args: cflags
)

libgvfsproxyvolumemonitordaemon_noin_dep = declare_dependency(
  link_with: libgvfsproxyvolumemonitordaemon_noin,
  include_directories: include_directories('.')
)
