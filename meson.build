project(
  'gvfs', 'c',
  version: '1.36.2',
  license: 'LGPL2+',
  default_options: [
    'b_lundef=true',
    'buildtype=debugoptimized'
  ],
  meson_version: '>= 0.43.0'
)

gvfs_name = meson.project_name()
gvfs_version = meson.project_version()

gvfs_prefix = get_option('prefix')
gvfs_bindir = join_paths(gvfs_prefix, get_option('bindir'))
gvfs_datadir = join_paths(gvfs_prefix, get_option('datadir'))
gvfs_includedir = join_paths(gvfs_prefix, get_option('includedir'))
gvfs_libdir = join_paths(gvfs_prefix, get_option('libdir'))
gvfs_libexecdir = join_paths(gvfs_prefix, get_option('libexecdir'))
gvfs_localedir = join_paths(gvfs_prefix, get_option('localedir'))
gvfs_mandir = join_paths(gvfs_prefix, get_option('mandir'))

gvfs_pkgdatadir = join_paths(gvfs_datadir, gvfs_name)
gvfs_pkglibdir = join_paths(gvfs_libdir, gvfs_name)

gvfs_rpath = gvfs_pkglibdir

gvfs_remote_volume_monitors_dir = join_paths(gvfs_pkgdatadir, 'remote-volume-monitors')

gvfs_mountdir = join_paths(gvfs_pkgdatadir, 'mounts')
gvfs_schema_dir = join_paths(gvfs_datadir, 'glib-2.0', 'schemas')

gvfs_namespace = 'org.gtk.vfs'

gvfs_debug = get_option('buildtype').contains('debug')

cc = meson.get_compiler('c')

config_h = configuration_data()

# defines
set_defines = [
  # package
  ['PACKAGE', gvfs_name],
  ['PACKAGE_BUGREPORT', 'http://bugzilla.gnome.org/enter_bug.cgi?product=' + gvfs_name],
  ['PACKAGE_NAME', gvfs_name],
  ['PACKAGE_STRING', '@0@ @1@'.format(gvfs_name, gvfs_version)],
  ['VERSION', gvfs_version],
  # i18n
  ['GETTEXT_PACKAGE', gvfs_name]
]

foreach define: set_defines
  config_h.set_quoted(define[0], define[1])
endforeach

# Globally define_GNU_SOURCE and therefore enable the GNU extensions
config_h.set('_GNU_SOURCE', true)

# Pull in the right libraries for various functions which might not be
# bundled into an exploded libc.
have_socketpair = cc.has_function('socketpair')
socket_deps = []

if not have_socketpair
  socket_dep = cc.find_library('socket', required: false)
  have_socketpair = socket_dep.found() and cc.has_function('socketpair', dependencies: socket_dep)

  if have_socketpair
    socket_deps += socket_dep
  endif
endif

config_h.set('HAVE_SOCKETPAIR', have_socketpair,
             description: 'Define if you have the socketpair function.')

util_dep = cc.find_library('util', required: false)
config_h.set('HAVE_UTIL_H', cc.has_header('util.h', dependendencies: util_dep))

have_openpty = cc.has_function('openpty')
if not have_openpty
  have_openpty = util_dep.found() and cc.has_function('openpty', dependencies: util_dep)
endif

config_h.set('HAVE_OPENPTY', have_openpty,
             description: 'Define if you have the openpty function.')

config_h.set('HAVE_LOGIN_TTY', util_dep.found() and cc.has_function('login_tty', dependencies: util_dep),
             description: 'Whether login_tty is available')

# headers
check_headers = [
  # fs + pty
  ['HAVE_SYS_PARAM_H', 'sys/param.h'],
  # fs
  ['HAVE_SYS_MOUNT_H', 'sys/mount.h'],
  ['HAVE_SYS_STATFS_H', 'sys/statfs.h'],
  ['HAVE_SYS_STATVFS_H', 'sys/statvfs.h'],
  ['HAVE_SYS_VFS_H', 'sys/vfs.h']
]

statfs_includes = ''

foreach header: check_headers
  has_header = cc.has_header(header[1])
  config_h.set10(header[0], has_header)
  if has_header
    statfs_includes += '#include <@0@>\n'.format(header[1])
  endif
endforeach

# if statfs() takes 2 arguments (Posix) or 4 (Solaris)
statfs_code = statfs_includes + '''
  int main() {
    struct statfs st;
    @0@;
  };
'''

