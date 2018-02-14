subdir('trashlib')

# D-Bus and systemd service files
dbus_service = gvfs_namespace + '.Daemon'
dbus_exec = 'gvfsd'
dbus_systemd_service = ''

if install_systemd_user_unit_dir
  service = 'gvfs-daemon.service'

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

daemon_service = configure_file(
  input: dbus_service_in,
  output: dbus_service + '.service',
  install: true,
  install_dir: dbus_service_dir,
  configuration: dbus_service_conf
)

gnome.mkenums(
  'org.gnome.system.gvfs.enums.xml',
  sources: 'gvfs-enums.h',
  comments: '<!-- @comment@ -->',
  fhead: '<schemalist>',
  vhead: '  <@type@ id="org.gnome.system.gvfs.@EnumName@">',
  vprod: '    <value nick="@valuenick@" value="@valuenum@"/>',
  vtail: '  </@type@>',
  ftail: '</schemalist>',
  install_header: true,
  install_dir: gvfs_schema_dir
)

sources = files(
  'gvfsbackend.c',
  'gvfschannel.c',
  'gvfsdaemon.c',
  'gvfsdaemonutils.c',
  'gvfsjob.c',
  'gvfsjobcloseread.c',
  'gvfsjobclosewrite.c',
  'gvfsjobcopy.c',
  'gvfsjobcreatemonitor.c',
  'gvfsjobdbus.c',
  'gvfsjobdelete.c',
  'gvfsjobenumerate.c',
  'gvfsjoberror.c',
  'gvfsjobmakedirectory.c',
  'gvfsjobmakesymlink.c',
  'gvfsjobmount.c',
  'gvfsjobmountmountable.c',
  'gvfsjobmove.c',
  'gvfsjobopenforread.c',
  'gvfsjobopenforwrite.c',
  'gvfsjobopeniconforread.c',
  'gvfsjobpollmountable.c',
  'gvfsjobprogress.c',
  'gvfsjobpull.c',
  'gvfsjobpush.c',
  'gvfsjobqueryattributes.c',
  'gvfsjobqueryfsinfo.c',
  'gvfsjobqueryinfo.c',
  'gvfsjobqueryinforead.c',
  'gvfsjobqueryinfowrite.c',
  'gvfsjobread.c',
  'gvfsjobseekread.c',
  'gvfsjobseekwrite.c',
  'gvfsjobsetattribute.c',
  'gvfsjobsetdisplayname.c',
  'gvfsjobsource.c',
  'gvfsjobstartmountable.c',
  'gvfsjobstopmountable.c',
  'gvfsjobtrash.c',
  'gvfsjobtruncate.c',
  'gvfsjobunmount.c',
  'gvfsjobunmountmountable.c',
  'gvfsjobwrite.c',
  'gvfskeyring.c',
  'gvfsmonitor.c',
  'gvfsreadchannel.c',
  'gvfswritechannel.c'
)

deps = glib_deps + socket_deps + [libgvfscommon_dep]

if enable_gcr
  deps += gcr_dep
endif

if enable_keyring
  deps += libsecret_dep
endif

cflags = [
  '-DLIBEXEC_DIR="@0@"'.format(gvfs_libexecdir),
  '-DMOUNTABLE_DIR="@0@"'.format(gvfs_mountdir),
  '-DGVFS_LOCALEDIR="@0@"'.format(gvfs_localedir)
]

libgvfsdaemon = shared_library(
  'gvfsdaemon',
  sources: sources,
  include_directories: top_inc,
  dependencies: deps,
  c_args: cflags,
  install: true,
  install_dir: gvfs_pkglibdir
)

libgvfsdaemon_dep = declare_dependency(
  link_with: libgvfsdaemon,
  include_directories: include_directories('.'),
  dependencies: deps,
  compile_args: cflags
)

sources = files(
  'main.c',
  'mount.c'
)

executable(
  'gvfsd',
  sources,
  include_directories: top_inc,
  dependencies: libgvfsdaemon_dep,
  install: true,
  install_rpath: gvfs_rpath,
  install_dir: gvfs_libexecdir
)

