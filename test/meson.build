if enable_installed_tests
  installed_tests_execdir = join_paths(gvfs_libexecdir, 'installed-tests', gvfs_name)
  installed_tests_metadir = join_paths(gvfs_datadir, 'installed-tests', gvfs_name)

  tests_conf = configuration_data()
  tests_conf.set('testdir', installed_tests_execdir)

  session = 'session.conf'

  configure_file(
    input: session + '.in',
    output: session,
    install: true,
    install_dir: installed_tests_execdir,
    configuration: tests_conf
  )

  all_tests = 'gvfs-all-tests'

  configure_file(
    input: all_tests + '.in',
    output: all_tests + '.test',
    install: true,
    install_dir: installed_tests_metadir,
    configuration: tests_conf
  )

  install_subdir(
    'files',
    install_dir: installed_tests_execdir
  )

  gvfs_test = files('gvfs-test')

  test_data = gvfs_test + [
    daemon_service,
    metadata_service
  ]

  if enable_afc
    test_data += [afc_monitor, afc_service]
  endif

  if enable_gdu
    test_data += [gdu_monitor, gdu_service]
  endif

  if enable_gphoto2
    test_data += [gphoto2_monitor, gphoto2_service]
  endif

  if enable_udisks2
    test_data += [udisks2_monitor, udisks2_service]
  endif

  install_data(
    test_data,
    install_dir: installed_tests_execdir
  )
endif

if enable_devel_utils
  tests = [
    'benchmark-gvfs-big-files',
    'benchmark-gvfs-small-files',
    'benchmark-posix-big-files',
    'benchmark-posix-small-files',
    'test-query-info-stream'
  ]

  foreach test: tests
    executable(
      test,
      test + '.c',
      include_directories: top_inc,
      dependencies: glib_deps
    )
  endforeach
endif