if cc.compiles(statfs_code.format('statfs("/", &st)'))
  config_h.set('STATFS_ARGS', 2)
elif cc.compiles(statfs_code.format('statfs("/", &st, sizeof (st), 0)'))
  config_h.set('STATFS_ARGS', 4)
else
  error('unable to determine number of arguments to statfs()')
endif

# pty
check_headers = [
  ['HAVE_STROPTS_H', 'stropts.h'],
  ['HAVE_SYS_UN_H', 'sys/un.h'],
  ['HAVE_TERMIOS_H', 'termios.h'],
  ['HAVE_UTMP_H', 'utmp.h']
]

foreach header: check_headers
  config_h.set(header[0], cc.has_header(header[1]))
endforeach

# functions
check_functions = [
  # pty
  ['HAVE_GETPT', 'getpt'],
  ['HAVE_GRANTPT', 'grantpt'],
  ['HAVE_POSIX_OPENPT', 'posix_openpt'],
  ['HAVE_PTSNAME', 'ptsname'],
  ['HAVE_PTSNAME_R', 'ptsname_r'],
  ['HAVE_UNLOCKPT', 'unlockpt'],
  # fs
  ['HAVE_STATFS', 'statfs'],
  ['HAVE_STATVFS', 'statvfs']
]

foreach func: check_functions
  config_h.set(func[0], cc.has_function(func[1]))
endforeach

# symbols
check_symbols = [
  # i18n
  ['HAVE_NL_ADDRESS_LANG_TERM', 'langinfo.h', '_NL_ADDRESS_LANG_TERM'],
  ['HAVE_NL_ADDRESS_COUNTRY_AB3', 'langinfo.h', '_NL_ADDRESS_COUNTRY_AB3']
]

foreach symbol: check_symbols
  config_h.set(symbol[0], cc.has_header_symbol(symbol[1], symbol[2]))
endforeach

# fs, check major, minor and makedev functions
check_major_functions = ['major', 'minor', 'makedev']

check_major_headers = [
  ['MAJOR_IN_MKDEV', 'sys/mkdev.h'],
  ['MAJOR_IN_SYSMACROS', 'sys/sysmacros.h']
]

foreach header: check_major_headers
  have_major = true
  foreach function: check_major_functions
    have_major = have_major and cc.has_header_symbol(header[1], function)
  endforeach

  config_h.set10(header[0], have_major)
endforeach

# types
check_types = [
  # type, header, fallback type
  ['gid_t', 'sys/types.h', 'int'],
  ['pid_t', 'sys/types.h', 'int'],
  ['size_t', 'sys/types.h', 'unsigned int'],
  ['uid_t', 'sys/types.h', 'int']
]

foreach type: check_types
  if not cc.has_type(type[0], prefix: '#include<@0@>'.format(type[1]))
    config_h.set(type[0], type[2])
  endif
endforeach

# members
check_members = [
  # define, typename, membername, prefix
  ['HAVE_STRUCT_STATFS_F_BAVAIL', 'struct statfs', 'f_bavail', 'sys/statfs.h'],
  ['HAVE_STRUCT_STATVFS_F_BASETYPE', 'struct statvfs', 'f_basetype', 'sys/statvfs.h'],
  ['HAVE_STRUCT_STAT_ST_ATIMENSEC', 'struct stat', 'st_atimensec', 'sys/stat.h'],
  ['HAVE_STRUCT_STAT_ST_ATIM_TV_NSEC', 'struct stat', 'st_atim.tv_nsec', 'sys/stat.h'],
  ['HAVE_STRUCT_STAT_ST_CTIMENSEC', 'struct stat', 'st_ctimensec', 'sys/stat.h'],
  ['HAVE_STRUCT_STAT_ST_CTIM_TV_NSEC', 'struct stat', 'st_ctim.tv_nsec', 'sys/stat.h'],
  ['HAVE_STRUCT_STAT_ST_MTIMENSEC', 'struct stat', 'st_mtimensec', 'sys/stat.h'],
  ['HAVE_STRUCT_STAT_ST_MTIM_TV_NSEC', 'struct stat', 'st_mtim.tv_nsec', 'sys/stat.h']
]

foreach member: check_members
  config_h.set(member[0], cc.has_members(member[1], member[2], prefix: '#include<@0@>'.format(member[3])))
endforeach

# compiler flags
common_flags = ['-DHAVE_CONFIG_H']

