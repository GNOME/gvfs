AC_INIT(gio/gfile.h)

AM_INIT_AUTOMAKE(gvfs, 0.0.1)
AM_CONFIG_HEADER(config.h)
	
AM_SANITY_CHECK
AM_MAINTAINER_MODE

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
AC_CHECK_MEMBERS([struct stat.st_blksize, struct stat.st_blocks])
CFLAGS=$saved_CFLAGS

AC_CHECK_HEADERS([pwd.h])

AC_CHECK_FUNCS(getc_unlocked readlink symlink chown lchown fchmod fchown link)

dnl Volume monitor stuff			     
AC_CHECK_FUNCS(setmntent endmntent hasmntopt getmntinfo)
AC_CHECK_HEADERS(mntent.h sys/mnttab.h sys/vfstab.h sys/cdio.h sys/mount.h sys/mntctl.h sys/vfs.h sys/vmount.h sys/sysctl.h fstab.h fnmatch.h util.h sys/sysmacros.h)
			     
AC_MSG_CHECKING([for Win32])
case "$host" in
  *-*-mingw*)
    glib_native_win32=yes
    ;;
  *)
    glib_native_win32=no
    ;;
esac

AC_MSG_RESULT([$glib_native_win32])
AM_CONDITIONAL(OS_WIN32, [test "$glib_native_win32" = "yes"])
AM_CONDITIONAL(OS_UNIX, [test "$glib_native_win32" != "yes"])

GTK_DOC_CHECK
DISTCHECK_CONFIGURE_FLAGS="--enable-gtk-doc"
AC_SUBST(DISTCHECK_CONFIGURE_FLAGS)

PKG_CHECK_MODULES(GLIB, glib-2.0 gthread-2.0 gobject-2.0 gmodule-no-export-2.0)
AC_SUBST(GLIB_CFLAGS)
AC_SUBST(GLIB_LIBS)

if test "$glib_native_win32" != yes; then
 PKG_CHECK_MODULES(DBUS, dbus-1)
 AC_SUBST(DBUS_CFLAGS)
 AC_SUBST(DBUS_LIBS)
 AC_ARG_WITH(dbus_service_dir, [  --with-dbus-service-dir=PATH choose directory for dbus service files, [default=PREFIX/share/dbus-1/services]], with_dbus_service_dir="$withval", with_dbus_service_dir=$datadir/dbus-1/services)
 DBUS_SERVICE_DIR=$with_dbus_service_dir
 AC_SUBST(DBUS_SERVICE_DIR)
fi
	
GETTEXT_PACKAGE=gvfs
AC_DEFINE_UNQUOTED(GETTEXT_PACKAGE,"$GETTEXT_PACKAGE", [The gettext domain name])
AC_SUBST(GETTEXT_PACKAGE)

AM_GLIB_GNU_GETTEXT

dnl ****************************
dnl *** Check for libselinux ***
dnl ****************************
AC_ARG_ENABLE(selinux, [  --disable-selinux       build without selinux support])
msg_selinux=no
SELINUX_LIBS=
if test "x$enable_selinux" != "xno"; then

 AC_CHECK_LIB(selinux, is_selinux_enabled,
   [AC_CHECK_HEADERS(selinux/selinux.h,
     [AC_SEARCH_LIBS(lgetfilecon_raw, selinux, 
       [AC_DEFINE(HAVE_SELINUX, 1, [Define to 1 if libselinux is available])
        SELINUX_LIBS="-lselinux"
        msg_selinux=yes])
     ])
   ])
fi
AC_SUBST(SELINUX_LIBS)

dnl ***********************
dnl *** Check for xattr ***
dnl ***********************
AC_ARG_ENABLE(xattr, [  --disable-xattr           build without xattr support])
msg_xattr=no
XATTR_LIBS=
if test "x$enable_xattr" != "xno"; then

dnl either glibc or libattr can provide xattr support

dnl for both of them, we check for getxattr being in
dnl the library and a valid xattr header.

dnl try glibc
 AC_CHECK_LIB(c, getxattr,
   [AC_CHECK_HEADERS(sys/xattr.h,
     [AC_DEFINE(HAVE_XATTR, 1, [Define to 1 if xattr is available])
      msg_xattr=yes])
   ])

  if test "x$msg_xattr" != "xyes"; then
dnl   failure. try libattr
   AC_CHECK_LIB(attr, getxattr,
      [AC_CHECK_HEADERS(attr/xattr.h,
       [AC_DEFINE(HAVE_XATTR, 1, [Define to 1 if xattr is available])
        XATTR_LIBS="-lattr"
        msg_xattr=yes])
      ])
  fi
fi
AC_SUBST(XATTR_LIBS)

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
				[AC_DEFINE(HAVE_SAMBA_FLAGS,, [Defined if flags availible in SMBCCTXT])],,
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
	-Werror \
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
gio/Makefile
gio/xdgmime/Makefile
common/Makefile
client/Makefile
daemon/Makefile
programs/Makefile
po/Makefile.in
gio.pc
])

echo 
echo "gvfs configuration summary:"
echo "
	Win32 build:          $glib_native_win32
	SELinux support:      $msg_selinux
	xattr support:        $msg_xattr
	Samba support:	      $msg_samba
	FUSE support:         $msg_fuse
"
