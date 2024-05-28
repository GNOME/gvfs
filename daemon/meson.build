subdir('trashlib')

# D-Bus and systemd service files
dbus_service = gvfs_namespace + '.Daemon'
dbus_exec = 'gvfsd'
dbus_systemd_service = ''

if install_systemd_systemduserunitdir
  service = 'gvfs-daemon.service'

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

daemon_service = configure_file(
  input: dbus_service_in,
  output: dbus_service + '.service',
  configuration: dbus_service_conf,
  install: true,
  install_dir: dbus_session_bus_services_dir,
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
  install_dir: gio_schemasdir,
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
  'gvfswritechannel.c',
)

deps = socket_deps + [
  gio_unix_dep,
  libgvfscommon_dep,
  gsettings_desktop_schemas_dep,
]

if enable_gcr
  deps += gcr_dep
endif

if enable_keyring
  deps += libsecret_dep
endif

cflags = [
  '-DLIBEXEC_DIR="@0@"'.format(gvfs_prefix / gvfs_libexecdir),
  '-DMOUNTABLE_DIR="@0@"'.format(gvfs_prefix / gvfs_mounts_dir),
  '-DGVFS_LOCALEDIR="@0@"'.format(gvfs_prefix / gvfs_localedir),
]

libgvfsdaemon = shared_library(
  'gvfsdaemon',
  sources: sources,
  include_directories: top_inc,
  dependencies: deps,
  c_args: cflags,
  install: true,
  install_rpath: gvfs_rpath,
  install_dir: gvfs_pkglibdir,
)

libgvfsdaemon_dep = declare_dependency(
  include_directories: '.',
  dependencies: libgvfscommon_dep,
  compile_args: cflags,
  link_with: libgvfsdaemon,
)

sources = files(
  'main.c',
  'mount.c',
)

executable(
  'gvfsd',
  sources,
  include_directories: top_inc,
  dependencies: libgvfsdaemon_dep,
  install: true,
  install_rpath: gvfs_rpath,
  install_dir: gvfs_libexecdir,
)

daemon_main_sources = files(
  'daemon-main.c',
  'daemon-main-generic.c',
)

programs = {}
mounts = []
schema_data = []
convert_data = []

cflags = [
  '-DBACKEND_HEADER=gvfsbackendlocaltest.h',
  '-DDEFAULT_BACKEND_TYPE=localtest',
  '-DBACKEND_TYPES="localtest", G_VFS_TYPE_BACKEND_LOCALTEST,',
]

programs += {'gvfsd-localtest': {'sources': files('gvfsbackendlocaltest.c'), 'c_args': cflags}}
mounts += ['localtest']

sources = files(
  'gvfsbackendftp.c',
  'gvfsftpconnection.c',
  'gvfsftpdircache.c',
  'gvfsftpfile.c',
  'gvfsftptask.c',
  'ParseFTPList.c',
)

cflags = [
  '-DBACKEND_HEADER=gvfsbackendftp.h',
  '-DDEFAULT_BACKEND_TYPE=ftp',
  '-DBACKEND_TYPES="ftp", G_VFS_TYPE_BACKEND_FTP, "ftps", G_VFS_TYPE_BACKEND_FTP, "ftpis", G_VFS_TYPE_BACKEND_FTP,',
  '-DMAX_JOB_THREADS=10',
]

programs += {'gvfsd-ftp': {'sources': sources, 'c_args': cflags}}
mounts += ['ftp', 'ftps', 'ftpis']

cflags = [
  '-DBACKEND_HEADER=gvfsbackendtrash.h',
  '-DDEFAULT_BACKEND_TYPE=trash',
  '-DBACKEND_TYPES="trash", G_VFS_TYPE_BACKEND_TRASH,',
  '-DMAX_JOB_THREADS=10',
]

programs += {'gvfsd-trash': {'sources': files('gvfsbackendtrash.c'), 'dependencies': [libtrash_dep], 'c_args': cflags}}
mounts += ['trash']

cflags = [
  '-DBACKEND_HEADER=gvfsbackendrecent.h',
  '-DDEFAULT_BACKEND_TYPE=recent',
  '-DBACKEND_TYPES="recent", G_VFS_TYPE_BACKEND_RECENT,',
  '-DBACKEND_USES_GVFS=1',
  '-DMAX_JOB_THREADS=10',
]

