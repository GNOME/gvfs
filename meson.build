project(
  'gvfs', 'c',
  version: '1.58.0',
  license: 'LGPL2+',
  default_options: [
    'buildtype=debugoptimized',
    'warning_level=1',
  ],
  meson_version: '>= 0.57.0',
)

gvfs_name = meson.project_name()
gvfs_version = meson.project_version()

gvfs_prefix = get_option('prefix')
gvfs_datadir = get_option('datadir')
gvfs_libdir = get_option('libdir')
gvfs_libexecdir = get_option('libexecdir')
gvfs_localedir = get_option('localedir')
gvfs_mandir = get_option('mandir')

gvfs_pkgdatadir = gvfs_datadir / gvfs_name
gvfs_pkglibdir = gvfs_libdir / gvfs_name

gvfs_rpath = gvfs_prefix / gvfs_pkglibdir

gvfs_remote_volume_monitors_dir = gvfs_pkgdatadir / 'remote-volume-monitors'

gvfs_mounts_dir = gvfs_pkgdatadir / 'mounts'

gvfs_namespace = 'org.gtk.vfs'

gvfs_debug = get_option('buildtype').contains('debug')

gnome = import('gnome')
i18n = import('i18n')
pkg = import('pkgconfig')

po_dir = meson.current_source_dir() / 'po'

top_inc = include_directories('.')

service_conf = {'libexecdir': gvfs_prefix / gvfs_libexecdir}

cc = meson.get_compiler('c')

config_h = configuration_data()

# defines
set_defines = {
  # package
  'PACKAGE_STRING': '@0@ @1@'.format(gvfs_name, gvfs_version),
  'VERSION': gvfs_version,
  # i18n
  'GETTEXT_PACKAGE': gvfs_name,
}

foreach define, value: set_defines
  config_h.set_quoted(define, value)
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
config_h.set('HAVE_UTIL_H', cc.has_header('util.h', dependencies: util_dep))

have_openpty = cc.has_function('openpty', dependencies: util_dep)

config_h.set('HAVE_OPENPTY', have_openpty,
             description: 'Define if you have the openpty function.')

config_h.set('HAVE_LOGIN_TTY', cc.has_function('login_tty', dependencies: util_dep), description: 'Whether login_tty is available')

# headers
check_headers = [
  # fs + pty
  'sys/param.h',
  # fs
  'sys/mount.h',
  'sys/statfs.h',
  'sys/statvfs.h',
  'sys/vfs.h',
]

statfs_includes = ''

foreach header: check_headers
  has_header = cc.has_header(header)
  config_h.set10('HAVE_' + header.underscorify().to_upper(), has_header)
  if has_header
    statfs_includes += '#include <@0@>\n'.format(header)
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
  'stropts.h',
  'sys/un.h',
  'termios.h',
  'utmp.h',
]

foreach header: check_headers
  config_h.set('HAVE_' + header.underscorify().to_upper(), cc.has_header(header))
endforeach

# functions
check_functions = [
  # pty
  'getpt',
  'grantpt',
  'posix_openpt',
  'ptsname',
  'ptsname_r',
  'unlockpt',
  # fs
  'statfs',
  'statvfs',
]

foreach func: check_functions
  config_h.set('HAVE_' + func.to_upper(), cc.has_function(func))
endforeach

# symbols
check_symbols = [
  # i18n
  ['_NL_ADDRESS_LANG_TERM', 'langinfo.h'],
  ['_NL_ADDRESS_COUNTRY_AB3', 'langinfo.h'],
]

foreach symbol: check_symbols
  config_h.set('HAVE' + symbol[0], cc.has_header_symbol(symbol[1], symbol[0]))
endforeach

# fs, check major, minor and makedev functions
foreach name: ['mkdev', 'sysmacros']
  have_major = true
  header = 'sys/@0@.h'.format(name)
  foreach func: ['major', 'minor', 'makedev']
    if not cc.has_header_symbol(header, func)
      have_major = false
      break
    endif
  endforeach

  config_h.set10('MAJOR_IN_' + name.to_upper(), have_major)
endforeach

# types
check_types = {
  # type, fallback type
  'gid_t': 'int',
  'pid_t': 'int',
  'size_t': 'unsigned int',
  'uid_t': 'int',
}

foreach type, value: check_types
  if not cc.has_type(type, prefix: '#include<sys/types.h>')
    config_h.set(type, value)
  endif
endforeach

