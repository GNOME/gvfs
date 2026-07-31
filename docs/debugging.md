# Debugging

This document describes techniques useful for debugging GVfs. It covers
collecting debug logs, testing custom builds, and manually spawning individual
backends. For a description of environment variables see the
[Environment variables](documentation.md#environment-variables) section in the
developer documentation.

## Getting debug logs

1. Terminate all GVfs daemons and the client application (e.g. Nautilus) first:

   ```
   pkill gvfs; pkill nautilus
   ```

   Be careful, this step will terminate also your pending file operations.

2. Start the main daemon with debug output enabled:

   ```
   GVFS_DEBUG=1 $(find /usr/lib* -name gvfsd 2>/dev/null) --replace 2>&1 | tee gvfsd.log
   ```

   You can combine additional
   [environment variables](documentation.md#environment-variables)
   for more verbose output from specific backends, e.g.:

   ```
   GVFS_SMB_DEBUG=10 GVFS_DEBUG=1 $(find /usr/lib* -name gvfsd 2>/dev/null) --replace 2>&1 | tee gvfsd.log
   ```

3. Reproduce your problem.

4. Terminate GVfs daemons after that:

   ```
   pkill gvfs
   ```

   GVfs will operate as usual after this step.

5. Attach `gvfsd.log` to a bug report.

## Testing custom builds

It is a bit cumbersome given the fact that GVfs consists of multiple D-Bus
services, shared libraries, and GIO modules. Several options exist with
different pros and cons:

* To test only changes to a specific backend, run the necessary bits manually.
  You can run just the modified backend as described in the following section,
  or run the main daemon manually from the prefix as described in the
  previous section. When you run the main daemon, it automatically uses backends
  from the prefix also. Volume monitors and client code are still used from the
  system installation, which is usually fine if unmodified. For client code
  changes, point the
  [`GIO_MODULE_DIR`](https://docs.gtk.org/gio/overview.html#running-gio-applications)
  environment variable to the directory where the modified `libgvfsdbus.so`
  library is installed, or use `LD_LIBRARY_PATH` to point to the build
  directory containing the modified library.

  This approach may not work if you test a different version
  than what is installed on the system.

* Modify the package for your distribution.

  This requires packaging knowledge.

* Install GVfs in system paths.

  This conflicts with system packages.

* Set up a
  [GNOME JHBuild session](https://gnome.pages.gitlab.gnome.org/jhbuild/jhbuild-and-gnome.html)
  with custom D-Bus services dir, or use a custom D-Bus config file to see
  services from the prefix. This requires system knowledge and a significant amount of time to build the
  whole JHBuild moduleset.

## Spawning backends manually

**This is just for debugging purposes, use `gio mount` if you need mounting
from the command line!**

Before spawning a backend manually, ensure that potential credentials are
already saved in the keyring, otherwise the backend fails without prompting.
You can do this e.g. using Nautilus with the "Remember password until you
logout" or "Remember forever" options. You can also use
[environment variables](documentation.md#environment-variables) for additional
debug output.

Default mount spec options:

```
[type=TYPE] [user=USER] [host=HOST] [port=PORT] [prefix=PREFIX]
```

Some backends have custom options:

```
gvfsd-afp [volume=VOLUME] ...
gvfsd-dav [ssl=true|false] ...
gvfsd-http uri=[URI]
gvfsd-smb [domain=DOMAIN] [user=USER] server=[SERVER] [port=PORT] share=[SHARE]
```

Examples:

```
gvfsd-dav ssl=true user=foo host=my.owndrive.com prefix=/remote.php/webdav
gvfsd-google user=foo host=gmail.com
gvfsd-http uri=https://google.com
gvfsd-mtp host=[usb:002,009]
gvfsd-sftp host=localhost user=foo
```