daemon_main_sources = files(
  'daemon-main.c',
  'daemon-main-generic.c'
)

programs = []
mounts = []
schema_data = []
convert_data = []

sources = files('gvfsbackendlocaltest.c')

cflags = [
  '-DBACKEND_HEADER=gvfsbackendlocaltest.h',
  '-DDEFAULT_BACKEND_TYPE=localtest',
  '-DBACKEND_TYPES="localtest", G_VFS_TYPE_BACKEND_LOCALTEST,'
]

programs += [['gvfsd-localtest', sources, [], cflags]]
mounts += ['localtest']

sources = files(
  'gvfsbackendftp.c',
  'gvfsftpconnection.c',
  'gvfsftpdircache.c',
  'gvfsftpfile.c',
  'gvfsftptask.c',
  'ParseFTPList.c'
)

cflags = [
  '-DBACKEND_HEADER=gvfsbackendftp.h',
  '-DDEFAULT_BACKEND_TYPE=ftp',
  '-DBACKEND_TYPES="ftp", G_VFS_TYPE_BACKEND_FTP, "ftps", G_VFS_TYPE_BACKEND_FTP,',
  '-DMAX_JOB_THREADS=10'
]

programs += [['gvfsd-ftp', sources, [], cflags]]
mounts += ['ftp', 'ftps']

sources = files('gvfsbackendtrash.c')

deps = [libtrash_dep]

cflags = [
  '-DBACKEND_HEADER=gvfsbackendtrash.h',
  '-DDEFAULT_BACKEND_TYPE=trash',
  '-DBACKEND_TYPES="trash", G_VFS_TYPE_BACKEND_TRASH,',
  '-DMAX_JOB_THREADS=10'
]

programs += [['gvfsd-trash', sources, deps, cflags]]
mounts += ['trash']

sources = files('gvfsbackendrecent.c')

cflags = [
  '-DBACKEND_HEADER=gvfsbackendrecent.h',
  '-DDEFAULT_BACKEND_TYPE=recent',
  '-DBACKEND_TYPES="recent", G_VFS_TYPE_BACKEND_RECENT,',
  '-DBACKEND_USES_GVFS=1',
  '-DMAX_JOB_THREADS=10'
]

programs += [['gvfsd-recent', sources, [], cflags]]
mounts += ['recent']

sources = files('gvfsbackendcomputer.c')

cflags = [
  '-DBACKEND_HEADER=gvfsbackendcomputer.h',
  '-DDEFAULT_BACKEND_TYPE=computer',
  '-DBACKEND_TYPES="computer", G_VFS_TYPE_BACKEND_COMPUTER,',
  '-DBACKEND_USES_GVFS=1',
  '-DMAX_JOB_THREADS=1'
]

programs += [['gvfsd-computer', sources, [], cflags]]
mounts += ['computer']

sources = files('gvfsbackendnetwork.c')

cflags = [
  '-DBACKEND_HEADER=gvfsbackendnetwork.h',
  '-DDEFAULT_BACKEND_TYPE=network',
  '-DBACKEND_TYPES="network", G_VFS_TYPE_BACKEND_NETWORK,',
  '-DBACKEND_USES_GVFS=1',
  '-DMAX_JOB_THREADS=1'
]

programs += [['gvfsd-network', sources, [], cflags]]
mounts += ['network']

sources = files('gvfsbackendburn.c')

cflags = [
  '-DBACKEND_HEADER=gvfsbackendburn.h',
  '-DDEFAULT_BACKEND_TYPE=burn',
  '-DBACKEND_TYPES="burn", G_VFS_TYPE_BACKEND_BURN,',
  '-DMAX_JOB_THREADS=1'
]

programs += [['gvfsd-burn', sources, [], cflags]]
mounts += ['burn']

sources = files(
  'gvfsbackendsftp.c',
  'pty_open.c'
)

deps = socket_deps + [util_dep]