if gvfs_debug
  test_cflags = [
    '-Wcast-align',
    '-Wmissing-declarations',
    '-Wmissing-prototypes',
    '-Wnested-externs',
    '-Wno-sign-compare',
    '-Wno-strict-aliasing'
  ]

  common_flags += cc.get_supported_arguments(test_cflags)
endif

add_project_arguments(common_flags, language: 'c')

ldflag = '-Wl,--version-script'
have_version_script = host_machine.system().contains('linux') and cc.has_argument(ldflag)

gio_dep = dependency('gio-2.0')

glib_deps = [
  gio_dep,
  dependency('gio-unix-2.0'),
  dependency('glib-2.0', version: '>= 2.51.0'),
  dependency('gmodule-no-export-2.0'),
  dependency('gobject-2.0')
]

# *** Check for libXML ***
libxml_dep = dependency('libxml-2.0', required: false)
have_libxml = libxml_dep.found()

# *** Check for libgcrypt ***
enable_gcrypt = get_option('gcrypt')
if enable_gcrypt
  libgcrypt_req_version = '>= 1.2.2'

  libgcrypt_config = find_program('libgcrypt-config')
  libgcrypt_version = run_command(libgcrypt_config, '--version').stdout().strip()

  assert(libgcrypt_version.version_compare(libgcrypt_req_version), 'gcrypt required but libgcrypt ' + libgcrypt_req_version + ' not found')

  libgcrypt_cflags = run_command(libgcrypt_config, '--cflags').stdout().strip().split()
  libgcrypt_libs = run_command(libgcrypt_config, '--libs').stdout().strip().split()

  libgcrypt_dep = declare_dependency(
    compile_args: libgcrypt_cflags,
    link_args: libgcrypt_libs,
    version: libgcrypt_version
  )
endif
config_h.set('HAVE_GCRYPT', enable_gcrypt)

# *** Check for dbus service dir ***
dbus_service_dir = get_option('dbus_service_dir')
if dbus_service_dir == ''
  dbus_dep = dependency('dbus-1', required: false)
  assert(dbus_dep.found(), 'dbus-1 required but not found, please provide a valid D-Bus service dir')
  dbus_service_dir = dbus_dep.get_pkgconfig_variable('session_bus_services_dir')
endif

dbus_service_in = files('dbus.service.in')

gio_module_dir = get_option('gio_module_dir')
if gio_module_dir == ''
  gio_module_dir = gio_dep.get_pkgconfig_variable('giomoduledir')
endif

# *** Check for systemd options ***
systemd_user_unit_dir = get_option('systemduserunitdir')
install_systemd_user_unit_dir = (systemd_user_unit_dir != 'no')

tmp_files_dir = get_option('tmpfilesdir')
install_tmp_files_dir = (tmp_files_dir != 'no')

if install_systemd_user_unit_dir or install_tmp_files_dir
  if systemd_user_unit_dir == '' or tmp_files_dir == ''
    systemd_dep = dependency('systemd', version: '>= 206', required: false)

    if install_systemd_user_unit_dir and systemd_user_unit_dir == ''
      assert(systemd_dep.found(), 'systemd required but not found, please provide a valid systemd user unit dir or disable it')
      systemd_user_unit_dir = systemd_dep.get_pkgconfig_variable('systemduserunitdir')
    endif

    if install_tmp_files_dir and tmp_files_dir == ''
      assert(systemd_dep.found(), 'systemd not found, if you use opentmpfiles please provide a valid systemd user unit dir or disable it')
      tmp_files_dir = systemd_dep.get_pkgconfig_variable('tmpfilesdir')
    endif
  endif
endif

# *** Check for gcr ***
enable_gcr = get_option('gcr')
if enable_gcr
  gcr_dep = dependency('gcr-base-3')
endif
config_h.set('HAVE_GCR', enable_gcr)

# *** Check if we should build with admin backend ***
enable_admin = get_option('admin')
if enable_admin
  libcap_dep = dependency('libcap')
  polkit_gobject_dep = dependency('polkit-gobject-1')
endif

# *** Check if we should build with http backend ***
enable_http = get_option('http')
if enable_http
  assert(have_libxml, 'http required but libxml-2.0 not found')
  libsoup_dep = dependency('libsoup-2.4', version: '>= 2.42.0')
endif

# *** Check if we should build with DNS-SD backend ***
enable_dnssd = get_option('dnssd')
if enable_dnssd
  avahi_client_dep = dependency('avahi-client', version: '>= 0.6')
  avahi_glib_dep = dependency('avahi-glib', version: '>= 0.6')
