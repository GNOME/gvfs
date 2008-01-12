AC_INIT(client/gdaemonvfs.h)

AM_INIT_AUTOMAKE(gvfs, 0.1.2)
AM_CONFIG_HEADER(config.h)
	
AM_SANITY_CHECK

AC_C_CONST
AC_ISC_POSIX
AC_PROG_CC
AC_PROG_CPP
AC_PROG_INSTALL
AC_PROG_LN_S
AC_PROG_MAKE_SET
AM_DISABLE_STATIC
AM_PROG_LIBTOOL
AC_PATH_PROG(PKG_CONFIG, pkg-config, no)
AM_PROG_CC_C_O

AC_TYPE_PID_T
AC_TYPE_SIGNAL
AC_TYPE_SIZE_T
AC_TYPE_UID_T

AH_VERBATIM([_GNU_SOURCE],
[/* Enable GNU extensions on systems that have them.  */
#ifndef _GNU_SOURCE
#  define _GNU_SOURCE
#endif])
	
saved_CFLAGS=$CFLAGS
AC_CHECK_MEMBERS([struct stat.st_mtimensec, struct stat.st_mtim.tv_nsec, struct stat.st_atimensec, struct stat.st_atim.tv_nsec, struct stat.st_ctimensec, struct stat.st_ctim.tv_nsec])
CFLAGS=$saved_CFLAGS

LIBTOOL_EXPORT_OPTIONS='-export-symbols-regex "^[[^_]].*"'
AC_SUBST(LIBTOOL_EXPORT_OPTIONS)
			
GTK_DOC_CHECK
DISTCHECK_CONFIGURE_FLAGS="--enable-gtk-doc"
AC_SUBST(DISTCHECK_CONFIGURE_FLAGS)

PKG_CHECK_MODULES(GLIB, glib-2.0 >= 2.15.1 gthread-2.0 gobject-2.0 gmodule-no-export-2.0 gio-unix-2.0 gio-2.0)
AC_SUBST(GLIB_CFLAGS)
AC_SUBST(GLIB_LIBS)

PKG_CHECK_MODULES(DBUS, dbus-1)
AC_SUBST(DBUS_CFLAGS)
AC_SUBST(DBUS_LIBS)

AC_ARG_WITH(dbus_service_dir, [  --with-dbus-service-dir=PATH choose directory for dbus service files, [default=PREFIX/share/dbus-1/services]], with_dbus_service_dir="$withval", with_dbus_service_dir=$datadir/dbus-1/services)
DBUS_SERVICE_DIR=$with_dbus_service_dir
AC_SUBST(DBUS_SERVICE_DIR)

PKG_CHECK_MODULES(HTTP, libsoup-2.2 >= 2.2.104)
AC_SUBST(HTTP_CFLAGS)
AC_SUBST(HTTP_LIBS)

dnl ****************************
dnl *** Checks for intltool  ***
dnl ****************************

AC_PROG_INTLTOOL([0.31.0])
GETTEXT_PACKAGE=gvfs

ALL_LINGUAS="ar es eu it nb ru"

AC_SUBST([GETTEXT_PACKAGE])
AC_DEFINE_UNQUOTED([GETTEXT_PACKAGE],["$GETTEXT_PACKAGE"],[The gettext domain name])
AM_GLIB_GNU_GETTEXT

dnl ****************************
dnl *** Checks for gettext   ***
dnl ****************************

dnl *** disabled by now, using intltool ***
dnl *** we'll revert when glib-gettexttize will be able
dnl *** to manage [enconding: UTF-8] in POTIFILES.in

dnl GETTEXT_PACKAGE=gvfs
dnl AC_DEFINE_UNQUOTED(GETTEXT_PACKAGE,"$GETTEXT_PACKAGE", [The gettext domain name])
dnl AC_SUBST(GETTEXT_PACKAGE)

dnl ALL_LINGUAS=""

dnl AM_GLIB_GNU_GETTEXT

dnl LIBS="$LIBS $INTLLIBS"

dnl AM_GLIB_DEFINE_LOCALEDIR(GVFS_LOCALEDIR)

dnl ****************************
dnl *** Checks for ssh stuff ***
dnl ****************************

AC_PATH_PROG(SSH_PROGRAM, ssh, "ssh")

dnl ****************************
dnl *** Checks for pty stuff ***
dnl ****************************

AC_CHECK_HEADERS(sys/un.h stropts.h termios.h utmp.h)

# Check for PTY handling functions.
AC_CHECK_FUNCS(getpt posix_openpt grantpt unlockpt ptsname ptsname_r)

# Pull in the right libraries for various functions which might not be
# bundled into an exploded libc.
AC_CHECK_FUNC(socketpair,[have_socketpair=1],AC_CHECK_LIB(socket,socketpair,[have_socketpair=1; LIBS="$LIBS -lsocket"]))
if test x$have_socketpair = x1 ; then
	AC_DEFINE(HAVE_SOCKETPAIR,1,[Define if you have the socketpair function.])
fi

AC_SEARCH_LIBS(login_tty, util, [AC_DEFINE([HAVE_LOGIN_TTY],[],[Whether login_tty is available])])

dnl **************************************************
dnl *** Check if we should build with CDDA backend ***
dnl **************************************************
AC_ARG_ENABLE(cdda, [  --disable-cdda           build without CDDA backend])
msg_cdda=no
CDDA_LIBS=
CDDA_CFLAGS=

if test "x$enable_cdda" != "xno"; then
  PKG_CHECK_EXISTS(libcdio_paranoia >= 0.78.2, msg_cdda=yes)

  if test "x$msg_cdda" == "xyes"; then
    PKG_CHECK_MODULES(CDDA, libcdio_paranoia)
    AC_DEFINE(HAVE_CDDA, 1, [Define to 1 if CDDA is going to be built])
  fi
fi

AC_SUBST(CDDA_LIBS)
AC_SUBST(CDDA_CFLAGS)

AM_CONDITIONAL(USE_CDDA, [test "$msg_cdda" = "yes"])

dnl **********************
dnl *** Check for FUSE ***
dnl **********************
AC_ARG_ENABLE(fuse, [  --disable-fuse           build without FUSE support])
msg_fuse=no
FUSE_LIBS=
FUSE_CFLAGS=

if test "x$enable_fuse" != "xno"; then
  PKG_CHECK_EXISTS(fuse, msg_fuse=yes)

  if test "x$msg_fuse" == "xyes"; then
    PKG_CHECK_MODULES(FUSE, fuse)
    AC_DEFINE(HAVE_FUSE, 1, [Define to 1 if FUSE is available])
  fi
fi

AC_SUBST(FUSE_LIBS)
AC_SUBST(FUSE_CFLAGS)

AM_CONDITIONAL(USE_FUSE, [test "$msg_fuse" = "yes"])

dnl **********************
dnl *** Check for HAL ***
dnl **********************
AC_ARG_ENABLE(hal, [  --disable-hal           build without HAL support])
msg_hal=no
have_hal_fast_init=no
HAL_LIBS=
HAL_CFLAGS=

if test "x$enable_hal" != "xno"; then
  PKG_CHECK_EXISTS(hal >= 0.5.9, msg_hal=yes)

  if test "x$msg_hal" == "xyes"; then
    PKG_CHECK_MODULES(HAL, hal >= 0.5.9)
    AC_DEFINE(HAVE_HAL, 1, [Define to 1 if HAL is available])
    AC_CHECK_LIB(hal, libhal_get_all_devices_with_properties, have_hal_fast_init=yes)
    if test "x$have_hal_fast_init" == "xyes"; then
      AC_DEFINE(HAVE_HAL_FAST_INIT, 1, [Define to 1 if libhal_get_all_devices_with_properties is available])
    fi
  fi
fi

AC_SUBST(HAL_LIBS)
AC_SUBST(HAL_CFLAGS)

AM_CONDITIONAL(USE_HAL, [test "$msg_hal" = "yes"])

dnl ==========================================================================
dnl Samba 3.0

AC_ARG_ENABLE(samba, [  --disable-samba       build without samba support])
msg_samba="no"
if test "x$enable_samba" != "xno"; then
  AC_ARG_WITH(samba-includes, [  --with-samba-includes=PREFIX     Location of samba includes.],
	      with_samba_includes="$withval", with_samba_includes="/usr/include")
  have_samba_includes="no"
  if test "x${with_samba_includes}" != "xno"; then
	CPPFLAGS_save="$CPPFLAGS"

	CPPFLAGS="$CPPFLAGS -I$with_samba_includes"
	AC_CHECK_HEADER(libsmbclient.h, [ samba_includes="yes" ])
	CPPFLAGS="$CPPFLAGS_save"
	
	if test "x{samba_includes}" != "xno" -a "x${samba_includes}" != "x"; then
		have_samba_includes="yes"
		if test "${with_samba_includes}" != "/usr/include" ; then
			SAMBA_CFLAGS="-I$with_samba_includes"
		else
			SAMBA_CFLAGS=""
		fi
		
		CPPFLAGS="$CPPFLAGS -I$with_samba_includes"
		AC_CHECK_MEMBER(SMBCCTX.flags,
				[AC_DEFINE(HAVE_SAMBA_FLAGS,, [Defined if flags available in SMBCCTXT])],,
				[#include <libsmbclient.h>])
		AC_CHECK_MEMBER(SMBCCTX.close,
				[AC_DEFINE(HAVE_SAMBA_OLD_CLOSE, , [Defined if old close is available in SMBCCTXT])],,
				[#include <libsmbclient.h>])
		CPPFLAGS="$CPPFLAGS_save"
	else
		SAMBA_CFLAGS=""
	fi
  fi
  AC_ARG_WITH(samba-libs, [  --with-samba-libs=PREFIX     	Location of Samba libs.],
	      with_samba_libs="$withval", with_samba_libs="/usr/lib")
  if test "x${with_samba_libs}" != "xno" -a "x${have_samba_includes}" != "xno"; then
	LDFLAGS_save="$LDFLAGS"
	
	LDFLAGS="$LDFLAGS -L$with_samba_libs"
	AC_CHECK_LIB(smbclient, smbc_option_get,samba_libs="yes", samba_libs="no")
	LDFLAGS="$LDFLAGS_save"
	if test "x${samba_libs}" != "xno"; then
		AC_DEFINE(HAVE_SAMBA,, [Define to 1 if you have the samba 3.0 libraries])
		msg_samba="yes"
                if test x$with_samba_libs != x/usr/lib; then
                        SAMBA_LIBS="-L$with_samba_libs -lsmbclient"
                else
                	SAMBA_LIBS="-lsmbclient"
                fi
	else
		AC_CHECK_LIB(smbclient, smbc_new_context,samba_old_libs="yes", samba_old_libs="no")
		if test "x${samba_old_libs}" != "xno"; then
		   msg_samba="Too old, need at least samba 3.0.22"
		fi
		SAMBA_CFLAGS=""
		SAMBA_LIBS=""
	fi
  fi
  AC_MSG_CHECKING(for Samba 3.0 libraries)
  AC_MSG_RESULT($msg_samba)
fi
AM_CONDITIONAL(HAVE_SAMBA, test "$msg_samba" = "yes")
AC_SUBST(SAMBA_CFLAGS)
AC_SUBST(SAMBA_LIBS)
	
dnl ==========================================================================
	
dnl Globally define_GNU_SOURCE and therefore enable the GNU extensions
AC_DEFINE(_GNU_SOURCE, 1, [Enable GNU Extensions])

dnl ==========================================================================
	
AC_DEFINE(_FILE_OFFSET_BITS, 64, [Enable LFS])
		       
dnl ==========================================================================

AC_PATH_PROG(GLIB_GENMARSHAL, glib-genmarshal)

dnl ==========================================================================
dnl Turn on the additional warnings last, so -Werror doesn't affect other tests.

AC_ARG_ENABLE(more-warnings,
[  --enable-more-warnings  Maximum compiler warnings],
set_more_warnings="$enableval",[
if test -f $srcdir/CVSVERSION; then
	is_cvs_version=true
	set_more_warnings=yes
else
	set_more_warnings=no
fi
])
AC_MSG_CHECKING(for more warnings, including -Werror)
if test "$GCC" = "yes" -a "$set_more_warnings" != "no"; then
	AC_MSG_RESULT(yes)
	CFLAGS="\
	-Wall \
	-Wchar-subscripts -Wmissing-declarations -Wmissing-prototypes \
	-Wnested-externs -Wpointer-arith \
	-Wcast-align -Wsign-compare \
	$CFLAGS"

	for option in -Wno-strict-aliasing -Wno-sign-compare; do
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
			       
AC_OUTPUT([
Makefile
common/Makefile
client/Makefile
daemon/Makefile
hal/Makefile
programs/Makefile
test/Makefile
po/Makefile.in
])

echo 
echo "gvfs configuration summary:"
echo "
	Samba support:	              $msg_samba
	FUSE support:                 $msg_fuse
        CDDA support:                 $msg_cdda
	Use HAL for volume monitor:   $msg_hal (with fast init path: $have_hal_fast_init)
"
