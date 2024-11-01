if enable_installed_tests
  installed_tests_execdir = gvfs_libexecdir / 'installed-tests' / gvfs_name
  installed_tests_metadir = gvfs_datadir / 'installed-tests' / gvfs_name

  tests_conf = {'testdir': gvfs_prefix / installed_tests_execdir}

  configure_file(
    input: 'session.conf.in',
    output: '@BASENAME@',
    configuration: tests_conf,
    install: true,
    install_dir: installed_tests_execdir,
  )

  all_tests = 'gvfs-all-tests'

  configure_file(
    input: all_tests + '.in',
    output: '@BASENAME@.test',
    configuration: tests_conf,
    install: true,
    install_dir: installed_tests_metadir,
  )

  install_subdir(
    'files',
    install_dir: installed_tests_execdir,
  )

  gvfs_test = files('gvfs-test')

  test_data = gvfs_test + [
    daemon_service,
    metadata_service,
  ]

  install_data(
    test_data + monitors_test_data,
    install_dir: installed_tests_execdir,
  )
endif

if enable_devel_utils
  tests = [
    'benchmark-gvfs-big-files',
    'benchmark-gvfs-small-files',
    'benchmark-posix-big-files',
    'benchmark-posix-small-files',
    'test-query-info-stream',
    'google'
  ]

  deps = [
    gio_dep,
    glib_dep,
    libgdata_dep,
    goa_dep
  ]

  foreach test: tests
    executable(
      test,
      test + '.c',
      c_args: ['-DG_LOG_DOMAIN="@0@"'.format(test)],
      include_directories: top_inc,
      dependencies: deps,
    )
  endforeach
endif

if enable_unit_tests
  subdir('unit')
endif