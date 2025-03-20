if enable_deprecated_apis
  headers = files(
    'gvfsurimapper.h',
    'gvfsuriutils.h',
  )

  install_headers(
    headers,
    subdir: 'gvfs-client/gvfs',
  )
endif

# Dynamic client lib
uri_parser_sources = files(
  'afpuri.c',
  'httpuri.c',
  'smburi.c',
)

uri_utils = files('gvfsuriutils.c')

sources = uri_parser_sources + uri_utils + files(
  'gdaemonmount.c',
  'gdaemonfile.c',
  'gdaemonfileenumerator.c',
  'gdaemonfileinputstream.c',
  'gdaemonfilemonitor.c',
  'gdaemonfileoutputstream.c',
  'gdaemonvfs.c',
  'gdaemonvolumemonitor.c',
  'gvfsdaemondbus.c',
  'gvfsiconloadable.c',
  'gvfsurimapper.c',
)

deps = [
  gio_unix_dep,
  libmetadata_dep,
]

cflags = [
  '-DG_LOG_DOMAIN="@0@"'.format(gvfs_name.to_upper()),
  '-DGVFS_LOCALEDIR="@0@"'.format(gvfs_prefix / gvfs_localedir),
  '-DGVFS_MODULE_DIR="@0@"'.format(gvfs_prefix / gvfs_pkglibdir / 'modules'),
]

symbol_map = meson.current_source_dir() / 'symbol.map'

ldflags = []
if have_version_script
  ldflags += '@0@,@1@'.format(version_script_ldflag, symbol_map)
endif

libgvfsdbus = shared_module(
  'gvfsdbus',
  sources: sources,
  include_directories: top_inc,
  dependencies: deps,
  c_args: cflags,
  link_args: ldflags,
  link_depends: symbol_map,
  install: true,
  install_rpath: gvfs_rpath,
  install_dir: gio_giomoduledir,
)

if enable_devel_utils
  test_name = 'test-uri-utils'

  executable(
    test_name,
    [test_name + '.c'] + uri_utils,
    include_directories: top_inc,
    dependencies: libgvfscommon_dep,
    c_args: cflags,
  )
endif

# FUSE daemon
if enable_fuse
  fuse_cflags = []
  if cc.has_function('fuse_set_feature_flag', dependencies: fuse_dep)
    fuse_cflags = ['-DHAVE_FUSE_FEATURE_FLAG_FUNCS']
  endif

  if install_systemd_tmpfilesdir
    install_data(
      'gvfsd-fuse-tmpfiles.conf',
      install_dir: systemd_tmpfilesdir,
    )
  endif

  deps = [
    fuse_dep,
    libgvfscommon_dep,
  ]

  executable(
    'gvfsd-fuse',
    'gvfsfusedaemon.c',
    include_directories: top_inc,
    dependencies: deps,
    c_args: cflags + ['-UG_LOG_DOMAIN'] + fuse_cflags,
    install: true,
    install_rpath: gvfs_rpath,
    install_dir: gvfs_libexecdir,
  )
endif
