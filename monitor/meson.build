subdir('proxy')

foreach monitor: monitors
  monitor_name = monitor.to_lower()

  monitor_data = files(monitor_name / monitor_name + '.monitor')
  monitors_test_data += monitor_data

  install_data(
    monitor_data,
    install_dir: gvfs_remote_volume_monitors_dir,
  )

  dbus_service = '@0@.@1@VolumeMonitor'.format(gvfs_namespace, monitor)
  dbus_exec = 'gvfs-@0@-volume-monitor'.format(monitor_name)
  dbus_systemd_service = ''

  if install_systemd_systemduserunitdir
    service = dbus_exec + '.service'

    configure_file(
      input: monitor_name / service + '.in',
      output: service,
      configuration: service_conf,
      install: true,
      install_dir: systemd_systemduserunitdir,
    )

    dbus_systemd_service = 'SystemdService=' + service
  endif

  dbus_service_conf = configuration_data()
  dbus_service_conf.set('service', dbus_service)
  dbus_service_conf.set('exec', gvfs_libexecdir / dbus_exec)
  dbus_service_conf.set('systemd_service', dbus_systemd_service)

  monitors_test_data += configure_file(
    input: dbus_service_in,
    output: dbus_service + '.service',
    configuration: dbus_service_conf,
    install: true,
    install_dir: dbus_session_bus_services_dir,
  )

  subdir(monitor_name)
endforeach
