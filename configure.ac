AC_PREREQ([2.64])

AC_INIT([gvfs],[1.36.2],[http://bugzilla.gnome.org/enter_bug.cgi?product=gvfs])
AM_INIT_AUTOMAKE([1.11.1 no-dist-gzip dist-xz tar-ustar -Wno-portability])

AC_CONFIG_HEADERS(config.h)
AC_CONFIG_MACRO_DIR([m4])

AM_SANITY_CHECK

AC_C_CONST
AC_SEARCH_LIBS([strerror],[cposix])
AC_PROG_CC
AC_PROG_CPP
AC_PROG_INSTALL
AC_PROG_LN_S
AC_PROG_MAKE_SET
AC_PATH_PROG(PKG_CONFIG, pkg-config, no)
AC_PATH_PROG(GIO_QUERYMODULES, gio-querymodules, no)
AM_PROG_CC_C_O
AC_PROG_SED

# Initialize libtool
LT_PREREQ([2.2])
LT_INIT([disable-static])
LIBTOOL_EXPORT_OPTIONS='-export-symbols-regex "^[[^_]].*"'
AC_SUBST(LIBTOOL_EXPORT_OPTIONS)

AC_ARG_ENABLE([documentation],
              [AS_HELP_STRING([--enable-documentation],
                              [build documentation])],,
              [enable_documentation=yes])
if test x$enable_documentation = xyes; then
   AC_PATH_PROG([XSLTPROC], [xsltproc])
   if test x$XSLTPROC = x; then
      AC_MSG_ERROR([xsltproc is required to build documentation])
   fi
fi
AM_CONDITIONAL([BUILD_DOCUMENTATION], [test x$enable_documentation = xyes])

m4_ifdef([AM_SILENT_RULES],[AM_SILENT_RULES([yes])])

AC_TYPE_PID_T
AC_TYPE_SIGNAL
AC_TYPE_SIZE_T
AC_TYPE_UID_T

GLIB_GSETTINGS

AH_VERBATIM([_GNU_SOURCE],
[/* Enable GNU extensions on systems that have them.  */
#ifndef _GNU_SOURCE
#  define _GNU_SOURCE
#endif])
	
saved_CFLAGS=$CFLAGS
AC_CHECK_MEMBERS([struct stat.st_mtimensec, struct stat.st_mtim.tv_nsec, struct stat.st_atimensec, struct stat.st_atim.tv_nsec, struct stat.st_ctimensec, struct stat.st_ctim.tv_nsec])
CFLAGS=$saved_CFLAGS

GTK_DOC_CHECK
DISTCHECK_CONFIGURE_FLAGS="--enable-gtk-doc"
AC_SUBST(DISTCHECK_CONFIGURE_FLAGS)

PKG_CHECK_MODULES([GLIB], [glib-2.0 >= 2.51.0 gobject-2.0 gmodule-no-export-2.0 gio-unix-2.0 gio-2.0])

AC_ARG_WITH([dbus_service_dir],
	    [AS_HELP_STRING([--with-dbus-service-dir=PATH],[choose directory for dbus service files, [default=PREFIX/share/dbus-1/services]])],
	    [with_dbus_service_dir="$withval"], [with_dbus_service_dir=$datadir/dbus-1/services])
DBUS_SERVICE_DIR=$with_dbus_service_dir
AC_SUBST(DBUS_SERVICE_DIR)

giomodulesdir=$libdir/gio/modules
AC_SUBST(giomodulesdir)

dnl *********************
dnl *** Check for gcr ***
dnl *********************

AC_ARG_ENABLE([gcr], [AS_HELP_STRING([--disable-gcr],[build without gcr])])
msg_gcr=no

if test "x$enable_gcr" != "xno"; then
  PKG_CHECK_MODULES([GCR], [gcr-base-3], [msg_gcr=yes])
  AC_DEFINE([HAVE_GCR], 1, [Define to 1 if gcr is available])
fi

dnl ****************************
dnl *** Checks for gettext   ***
dnl ****************************
AM_GNU_GETTEXT_VERSION([0.19.4])
AM_GNU_GETTEXT([external])

GETTEXT_PACKAGE=gvfs
AC_SUBST([GETTEXT_PACKAGE])
AC_DEFINE_UNQUOTED([GETTEXT_PACKAGE],["$GETTEXT_PACKAGE"],[The gettext domain name])


dnl ****************************
dnl *** Checks for ssh stuff ***
dnl ****************************

AC_PATH_PROG(SSH_PROGRAM, ssh, "ssh")

dnl ****************************
dnl *** Checks for pty stuff ***
dnl ****************************

AC_CHECK_HEADERS(sys/un.h stropts.h termios.h util.h utmp.h sys/uio.h sys/param.h)

# Check for PTY handling functions.
AC_CHECK_FUNCS(getpt posix_openpt grantpt unlockpt ptsname ptsname_r)

# Pull in the right libraries for various functions which might not be
# bundled into an exploded libc.
AC_CHECK_FUNC(socketpair,[have_socketpair=1],AC_CHECK_LIB(socket,socketpair,[have_socketpair=1; LIBS="$LIBS -lsocket"]))
AC_CHECK_FUNC(openpty,[have_openpty=1],AC_CHECK_LIB(util,openpty,[have_openpty=1; LIBS="$LIBS -lutil"]))
if test x$have_socketpair = x1 ; then
	AC_DEFINE([HAVE_SOCKETPAIR],1,[Define if you have the socketpair function.])
fi
if test x$have_openpty = x1 ; then
	AC_DEFINE([HAVE_OPENPTY],1,[Define if you have the openpty function.])
fi

AC_SEARCH_LIBS([login_tty], [util], [AC_DEFINE([HAVE_LOGIN_TTY],[],[Whether login_tty is available])])

dnl ***************************************************
dnl *** Check if we should build with admin backend ***
dnl ***************************************************
AC_ARG_ENABLE([admin], [AS_HELP_STRING([--disable-admin],[build without admin backend])])
msg_admin=no

if test "x$enable_admin" != "xno"; then
  PKG_CHECK_MODULES([ADMIN], [polkit-gobject-1 libcap], [msg_admin=yes])

  if test "x$msg_admin" = "xyes"; then
    AC_DEFINE([HAVE_ADMIN], 1, [Define to 1 if admin backend is going to be built])
  fi
fi

AC_SUBST(ADMIN_CFLAGS)
AC_SUBST(ADMIN_LIBS)
AM_CONDITIONAL([HAVE_ADMIN], [test "$msg_admin" = "yes"])

dnl **************************************************
dnl *** Check if we should build with http backend ***
dnl **************************************************
AC_ARG_ENABLE([http], [AS_HELP_STRING([--disable-http],[build without http/dav backend])])
msg_http=no

if test "x$enable_http" != "xno"; then
  PKG_CHECK_EXISTS([libsoup-2.4 >= 2.42.0], [msg_http=yes])

  if test "x$msg_http" = "xyes"; then
    PKG_CHECK_MODULES([HTTP], [libsoup-2.4 libxml-2.0])
    AC_DEFINE([HAVE_HTTP], 1, [Define to 1 if http/dav is going to be built])
  fi
fi

AC_SUBST(HTTP_CFLAGS)
AC_SUBST(HTTP_LIBS)
AM_CONDITIONAL([HAVE_HTTP], [test "$msg_http" = "yes"])

dnl **************************************************
dnl *** Check if we should build with DNS-SD backend ***
dnl **************************************************
AM_CONDITIONAL(HAVE_AVAHI, false)
			  
AC_ARG_ENABLE([avahi], [AS_HELP_STRING([--disable-avahi],[build without avahi support])])
msg_avahi=no
if test "x$enable_avahi" != "xno"; then
	PKG_CHECK_MODULES([AVAHI], [avahi-glib >= 0.6 avahi-client >= 0.6],
			[AM_CONDITIONAL([HAVE_AVAHI], [true])
			AC_DEFINE([HAVE_AVAHI], [], [Set if we can use avahi])]
                        [msg_avahi=yes],
			[AM_CONDITIONAL([HAVE_AVAHI], [false])])
	AC_SUBST(AVAHI_CFLAGS)
	AC_SUBST(AVAHI_LIBS)
fi

dnl ************************
dnl *** Check for libXML ***
dnl ************************
PKG_CHECK_MODULES([LIBXML], [libxml-2.0],
	[AM_CONDITIONAL([HAVE_LIBXML], [true])
	AC_DEFINE([HAVE_LIBXML], [], [Set if we have libxml])]
	[msg_libxml=yes],
        [AM_CONDITIONAL([HAVE_LIBXML], [false])])

dnl **********************
dnl *** Check for gudev ***
dnl **********************
AC_ARG_ENABLE([gudev], [AS_HELP_STRING([--disable-gudev],[build without gudev support])])
msg_gudev=no
GUDEV_REQUIRED=147

if test "x$enable_gudev" != "xno"; then
  PKG_CHECK_EXISTS([gudev-1.0 >= $GUDEV_REQUIRED], [msg_gudev=yes])

  if test "x$msg_gudev" = "xyes"; then
    PKG_CHECK_MODULES([GUDEV], [gudev-1.0 >= $GUDEV_REQUIRED])
    AC_DEFINE([HAVE_GUDEV], 1, [Define to 1 if gudev is available])
  fi
fi
AM_CONDITIONAL([USE_GUDEV], [test "$msg_gudev" = "yes"])

dnl **********************
dnl *** Check for FUSE ***
dnl **********************
AC_ARG_ENABLE([fuse], [AS_HELP_STRING([--disable-fuse],[build without FUSE support])])
msg_fuse=no

if test "x$enable_fuse" != "xno"; then
  PKG_CHECK_EXISTS([fuse], [msg_fuse=yes])

  if test "x$msg_fuse" = "xyes"; then
    PKG_CHECK_MODULES([FUSE], [fuse >= 2.8.0])
    AC_DEFINE([HAVE_FUSE], 1, [Define to 1 if FUSE is available])
  fi
fi

AM_CONDITIONAL([USE_FUSE], [test "$msg_fuse" = "yes"])

dnl ************************************
dnl *** Check for gnome-disk-utility ***
dnl ************************************

AC_ARG_ENABLE([gdu], [AS_HELP_STRING([--disable-gdu],[build without GDU volume monitor])])
msg_gdu=no
GDU_REQUIRED=3.0.2

if test "x$enable_gdu" != "xno"; then
  PKG_CHECK_EXISTS([gdu >= $GDU_REQUIRED], [msg_gdu=yes])

  if test "x$msg_gdu" = "xyes"; then
    PKG_CHECK_MODULES([GDU],[gdu >= $GDU_REQUIRED])
    AC_DEFINE([HAVE_GDU], 1, [Define to 1 if gnome-disk-utility is available])
  fi
fi

AM_CONDITIONAL(USE_GDU, [test "$msg_gdu" = "yes"])

dnl *************************
dnl *** Check for udisks2 ***
dnl *************************

AC_ARG_ENABLE([udisks2], [AS_HELP_STRING([--disable-udisks2],[build without libudisks2])])
msg_udisks2=no
UDISKS2_REQUIRED=1.97

if test "x$enable_udisks2" != "xno" -a "x$msg_gudev" = "xyes"; then
  PKG_CHECK_EXISTS([udisks2 >= $UDISKS2_REQUIRED], [msg_udisks2=yes])

  if test "x$msg_udisks2" = "xyes"; then
    PKG_CHECK_MODULES([UDISKS2],[udisks2 >= $UDISKS2_REQUIRED])
    AC_DEFINE([HAVE_UDISKS2], 1, [Define to 1 if libudisks2 is available])
  fi
fi

AM_CONDITIONAL(USE_UDISKS2, [test "$msg_udisks2" = "yes"])

dnl ************************************
dnl *** Check for systemd user units ***
dnl ************************************

msg_systemd_user_units=no

AC_ARG_WITH([systemduserunitdir],
            [AS_HELP_STRING([--with-systemduserunitdir=PATH], [Set directory for systemd user units, or 'no' to disable])],
            [systemd_userdir="$withval"],
            [systemd_userdir="${prefix}/lib/systemd/user"])
AC_SUBST([systemd_userdir])

if test "x$systemd_userdir" != "xno"; then
  PKG_CHECK_EXISTS([systemd >= 206], [msg_systemd_user_units=yes])
fi

AM_CONDITIONAL([USE_SYSTEMD_USER_UNITS], [test "$msg_systemd_user_units" = "yes"])

dnl **********************************
dnl *** Check for libsystemd-login ***
dnl **********************************

AC_ARG_ENABLE([libsystemd_login], [AS_HELP_STRING([--disable-libsystemd-login],[build without libsystemd-login])])
msg_libsystemd_login=no
LIBSYSTEMD_LOGIN_REQUIRED=44

if test "x$enable_libsystemd_login" != "xno"; then
  PKG_CHECK_EXISTS([libsystemd], [msg_libsystemd_login=yes],
                   [PKG_CHECK_EXISTS([libsystemd-login >= $LIBSYSTEMD_LOGIN_REQUIRED], [msg_libsystemd_login=yes])])

  if test "x$msg_libsystemd_login" = "xyes"; then
    PKG_CHECK_MODULES([LIBSYSTEMD_LOGIN],[libsystemd],,
                      [PKG_CHECK_MODULES([LIBSYSTEMD_LOGIN],[libsystemd-login >= $LIBSYSTEMD_LOGIN_REQUIRED])])
    AC_DEFINE([HAVE_LIBSYSTEMD_LOGIN], 1, [Define to 1 if libsystemd_login is available])
  fi
fi

AM_CONDITIONAL(USE_LIBSYSTEMD_LOGIN, [test "$msg_libsystemd_login" = "yes"])

dnl **************************************************
dnl *** Check if we should build with CDDA backend ***
dnl **************************************************
AC_ARG_ENABLE([cdda], [AS_HELP_STRING([--disable-cdda],[build without CDDA backend])])
msg_cdda=no

if test "x$enable_cdda" != "xno" -a "x$msg_gudev" = "xyes"; then
  PKG_CHECK_EXISTS([libcdio_paranoia >= 0.78.2], [msg_cdda=yes])

  if test "x$msg_cdda" = "xyes"; then
    PKG_CHECK_MODULES([CDDA], [libcdio_paranoia])
    AC_DEFINE([HAVE_CDDA], 1, [Define to 1 if CDDA is going to be built])

    # test for actual headers location
    CPPFLAGS_save="$CPPFLAGS"
    CPPFLAGS="$CPPFLAGS $CDDA_CFLAGS"
    AC_CHECK_HEADER(cdio/paranoia/paranoia.h, [
       AC_DEFINE([HAVE_PARANOIA_NEW_INCLUDES], 1, [Define to 1 if cdio/paranoia/paranoia.h is present]) ])
    CPPFLAGS="$CPPFLAGS_save"
  fi
fi

AC_SUBST(CDDA_LIBS)
AC_SUBST(CDDA_CFLAGS)

AM_CONDITIONAL([USE_CDDA], [test "$msg_cdda" = "yes"])

dnl *************************************************
dnl *** Check if we should build with AFC backend ***
dnl *************************************************
AC_ARG_ENABLE([afc], [AS_HELP_STRING([--disable-afc],[build without AFC backend])])
msg_afc=no

if test "x$enable_afc" != "xno" ; then
  PKG_CHECK_EXISTS([libimobiledevice-1.0 >= 1.2 libplist >= 0.15], [msg_afc=yes])

  if test "x$msg_afc" = "xyes"; then
    PKG_CHECK_MODULES([AFC], [libimobiledevice-1.0 libplist])
    AC_DEFINE([HAVE_AFC], 1, [Define to 1 if AFC is going to be built])
  fi
fi

AC_SUBST(AFC_LIBS)
AC_SUBST(AFC_CFLAGS)

AM_CONDITIONAL([USE_AFC], [test "$msg_afc" = "yes"])

dnl ********************************************************
dnl *** Check if we should build with GOA volume monitor ***
dnl ********************************************************
AC_ARG_ENABLE([goa], [AS_HELP_STRING([--disable-goa],[build without GOA volume monitor])])
msg_goa=no

if test "x$enable_goa" != "xno" ; then
  PKG_CHECK_EXISTS([goa-1.0 >= 3.7.1], [msg_goa=yes])

  if test "x$msg_goa" = "xyes"; then
    PKG_CHECK_MODULES([GOA], [goa-1.0])
    AC_DEFINE([HAVE_GOA], 1, [Define to 1 if GOA is going to be built])
  fi
fi

AC_SUBST(GOA_LIBS)
AC_SUBST(GOA_CFLAGS)

AM_CONDITIONAL([USE_GOA], [test "$msg_goa" = "yes"])

dnl ****************************************************
dnl *** Check if we should build with Google backend ***
dnl ****************************************************
AC_ARG_ENABLE([google], [AS_HELP_STRING([--disable-google],[build without Google backend])])
msg_google=no

if test "x$enable_google" != "xno" ; then
  PKG_CHECK_EXISTS([goa-1.0 >= 3.17.1 libgdata >= 0.17.3], [msg_google=yes])

  if test "x$msg_google" = "xyes"; then
    PKG_CHECK_MODULES([GOOGLE], [goa-1.0 libgdata])
    AC_DEFINE([HAVE_GOOGLE], 1, [Define to 1 if Google is going to be built])

    PKG_CHECK_MODULES(LIBGDATA_0_17_7, libgdata >= 0.17.7,
        AC_DEFINE(HAVE_LIBGDATA_0_17_7, 1, [Define to 1 if libgdata 0.17.7 is available]),
        []
    )

    PKG_CHECK_MODULES(LIBGDATA_0_17_9, libgdata >= 0.17.9,
        AC_DEFINE(HAVE_LIBGDATA_0_17_9, 1, [Define to 1 if libgdata 0.17.9 is available]),
        []
    )
  fi
fi

AC_SUBST(GOOGLE_LIBS)
AC_SUBST(GOOGLE_CFLAGS)

AM_CONDITIONAL(USE_GOOGLE, [test "$msg_google" = "yes"])

dnl *************************
dnl *** Check for gphoto2 ***
dnl *************************
AC_ARG_ENABLE([gphoto2], [AS_HELP_STRING([--disable-gphoto2],[build without gphoto2 support])])
msg_gphoto2=no

if test "x$enable_gphoto2" != "xno" -a "x$msg_gudev" = "xyes"; then
  PKG_CHECK_MODULES([GPHOTO2], [libgphoto2 >= 2.4.0], [msg_gphoto2=yes])

  # Need OS tweaks in volume monitor backend
  case "$host" in
    *-linux* | *-freebsd* | *-kfreebsd*-gnu)
      use_gphoto2=yes
      ;;
    *)
      use_gphoto2=no
      ;;
  esac

  if test "x$enable_gphoto2" = "xyes" -a "x$msg_gphoto2" != "xyes"; then
    AC_MSG_ERROR([gphoto2 support requested but not available])
  fi

  if test "x$msg_gphoto2" = "xyes"; then
    if test "x$use_gphoto2" = "xyes"; then
      AC_DEFINE([HAVE_GPHOTO2], 1, [Define to 1 if gphoto2 is available])
      PKG_CHECK_MODULES([GPHOTO25], [libgphoto2 >= 2.5.0],
         [AC_DEFINE([HAVE_GPHOTO25], 1, [Define to 1 if libgphoto2 2.5 is available])],
	 []
      )
    else
      if test "x$enable_gphoto2" = "xyes"; then
        AC_MSG_ERROR([Cannot build with gphoto2 support. Need OS tweaks in volume monitor.])
      else
        AC_MSG_WARN([Not building with gphoto2 support. Need OS tweaks in volume monitor.])
      fi
      msg_gphoto2=no
    fi
  fi
fi

AM_CONDITIONAL([USE_GPHOTO2], [test "$msg_gphoto2" = "yes"])

dnl *******************************
dnl *** Check for GNOME Keyring ***
dnl *******************************
AC_ARG_ENABLE([keyring], [AS_HELP_STRING([--disable-keyring],[build without GNOME Keyring support])])
msg_keyring=no

if test "x$enable_keyring" != "xno"; then
  PKG_CHECK_EXISTS([libsecret-unstable], [msg_keyring=yes])

  if test "x$msg_keyring" = "xyes"; then
    PKG_CHECK_MODULES([KEYRING], [libsecret-unstable])
    AC_DEFINE([HAVE_KEYRING], 1, [Define to 1 if GNOME Keyring is available])
  fi
fi

AM_CONDITIONAL([USE_KEYRING], [test "$msg_keyring" = "yes"])

dnl ***********************************************
dnl *** Check if we should build with libbluray ***
dnl ***********************************************
AC_ARG_ENABLE([bluray], [AS_HELP_STRING([--disable-bluray],[build without bluray metadata support])])
msg_bluray=no

if test "x$enable_bluray" != "xno"; then
  PKG_CHECK_EXISTS([libbluray], [msg_bluray=yes])

  if test "x$msg_bluray" = "xyes"; then
    PKG_CHECK_MODULES([BLURAY], [libbluray])
    AC_DEFINE([HAVE_BLURAY], 1, [Define to 1 if bluray metadata is going to be built])
  fi
fi

AC_SUBST(BLURAY_CFLAGS)
AC_SUBST(BLURAY_LIBS)
AM_CONDITIONAL([HAVE_BLURAY], [test "$msg_bluray" = "yes"])

dnl ************************************************
dnl *** Check if we should build with libusb-1.0 ***
dnl ************************************************
AC_ARG_ENABLE([libusb], [AS_HELP_STRING([--disable-libusb],[build without libusb support])])
msg_libusb=no

if test "x$enable_libusb" != "xno"; then
  PKG_CHECK_EXISTS([libusb-1.0 >= 1.0.21], [msg_libusb=yes])

  if test "x$msg_libusb" = "xyes"; then
    PKG_CHECK_MODULES([LIBUSB], [libusb-1.0 >= 1.0.21])
    AC_DEFINE([HAVE_LIBUSB], 1, [Define to 1 if libusb is going to be built])
  fi
fi

AC_SUBST(LIBUSB_CFLAGS)
AC_SUBST(LIBUSB_LIBS)
AM_CONDITIONAL([HAVE_LIBUSB], [test "$msg_libusb" = "yes"])

dnl *************************
dnl *** Check for libmtp ***
dnl *************************
AC_ARG_ENABLE([libmtp], [AS_HELP_STRING([--disable-libmtp],[build without libmtp support])])
msg_libmtp=no

if test "x$enable_libmtp" != "xno" -a "x$msg_gudev" = "xyes"; then
  PKG_CHECK_EXISTS([libmtp], [msg_libmtp=yes])

  if test "x$msg_libmtp" = "xyes"; then
    PKG_CHECK_MODULES([LIBMTP], [libmtp >= 1.1.0])
    AC_DEFINE([HAVE_LIBMTP], 1, [Define to 1 if libmtp is available])

    PKG_CHECK_MODULES([LIBMTP_1_1_5], [libmtp >= 1.1.5],
        [AC_DEFINE([HAVE_LIBMTP_1_1_5], 1, [Define to 1 if libmtp 1.1.5 is available])],
        []
    )

    PKG_CHECK_MODULES([LIBMTP_1_1_6], [libmtp >= 1.1.6],
        [AC_DEFINE([HAVE_LIBMTP_1_1_6], 1, [Define to 1 if libmtp 1.1.6 is available])],
        []
    )

    PKG_CHECK_MODULES([LIBMTP_1_1_9], [libmtp >= 1.1.9],
        [AC_DEFINE([HAVE_LIBMTP_1_1_9], 1, [Define to 1 if libmtp 1.1.9 is available])],
        []
    )

    PKG_CHECK_MODULES(LIBMTP_1_1_12, libmtp >= 1.1.12,
        AC_DEFINE(HAVE_LIBMTP_1_1_12, 1, [Define to 1 if libmtp 1.1.12 is available]),
        []
    )
  fi
fi

AC_SUBST(LIBMTP_CFLAGS)
AC_SUBST(LIBMTP_LIBS)
AM_CONDITIONAL(USE_LIBMTP, [test "$msg_libmtp" = "yes"])

AM_CONDITIONAL(USE_GPHOTO2UTILS,
               [test "$msg_libmtp" = "yes" -o \( "$msg_gphoto2" = "yes" -a "$msg_gudev" = "yes" \)])

dnl ***********************
dnl *** Check for samba ***
dnl ***********************
AC_ARG_ENABLE([samba], [AS_HELP_STRING([--disable-samba],[build without samba support])])
msg_samba=no

if test "x$enable_samba" != "xno"; then
  PKG_CHECK_EXISTS([smbclient], [msg_samba=yes])

  if test "x$msg_samba" = "xyes"; then
    PKG_CHECK_MODULES([SAMBA], [smbclient])
    AC_DEFINE([HAVE_SAMBA], 1, [Define to 1 if you have the samba libraries])
  fi
fi

AM_CONDITIONAL([HAVE_SAMBA], [test "$msg_samba" = "yes"])
AC_SUBST(SAMBA_CFLAGS)
AC_SUBST(SAMBA_LIBS)

dnl ****************************
dnl *** Check for libarchive ***
dnl ****************************
AC_ARG_ENABLE([archive], [AS_HELP_STRING([--disable-archive],[build without archive support])])
msg_archive=no

if test "x$enable_archive" != "xno"; then
  PKG_CHECK_EXISTS([libarchive], [msg_archive=yes])

  if test "x$msg_archive" = "xyes"; then
    PKG_CHECK_MODULES([ARCHIVE], [libarchive])
    AC_DEFINE([HAVE_ARCHIVE], 1, [Define to 1 if you have the archive libraries])
  fi
fi

AM_CONDITIONAL([HAVE_ARCHIVE], [test "$msg_archive" = "yes"])
AC_SUBST(ARCHIVE_CFLAGS)
AC_SUBST(ARCHIVE_LIBS)

dnl ***************************
dnl *** Check for libgcrypt ***
dnl ***************************
GCRYPT_VERSION=1.2.2
GCRYPT_LIBVER=1

AM_PATH_LIBGCRYPT($GCRYPT_LIBVER:$GCRYPT_VERSION, have_gcrypt="yes",
	      have_gcrypt="no")

if test "x$have_gcrypt" != "xno"; then
  AC_DEFINE(HAVE_GCRYPT, 1, [Define to 1 if libgcrypt is available])
fi
AC_SUBST([LIBGCRYPT_CFLAGS])
AC_SUBST([LIBGCRYPT_LIBS])

dnl *******************
dnl *** AFP backend ***
dnl *******************
AC_ARG_ENABLE([afp], [AS_HELP_STRING([--disable-afp], [build without AFP support])])
msg_afp="no"
if test "x$enable_afp" != "xno"; then

  if test "x$have_gcrypt" = "xyes"; then
    msg_afp="yes"
  else
    msg_afp="partial (crypt support missing! Only anonymous logins)"
  fi
fi
AM_CONDITIONAL([USE_AFP], [test "x$enable_afp" != "xno"])

dnl *******************
dnl *** NFS backend ***
dnl *******************
AC_ARG_ENABLE([nfs], [AS_HELP_STRING([--disable-nfs], [build without NFS support])])
msg_nfs="no"

if test "x$enable_nfs" != "xno"; then
  PKG_CHECK_EXISTS([libnfs >= 1.9.8], [msg_nfs=yes])

  if test "x$msg_nfs" = "xyes"; then
    PKG_CHECK_MODULES([NFS],[libnfs >= 1.9.8])
    AC_DEFINE([HAVE_NFS], 1, [Define to 1 if nfs is going to be built])
  fi
fi

AC_SUBST(NFS_CFLAGS)
AC_SUBST(NFS_LIBS)
AM_CONDITIONAL(HAVE_NFS, [test "$msg_nfs" = "yes"])

dnl ***************************************
dnl *** Check for nl_langinfo constants ***
dnl ***************************************
AC_CHECK_DECL(_NL_ADDRESS_LANG_TERM,
              AC_DEFINE(HAVE_NL_ADDRESS_LANG_TERM, 1, Define to 1 if _NL_ADDRESS_LANG_TERM is declared),,
              [#include <langinfo.h>])
AC_CHECK_DECL(_NL_ADDRESS_COUNTRY_AB3,
              AC_DEFINE(HAVE_NL_ADDRESS_COUNTRY_AB3, 1, Define to 1 if _NL_ADDRESS_COUNTRY_AB3 is declared),,
              [#include <langinfo.h>])

dnl ==========================================================================
	
dnl Globally define_GNU_SOURCE and therefore enable the GNU extensions
AC_DEFINE([_GNU_SOURCE], 1, [Enable GNU Extensions])

dnl ==========================================================================
	
AC_DEFINE([_FILE_OFFSET_BITS], 64, [Enable LFS])
		       
dnl ==========================================================================

AC_PATH_PROG([GLIB_GENMARSHAL], [glib-genmarshal])

dnl ==========================================================================
dnl Look for various fs info getters

AC_HEADER_MAJOR
AC_CHECK_HEADERS([sys/statfs.h sys/statvfs.h sys/vfs.h sys/mount.h sys/param.h])
AC_CHECK_FUNCS(statvfs statfs)
AC_CHECK_MEMBERS([struct statfs.f_fstypename, struct statfs.f_bavail],,, [#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef HAVE_SYS_STATFS_H
#include <sys/statfs.h>
#endif
#ifdef HAVE_SYS_PARAM_H
#include <sys/param.h>
#endif
#ifdef HAVE_SYS_MOUNT_H
#include <sys/mount.h>
#endif])
# struct statvfs.f_basetype is available on Solaris but not for Linux. 
AC_CHECK_MEMBERS([struct statvfs.f_basetype],,, [#include <sys/statvfs.h>])

dnl
dnl if statfs() takes 2 arguments (Posix) or 4 (Solaris)
dnl
if test "$ac_cv_func_statfs" = yes ; then
  AC_MSG_CHECKING([number of arguments to statfs()])
  AC_TRY_COMPILE([#include <unistd.h>
  #ifdef HAVE_SYS_PARAM_H
  #include <sys/param.h>
  #endif
  #ifdef HAVE_SYS_VFS_H
  #include <sys/vfs.h>
  #endif
  #ifdef HAVE_SYS_MOUNT_H
  #include <sys/mount.h>
  #endif
  #ifdef HAVE_SYS_STATFS_H
  #include <sys/statfs.h>
  #endif], [struct statfs st;
  statfs("/", &st);],[
    AC_MSG_RESULT([2])
    AC_DEFINE(STATFS_ARGS, 2, [Number of arguments to statfs()])],[
    AC_TRY_COMPILE([#include <unistd.h>
  #ifdef HAVE_SYS_PARAM_H
  #include <sys/param.h>
  #endif
  #ifdef HAVE_SYS_VFS_H
  #include <sys/vfs.h>
  #endif
  #ifdef HAVE_SYS_MOUNT_H
  #include <sys/mount.h>
  #endif
  #ifdef HAVE_SYS_STATFS_H
  #include <sys/statfs.h>
  #endif], [struct statfs st;
  statfs("/", &st, sizeof (st), 0);],[
      AC_MSG_RESULT([4])
      AC_DEFINE(STATFS_ARGS, 4, [Number of arguments to statfs()])],[
      AC_MSG_RESULT(unknown)
      AC_MSG_ERROR([unable to determine number of arguments to statfs()])])])
fi

dnl ==========================================================================
dnl Turn on the additional warnings last, so -Werror doesn't affect other tests.

AC_ARG_ENABLE([more-warnings],
[AS_HELP_STRING([--enable-more-warnings],[Maximum compiler warnings])],
[set_more_warnings="$enableval"],[
if test -f $srcdir/.git; then
	set_more_warnings=yes
else
	set_more_warnings=no
fi
])
AC_MSG_CHECKING([for more warnings])
if test "$GCC" = "yes" -a "$set_more_warnings" != "no"; then
	AC_MSG_RESULT(yes)
	CFLAGS="\
	-Wall \
	-Wchar-subscripts -Wmissing-declarations -Wmissing-prototypes \
	-Wnested-externs -Wpointer-arith \
	-Wcast-align -Wsign-compare \
	$CFLAGS"

	for option in -Wno-strict-aliasing -Wno-sign-compare -Wtype-limits; do
		SAVE_CFLAGS="$CFLAGS"
		CFLAGS="$CFLAGS $option"
		AC_MSG_CHECKING([whether gcc understands $option])
		AC_TRY_COMPILE([], [],
			has_option=yes,
			has_option=no,)
		if test $has_option = no; then
			CFLAGS="$SAVE_CFLAGS"
		fi
		AC_MSG_RESULT($has_option)
		unset has_option
		unset SAVE_CFLAGS
	done
	unset option
else
	AC_MSG_RESULT(no)
fi

msg_installed_tests="no"
AC_ARG_ENABLE([installed-tests],
              [AS_HELP_STRING([--enable-installed-tests],
                              [Enable installation of some test cases])],
              [case ${enableval} in
                yes) ENABLE_INSTALLED_TESTS="1"  ;;
                no)  ENABLE_INSTALLED_TESTS="" ;;
                *) AC_MSG_ERROR([bad value ${enableval} for --enable-installed-tests]) ;;
               esac])
AM_CONDITIONAL([ENABLE_INSTALLED_TESTS], [test "$ENABLE_INSTALLED_TESTS" = "1"])
AC_ARG_ENABLE([always-build-tests],
              [AS_HELP_STRING([--enable-always-build-tests],
                              [Enable always building tests during 'make all'])],
              [case ${enableval} in
                yes) ENABLE_ALWAYS_BUILD_TESTS="1"  ;;
                no)  ENABLE_ALWAYS_BUILD_TESTS="" ;;
                *) AC_MSG_ERROR([bad value ${enableval} for --enable-always-build-tests]) ;;
               esac])
AM_CONDITIONAL([ENABLE_ALWAYS_BUILD_TESTS], [test "$ENABLE_ALWAYS_BUILD_TESTS" = "1"])
if test "$ENABLE_INSTALLED_TESTS" = "1"; then
  AC_SUBST(installed_test_metadir, [${datadir}/installed-tests/]AC_PACKAGE_NAME)
  AC_SUBST(installed_testdir, [${libexecdir}/installed-tests/]AC_PACKAGE_NAME)
  msg_installed_tests="yes"
fi

AC_CONFIG_FILES([
Makefile
common/Makefile
client/Makefile
metadata/Makefile
daemon/trashlib/Makefile
daemon/Makefile
monitor/Makefile
monitor/proxy/Makefile
monitor/gdu/Makefile
monitor/udisks2/Makefile
monitor/gphoto2/Makefile
monitor/afc/Makefile
monitor/mtp/Makefile
monitor/goa/Makefile
programs/Makefile
man/Makefile
test/Makefile
po/Makefile.in
])
AC_OUTPUT

echo 
echo "gvfs configuration summary:"
echo "
        gio module directory : $giomodulesdir

	Blu-ray metadata support:     $msg_bluray
	Google support:               $msg_google
        HTTP/WebDAV support:          $msg_http
	Samba support:	              $msg_samba
	FUSE support:                 $msg_fuse
        CDDA support:                 $msg_cdda
        Gphoto2 support:              $msg_gphoto2
        MTP support:                  $msg_libmtp
	Polkit support:               $msg_admin
        USB support:                  $msg_libusb
	archive support:              $msg_archive
	AFC support:                  $msg_afc
        AFP support:                  $msg_afp
        NFS support:                  $msg_nfs
        DNS-SD support:               $msg_avahi
	Build GDU volume monitor:     $msg_gdu
	Build udisks2 volume monitor: $msg_udisks2
	Build GOA volume monitor:     $msg_goa
        Use systemd user units:       $msg_systemd_user_units
        Use libsystemd-login:         $msg_libsystemd_login
        Use GCR:                      $msg_gcr
	GNOME Keyring support:        $msg_keyring
	Installed tests:              $msg_installed_tests
"

# The gudev gphoto monitor needs a recent libgphoto; point to the required patch if the version is too old
if test "x$msg_gudev" = "xyes"; then
  PKG_CHECK_EXISTS([libgphoto2 >= 2.4.7],, [msg_gphoto_patch=yes])
  if test "x$msg_gphoto_patch" = "xyes"; then
    AC_MSG_WARN([You are using a libgphoto2 version earlier than 2.4.7. To work with gudev, you must apply the patch in http://sourceforge.net/tracker/?func=detail&aid=2801117&group_id=8874&atid=308874])
  fi
fi