cflags = [
  '-DBACKEND_HEADER=gvfsbackendsftp.h',
  '-DDEFAULT_BACKEND_TYPE=sftp',
  '-DBACKEND_TYPES="sftp", G_VFS_TYPE_BACKEND_SFTP,',
  '-DMAX_JOB_THREADS=1',
  '-DSSH_PROGRAM="@0@"'.format(find_program('ssh').path())
]

programs += [['gvfsd-sftp', sources, deps, cflags]]
mounts += ['sftp']

if enable_samba
  sources = files('gvfsbackendsmb.c')

  deps = [smbclient_dep]

  cflags = [
    '-DBACKEND_HEADER=gvfsbackendsmb.h',
    '-DDEFAULT_BACKEND_TYPE=smb',
    '-DBACKEND_TYPES="smb-share", G_VFS_TYPE_BACKEND_SMB,',
    '-DMAX_JOB_THREADS=1'
  ]

  programs += [['gvfsd-smb', sources, deps, cflags]]
  mounts += ['smb']
  schema_data += files('org.gnome.system.smb.gschema.xml')
  convert_data += files('gvfs-smb.convert')

  sources += files('gvfsbackendsmbbrowse.c')

  cflags = [
    '-DBACKEND_HEADER=gvfsbackendsmbbrowse.h',
    '-DDEFAULT_BACKEND_TYPE=smb-network',
    '-DBACKEND_TYPES="smb-network", G_VFS_TYPE_BACKEND_SMB_BROWSE, "smb-server",  G_VFS_TYPE_BACKEND_SMB_BROWSE,',
    '-DMAX_JOB_THREADS=1',
    '-DMOUNTABLE_DBUS_NAME=' + gvfs_namespace + '.mountpoint_smb_browse'
  ]

  programs += [['gvfsd-smb-browse', sources, deps, cflags]]
  mounts += ['smb-browse']
endif

if enable_dnssd
  sources = files('gvfsbackenddnssd.c')

  deps = [
    avahi_client_dep,
    avahi_glib_dep,
    libgvfscommon_dnssd_dep
  ]

  cflags = [
    '-DBACKEND_HEADER=gvfsbackenddnssd.h',
    '-DDEFAULT_BACKEND_TYPE=dns-sd',
    '-DBACKEND_TYPES="dns-sd", G_VFS_TYPE_BACKEND_DNS_SD,',
    '-DMAX_JOB_THREADS=1',
    '-DMOUNTABLE_DBUS_NAME=' + gvfs_namespace + '.mountpoint_dnssd'
  ]

  programs += [['gvfsd-dnssd', sources, deps, cflags]]
  mounts += ['dns-sd']
  schema_data += files('org.gnome.system.dns_sd.gschema.xml')
  convert_data += files('gvfs-dns-sd.convert')
endif

if enable_archive
  sources = files('gvfsbackendarchive.c')

  deps = [libarchive_dep]

  cflags = [
    '-DBACKEND_HEADER=gvfsbackendarchive.h',
    '-DDEFAULT_BACKEND_TYPE=archive',
    '-DBACKEND_TYPES="archive", G_VFS_TYPE_BACKEND_ARCHIVE,',
    '-DMAX_JOB_THREADS=1',
    '-DBACKEND_USES_GVFS=1'
  ]

  programs += [['gvfsd-archive', sources, deps, cflags]]
  mounts += ['archive']
endif

if enable_cdda
  sources = files('gvfsbackendcdda.c')

  deps = [
    gudev_dep,
    libcdio_paranoia_dep
  ]

  cflags = [
    '-DBACKEND_HEADER=gvfsbackendcdda.h',
    '-DDEFAULT_BACKEND_TYPE=cdda',
    '-DBACKEND_TYPES="cdda", G_VFS_TYPE_BACKEND_CDDA,',
    '-DMAX_JOB_THREADS=1'
  ]

  programs += [['gvfsd-cdda', sources, deps, cflags]]
  mounts += ['cdda']
endif

