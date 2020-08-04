dbus_service = gvfs_namespace + '.Metadata'
dbus_exec = 'gvfsd-metadata'
dbus_systemd_service = ''

if install_systemd_systemduserunitdir
  service = 'gvfs-metadata.service'

  configure_file(
    input: service + '.in',
    output: '@BASENAME@',
    configuration: service_conf,
    install: true,
    install_dir: systemd_systemduserunitdir,
  )

  dbus_systemd_service = 'SystemdService=' + service
endif

dbus_service_conf = {
  'service': dbus_service,
  'exec': gvfs_prefix / gvfs_libexecdir / dbus_exec,
  'systemd_service': dbus_systemd_service,
}

metadata_service = configure_file(
  input: dbus_service_in,
  output: dbus_service + '.service',
  configuration: dbus_service_conf,
  install: true,
  install_dir: dbus_session_bus_services_dir,
)

dbus_sources = gnome.gdbus_codegen(
  'metadata-dbus',
  'dbus-interface.xml',
  interface_prefix: gvfs_namespace + '.',
  namespace: 'GVfs',
  autocleanup: 'objects',
)

sources = files(
  'crc32.c',
  'metabuilder.c',
  'metatree.c',
)

deps = [
  gio_dep,
  glib_dep,
  libgvfscommon_dep,
]

cflags = [
  '-DDBUS_API_SUBJECT_TO_CHANGE',
  '-DG_LOG_DOMAIN="@0@"'.format(gvfs_name.to_upper()),
  '-DGVFS_LOCALEDIR="@0@"'.format(gvfs_prefix / gvfs_localedir),
]

libmetadata = static_library(
  'metadata',
  sources: sources + [dbus_sources],
  include_directories: top_inc,
  dependencies: deps + [gio_unix_dep],
  c_args: cflags,
  pic: true,
)

libmetadata_dep = declare_dependency(
  sources: dbus_sources[1],
  include_directories: '.',
  dependencies: deps,
  link_with: libmetadata,
)

deps = [libmetadata_dep]

if enable_gudev
  deps += gudev_dep
endif

executable(
  'gvfsd-metadata',
  'meta-daemon.c',
  include_directories: top_inc,
  dependencies: deps,
  c_args: cflags,
  install: true,
  install_rpath: gvfs_rpath,
  install_dir: gvfs_libexecdir,
)

if enable_devel_utils
  apps = [
    'meta-ls',
    'meta-get',
    'meta-set',
    'meta-get-tree',
  ]

  foreach app: apps
    executable(
      app,
      app + '.c',
      include_directories: top_inc,
      dependencies: libmetadata_dep,
      c_args: cflags,
    )
  endforeach

  if have_libxml
    executable(
      'convert-nautilus-metadata',
      'metadata-nautilus.c',
      include_directories: top_inc,
      dependencies: [
        libmetadata_dep,
        libxml_dep,
      ],
      c_args: cflags,
    )
  endif
endif