# members
check_members = [
  # define, typename, membername, prefix
  ['struct statfs', 'f_bavail', 'sys/statfs.h'],
  ['struct statvfs', 'f_basetype', 'sys/statvfs.h'],
  ['struct stat', 'st_atimensec', 'sys/stat.h'],
  ['struct stat', 'st_atim.tv_nsec', 'sys/stat.h'],
  ['struct stat', 'st_ctimensec', 'sys/stat.h'],
  ['struct stat', 'st_ctim.tv_nsec', 'sys/stat.h'],
  ['struct stat', 'st_mtimensec', 'sys/stat.h'],
  ['struct stat', 'st_mtim.tv_nsec', 'sys/stat.h'],
]

foreach member: check_members
  config_h.set('HAVE_@0@_@1@'.format(member[0].underscorify().to_upper(), member[1].underscorify().to_upper()), cc.has_members(member[0], member[1], prefix: '#include<@0@>'.format(member[2])))
endforeach

# compiler flags
common_flags = ['-DHAVE_CONFIG_H']

if gvfs_debug
  common_flags += cc.get_supported_arguments([
    '-Wcast-align',
    '-Wmissing-declarations',
    '-Wmissing-prototypes',
    '-Wnested-externs',
    '-Wno-sign-compare',
    '-Wno-strict-aliasing',
  ])
endif

add_project_arguments(common_flags, language: 'c')

version_script_ldflag = '-Wl,--version-script'
have_version_script = cc.has_link_argument('@0@,@1@'.format(version_script_ldflag, meson.current_source_dir() / 'client/symbol.map'))

gio_dep = dependency('gio-2.0')
gio_unix_dep = dependency('gio-unix-2.0')
glib_dep = dependency('glib-2.0', version: '>= 2.83.0')
gobject_dep = dependency('gobject-2.0')
gsettings_desktop_schemas_dep = dependency('gsettings-desktop-schemas', version: '>= 3.33.0')

# *** Check for libXML ***
libxml_dep = dependency('libxml-2.0', required: false)
have_libxml = libxml_dep.found()

# *** Check for libgcrypt ***
enable_gcrypt = get_option('gcrypt')
if enable_gcrypt
  libgcrypt_dep = dependency('libgcrypt', version: '>= 1.2.2')
endif
config_h.set('HAVE_GCRYPT', enable_gcrypt)

# *** Check for dbus service dir ***
dbus_session_bus_services_dir = dependency('dbus-1').get_variable(
  pkgconfig: 'session_bus_services_dir',
  pkgconfig_define: ['datadir', gvfs_prefix / gvfs_datadir],
)

dbus_service_in = files('dbus.service.in')

# *** Check for giomoduledir and schemasdir ***
gio_giomoduledir = gio_dep.get_variable(
  pkgconfig: 'giomoduledir',
  pkgconfig_define: ['libdir', gvfs_prefix / gvfs_libdir],
)

gio_schemasdir = gio_dep.get_variable(
  pkgconfig: 'schemasdir',
  pkgconfig_define: ['datadir', gvfs_prefix / gvfs_datadir],
  default_value: gvfs_prefix / gvfs_datadir / 'glib-2.0/schemas',
)

# *** Check for systemd options ***
systemd_systemduserunitdir = get_option('systemduserunitdir')
install_systemd_systemduserunitdir = (systemd_systemduserunitdir != 'no')

systemd_tmpfilesdir = get_option('tmpfilesdir')
install_systemd_tmpfilesdir = (systemd_tmpfilesdir != 'no')

if install_systemd_systemduserunitdir or install_systemd_tmpfilesdir
  if systemd_systemduserunitdir == '' or systemd_tmpfilesdir == ''
    systemd_dep = dependency('systemd', version: '>= 206', required: false)

    if install_systemd_systemduserunitdir and systemd_systemduserunitdir == ''
      assert(systemd_dep.found(), 'systemd required but not found, please provide a valid systemd user unit dir or disable it')
      systemd_systemduserunitdir = systemd_dep.get_variable(pkgconfig: 'systemduserunitdir', pkgconfig_define: ['prefix', gvfs_prefix])
    endif

    if install_systemd_tmpfilesdir and systemd_tmpfilesdir == ''
      assert(systemd_dep.found(), 'systemd not found, if you use opentmpfiles please provide a valid systemd user unit dir or disable it')
      systemd_tmpfilesdir = systemd_dep.get_variable(pkgconfig: 'tmpfilesdir', pkgconfig_define: ['prefix', gvfs_prefix])
    endif
  endif
endif

# *** Check for gcr ***
enable_gcr = get_option('gcr')
if enable_gcr
  gcr_dep = dependency('gcr-4')
endif
config_h.set('HAVE_GCR', enable_gcr)