programs += {'gvfsd-recent': {'sources': files('gvfsbackendrecent.c'), 'c_args': cflags}}
mounts += ['recent']

cflags = [
  '-DBACKEND_HEADER=gvfsbackendcomputer.h',
  '-DDEFAULT_BACKEND_TYPE=computer',
  '-DBACKEND_TYPES="computer", G_VFS_TYPE_BACKEND_COMPUTER,',
  '-DBACKEND_USES_GVFS=1',
  '-DMAX_JOB_THREADS=1',
]

programs += {'gvfsd-computer': {'sources': files('gvfsbackendcomputer.c'), 'dependencies': [gio_unix_dep], 'c_args': cflags}}
mounts += ['computer']

cflags = [
  '-DBACKEND_HEADER=gvfsbackendnetwork.h',
  '-DDEFAULT_BACKEND_TYPE=network',
  '-DBACKEND_TYPES="network", G_VFS_TYPE_BACKEND_NETWORK,',
  '-DBACKEND_USES_GVFS=1',
  '-DMAX_JOB_THREADS=1',
]

programs += {'gvfsd-network': {'sources': files('gvfsbackendnetwork.c'), 'c_args': cflags}}
mounts += ['network']

if enable_burn
  cflags = [
    '-DBACKEND_HEADER=gvfsbackendburn.h',
    '-DDEFAULT_BACKEND_TYPE=burn',
    '-DBACKEND_TYPES="burn", G_VFS_TYPE_BACKEND_BURN,',
    '-DMAX_JOB_THREADS=1',
  ]

  programs += {'gvfsd-burn': {'sources': files('gvfsbackendburn.c'), 'dependencies': [gio_unix_dep], 'c_args': cflags}}
  mounts += ['burn']
endif

if enable_sftp
  sources = files(
    'gvfsbackendsftp.c',
    'pty_open.c',
  )

  deps = socket_deps + [
    gio_unix_dep,
    util_dep
  ]

  cflags = [
    '-DBACKEND_HEADER=gvfsbackendsftp.h',
    '-DDEFAULT_BACKEND_TYPE=sftp',
    '-DBACKEND_TYPES="sftp", G_VFS_TYPE_BACKEND_SFTP,',
    '-DMAX_JOB_THREADS=1',
    '-DSSH_PROGRAM="ssh"',
  ]

  programs += {'gvfsd-sftp': {'sources': sources, 'dependencies': deps, 'c_args': cflags}}
  mounts += ['sftp']
endif

if enable_samba
  sources = files('gvfsbackendsmb.c')

  cflags = [
    '-DBACKEND_HEADER=gvfsbackendsmb.h',
    '-DDEFAULT_BACKEND_TYPE=smb',
    '-DBACKEND_TYPES="smb-share", G_VFS_TYPE_BACKEND_SMB,',
    '-DMAX_JOB_THREADS=1',
  ]

  programs += {'gvfsd-smb': {'sources': sources, 'dependencies': [smbclient_dep], 'c_args': cflags}}
  mounts += ['smb']
  schema_data += files('org.gnome.system.smb.gschema.xml')
  convert_data += files('gvfs-smb.convert')

  cflags = [
    '-DBACKEND_HEADER=gvfsbackendsmbbrowse.h',
    '-DDEFAULT_BACKEND_TYPE=smb-network',
    '-DBACKEND_TYPES="smb-network", G_VFS_TYPE_BACKEND_SMB_BROWSE, "smb-server",  G_VFS_TYPE_BACKEND_SMB_BROWSE,',
    '-DMAX_JOB_THREADS=1',
    '-DMOUNTABLE_DBUS_NAME=' + gvfs_namespace + '.mountpoint_smb_browse',
  ]

  programs += {'gvfsd-smb-browse': {'sources': sources + files('gvfsbackendsmbbrowse.c'), 'dependencies': [smbclient_dep], 'c_args': cflags}}
  mounts += ['smb-browse']
endif

