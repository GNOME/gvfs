# Developer Documentation

This document describes some aspects of GVfs architecture and explains reasons
why things are done the way they are. It is intended for developers as a
starting point, to help easily find the area to look at. Most information can
be read from the sources and I'm also not going to describe all details here.

## What it is and what it does

GVfs is a set of libraries and daemons extending the GIO API. In fact, it's an
extension point, default GIO implementation of non-local services. It's designed
to provide information suitable for usage in the GUI, that means in a polite and
filtered way.

It provides everything needed for accessing files on remote servers (networking)
or devices attached to the physical computer. It also takes care of local
storage management.

## Requirements, running and usage

The only requirement is a running user D-Bus session, typically started with the
desktop session on login. When using GVfs in an isolated shell (console, SSH
login), make sure to start the D-Bus session (the `dbus-launch` command, e.g.
`export $(dbus-launch)`). Active GVfs mounts are only available to the specific
session, making it work nicely in a concurrent environment like multi-seat
systems or terminal servers.

Everything is autostarted on-demand on first access through GIO. No need to
start any daemon explicitly. Some mounts may require credentials, this is done
using standard `GMountOperation` object.

All daemons are listening to session bus changes and will exit once the bus is
torn down. Should this not be true, please file a bug.

## Environment variables

See also
[Running GIO applications](https://docs.gtk.org/gio/overview.html#running-gio-applications)
in the GIO Reference Manual for common GIO environment variables.

The following environment variables can be used for controlling certain aspects
of some services:

* `DBUS_SESSION_BUS_ADDRESS` - if unset, gvfs will refuse to load to prevent
  spawning private bus
* `GVFS_DEBUG` - if set (no matter the value), daemons will be printing more
  information on console. This is useful for debugging and reveals parts of
  internal protocol. You can also toggle the debugging for each backend using
  `SIGUSR2`.
* `GVFS_DEBUG_FUSE` - if set (no matter the value), fuse daemon will be printing
  more information on console.
* `GVFS_MOUNTABLE_EXTENSION` - filename extension of gvfs backend setup files.
  Default value is ".mount"
* `GVFS_MOUNTABLE_DIR` - a path to look for the backend setup files. Default
  value is determined by configure script
* `GVFS_MONITOR_DIR` - a path to look for volume monitors setup files
  (`.monitor` suffix). Default value is determined by configure script
* `GVFS_REMOTE_VOLUME_MONITOR_IGNORE` - if set (no matter the value), gvfs
  volume monitors will not be activated
* `GVFS_DISABLE_FUSE` - if set (no matter the value), FUSE daemon will not be
  autostarted
* `GVFS_AFC_DEBUG` - if set (no matter the value), the AFC backend will print
  more debugging messages
* `GVFS_GPHOTO2_DEBUG` - controls amount of debug information printed out by the
  gphoto2 backend. Allowed values are "all", "data", "debug", "verbose".
* `GVFS_HTTP_DEBUG` - controls amount of debug information printed out by the
  http backend. Allowed values are "all", "body", "header".
* `GVFS_SMB_DEBUG` - sets libsmbclient debug level. Allowed values are integers
  from 0 to 10.
* `GVFS_MTP_DEBUG` - sets libmtp debug level. Allowed values are "all", "data",
  "usb", "ptp".
* `GVFS_NFS_DEBUG` - sets libnfs verbosity. Allowed values are integers from 0
  to ?.
* `GVFS_WSDD_DEBUG` - if set (no matter the value), the `gvfsd-wsdd` daemon will
  print more debug messages. If set to "all", the underlying wsdd daemon will be
  spawned with "-vvv".

# Components, the architecture

The project consists of three more or less independent parts. Those parts
comprise of one or more daemons. Everything is glued together using D-Bus, it's
best to illustrate implementation details on the
[gvfs D-Bus interface](https://gitlab.gnome.org/GNOME/gvfs/-/raw/master/common/org.gtk.vfs.xml).

Since GVfs is implemented as a GIO extension point, its libraries are loaded in
every process. This may be expensive and GVfs may need some time to initialize,
etc. That's one of the reasons a client-server architecture was chosen; client
libraries (loaded in applications, GIO clients) are basically only proxy modules
communicating with the gvfs server side through D-Bus.

## Volume monitors

Volume monitors provide a set of `GDrive`/`GVolume`/`GMount` objects
representing physical device or service hierarchy. So-called native volume
monitors provide access to locally available devices, i.e. those appearing in
`/dev` and mountable by standard POSIX ways (`mount`, `umount`). Currently,
GVfs provides only one native monitor (`udisks2`).

Every volume monitor possesses its own process, communicating with the client
side through `GProxyVolumeMonitor` infrastructure over D-Bus (see the note about
client-server architecture above). See the `/monitor/proxy/dbus-interfaces.xml`
file for D-Bus interface description. Other than usual volume monitor-related
signals and methods, the interface also wraps `GMountOperation` implemented by a
combination of method calls and signals for bi-directional communication.
Signals have been chosen to work around possible deadlocks.

Every volume monitor ships a `.monitor` file containing key-value information
and that is used for registration. Registration is done on startup (more
specifically on `libgioremote-volume-monitor.so` load) by going through the
`.monitor` files in `/usr/share/gvfs/remote-volume-monitors` directory. The
`.monitor` file provides a D-Bus service name used for daemon autostart; a D-Bus
service file should be installed along this.

Other than native volume monitors, GVfs ships several others that are related to
some kind of service or device; in both cases, access is provided usually only
through GVfs backends. The goal is to present `GVolume` objects mountable by
users on demand. This applies e.g. to digital cameras or media players connected
to the computer.

On the client side, `GVolumeMonitor` will mix results from all volume monitors
together, hiding implementation details. Check the output of `gvfs-mount -l` for
illustration.

### udisks2 volume monitor

The `udisks2` volume monitor is the preferred native volume monitor for the
moment. It's the most up-to-date and fully featured among others, and the
majority of fixes go there.

David Zeuthen wrote a nice document describing what is exposed to the UI and how
to control it:
[/monitor/udisks2/what-is-shown.txt](https://gitlab.gnome.org/GNOME/gvfs/raw/master/monitor/udisks2/what-is-shown.txt)

## Core VFS Daemon

The master process, `gvfsd`, is basically a manager and message router.
Initialized on the client side again as an extension point by loading the
`libgvfsdbus.so` library, the master daemon is autospawned on first use. The
client library (running in a GIO application) provides a number of interfaces
that are proxied to the master daemon or its backends.

All the hard I/O work is done by backends - separate processes for one or more
mounts, and the master gvfs daemon is just controlling spawning, mounting,
unmounting, and informs clients which backend to use for the requested file.
Having backends out of process brings robustness (crashed backend is treated as
unmounted) and solves some technical difficulties such as threading, locking,
forking, usually specific to an underlying library. Separate processes can also
link to non-GPL compatible code without violating licenses of the other parts.

### Mount tracker, GDaemonVolumeMonitor

Among other things, the master daemon is doing on startup, it is registering the
`org.gtk.vfs.MountTracker` interface. It keeps track of active mounts, handles
mounting, unmounting, and provides runtime mount information. On the client
side, `GDaemonVolumeMonitor` for gvfs mounts is available by registering itself
at the gvfs extension point, tracking changes on the mount tracker. This is also
used by the FUSE daemon to detect new mounts.

### Spawning and unmounting backends

Mounting is typically initiated by the `g_file_mount_enclosing_volume()` call,
which goes through `GDaemonFile`, calling the
`org.gtk.vfs.MountTracker.MountLocation()` D-Bus method. The mount tracker
(master gvfs daemon) then checks whether the location is not mounted and either
spawns a new process for the particular mount, or, in case of "Mountable",
it uses D-Bus service autostart feature for the associated D-Bus name.

The difference is that Mountable backends are able to handle any request for
specified schemes, sharing one process for any target hostname, contrary to
standard backends having separate processes for each hostname. This is indicated
by `.mount` files (the `/usr/share/gvfs/mounts/` path by default or
`$GVFS_MOUNTABLE_DIR` and `$GVFS_MOUNTABLE_EXTENSION` for override) carrying
backend-specific information. The principle of setup files is nearly identical
to `.monitor` files described in the Volume monitors section.

Backends are typically spawned by the master daemon but can also be started
manually, e.g. when debugging. In the case of being spawned by the master
daemon, a `--spawner` command line argument with a value of the master daemon
D-Bus connection name and D-Bus path are appended. These are then used by the
backend to announce itself (using the `org.gtk.vfs.Spawner` D-Bus interface),
awaiting more information. The master daemon calls
`org.gtk.vfs.Mountable.Mount()` method back to the backend with more arguments
and a mount spec.

The mount spec is a crucial piece of information required for the mount
operation to succeed. When started manually, the mount spec must be provided as
command line arguments (in the form of `key1=value key2=value ...`) since the
master daemon is not aware of this initiative and can't provide any information.
Once the mount spec is provided, the backend can start the mount process,
creating `GVfsJobMount`, going through the backend methods. Once that job is
finished, the result is sent back to the master daemon by the
`org.gtk.vfs.MountTracker.RegisterMount()` call and only then the mount is
treated as mounted. A *Mounted* signal is emitted by the mount tracker and
caught by subscribed `GDaemonVolumeMonitor` instances on client sides.

Unmounting is easier; the mount tracker watches the D-Bus name of all registered
backends, and when it disappears, i.e. exits or crashes, the mount tracker emits
an *Unmounted* signal. Historically, this used to be the only way of unmount
signaling, letting the backend exit gracefully first. However, certain issues
with unexpected forking in backends were discovered (triggered by an external
library used), preventing the detection from working properly, so
`org.gtk.vfs.MountTracker.UnregisterMount()` D-Bus method has been put in use.
It's also the preferred way of unmounting, called at the `GVfsJobUnmount`
operation finish.

### Automounting

Some backends don't need credentials or specific mount options; this is the case
of general mounts like `computer://`, `network://`, `trash://`, etc. For this,
the automounting feature has been introduced; backends need to indicate that in
their `.mount` setup files. Technically, such backends are not running by
default and are only spawned on first use. Automounted mounts need to be of type
*Mountable* as they can be mounted only once. Automounted mounts are usually
hidden from `GDaemonVolumeMonitor`, and neither the mount tracker is propagating
their presence.

### GFile methods

Most of `GFile` methods are implemented in `GDaemonFile` class, on the client
side. All methods have a common start, doing some validation (mounted location
check), creating a private connection to the backend, and returning `GDBusProxy`
instance for immediate use. This proxy is then used for calling respective
`org.gtk.vfs.Mount` methods that roughly correspond to `GFile` methods. The
first argument of every call represents the path inside the mount, i.e. stripped
from path elements that should be part of the mount info - mount prefix (think
of `smb://server/share/path`).

On the daemon side, incoming `org.gtk.vfs.Mount` method calls are directly
transformed to new jobs. The job, descendant of the `GVfsJobDBus` class, takes
reference to the incoming D-Bus message (`GDBusMethodInvocation`) and keeps it
until the job is finished. Once finished, the result from the job is sent as a
method call reply, together with any (optional) data. If the job has failed, an
error is returned and is properly handled by the client side, propagated back to
the `GDaemonFile` method caller, usually displayed in UI. For this reason, it's
crucial to set method call timeouts in a sane way, i.e. for long I/O operations
the timeout should be set to infinite.

### Jobs

Jobs are essentially wrappers around backend methods, carrying all arguments
from the D-Bus method call. Backend methods are those doing actual I/O; for that
reason, we need to have some kind of control over them. Two types of methods
exist in the backend. Methods prefixed with `try_` are supposed to be
asynchronous and non-blocking as much as possible. The return value of boolean
indicates whether the request has been handled or not. If not (or the `try_`
method is not implemented), the `do_` prefixed method is called (or non-prefixed
version respectively). This call is more heavyweight and runs in a thread pool.
That way, the backend could handle multiple requests simultaneously. Daemon
thread pool is on the daemon (process, see below) basis and the maximum number
of threads is controlled by the `MAX_JOB_THREADS` define during compilation
(see `meson.build`).

When talking about backends, let's see how objects are organized. A daemon
instance represents the process itself. Daemon can handle one or more mounts
(this is actually determined by the master gvfs daemon, the backend daemon
infrastructure is generally always able to handle multiple mounts) and maintains
a list of job sources. The default job source is created for every single mount
(`GVfsBackend` instance); additional job sources are created for each opened
file (both for reading and writing, `GVfsChannel`). Job source (`GVfsJobSource`)
is an interface implemented by `GVfsBackend` and `GVfsChannel`.

Every job source maintains a list of actual jobs. Jobs are submitted via the
`g_vfs_daemon_queue_job()` call. Typically, every new job created by the
`org.gtk.vfs.Mount` method call handler queues itself once all arguments are
collected. More jobs are queued during file transfer, from the `GVfsChannel`
side. This will then go through the `try_` and `do_` backend methods which
effectively start the operation.

Backends are required to return the status of the operation by calling either
`g_vfs_job_succeed()` or `g_vfs_job_failed()` in case of error. That will
indicate the `GVfsJob` instance to finish. This is also the way the asynchronous
`try_` methods should complete. Some job classes may provide more methods that
are required to be called in order to pass some data to the job (e.g. file infos
for `GVfsJobEnumerate`).

If anything happens to the master gvfs daemon (crashes or is being replaced by
another instance) and its D-Bus name owner changes, every running backend daemon
will go through the list of active job sources and re-register all mounted
backends with the new master daemon. This is a nice way of crash recovery,
bringing more robustness.

### Cancellation

Cancellation is well integrated by GIO nature and GVfs needs to provide a
channel to cancel running operations. This is done by calling the
`org.gtk.vfs.Daemon.Cancel()` method taking the serial number as the only
argument. This is available on both session bus and private connections.

The job system in backends works by taking reference to the originating D-Bus
message for later reply. Every D-Bus message contains a unique serial number
and by going through the list of running operations we are able to find the
right one to cancel.

### Private peer-to-peer connection

For particular `GDaemonFile` operations, a private peer-to-peer D-Bus connection
is created between the client library and the backend directly. This allows us
to overcome potential weak points of overloaded session bus daemon, relieve from
data marshalling, and be somewhat independent of the session bus. This is quite
independent implementation and can be easily switched back to the use of a
session bus. Similarly, we provide private sockets for raw data transfers, to
maximize throughput.

Initiated from most `GDaemonFile` methods, the way of setting up a private
connection is rather complex. On the client side, we first need to know the
backend D-Bus ID. By calling the backend `org.gtk.vfs.Daemon.GetConnection()`
method, we get a D-Bus address. Using this address, we create a new D-Bus
connection that is used for further `org.gtk.vfs.Mount` calls. Every client
maintains a cache of thread-local private connections and tries to reuse them
whenever possible.

On the backend side, on `GetConnection()` request, a socket with a unique name
is created and GDBus server is started on it. The server then awaits incoming
client and registers several interfaces on the connection once the client comes.
The server is then killed as no more clients are expected, leaving the active
connection open. When the connection is closed, the backend cancels all active
jobs.

### Skeletons, registered paths

Related to private connections, we need to make available several interfaces on
them. Every time a new private connection is opened, several
`GDBusInterfaceSkeleton` objects are registered on it.

Historically, GDBus was not able to export a single `GDBusInterfaceSkeleton`
over multiple connections, and a workaround solution was created. By calling
`g_vfs_daemon_register_path()`, interested parties register themselves handing
over an object path and a callback that is used for creating new skeletons for
the new connection. This way we export `org.gtk.vfs.Mount` and
`org.gtk.vfs.Monitor` interfaces on the private connection.

Similar approach is taken on the client side for `GDaemonFile` methods;
interested parties register their ability to provide certain services by calling
`_g_dbus_register_vfs_filter()` and their callback is then used to create
interface skeletons on every new private connection (by calling
`_g_dbus_connect_vfs_filters()`). This way we export `org.gtk.vfs.Enumerator`
and `org.gtk.vfs.MonitorClient` interfaces, typically used as a form of callback
on events from daemon to clients.

### Enumerator

Enumeration (initiated by `g_file_enumerate_children()`), just like several
other `GFile` methods, needs to be stateful to be able to continuously provide
data back to the client. For this, GVfs provides `GDaemonFileEnumerator` that is
used on the client side. A new VFS filter (see above) is registered to provide
the `org.gtk.vfs.Enumerator` interface on the connection. That's wrapped by
`GDaemonFileEnumerator` and is used to receive data from the backend. Every
`GDaemonFileEnumerator` instance creates a unique ID that is used as an object
path for creating interface skeleton on the client side and for `GDBusProxy`
construction on the backend side. It's worth noting that `GDaemonFileEnumerator`
can handle both sync and async ways of enumeration.

`GVfsJobEnumerate` provides several methods used by backends to send data back
to the client. Collected infos are sent using the
`org.gtk.vfs.Enumerator.GotInfo()` call as an array, and when enumeration is
finished, `org.gtk.vfs.Enumerator.Done()` is called to indicate that to the
client side. Additional backend info is added in `GVfsJobEnumerate`, and
metadata info is added on the client side if applicable.

Note that any unsuccessful method call will lead to a warning message printed to
the console; sometimes messages can be seen when the client closes the
enumerator before it's fully finished.

### Monitoring

Structure-wise, monitoring is very similar to enumeration. Client
`g_file_monitor*()` calls will result in `GVfsJobCreateMonitor` on the backend
side, which lets the backend create a `GVfsMonitor` instance and hand it over to
the job. This instance wraps around the `org.gtk.vfs.Monitor` interface, waiting
for clients to subscribe and unsubscribe particular files and directories
they're interested in monitoring. This is done on the client side; when the
`GVfsJobCreateMonitor` call is finished, the client creates a
`GDaemonFileMonitor` instance and subscribes itself to the backend-side monitor.
Every subscription takes a reference to the `GVfsMonitor` instance, every
unsubscribe releases it. That way, the monitor is automatically destroyed when
no one is interested in monitoring anymore. Subscriptions on a particular
private D-Bus connection are automatically unsubscribed when the connection is
closed.

When the backend emits a change event, `GVfsMonitor` calls the
`org.gtk.vfs.MonitorClient.Changed()` method, and the client side
`GDaemonFileMonitor` emits an event to the original client.

### GMountOperation proxy

For `GDaemonFile` methods that are taking a `GMountOperation` argument, we need
to provide a way to proxy that to the daemon side. This is done by the
`g_mount_operation_dbus_wrap()` call which wraps the foreign instance on the
specified D-Bus connection. Internally, that means creating an
`org.gtk.vfs.MountOperation` interface skeleton on the client side and assigning
its D-Bus specific data to a newly created `GMountSource` instance. These are
then used by several `org.gtk.vfs.Mount` methods to reconstruct a `GMountSource`
instance on the daemon side and finally get a `GMountOperation` instance through
`g_mount_source_get_operation()`. This call creates a new local
`GMountOperation` instance catching its signals. When asked for something, the
machinery makes a call back to the client and sets the `GMountOperation`
credentials if succeeded.

### File copy

File copy or move in the form of copy-and-delete is supported in GVfs; however,
the open-read-write-close fallback is done automatically by GIO when
`G_IO_ERROR_NOT_SUPPORTED` is returned. To overcome potential backend
limitations and to optimize the data flow, two new backend operations have been
introduced along standard `copy()` and `move()`: `push()` and `pull()`. These
methods work with one endpoint being a local file; `pull()` having the source
file local, `push()` transfers data from backend to a local file. The standard
`copy()` and `move()` are supposed to work within the same mount only.

Some backends are not able to provide universal open, read, write, close methods
either due to the service nature or due to a limitation of the underlying
library. The `push()`/`pull()` methods take care of file transfer completely,
i.e. file open, data transfer, and file close, with optional progress callback
reporting. These methods are only used if implemented by the backend and if one
of the endpoints is a local file.

In order to provide progress reports during the transfer, the
`org.gtk.vfs.Progress` D-Bus interface is set up on the client side of the
connection, awaiting incoming `Progress()` method calls that will then call the
progress callback specified in the original `GDaemonFile` call.

### Reading and writing data

Since the open-read-write-close operations are used as a copy fallback by GIO,
it's recommended to have these methods implemented to maintain a degree of
universality. This is impossible for some backends though (e.g. `mtp`) where the
full file has to be always transferred.

Unlike file copy, which handles data blocks internally, the
`GDaemonFile.read()`, `GDaemonFile.create()`, and similar methods are returning
`GFileInputStream` and `GFileOutputStream` respectively, meaning the data are
being transferred outside of GVfs. In order to efficiently transfer data blocks
between two processes, a Unix socket is created, and using the GDBus fd-passing
feature, the client receives an fd for reading/writing. `GFileInputStream` and
`GFileOutputStream` are implemented by `GDaemonFileInputStream` and
`GDaemonFileOutputStream` respectively, providing all necessary infrastructure
for data handling. This allows us to overcome unnecessary round-trips through
the D-Bus daemon, achieving almost native speed.

The daemon open job creates a `GVfsReadChannel` or `GVfsWriteChannel`
respectively, taking a pointer returned by the backend as an internal file
handle. The `GVfsChannel` base class takes care of creating a Unix socket for
data transfer, buffering, simple protocol, and queue handling, etc. Its
descendants implement more functionality like readahead and are processing the
channel protocol, calling particular jobs (`GVfsJobRead`, `GVfsJobWrite`,
`GVfsJobSeekRead`, `GVfsJobSeekWrite`, `GVfsJobQueryInfoRead`,
`GVfsJobQueryInfoWrite`, `GVfsJobCloseRead`, `GVfsJobCloseWrite`) to do the
actual work.

### Mount spec

Mount spec is a tiny class that helps to identify and match mounts. It carries
key-value pairs, e.g. information about the URI scheme used, backend type,
hostname, port, username, domain, etc.

Specifically, during the mount operation, two instances of mount spec are used.
The source one carries requested information and is handed over to the backend's
`GVfsJobMount` operation. It is the backend's responsibility to process this
mount spec, create a new one based on the source one, and send it back to the
mount tracker for registration. The backend can filter out some information that
are not strictly required and only sets those values required for unique mount
identification.

Mount spec matching is done by comparing all its values; it must be completely
equal. In practice, this also means that e.g. `scheme://hostname/` and
`scheme://username@hostname/` may result in two mounts even if the user enters
the same username in a credential prompt. There's another limitation in real
applications at the moment; requesting mount on `scheme://hostname/` wouldn't
match with `scheme://username@hostname/` at the beginning when the mount tracker
is trying to find existing mount since we don't know what username to use at
this point. Consequently, when going through the mount process and the username
is known, we don't have a tool for canceling the current mount operation and
redirecting ourselves to an active mount.

### URI mappers

URI mappers are tiny classes helping to parse URI strings with respect to
special parts, like a domain in `smb://` or to distinguish between `smb-share`
and `smb-server` backend types, etc. Used mostly when converting URI to a mount
spec and back.

Default parser is used when a specific URI mapper is not available for the given
scheme.

### Keyring integration

Secret storage integration is actually very basic; two helper functions are
available for direct backend use, for storing and retrieving credentials with
additional arguments. Also, please take the mount spec specifics into account,
having two nearly identical records with the difference of having the username
set is perfectly okay.

This feature also helps debugging significantly, allowing developers to overcome
credential prompts when running the backend manually since it's able to pick
credentials up from the keyring. Just be sure to pass correct mount spec
arguments so that matching is successful.

## FUSE daemon

The package provides a convenient FUSE mount for easy POSIX access to active
GIO mounts. This is handled by the `gvfsd-fuse` process, a daemon that is
autostarted when `gvfsd` is spawned. The default mount point is
`/run/user/<UID>/gvfs`, usually located on tmpfs, with a fallback to the old
location `~/.gvfs` when not available. On some systems, the user needs to be in
the `fuse` group to be actually able to mount FUSE filesystems.

Even if the FUSE daemon is not autostarted (by running `gvfsd --no-fuse`), it
can be started manually anytime. The daemon registers itself with the master
gvfs daemon by calling `org.gtk.vfs.MountTracker.RegisterFuse()` D-Bus method.
When the master gvfs daemon disappears from the session bus, the FUSE daemon
is terminated.

The daemon creates a flat structure of active GIO mounts at the topmost level,
named by internal `GMountSpec` string representation (in the form of
`protocol:arg1=val,arg2=val`). This string is not guaranteed and may change in
the future.

Please note that due to the different nature of POSIX and GIO filesystems, not
everything is possible in the POSIX world and there will always be trade-offs.
Moreover, not all backends implement every `GVfsBackend` method, e.g. a backend
implements file open/read/write/close but not seeking. Some POSIX applications
may not function properly due to that; the translation is not 1:1. Things like
returning zero filesystem size and free space just because the actual backend
has no native support may make some apps confused, refusing to write any data.

There are several security concerns; the first and most visible is denied access
to other UIDs including UID 0. This causes troubles to system commands and
daemons stat-ing the filesystem (i.e. `df` or `systemd-tmpfiles`) with an error
printed out. See [bug 560658](https://bugzilla.gnome.org/show_bug.cgi?id=560658)
for details and discussion.

The other security concern is revealing symlinks from backends. Unmodified, they
may point to the host system in case of absolute target paths and may not be
constrained in the mount point in any way. For now, the FUSE daemon dereferences
all symlinks and presents them as regular files or directories. Only broken
symlinks are still presented as symlinks. See
[bug 696298](https://bugzilla.gnome.org/show_bug.cgi?id=696298) for discussion.

### Local path mapping

The `g_file_get_path()` method, as implemented in `GDaemonFile`, can provide a
local pathname when the FUSE mount point is registered and connected with the
master gvfs daemon. This path can then be easily used for spawning applications
with a local path. When an application is asked to create a new `GFile` instance
using the local FUSE path, it is automatically converted to a native URI. That
way, we don't need to pass URI strings outside of the GIO world.

## Metadata

GVfs provides its own persistent storage for simple key-value pairs associated
with a particular file. Typically used for runtime data such as icon positions,
emblems, position within the document, notes, etc. Only non-critical data should
be stored. Usage-wise, it's a separate file attribute namespace for easy use
with `GFileInfo`.

All data are stored in so-called metadata database, a set of binary files in
`~/.local/share/gvfs-metadata`. Every database file carries a journal where the
most recent changes are stored for a short moment of time before they're written
to the master database file. Every file roughly corresponds to a `GMount`,
identified either by mount spec (for GVfs mounts) or by UUID or device name for
physical mounts. That brings some flexibility; when a mount is mounted in a
different path (automount; usually physical devices), the metadata subsystem is
still able to identify the device and match the existing database. A fallback to
database "root" is in place, taking everything starting with "/".

Retrieving data is done within `GDaemonFile` (and `GDaemonFileEnumerator`)
methods, directly accessing database files. Storing data is going through a
separate daemon, `gvfsd-metadata`, as a single point queueing all requests and
not blocking applications. Incoming data are first written in a journal file and
then, after a certain time (60 seconds) or in case of a full journal, they're
written to the master database file (metatree file). The journal contains all
incoming requests chronologically stacked up while the metatree file contains
clean key-value pairs with no duplicates.

The process of writeout is called rotation. The daemon first reads the existing
metatree file and iterates over the journal, going from oldest entries to newest
ones, applying the changes on top of existing values. Data are written in a new
metatree file and this file then atomically replaces the old database. Keeping
the old database file still open, the "rotated" bit is written in and the file
is closed. Since it was atomically replaced by the new database, it is unlinked
and data are destroyed by the operating system in an ideal case. There still may
be clients having the very same file open; however, in the process of data
retrieval, the rotated bit then indicates that the currently open database is
old and reopen is necessary.

Special care is taken when the metadata directory resides on NFS (e.g. having
the home directory on NFS). See `safe_open()` in `metadata/metatree.c` for
details; the code is trying to work around possible stale file handles by
linking to a temp file, opening, and unlinking again so that we actually don't
operate on the file directly, just modifying the same data.

This all works natively for files on GVfs mounts (`GDaemonFile`); for local
`file://` scheme, the GIO API has been extended to allow `GLocalFile` to go
through the default VFS implementation (`g_vfs_get_default()`, `GVfs` class) and
ask for metadata associated with a local file specifically.

Metadata are tied to the particular file or directory. If it's moved or deleted
(using GIO), changes are reflected in the metadata database the same way. It
works very similarly to xattrs.

There were some thoughts about feeding metadata into a Tracker store for easy
indexing; not sure if there are any drivers for that nowadays.

## Tools

GLib distributes `gio` cmd tool providing convenient access to GIO resources.
Imitating well-known POSIX commands, taking URI instead of filenames. This is
useful for testing.

## Tests

An integrated test suite is available and covers quite a large area. Testing a
project like GVfs is difficult as you need to provide either services for the
client (gvfs backends) or even simulate physical devices (for volume monitors).
This is more or less done by using available projects (apache, samba, twisted)
or system infrastructure (`scsi-debug` kernel module, systemd). Special care
needs to be held for distro support, taking into account various versions of
required dependencies and different (config files) locations.

The test suite is able to run in several modes; the most useful is the in-tree
sandboxed mode, which starts its own D-Bus session bus and uses the binaries
from the source tree. More advanced tests are available when executed as root.

# What's missing?

* Icon extensions

# FAQ

## Running applications under root

Starting a desktop session under root is generally discouraged and considered a
security risk. However, since it's a complete session, GVfs will work just fine.

A bigger problem is starting particular applications through `su`, `sudo`, or
any other graphical equivalent. This also applies to tunneled X connections.
Please understand that this effectively cuts the original user's D-Bus session
out; error messages printed on the console will clearly show that as well.
Tunneling D-Bus connections can be treated as a security risk and is disabled
AFAIK. You won't be able to access any active mounts; they all need to be opened
again within the new session.

The right solution is to use `admin://` backend that is based on `PolicyKit`.

## Writing your own backend

GVfs doesn't provide any public headers, and the daemon D-Bus protocol is
considered private and may change at any time (and that actually happened with
the GDBus port). Out-of-tree backends are simply not supported; the preferred
way is to fork/branch gvfs sources and write your backend on top of it. If you
then send the resulting patch to upstream developers, they'll be happy to review
it for you for potential inclusion.

The best way to start is grabbing an existing backend and studying its contents.
The local test backend is basically a proxy to the `file://` scheme originally
written for testing purposes. Backends are quite independent from the rest of
the GVfs codebase, and you shouldn't be needing any further changes outside the
backend.