endif
config_h.set('HAVE_AVAHI', enable_dnssd)

# *** Check for gudev ***
enable_gudev = get_option('gudev')
if enable_gudev
  gudev_dep = dependency('gudev-1.0', version: '>= 147')
endif
config_h.set('HAVE_GUDEV', enable_gudev)

# *** Check for FUSE ***
enable_fuse = get_option('fuse')
if enable_fuse
  fuse_dep = dependency('fuse', version: '>= 2.8.0')
endif
config_h.set('HAVE_FUSE', enable_fuse)

# *** Check for gnome-disk-utility ***
enable_gdu = get_option('gdu')
if enable_gdu
  gdu_dep = dependency('gdu', version: '>= 3.0.2')
endif

# *** Check for udisks2 ***
enable_udisks2 = get_option('udisks2')
if enable_udisks2
  assert(enable_gudev, 'UDisks2 requested but gudev is required')
  udisks2_dep = dependency('udisks2', version: '>= 1.97')
endif

# *** Check for libsystemd-login ***
enable_logind = get_option('logind')
if enable_logind
  logind_dep = dependency('libsystemd', required: false)
  if not logind_dep.found()
    logind_dep = dependency('libelogind', version: '>= 229', required: false)
  endif

  assert(logind_dep.found(), 'logind requested but libsystemd nor libelogind not found')
endif
config_h.set('HAVE_LOGIND', enable_logind)

# *** Check if we should build with AFC backend ***
enable_afc = get_option('afc')
if enable_afc
  libimobiledevice_dep = dependency('libimobiledevice-1.0', version: '>= 1.2')
  libplist_dep = dependency('libplist', version: '>= 0.15')
endif

# *** Check if we should build with GOA volume monitor ***
enable_goa = get_option('goa')
if enable_goa
  goa_dep = dependency('goa-1.0', version: '>= 3.17.1')
endif

# *** Check for GNOME Keyring ***
enable_keyring = get_option('keyring')
if enable_keyring
  libsecret_dep = dependency('libsecret-unstable')
endif
config_h.set('HAVE_KEYRING', enable_keyring)

# *** Check if we should build with libbluray ***
enable_bluray = get_option('bluray')
if enable_bluray
  libbluray_dep = dependency('libbluray')
endif
config_h.set('HAVE_BLURAY', enable_bluray)

# *** Check if we should build with libusb-1.0 ***
enable_libusb = get_option('libusb')
if enable_libusb
  libusb_dep = dependency('libusb-1.0', version: '>= 1.0.21')
endif
config_h.set10('HAVE_LIBUSB', enable_libusb)

# *** Check for samba ***
enable_samba = get_option('smb')
if enable_samba
  smbclient_dep = dependency('smbclient')
endif

# *** Check for libarchive ***
enable_archive = get_option('archive')
if enable_archive
  libarchive_dep = dependency('libarchive')
endif

# *** Check if we should build with CDDA backend ***
enable_cdda = get_option('cdda')
if enable_cdda
  assert(enable_gudev, 'CDDA backend requested but gudev is required')

  libcdio_paranoia_dep = dependency('libcdio_paranoia', version: '>= 0.78.2')
  config_h.set('HAVE_PARANOIA_NEW_INCLUDES', cc.has_header('cdio/paranoia/paranoia.h', dependencies: libcdio_paranoia_dep))
endif

# *** Check if we should build with Google backend ***
enable_google = get_option('google')
if enable_google
  assert(enable_goa, 'Google backend requested but GOA is required')

  libgdata_dep = dependency('libgdata', version: '>= 0.17.3')
  config_h.set10('HAVE_LIBGDATA_0_17_7', libgdata_dep.version().version_compare('>= 0.17.7'))
  config_h.set10('HAVE_LIBGDATA_0_17_9', libgdata_dep.version().version_compare('>= 0.17.9'))
endif

# *** Check for gphoto2 ***
enable_gphoto2 = get_option('gphoto2')
if enable_gphoto2
  assert(enable_gudev, 'gphoto2 requested but gudev is required')
  assert(host_machine.system().contains('linux') or host_machine.system().contains('bsd'),
         'Cannot build with gphoto2 support. Need OS tweaks in volume monitor.')

  libgphoto2_dep = dependency('libgphoto2', version: '>= 2.4.0')
  config_h.set('HAVE_GPHOTO25', libgphoto2_dep.version().version_compare('>= 2.5.0'))