if enable_dnssd
  deps = [
    avahi_client_dep,
    avahi_glib_dep,
    libgvfscommon_dnssd_dep,
  ]

  cflags = [
    '-DBACKEND_HEADER=gvfsbackenddnssd.h',
    '-DDEFAULT_BACKEND_TYPE=dns-sd',
    '-DBACKEND_TYPES="dns-sd", G_VFS_TYPE_BACKEND_DNS_SD,',
    '-DMAX_JOB_THREADS=1',
    '-DMOUNTABLE_DBUS_NAME=' + gvfs_namespace + '.mountpoint_dnssd',
  ]

  programs += {'gvfsd-dnssd': {'sources': files('gvfsbackenddnssd.c'), 'dependencies': deps, 'c_args': cflags}}
  mounts += ['dns-sd']
  schema_data += files('org.gnome.system.dns_sd.gschema.xml')
  convert_data += files('gvfs-dns-sd.convert')
endif

if enable_archive
  cflags = [
    '-DBACKEND_HEADER=gvfsbackendarchive.h',
    '-DDEFAULT_BACKEND_TYPE=archive',
    '-DBACKEND_TYPES="archive", G_VFS_TYPE_BACKEND_ARCHIVE,',
    '-DMAX_JOB_THREADS=1',
    '-DBACKEND_USES_GVFS=1',
  ]

  programs += {'gvfsd-archive': {'sources': files('gvfsbackendarchive.c'), 'dependencies': [libarchive_dep], 'c_args': cflags}}
  mounts += ['archive']
endif

if enable_cdda
  deps = [
    gudev_dep,
    libcdio_paranoia_dep,
  ]

  cflags = [
    '-DBACKEND_HEADER=gvfsbackendcdda.h',
    '-DDEFAULT_BACKEND_TYPE=cdda',
    '-DBACKEND_TYPES="cdda", G_VFS_TYPE_BACKEND_CDDA,',
    '-DMAX_JOB_THREADS=1',
  ]

  programs += {'gvfsd-cdda': {'sources': files('gvfsbackendcdda.c'), 'dependencies': deps, 'c_args': cflags}}
  mounts += ['cdda']
endif

if enable_admin
  deps = [
    libcap_dep,
    polkit_gobject_dep,
  ]

  cflags = [
    '-DBACKEND_HEADER=gvfsbackendadmin.h',
    '-DDEFAULT_BACKEND_TYPE=admin',
    '-DBACKEND_TYPES="admin", G_VFS_TYPE_BACKEND_ADMIN,',
    '-DMOUNTABLE_DBUS_NAME=' + gvfs_namespace + '.mountpoint_admin',
  ]

  programs += {'gvfsd-admin': {'sources': files('gvfsbackendadmin.c'), 'dependencies': deps, 'c_args': cflags}}
  mounts += ['admin']

  policy = gvfs_namespace + '.file-operations.policy'

  i18n.merge_file(
    input: configure_file(
      input: policy + '.in.in',
      output: '@BASENAME@',
      configuration: service_conf,
    ),
    output: '@BASENAME@',
    po_dir: po_dir,
    install: true,
    install_dir: gvfs_datadir / 'polkit-1/actions',
  )

  configure_file(
    input: gvfs_namespace + '.file-operations.rules.in',
    output: '@BASENAME@',
    configuration: {'PRIVILEGED_GROUP': privileged_group},
    install_dir: gvfs_datadir / 'polkit-1/rules.d',
  )
endif

if enable_google
  deps = [
    goa_dep,
    libgdata_dep,
  ]

  cflags = [
    '-DBACKEND_HEADER=gvfsbackendgoogle.h',
    '-DDEFAULT_BACKEND_TYPE=google-drive',
    '-DBACKEND_TYPES="google-drive", G_VFS_TYPE_BACKEND_GOOGLE,',
  ]

  programs += {'gvfsd-google': {'sources': files('gvfsbackendgoogle.c'), 'dependencies': deps, 'c_args': cflags}}
  mounts += ['google']
endif

