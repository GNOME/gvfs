common_inc = include_directories('.')

common_cflags = ['-DG_LOG_DOMAIN="@0@"'.format(gvfs_name.to_upper())]

sources = files(
  'gmountoperationdbus.c',
  'gmountsource.c',
  'gmountspec.c',
  'gmounttracker.c',
  'gvfsdaemonprotocol.c',
  'gvfsfileinfo.c',
  'gvfsicon.c',
  'gvfsmonitorimpl.c',
  'gvfsutils.c',
)

dbus_sources = gnome.gdbus_codegen(
  'gvfsdbus',
  gvfs_namespace + '.xml',
  interface_prefix: gvfs_namespace + '.',
  namespace: 'GVfsDBus',
  autocleanup: 'objects',
)

deps = [
  gio_dep,
  glib_dep,
  gobject_dep,
]

cflags = common_cflags + ['-DREMOTE_VOLUME_MONITORS_DIR="@0@"'.format(gvfs_prefix / gvfs_remote_volume_monitors_dir)]

libgvfscommon = shared_library(
  'gvfscommon',
  sources: sources + [dbus_sources],
  include_directories: top_inc,
  dependencies: deps + [gio_unix_dep],
  c_args: cflags,
  install: true,
  install_dir: gvfs_pkglibdir,
)

libgvfscommon_dep = declare_dependency(
  sources: dbus_sources[1],
  include_directories: common_inc,
  dependencies: deps,
  link_with: libgvfscommon,
)

deps = [
  gio_dep,
  glib_dep,
]

if enable_bluray
  deps += libbluray_dep
endif

libgvfscommon_monitor = static_library(
  'gvfscommon-monitor',
  sources: 'gvfsmountinfo.c',
  include_directories: top_inc,
  dependencies: deps,
  c_args: common_cflags,
  link_with: libgvfscommon,
)

libgvfscommon_monitor_dep = declare_dependency(
  include_directories: common_inc,
  dependencies: gio_dep,
  link_with: libgvfscommon_monitor,
)

if enable_dnssd
  sources = files(
    'gvfsdnssdresolver.c',
    'gvfsdnssdutils.c',
  )

  deps = [
    avahi_client_dep,
    avahi_glib_dep,
    gio_dep,
    glib_dep,
  ]

  libgvfscommon_dnssd = static_library(
    'gvfscommon-dnssd',
    sources: sources,
    include_directories: top_inc,
    dependencies: deps,
    c_args: common_cflags,
    link_with: libgvfscommon,
  )

  libgvfscommon_dnssd_dep = declare_dependency(
    include_directories: common_inc,
    dependencies: gio_dep,
    link_with: libgvfscommon_dnssd,
  )
endif

if enable_mtp or enable_gphoto2
  deps = [
    glib_dep,
    gudev_dep,
  ]

  libgvfscommon_gphoto2 = static_library(
    'gvfscommon-gphoto2',
    sources: 'gvfsgphoto2utils.c',
    include_directories: top_inc,
    dependencies: deps,
  )

  libgvfscommon_gphoto2_dep = declare_dependency(
    include_directories: common_inc,
    dependencies: deps,
    link_with: libgvfscommon_gphoto2,
  )
endif