endif

# *** Check for libmtp ***
enable_mtp = get_option('mtp')
if enable_mtp
  assert(enable_gudev, 'libmtp requested but gudev is required')

  libmtp_dep = dependency('libmtp', version: '>= 1.1.0')
  config_h.set10('HAVE_LIBMTP_1_1_5', libmtp_dep.version().version_compare('>= 1.1.5'))
  config_h.set10('HAVE_LIBMTP_1_1_6', libmtp_dep.version().version_compare('>= 1.1.6'))
  config_h.set10('HAVE_LIBMTP_1_1_9', libmtp_dep.version().version_compare('>= 1.1.9'))
  config_h.set10('HAVE_LIBMTP_1_1_12', libmtp_dep.version().version_compare('>= 1.1.12'))
endif
config_h.set('HAVE_LIBMTP', enable_mtp)

# *** AFP backend ***
enable_afp = get_option('afp')

# *** NFS backend ***
enable_nfs = get_option('nfs')
if enable_nfs
  libnfs_dep = dependency('libnfs', version: '>= 1.9.8')
endif

# *** Enable development utils ***
enable_devel_utils = get_option('devel_utils')
enable_installed_tests = get_option('installed_tests')

gnome = import('gnome')
i18n = import('i18n')
pkg = import('pkgconfig')

service_conf = configuration_data()
service_conf.set('libexecdir', gvfs_libexecdir)

its_dir = join_paths(meson.source_root(), 'gettext')
po_dir = join_paths(meson.source_root(), 'po')

top_inc = include_directories('.')

subdir('common')
subdir('metadata')
subdir('client')
subdir('daemon')
subdir('monitor')
subdir('po')

enable_deprecated_programs = get_option('deprecated_programs')
if enable_deprecated_programs
  subdir('programs')
endif

enable_man = get_option('man')
if enable_man
  subdir('man')
endif

subdir('test')

configure_file(
  output: 'config.h',
  configuration: config_h
)

meson.add_install_script(
  'meson_post_install.py',
  gvfs_datadir,
  gvfs_libdir
)

output = gvfs_name + ' ' + gvfs_version + ' configuration summary:\n'
output += '\n'
output += '  dbus_service_dir: ' + dbus_service_dir + '\n'
output += '  gio_module_dir: ' + gio_module_dir + '\n'
output += '  systemduserunitdir: ' + systemd_user_unit_dir + '\n'
output += '  tmpfilesdir: ' + tmp_files_dir + '\n'
output += '\n'
output += '  admin: ' + enable_admin.to_string() + '\n'
output += '  afc: ' + enable_afc.to_string() + '\n'
output += '  afp: ' + enable_afp.to_string() + '\n'
output += '  archive: ' + enable_archive.to_string() + '\n'
output += '  cdda: ' + enable_cdda.to_string() + '\n'
output += '  dnssd: ' + enable_dnssd.to_string() + '\n'
output += '  gdu: ' + enable_gdu.to_string() + '\n'
output += '  goa: ' + enable_goa.to_string() + '\n'
output += '  google: ' + enable_google.to_string() + '\n'
output += '  gphoto2: ' + enable_gphoto2.to_string() + '\n'
output += '  http: ' + enable_http.to_string() + '\n'
output += '  mtp: ' + enable_mtp.to_string() + '\n'
output += '  nfs: ' + enable_nfs.to_string() + '\n'
output += '  smb: ' + enable_samba.to_string() + '\n'
output += '  udisks2: ' + enable_udisks2.to_string() + '\n'
output += '\n'
output += '  bluray: ' + enable_bluray.to_string() + '\n'
output += '  fuse: ' + enable_fuse.to_string() + '\n'
output += '  gcr: ' + enable_gcr.to_string() + '\n'
output += '  gcrypt: ' + enable_gcrypt.to_string() + '\n'
output += '  gudev: ' + enable_gudev.to_string() + '\n'
output += '  keyring: ' + enable_keyring.to_string() + '\n'
output += '  logind: ' + enable_logind.to_string() + '\n'
output += '  libusb: ' + enable_libusb.to_string() + '\n'
output += '\n'
output += '  deprecated_programs: ' + enable_deprecated_programs.to_string() + '\n'
output += '  devel_utils: ' + enable_devel_utils.to_string() + '\n'
output += '  installed_tests: ' + enable_installed_tests.to_string() + '\n'
output += '  man: ' + enable_man.to_string() + '\n'
message(output)