# *** Check if we should build with admin backend ***
privileged_group = get_option('privileged_group')
enable_admin = get_option('admin')
if enable_admin
  libcap_dep = dependency('libcap')
  polkit_gobject_dep = dependency('polkit-gobject-1', version: '>= 0.114')
endif

# *** Check if we should build with http backend ***
enable_http = get_option('http')
if enable_http
  assert(have_libxml, 'http required but libxml-2.0 not found')
  libsoup_dep = dependency('libsoup-3.0', version: '>= 3.0.0')
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
  fuse_dep = dependency('fuse3', version: '>= 3.0.0')
endif
config_h.set('HAVE_FUSE', enable_fuse)

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
  libplist_dep = dependency('libplist-2.0', required: false)
  if not libplist_dep.found()
    libplist_dep = dependency('libplist', version: '>= 0.15')
  endif
endif

# *** Check if we should build with GOA volume monitor ***
enable_goa = get_option('goa')
if enable_goa
  goa_dep = dependency('goa-1.0', version: '>= 3.53.1')
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
  config_h.set('HAVE_SMBC_READDIRPLUS2', cc.has_function('smbc_readdirplus2', dependencies: smbclient_dep))
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

  config_h.set('HAVE_CDDA', enable_cdda)
endif

# *** Check if we should build with Google backend ***
enable_google = get_option('google')
if enable_google
  assert(enable_goa, 'Google backend requested but GOA is required')

  libgdata_dep = dependency('libgdata', version: '>= 0.18.0')
endif

# *** Check if we should build with OneDrive backend ***
enable_onedrive = get_option('onedrive')
if enable_onedrive
  assert(enable_goa, 'OneDrive backend requested but GOA is required')

  msgraph_dep = dependency('msgraph-1', version: '>= 0.3.0')
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

  libmtp_dep = dependency('libmtp', version: '>= 1.1.12')
  foreach version: ['1.1.15', '1.1.21']
    config_h.set10('HAVE_LIBMTP_' + version.underscorify(), libmtp_dep.version().version_compare('>= ' + version))
  endforeach
endif
config_h.set('HAVE_LIBMTP', enable_mtp)

# *** AFP backend ***
enable_afp = get_option('afp')

# *** NFS backend ***
enable_nfs = get_option('nfs')
if enable_nfs
  libnfs_dep = dependency('libnfs', version: '>= 1.9.8')
endif

# *** SFTP backend ***
enable_sftp = get_option('sftp')

# *** WSDD backend ***
enable_wsdd = get_option('wsdd')

# *** burn backend ***
enable_burn = get_option('burn')
config_h.set('HAVE_BURN', enable_burn)

# *** Enable development utils ***
enable_deprecated_apis = get_option('deprecated_apis')
enable_devel_utils = get_option('devel_utils')
enable_installed_tests = get_option('installed_tests')
enable_unit_tests = get_option('unit_tests')

subdir('common')
subdir('metadata')
subdir('client')
subdir('daemon')
subdir('monitor')
subdir('po')

enable_man = get_option('man')
if enable_man
  subdir('man')
endif

subdir('test')

configure_file(
  output: 'config.h',
  configuration: config_h,
)

gnome.post_install(
  glib_compile_schemas: true,
  gio_querymodules: gio_giomoduledir,
)

summary({
  'systemduserunitdir': systemd_systemduserunitdir,
  'tmpfilesdir': systemd_tmpfilesdir,
  'privileged_group': privileged_group,
}, section: 'Configuration')

summary({
  'admin': enable_admin,
  'afc': enable_afc,
  'afp': enable_afp,
  'archive': enable_archive,
  'burn': enable_burn,
  'cdda': enable_cdda,
  'dnssd': enable_dnssd,
  'goa': enable_goa,
  'google': enable_google,
  'gphoto2': enable_gphoto2,
  'http': enable_http,
  'mtp': enable_mtp,
  'nfs': enable_nfs,
  'onedrive': enable_onedrive,
  'sftp': enable_sftp,
  'smb': enable_samba,
  'udisks2': enable_udisks2,
  'wsdd': enable_wsdd,
}, section: 'Backends')

summary({
  'bluray': enable_bluray,
  'fuse': enable_fuse,
  'gcr': enable_gcr,
  'gcrypt': enable_gcrypt,
  'gudev': enable_gudev,
  'keyring': enable_keyring,
  'logind': enable_logind,
  'libusb': enable_libusb,
}, section: 'Dependencies')

summary({
  'deprecated_apis': enable_deprecated_apis,
  'devel_utils': enable_devel_utils,
  'installed_tests': enable_installed_tests,
  'man': enable_man,
})
