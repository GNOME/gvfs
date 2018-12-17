subdir('proxy')

# [[service name suffix, install monitor test data]]
monitors = []

if enable_afc
  monitors += [['Afc', true]]
endif

if enable_goa
  monitors += [['Goa', false]]
endif

if enable_gphoto2
  monitors += [['GPhoto2', true]]
endif

if enable_mtp
  monitors += [['MTP', false]]
endif

if enable_udisks2
  monitors += [['UDisks2', true]]
endif

monitors_test_data = []
foreach monitor: monitors
  monitor_name = monitor[0].to_lower()

  dbus_service = '@0@.@1@VolumeMonitor'.format(gvfs_namespace, monitor[0])
  dbus_exec = 'gvfs-@0@-volume-monitor'.format(monitor_name)
  dbus_systemd_service = ''

  if install_systemd_user_unit_dir
    service = dbus_exec + '.service'

    configure_file(
      input: monitor_name / service + '.in',
      output: service,
      install: true,
      install_dir: systemd_user_unit_dir,
      configuration: service_conf,
    )

    dbus_systemd_service = 'SystemdService=' + service
  endif

  monitor_data = files(monitor_name / monitor_name + '.monitor')

  install_data(
    monitor_data,
    install_dir: gvfs_remote_volume_monitors_dir,
  )

  dbus_service_conf = configuration_data()
  dbus_service_conf.set('service', dbus_service)
  dbus_service_conf.set('exec', gvfs_libexecdir / dbus_exec)
  dbus_service_conf.set('systemd_service', dbus_systemd_service)

  monitor_service = configure_file(
    input: dbus_service_in,
    output: dbus_service + '.service',
    install: true,
    install_dir: dbus_service_dir,
    configuration: dbus_service_conf,
  )

  if monitor[1]
    monitors_test_data += [monitor_data, monitor_service]
  endif

  subdir(monitor_name)
endforeach