if enable_onedrive
  sources = files(
    'gvfsbackendonedrive.c',
  )
  deps = [
    goa_dep,
    msgraph_dep
  ]

  cflags = [
    '-DBACKEND_HEADER=gvfsbackendonedrive.h',
    '-DDEFAULT_BACKEND_TYPE=onedrive',
    '-DBACKEND_TYPES="onedrive", G_VFS_TYPE_BACKEND_ONEDRIVE,',
  ]

  programs += {'gvfsd-onedrive': {'sources': sources, 'dependencies': deps, 'c_args': cflags}}
  mounts += ['onedrive']
endif

if enable_gphoto2
  deps = [
    gio_unix_dep,
    libgphoto2_dep,
    libgvfscommon_gphoto2_dep,
  ]

  cflags = [
    '-DBACKEND_HEADER=gvfsbackendgphoto2.h',
    '-DDEFAULT_BACKEND_TYPE=gphoto2',
    '-DBACKEND_TYPES="gphoto2", G_VFS_TYPE_BACKEND_GPHOTO2,',
    '-DMAX_JOB_THREADS=1',
  ]

  programs += {'gvfsd-gphoto2': {'sources': files('gvfsbackendgphoto2.c'), 'dependencies': deps, 'c_args': cflags}}
  mounts += ['gphoto2']
endif

if enable_mtp
  deps = [
    libgvfscommon_gphoto2_dep,
    libmtp_dep
  ]

  cflags = [
    '-DBACKEND_HEADER=gvfsbackendmtp.h',
    '-DDEFAULT_BACKEND_TYPE=mtp',
    '-DBACKEND_TYPES="mtp", G_VFS_TYPE_BACKEND_MTP,',
    '-DMAX_JOB_THREADS=1',
  ]

  if enable_libusb
    deps += libusb_dep
  endif

  programs += {'gvfsd-mtp': {'sources': files('gvfsbackendmtp.c'), 'dependencies': deps, 'c_args': cflags}}
  mounts += ['mtp']
endif

if enable_http
  sources = files(
    'gvfsbackendhttp.c',
    'gvfshttpinputstream.c',
  )

  deps = [
    libsoup_dep,
    libxml_dep,
  ]

  cflags = [
    '-DBACKEND_HEADER=gvfsbackendhttp.h',
    '-DDEFAULT_BACKEND_TYPE=http',
    '-DBACKEND_TYPES="http", G_VFS_TYPE_BACKEND_HTTP,',
    '-DMAX_JOB_THREADS=1',
    '-DMOUNTABLE_DBUS_NAME=' + gvfs_namespace + '.mountpoint_http',
  ]

  programs += {'gvfsd-http': {'sources': sources, 'dependencies': deps, 'c_args': cflags}}
  mounts += ['http']

  cflags = [
    '-DBACKEND_HEADER=gvfsbackenddav.h',
    '-DDEFAULT_BACKEND_TYPE=dav',
    '-DMAX_JOB_THREADS=1',
  ]

  if enable_dnssd
    deps += libgvfscommon_dnssd_dep

    cflags += '-DBACKEND_TYPES="dav", G_VFS_TYPE_BACKEND_DAV, "dav+sd", G_VFS_TYPE_BACKEND_DAV, "davs+sd", G_VFS_TYPE_BACKEND_DAV,'

    mounts += ['dav+sd']
  else
    cflags += '-DBACKEND_TYPES="dav", G_VFS_TYPE_BACKEND_DAV,'
  endif

  programs += {'gvfsd-dav': {'sources': sources + files('gvfsbackenddav.c'), 'dependencies': deps, 'c_args': cflags}}
  mounts += ['dav']
endif

if enable_afc
  deps = [
    libimobiledevice_dep,
    libplist_dep,
  ]

  cflags = [
    '-DBACKEND_HEADER=gvfsbackendafc.h',
    '-DDEFAULT_BACKEND_TYPE=afc',
    '-DBACKEND_TYPES="afc", G_VFS_TYPE_BACKEND_AFC,',
    '-DMAX_JOB_THREADS=1',
    '-DBACKEND_USES_GVFS=1',
  ]

  programs += {'gvfsd-afc': {'sources': files('gvfsbackendafc.c'), 'dependencies': deps, 'c_args': cflags}}
  mounts += ['afc']
endif