if enable_admin
  sources = files('gvfsbackendadmin.c')

  deps = [
    libcap_dep,
    polkit_gobject_dep
  ]

  cflags = [
    '-DBACKEND_HEADER=gvfsbackendadmin.h',
    '-DDEFAULT_BACKEND_TYPE=admin',
    '-DBACKEND_TYPES="admin", G_VFS_TYPE_BACKEND_ADMIN,',
    '-DMOUNTABLE_DBUS_NAME=' + gvfs_namespace + '.mountpoint_admin'
  ]

  programs += [['gvfsd-admin', sources, deps, cflags]]
  mounts += ['admin']

  policy = gvfs_namespace + '.file-operations.policy'

  policy_in = configure_file(
    input: policy + '.in.in',
    output: policy + '.in',
    configuration: service_conf
  )

  i18n.merge_file(
    policy,
    input: policy_in,
    output: policy,
    po_dir: po_dir,
    data_dirs: its_dir,
    install: true,
    install_dir: join_paths(gvfs_datadir, 'polkit-1', 'actions')
  )

  install_data(
    gvfs_namespace + '.file-operations.rules',
    install_dir: join_paths(gvfs_datadir, 'polkit-1', 'rules.d')
  )
endif

if enable_google
  sources = files('gvfsbackendgoogle.c')

  deps = [
    goa_dep,
    libgdata_dep
  ]

  cflags = [
    '-DBACKEND_HEADER=gvfsbackendgoogle.h',
    '-DDEFAULT_BACKEND_TYPE=google-drive',
    '-DBACKEND_TYPES="google-drive", G_VFS_TYPE_BACKEND_GOOGLE,'
  ]

  programs += [['gvfsd-google', sources, deps, cflags]]
  mounts += ['google']
endif

if enable_gphoto2
  sources = files('gvfsbackendgphoto2.c')

  deps = [
    gudev_dep,
    libgphoto2_dep,
    libgvfscommon_gphoto2_dep
  ]

  cflags = [
    '-DBACKEND_HEADER=gvfsbackendgphoto2.h',
    '-DDEFAULT_BACKEND_TYPE=gphoto2',
    '-DBACKEND_TYPES="gphoto2", G_VFS_TYPE_BACKEND_GPHOTO2,',
    '-DMAX_JOB_THREADS=1'
  ]

  programs += [['gvfsd-gphoto2', sources, deps, cflags]]
  mounts += ['gphoto2']
endif

if enable_mtp
  sources = files('gvfsbackendmtp.c')

  deps = [
    gudev_dep,
    libgvfscommon_gphoto2_dep,
    libmtp_dep
  ]

  cflags = [
    '-DBACKEND_HEADER=gvfsbackendmtp.h',
    '-DDEFAULT_BACKEND_TYPE=mtp',
    '-DBACKEND_TYPES="mtp", G_VFS_TYPE_BACKEND_MTP,',
    '-DMAX_JOB_THREADS=1'
  ]

  if enable_libusb
    deps += libusb_dep
  endif

  programs += [['gvfsd-mtp', sources, deps, cflags]]
  mounts += ['mtp']
endif

if enable_http
  sources = files(
    'gvfsbackendhttp.c',
    'gvfshttpinputstream.c'
  )

  deps = [
    libsoup_dep,
    libxml_dep
  ]

  cflags = [
    '-DBACKEND_HEADER=gvfsbackendhttp.h',
    '-DDEFAULT_BACKEND_TYPE=http',
    '-DBACKEND_TYPES="http", G_VFS_TYPE_BACKEND_HTTP,',
    '-DMAX_JOB_THREADS=1',
    '-DMOUNTABLE_DBUS_NAME=' + gvfs_namespace + '.mountpoint_http'
  ]

  programs += [['gvfsd-http', sources, deps, cflags]]
  mounts += ['http']

  sources = sources + files('gvfsbackenddav.c')

  cflags = [
    '-DBACKEND_HEADER=gvfsbackenddav.h',
    '-DDEFAULT_BACKEND_TYPE=dav',
    '-DMAX_JOB_THREADS=1'
  ]

  if enable_dnssd
    deps += libgvfscommon_dnssd_dep

    cflags += '-DBACKEND_TYPES="dav", G_VFS_TYPE_BACKEND_DAV, "dav+sd", G_VFS_TYPE_BACKEND_DAV, "davs+sd", G_VFS_TYPE_BACKEND_DAV,'

    mounts += ['dav+sd']
  else
    cflags += '-DBACKEND_TYPES="dav", G_VFS_TYPE_BACKEND_DAV,'
  endif

  programs += [['gvfsd-dav', sources, deps, cflags]]
  mounts += ['dav']
