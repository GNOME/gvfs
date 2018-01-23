dbus_service = gvfs_namespace + '.Metadata'
dbus_exec = 'gvfsd-metadata'
dbus_systemd_service = ''

if install_systemd_user_unit_dir
  service = 'gvfs-metadata.service'

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

metadata_service = configure_file(
  input: dbus_service_in,
  output: dbus_service + '.service',
  install: true,
  install_dir: dbus_service_dir,
  configuration: dbus_service_conf
)

metadata_dbus = gnome.gdbus_codegen(
  'metadata-dbus',
  'dbus-interface.xml',
  interface_prefix: gvfs_namespace + '.',
  namespace: 'GVfs'
)

sources = files(
  'crc32.c',
  'metabuilder.c',
  'metatree.c'
)

cflags = [
  '-DDBUS_API_SUBJECT_TO_CHANGE',
  '-DG_LOG_DOMAIN="@0@"'.format(gvfs_name.to_upper()),
  '-DGVFS_LOCALEDIR="@0@"'.format(gvfs_localedir)
]

libmetadata = static_library(
  'metadata',
  sources: sources + [metadata_dbus],
  include_directories: [top_inc, common_inc],
  dependencies: glib_deps,
  c_args: cflags,
  pic: true
)

libmetadata_dep = declare_dependency(
  sources: metadata_dbus,
  link_with: libmetadata,
  include_directories: include_directories('.'),
  dependencies: glib_deps
)

metadata_deps = [
  libmetadata_dep,
  libgvfscommon_dep
]

if enable_gudev
  metadata_deps += gudev_dep
endif

executable(
  'gvfsd-metadata',
  'meta-daemon.c',
  include_directories: top_inc,
  dependencies: metadata_deps,
  c_args: cflags,
  install: true,
  install_rpath: gvfs_rpath,
  install_dir: gvfs_libexecdir
)

if enable_devel_utils
  apps = [
    'meta-ls',
    'meta-get',
    'meta-set',
    'meta-get-tree'
  ]

  foreach app: apps
    executable(
      app,
      app + '.c',
      include_directories: top_inc,
      dependencies: [
        libmetadata_dep,
        libgvfscommon_dep
      ],
      c_args: cflags
    )
  endforeach

  if have_libxml
    executable(
      'convert-nautilus-metadata',
      'metadata-nautilus.c',
      include_directories: top_inc,
      dependencies: [
        libmetadata_dep,
        libxml_dep
      ],
      c_args: cflags
    )
  endif
endif