if enable_afp
  common_sources = files(
    'gvfsafptypes.h',
    'gvfsafputils.c',
    'gvfsafpconnection.c',
    'gvfsafpserver.c',
    'gvfsafpvolume.c',
  )

  deps = []
  if enable_gcrypt
    deps += libgcrypt_dep
  endif

  cflags = [
    '-DBACKEND_HEADER=gvfsbackendafp.h',
    '-DDEFAULT_BACKEND_TYPE=afp-volume',
    '-DBACKEND_TYPES="afp-volume", G_VFS_TYPE_BACKEND_AFP,',
    '-DMAX_JOB_THREADS=1',
  ]

  programs += {'gvfsd-afp': {'sources': common_sources + files('gvfsbackendafp.c'), 'dependencies': deps, 'c_args': cflags}}
  mounts += ['afp']

  cflags = [
    '-DBACKEND_HEADER=gvfsbackendafpbrowse.h',
    '-DDEFAULT_BACKEND_TYPE=afp-server',
    '-DBACKEND_TYPES="afp-server", G_VFS_TYPE_BACKEND_AFP_BROWSE,',
    '-DMAX_JOB_THREADS=1',
  ]

  programs += {'gvfsd-afp-browse': {'sources': common_sources + files('gvfsbackendafpbrowse.c'), 'dependencies': deps, 'c_args': cflags}}
  mounts += ['afp-browse']
endif

if enable_nfs
  cflags = [
    '-DBACKEND_HEADER=gvfsbackendnfs.h',
    '-DDEFAULT_BACKEND_TYPE=nfs',
    '-DBACKEND_TYPES="nfs", G_VFS_TYPE_BACKEND_NFS,',
    '-DMAX_JOB_THREADS=1',
  ]

  programs += {'gvfsd-nfs': {'sources': files('gvfsbackendnfs.c'), 'dependencies': [libnfs_dep], 'c_args': cflags}}
  mounts += ['nfs']
endif

if enable_wsdd
  deps = [
    gio_unix_dep
  ]

  sources = files(
    'gvfswsdddevice.c',
    'gvfswsddservice.c',
    'gvfswsddresolver.c',
    'gvfsbackendwsdd.c',
  )

  cflags = [
    '-DBACKEND_HEADER=gvfsbackendwsdd.h',
    '-DDEFAULT_BACKEND_TYPE=wsdd',
    '-DBACKEND_TYPES="wsdd", G_VFS_TYPE_BACKEND_WSDD,',
    '-DMAX_JOB_THREADS=1',
    '-DMOUNTABLE_DBUS_NAME=' + gvfs_namespace + '.mountpoint_wsdd',
    '-DWSDD_PROGRAM="wsdd"',
  ]

  programs += {'gvfsd-wsdd': {'sources': sources, 'dependencies': deps, 'c_args': cflags}}
  mounts += ['wsdd']
  schema_data += files('org.gnome.system.wsdd.gschema.xml')
endif

foreach program, options: programs
  kwargs = {
    'sources': daemon_main_sources + options.get('sources', []),
    'dependencies': [libgvfsdaemon_dep] + options.get('dependencies', []),
    'c_args': options.get('c_args', ''),
  }

  executable(
    program,
    include_directories: top_inc,
    kwargs: kwargs,
    install: true,
    install_rpath: gvfs_rpath,
    install_dir: gvfs_libexecdir,
  )
endforeach

foreach mount: mounts
  configure_file(
    input: mount + '.mount.in',
    output: '@BASENAME@',
    configuration: service_conf,
    install: true,
    install_dir: gvfs_mounts_dir,
  )
endforeach

if enable_devel_utils
  sources = daemon_main_sources + files('gvfsbackendtest.c')

  cflags = [
    '-DBACKEND_HEADER=gvfsbackendtest.h',
    '-DDEFAULT_BACKEND_TYPE=test',
    '-DBACKEND_TYPES="test", G_VFS_TYPE_BACKEND_TEST,',
  ]

  executable(
    'gvfsd-test',
    sources,
    include_directories: top_inc,
    dependencies: libgvfsdaemon_dep,
    c_args: cflags,
  )
endif

install_data(
  schema_data,
  install_dir: gio_schemasdir,
)

install_data(
  convert_data,
  install_dir: gvfs_datadir / 'GConf/gsettings',
)
