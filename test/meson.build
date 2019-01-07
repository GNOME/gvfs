if enable_installed_tests
  installed_tests_execdir = gvfs_libexecdir / 'installed-tests' / gvfs_name
  installed_tests_metadir = gvfs_datadir / 'installed-tests' / gvfs_name

  tests_conf = configuration_data()
  tests_conf.set('testdir', installed_tests_execdir)

  session = 'session.conf'

  configure_file(
    input: session + '.in',
    output: session,
    configuration: tests_conf,
    install: true,
    install_dir: installed_tests_execdir,
  )

  all_tests = 'gvfs-all-tests'

  configure_file(
    input: all_tests + '.in',
    output: all_tests + '.test',
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
  ]

  deps = [
    gio_dep,
    glib_dep,
  ]

  foreach test: tests
    executable(
      test,
      test + '.c',
      include_directories: top_inc,
      dependencies: deps,
    )
  endforeach
endif