endif

if enable_afc
  sources = files('gvfsbackendafc.c')

  deps = [
    libimobiledevice_dep,
    libplist_dep
  ]

  cflags = [
    '-DBACKEND_HEADER=gvfsbackendafc.h',
    '-DDEFAULT_BACKEND_TYPE=afc',
    '-DBACKEND_TYPES="afc", G_VFS_TYPE_BACKEND_AFC,',
    '-DMAX_JOB_THREADS=1',
    '-DBACKEND_USES_GVFS=1'
  ]

  programs += [['gvfsd-afc', sources, deps, cflags]]
  mounts += ['afc']
endif

if enable_afp
  common_sources = files(
    'gvfsafptypes.h',
    'gvfsafputils.c',
    'gvfsafpconnection.c',
    'gvfsafpserver.c',
    'gvfsafpvolume.c'
  )

  sources = common_sources + files('gvfsbackendafp.c')

  deps = []
  if enable_gcrypt
    deps += libgcrypt_dep
  endif

  cflags = [
    '-DBACKEND_HEADER=gvfsbackendafp.h',
    '-DDEFAULT_BACKEND_TYPE=afp-volume',
    '-DBACKEND_TYPES="afp-volume", G_VFS_TYPE_BACKEND_AFP,',
    '-DMAX_JOB_THREADS=1'
  ]

  programs += [['gvfsd-afp', sources, deps, cflags]]
  mounts += ['afp']

  sources = common_sources + files('gvfsbackendafpbrowse.c')

  cflags = [
    '-DBACKEND_HEADER=gvfsbackendafpbrowse.h',
    '-DDEFAULT_BACKEND_TYPE=afp-server',
    '-DBACKEND_TYPES="afp-server", G_VFS_TYPE_BACKEND_AFP_BROWSE,',
    '-DMAX_JOB_THREADS=1'
  ]

  programs += [['gvfsd-afp-browse', sources, deps, cflags]]
  mounts += ['afp-browse']
endif

if enable_nfs
  sources = files('gvfsbackendnfs.c')

  deps = [libnfs_dep]

  cflags = [
    '-DBACKEND_HEADER=gvfsbackendnfs.h',
    '-DDEFAULT_BACKEND_TYPE=nfs',
    '-DBACKEND_TYPES="nfs", G_VFS_TYPE_BACKEND_NFS,',
    '-DMAX_JOB_THREADS=1'
  ]

  programs += [['gvfsd-nfs', sources, deps, cflags]]
  mounts += ['nfs']
endif

foreach program: programs
  executable(
    program[0],
    daemon_main_sources + program[1],
    include_directories: top_inc,
    dependencies: [libgvfsdaemon_dep] + program[2],
    c_args: program[3],
    install: true,
    install_rpath: gvfs_rpath,
    install_dir: gvfs_libexecdir
  )
endforeach

foreach mount: mounts
  name = mount + '.mount'

  configure_file(
    input: name + '.in',
    output: name,
    install: true,
    install_dir: gvfs_mountdir,
    configuration: service_conf
  )
endforeach

if enable_devel_utils
  sources = daemon_main_sources + files('gvfsbackendtest.c')

  cflags = [
    '-DBACKEND_HEADER=gvfsbackendtest.h',
    '-DDEFAULT_BACKEND_TYPE=test',
    '-DBACKEND_TYPES="test", G_VFS_TYPE_BACKEND_TEST,'
  ]

  executable(
    'gvfsd-test',
    sources,
    include_directories: top_inc,
    dependencies: libgvfsdaemon_dep,
    c_args: cflags
  )
endif

install_data(
  schema_data,
  install_dir: gvfs_schema_dir
)

install_data(
  convert_data,
  install_dir: join_paths(gvfs_datadir, 'GConf', 'gsettings')
)
