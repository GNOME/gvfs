#include <config.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#ifndef HAVE_SYSCTLBYNAME
#ifdef HAVE_SYS_PARAM_H
#include <sys/param.h>
#endif
#ifdef HAVE_SYS_MOUNT_H
#include <sys/mount.h>
#endif
#ifdef HAVE_SYS_POLL_H
#include <sys/poll.h>
#endif
#endif
#ifdef HAVE_POLL_H
#include <poll.h>
#endif
#include <stdio.h>
#include <unistd.h>
#include <sys/time.h>
#include <errno.h>
#include <string.h>
#include <signal.h>

#include "gunixmounts.h"

#define MOUNT_POLL_INTERVAL 4000

#ifdef HAVE_SYS_MNTTAB_H
#define MNTOPT_RO	"ro"
#endif

#ifdef HAVE_MNTENT_H
#include <mntent.h>
#elif defined (HAVE_SYS_MNTTAB_H)
#include <sys/mnttab.h>
#endif

#ifdef HAVE_SYS_VFSTAB_H
#include <sys/vfstab.h>
#endif

#if defined(HAVE_SYS_MNTCTL_H) && defined(HAVE_SYS_VMOUNT_H) && defined(HAVE_SYS_VFS_H)
#include <sys/mntctl.h>
#include <sys/vfs.h>
#include <sys/vmount.h>
#include <fshelp.h>
#endif

#if defined(HAVE_GETMNTINFO) && defined(HAVE_FSTAB_H) && defined(HAVE_SYS_MOUNT_H)
#include <sys/param.h>
#include <sys/ucred.h>
#include <sys/mount.h>
#include <fstab.h>
#ifdef HAVE_SYS_SYSCTL_H
#include <sys/sysctl.h>
#endif
#endif

#ifndef HAVE_SETMNTENT
#define setmntent(f,m) fopen(f,m)
#endif
#ifndef HAVE_ENDMNTENT
#define endmntent(f) fclose(f)
#endif

#ifdef HAVE_MNTENT_H

static char *
get_mtab_read_file (void)
{
#ifdef _PATH_MOUNTED
# ifdef __linux__
  return "/proc/mounts";
# else
  return _PATH_MOUNTED;
# endif
#else	
  return "/etc/mtab";
#endif
}

static char *
get_mtab_monitor_file (void)
{
#ifdef _PATH_MOUNTED
  return _PATH_MOUNTED;
#else	
  return "/etc/mtab";
#endif
}

gboolean
_g_get_unix_mounts (GList **return_list)
{
  static time_t last_mtime = 0;
  static off_t last_size = 0;
  struct mntent *mntent;
  FILE *file;
  char *read_file;
  char *stat_file;
  struct stat sb;
  GUnixMount *mount_entry;
  GHashTable *mounts_hash;
  
  read_file = get_mtab_read_file ();
  stat_file = get_mtab_monitor_file ();
  
  *return_list = NULL;
  
  if (stat (stat_file, &sb) < 0)
    {
      g_warning ("Unable to stat %s: %s", stat_file,
		 g_strerror (errno));
      return TRUE;
    }
  
  if (sb.st_mtime == last_mtime && sb.st_size == last_size)
    return FALSE;
  
  last_mtime = sb.st_mtime;
  last_size = sb.st_size;
  
  file = setmntent (read_file, "r");
  if (file == NULL)
    return TRUE;
  
  mounts_hash = g_hash_table_new (g_str_hash, g_str_equal);
  
  while ((mntent = getmntent (file)) != NULL)
    {
      /* ignore any mnt_fsname that is repeated and begins with a '/'
       *
       * We do this to avoid being fooled by --bind mounts, since
       * these have the same device as the location they bind to.
       * Its not an ideal solution to the problem, but its likely that
       * the most important mountpoint is first and the --bind ones after
       * that aren't as important. So it should work.
       *
       * The '/' is to handle procfs, tmpfs and other no device mounts.
       */
      if (mntent->mnt_fsname != NULL &&
	  mntent->mnt_fsname[0] == '/' &&
	  g_hash_table_lookup (mounts_hash, mntent->mnt_fsname))
	continue;
      
      mount_entry = g_new0 (GUnixMount, 1);
      mount_entry->mount_path = g_strdup (mntent->mnt_dir);
      mount_entry->device_path = g_strdup (mntent->mnt_fsname);
      mount_entry->filesystem_type = g_strdup (mntent->mnt_type);
      
      g_hash_table_insert (mounts_hash,
			   mount_entry->device_path,
			   mount_entry->device_path);
      
#if defined (HAVE_HASMNTOPT)
      if (hasmntopt (mntent, MNTOPT_RO) != NULL)
	mount_entry->is_read_only = TRUE;
#endif
      *return_list = g_list_prepend (*return_list, mount_entry);
    }
  g_hash_table_destroy (mounts_hash);
  
  endmntent (file);
  
  *return_list = g_list_reverse (*return_list);
  
  return TRUE;
}

#elif defined (HAVE_SYS_MNTTAB_H)

static char *
get_mtab_read_file (void)
{
#ifdef _PATH_MOUNTED
  return _PATH_MOUNTED;
#else	
  return "/etc/mnttab";
#endif
}

static char *
get_mtab_monitor_file (void)
{
  return get_mtab_read_file ();
}

gboolean
_g_get_unix_mounts (GList **return_list)
{
  static time_t last_mtime = 0;
  static off_t last_size = 0;
  struct mnttab mntent;
  FILE *file;
  char *read_file;
  char *stat_file;
  struct stat sb;
  GUnixMount *mount_entry;
  
  read_file = get_mtab_read_file ();
  stat_file = get_mtab_monitor_file ();
  
  *return_list = NULL;
  
  if (stat (stat_file, &sb) < 0)
    {
      g_warning ("Unable to stat %s: %s", stat_file,
		 g_strerror (errno));
      return TRUE;
    }

  if (sb.st_mtime == last_mtime && sb.st_size == last_size)
    return FALSE;

  last_mtime = sb.st_mtime;
  last_size = sb.st_size;
  
  file = setmntent (read_file, "r");
  if (file == NULL)
    return TRUE;
  
  while (! getmntent (file, &mntent))
    {
      mount_entry = g_new0 (GUnixMount, 1);
      
      mount_entry->mount_path = g_strdup (mntent.mnt_mountp);
      mount_entry->device_path = g_strdup (mntent.mnt_special);
      mount_entry->filesystem_type = g_strdup (mntent.mnt_fstype);
      
#if defined (HAVE_HASMNTOPT)
      if (hasmntopt (&mntent, MNTOPT_RO) != NULL)
	mount_entry->is_read_only = TRUE;
#endif
      
      *return_list = g_list_prepend (*return_list, mount_entry);
    }
  
  endmntent (file);
  
  *return_list = g_list_reverse (*return_list);
  
  return TRUE;
}

#elif defined(HAVE_SYS_MNTCTL_H) && defined(HAVE_SYS_VMOUNT_H) && defined(HAVE_SYS_VFS_H)

static char *
get_mtab_monitor_file (void)
{
  return NULL;
}

gboolean
_g_get_unix_mounts (GList **return_list)
{
  struct vfs_ent *fs_info;
  struct vmount *vmount_info;
  int vmount_number;
  unsigned int vmount_size;
  int current;
  
  *return_list = NULL;
  
  if (mntctl (MCTL_QUERY, sizeof (vmount_size), &vmount_size) != 0)
    {
      g_warning ("Unable to know the number of mounted volumes\n");
      
      return TRUE;
    }

  vmount_info = (struct vmount*)g_malloc (vmount_size);

  vmount_number = mntctl (MCTL_QUERY, vmount_size, vmount_info);
  
  if (vmount_info->vmt_revision != VMT_REVISION)
    g_warning ("Bad vmount structure revision number, want %d, got %d\n", VMT_REVISION, vmount_info->vmt_revision);

  if (vmount_number < 0)
    {
      g_warning ("Unable to recover mounted volumes information\n");
      
      g_free (vmount_info);
      return TRUE;
    }
  
  while (vmount_number > 0)
    {
      mount_entry = g_new0 (GUnixMount, 1);
      
      mount_entry->device_path = g_strdup (vmt2dataptr (vmount_info, VMT_OBJECT));
      mount_entry->mount_path = g_strdup (vmt2dataptr (vmount_info, VMT_STUB));
      /* is_removable = (vmount_info->vmt_flags & MNT_REMOVABLE) ? 1 : 0; */
      mount_entry->is_read_only = (vmount_info->vmt_flags & MNT_READONLY) ? 1 : 0;
      
      fs_info = getvfsbytype (vmount_info->vmt_gfstype);
      
      if (fs_info == NULL)
	mount_entry->filesystem_type = g_strdup ("unknown");
      else
	mount_entry->filesystem_type = g_strdup (fs_info->vfsent_name);
      
      *return_list = g_list_prepend (*return_list, mount_entry);
      
      vmount_info = (struct vmount *)( (char*)vmount_info 
				       + vmount_info->vmt_length);
      vmount_number--;
    }
  
  
  g_free (vmount_info);
  
  *return_list = g_list_reverse (*return_list);
  
  return TRUE;
}

#elif defined(HAVE_GETMNTINFO) && defined(HAVE_FSTAB_H) && defined(HAVE_SYS_MOUNT_H)

static char *
get_mtab_monitor_file (void)
{
  return NULL;
}

gboolean
_g_get_unix_mounts (GList **return_list)
{
  struct statfs *mntent = NULL;
  int num_mounts, i;
  GUnixMount *mount_entry;
  
  *return_list = NULL;
  
  /* Pass MNT_NOWAIT to avoid blocking trying to update NFS mounts. */
  if ((num_mounts = getmntinfo (&mntent, MNT_NOWAIT)) == 0)
    return TRUE;
  
  for (i = 0; i < num_mounts; i++)
    {
      mount_entry = g_new0 (GUnixMount, 1);
      
      mount_entry->mount_path = g_strdup (mntent[i].f_mntonname);
      mount_entry->device_path = g_strdup (mntent[i].f_mntfromname);
      mount_entry->filesystem_type = g_strdup (mntent[i].f_fstypename);
      if (mntent[i].f_flags & MNT_RDONLY)
	mount_entry->is_read_only = TRUE;
      
      *return_list = g_list_prepend (*return_list, mount_entry);
    }
  
  *return_list = g_list_reverse (*return_list);
  
  return TRUE;
}
#else
#error No _g_get_unix_mounts() implementation for system
#endif


/* _g_get_unix_mount_points():
 * read the fstab.
 * don't return swap and ignore mounts.
 */

static char *
get_fstab_file (void)
{
#if defined(HAVE_SYS_MNTCTL_H) && defined(HAVE_SYS_VMOUNT_H) && defined(HAVE_SYS_VFS_H)
  /* AIX */
  return "/etc/filesystems";
#elif defined(_PATH_MNTTAB)
  return _PATH_MNTTAB;
#elif defined(VFSTAB)
  return VFSTAB;
#else
  return "/etc/fstab";
#endif
}

#ifdef HAVE_MNTENT_H
gboolean
_g_get_unix_mount_points (GList **return_list)
{
  static time_t last_mtime = 0;
  static off_t last_size = 0;
  struct mntent *mntent;
  FILE *file;
  char *read_file;
  char *stat_file;
  char *opt, *opt_end;
  struct stat sb;
  GUnixMountPoint *mount_entry;
  
  stat_file = read_file = get_fstab_file ();
  
  *return_list = NULL;
  
  if (stat (stat_file, &sb) < 0)
    {
      g_warning ("Unable to stat %s: %s", stat_file,
		 g_strerror (errno));
      return TRUE;
    }

  if (sb.st_mtime == last_mtime && sb.st_size == last_size)
    return FALSE;

  last_mtime = sb.st_mtime;
  last_size = sb.st_size;
  
  file = setmntent (read_file, "r");
  if (file == NULL)
    return TRUE;

  while ((mntent = getmntent (file)) != NULL)
    {
      if ((strcmp (mntent->mnt_dir, "ignore") == 0) ||
	  (strcmp (mntent->mnt_dir, "swap") == 0))
	continue;
      
      mount_entry = g_new0 (GUnixMountPoint, 1);
      mount_entry->mount_path = g_strdup (mntent->mnt_dir);
      mount_entry->device_path = g_strdup (mntent->mnt_fsname);
      mount_entry->filesystem_type = g_strdup (mntent->mnt_type);
      
#ifdef HAVE_HASMNTOPT
      if (hasmntopt (mntent, MNTOPT_RO) != NULL)
	mount_entry->is_read_only = TRUE;
      
      if (hasmntopt (mntent, "loop") != NULL)
	mount_entry->is_loopback = TRUE;
      
      if ((opt = hasmntopt (mntent, "dev=")) != NULL)
	{
	  opt = opt + strlen("dev=");
	  opt_end = strchr (opt, ',');
	  if (opt_end)
	    mount_entry->dev_opt = g_strndup (opt, opt_end - opt);
	  else
	    mount_entry->dev_opt = g_strdup (opt);
	}
#endif
      
      if ((mntent->mnt_type != NULL && strcmp ("supermount", mntent->mnt_type) == 0)
#ifdef HAVE_HASMNTOPT
	  || (hasmntopt (mntent, "user") != NULL
	      && hasmntopt (mntent, "user") != hasmntopt (mntent, "user_xattr"))
	  || hasmntopt (mntent, "pamconsole") != NULL
	  || hasmntopt (mntent, "users") != NULL
	  || hasmntopt (mntent, "owner") != NULL
#endif
	  )
	mount_entry->is_user_mountable = TRUE;
      
      *return_list = g_list_prepend (*return_list, mount_entry);
    }
  
  endmntent (file);
  
  *return_list = g_list_reverse (*return_list);
  
  return TRUE;
}

#elif defined (HAVE_SYS_MNTTAB_H)

gboolean
_g_get_unix_mount_points (GList **return_list)
{
  static time_t last_mtime = 0;
  static off_t last_size = 0;
  struct mnttab mntent;
  FILE *file;
  char *read_file;
  char *stat_file;
  struct stat sb;
  GUnixMountPoint *mount_entry;
  
  stat_file = read_file = get_fstab_file ();
  
  *return_list = NULL;
  
  if (stat (stat_file, &sb) < 0)
    {
      g_warning ("Unable to stat %s: %s", stat_file,
		 g_strerror (errno));
      return TRUE;
    }

  if (sb.st_mtime == last_mtime && sb.st_size == last_size)
    return FALSE;

  last_mtime = sb.st_mtime;
  last_size = sb.st_size;
  
  file = setmntent (read_file, "r");
  if (file == NULL)
    return TRUE;

  while (! getmntent (file, &mntent))
    {
      if ((strcmp (mntent.mnt_mountp, "ignore") == 0) ||
	  (strcmp (mntent.mnt_mountp, "swap") == 0))
	continue;
      
      mount_entry = g_new0 (GUnixMountPoint, 1);
      
      mount_entry->mount_path = g_strdup (mntent.mnt_mountp);
      mount_entry->device_path = g_strdup (mntent.mnt_special);
      mount_entry->filesystem_type = g_strdup (mntent.mnt_fstype);
      
#ifdef HAVE_HASMNTOPT
      if (hasmntopt (&mntent, MNTOPT_RO) != NULL)
	mount_entry->is_read_only = TRUE;
      
      if (hasmntopt (&mntent, "lofs") != NULL)
	mount_entry->is_loopback = TRUE;
#endif
      
      if ((mntent.mnt_fstype != NULL)
#ifdef HAVE_HASMNTOPT
	  || (hasmntopt (&mntent, "user") != NULL
	      && hasmntopt (&mntent, "user") != hasmntopt (&mntent, "user_xattr"))
	  || hasmntopt (&mntent, "pamconsole") != NULL
	  || hasmntopt (&mntent, "users") != NULL
	  || hasmntopt (&mntent, "owner") != NULL
#endif
	  )
	mount_entry->is_user_mountable = TRUE;
      
      
      *return_list = g_list_prepend (*return_list, mount_entry);
    }
  
  endmntent (file);
  
  *return_list = g_list_reverse (*return_list);
  
  return TRUE;
}
#elif defined(HAVE_SYS_MNTCTL_H) && defined(HAVE_SYS_VMOUNT_H) && defined(HAVE_SYS_VFS_H)

/* functions to parse /etc/filesystems on aix */

/* read character, ignoring comments (begin with '*', end with '\n' */
static int
aix_fs_getc (FILE *fd)
{
  int c;
  
  while ((c = getc (fd)) == '*')
    {
      while (((c = getc (fd)) != '\n') && (c != EOF))
	;
    }
}

/* eat all continuous spaces in a file */
static int
aix_fs_ignorespace (FILE *fd)
{
  int c;
  
  while ((c = aix_fs_getc (fd)) != EOF)
    {
      if (!g_ascii_isspace (c))
	{
	  ungetc (c,fd);
	  return c;
	}
    }
  
  return EOF;
}

/* read one word from file */
static int
aix_fs_getword (FILE *fd, char *word)
{
  int c;
  
  aix_fs_ignorespace (fd);

  while (((c = aix_fs_getc (fd)) != EOF) && !g_ascii_isspace (c))
    {
      if (c == '"')
	{
	  while (((c = aix_fs_getc (fd)) != EOF) && (c != '"'))
	    *word++ = c;
	  else
	    *word++ = c;
	}
    }
  *word = 0;
  
  return c;
}

typedef struct {
  char mnt_mount[PATH_MAX];
  char mnt_special[PATH_MAX];
  char mnt_fstype[16];
  char mnt_options[128];
} AixMountTableEntry;

/* read mount points properties */
static int
aix_fs_get (FILE *fd, AixMountTableEntry *prop)
{
  static char word[PATH_MAX] = { 0 };
  char value[PATH_MAX];
  
  /* read stanza */
  if (word[0] == 0)
    {
      if (aix_fs_getword (fd, word) == EOF)
	return EOF;
    }

  word[strlen(word) - 1] = 0;
  strcpy (prop->mnt_mount, word);
  
  /* read attributes and value */
  
  while (aix_fs_getword (fd, word) != EOF)
    {
      /* test if is attribute or new stanza */
      if (word[strlen(word) - 1] == ':')
	return 0;
      
      /* read "=" */
      aix_fs_getword (fd, value);
      
      /* read value */
      aix_fs_getword (fd, value);
      
      if (strcmp (word, "dev") == 0)
	strcpy (prop->mnt_special, value);
      else if (strcmp (word, "vfs") == 0)
	strcpy (prop->mnt_fstype, value);
      else if (strcmp (word, "options") == 0)
	strcpy(prop->mnt_options, value);
    }
  
  return 0;
}

gboolean
_g_get_unix_mount_points (GList **return_list)
{
  static time_t last_mtime = 0;
  static off_t last_size = 0;
  struct mntent *mntent;
  FILE *file;
  char *read_file;
  char *stat_file;
  struct stat sb;
  GUnixMountPoint *mount_entry;
  AixMountTableEntry mntent;
  
  stat_file = read_file = get_fstab_file ();
  
  *return_list = NULL;
  
  if (stat (stat_file, &sb) < 0)
    {
      g_warning ("Unable to stat %s: %s", stat_file,
		 g_strerror (errno));
      return TRUE;
    }
		
  if (last_mtime != 0 && fsb.st_mtime == last_mtime && fsb.st_size == last_size)
    return FALSE;

  last_mtime = fsb.st_mtime;
  last_size = fsb.st_size;
  
  file = setmntent (read_file, "r");
  if (file == NULL)
    return TRUE;
  
  while (!aix_fs_get (file, &mntent))
    {
      if (strcmp ("cdrfs", mntent.mnt_fstype) == 0)
	{
	  mount_entry = g_new0 (GUnixMountPoint, 1);
	  
	  
	  mount_entry->mount_path = g_strdup (mntent.mnt_mount);
	  mount_entry->device_path = g_strdup (mntent.mnt_special);
	  mount_entry->filesystem_type = g_strdup (mntent.mnt_fstype);
	  mount_entry->is_read_only = TRUE;
	  mount_entry->is_user_mountable = TRUE;
	  
	  *return_list = g_list_prepend (*return_list, mount_entry);
	}
    }
	
  endmntent (file);
  
  *return_list = g_list_reverse (*return_list);
  
  return TRUE;
}

#elif defined(HAVE_GETMNTINFO) && defined(HAVE_FSTAB_H) && defined(HAVE_SYS_MOUNT_H)

gboolean
_g_get_unix_mount_points (GList **return_list)
{
  static time_t last_mtime = 0;
  static off_t last_size = 0;
  struct fstab *fstab = NULL;
  char *stat_file;
  struct stat fsb;
  GUnixMountPoint *mount_entry;
#ifdef HAVE_SYS_SYSCTL_H
  int usermnt = 0;
  size_t len = sizeof(usermnt);
  struct stat sb;
#endif
  
  stat_file = get_fstab_file ();
  
  *return_list = NULL;
  
  if (stat (stat_file, &fsb) < 0)
    {
      g_warning ("Unable to stat %s: %s", stat_file,
		 g_strerror (errno));
      return TRUE;
    }
  
  if (last_mtime != 0 && fsb.st_mtime == last_mtime && fsb.st_size == last_size)
    return FALSE;

  last_mtime = fsb.st_mtime;
  last_size = fsb.st_size;
  
  *return_list = NULL;
  
  if (!setfsent ())
    return TRUE;

#ifdef HAVE_SYS_SYSCTL_H
#if defined(HAVE_SYSCTLBYNAME)
  sysctlbyname ("vfs.usermount", &usermnt, &len, NULL, 0);
#elif defined(CTL_VFS) && defined(VFS_USERMOUNT)
  {
    int mib[2];
    
    mib[0] = CTL_VFS;
    mib[1] = VFS_USERMOUNT;
    sysctl (mib, 2, &usermnt, &len, NULL, 0);
  }
#elif defined(CTL_KERN) && defined(KERN_USERMOUNT)
  {
    int mib[2];
    
    mib[0] = CTL_KERN;
    mib[1] = KERN_USERMOUNT;
    sysctl (mib, 2, &usermnt, &len, NULL, 0);
  }
#endif
#endif
  
  while ((fstab = getfsent ()) != NULL)
    {
      if (strcmp (fstab->fs_vfstype, "swap") == 0)
	continue;
      
      mount_entry = g_new0 (GUnixMountPoint, 1);
      
      mount_entry->mount_path = g_strdup (fstab->fs_file);
      mount_entry->device_path = g_strdup (fstab->fs_spec);
      mount_entry->filesystem_type = g_strdup (fstab->fs_vfstype);
      
      if (strcmp (fstab->fs_type, "ro") == 0)
	mount_entry->is_read_only = TRUE;

#ifdef HAVE_SYS_SYSCTL_H
      if (usermnt != 0)
	{
	  uid_t uid = getuid ();
	  if (stat (fstab->fs_file, &sb) == 0)
	    {
	      if (uid == 0 || sb.st_uid == uid)
		mount_entry->is_user_mountable = TRUE;
	    }
	}
#endif

      *return_list = g_list_prepend (*return_list, mount_entry);
    }
  
  endfsent ();
  
  *return_list = g_list_reverse (*return_list);
  
  return TRUE;
}
#else
#error No _g_get_mount_table() implementation for system
#endif

typedef struct {
  GUnixMountCallback mountpoints_changed;
  GUnixMountCallback mounts_changed;
  gpointer user_data;
} MountMonitor;


/*
static GMonitorHandle *fstab_monitor = NULL;
static GMonitorHandle *mtab_monitor = NULL;
*/
static guint poll_tag = 0;
static GList *mount_monitors = NULL;

#if 0 /* Needs file monitoring */
static void
fstab_monitor_callback (GMonitorHandle *handle,
			const char *monitor_uri,
			const char *info_uri,
			GMonitorEventType event_type,
			gpointer user_data)
{
  (*fstab_callback) (user_data);
}

static void
mtab_monitor_callback (GMonitorHandle *handle,
		       const char *monitor_uri,
		       const char *info_uri,
		       GMonitorEventType event_type,
		       gpointer user_data)
{
  (*mtab_callback) (user_data);
}

#endif

static gboolean
poll_mounts (gpointer user_data)
{
  GList *l;

  for (l = mount_monitors; l != NULL; l = l->next)
    {
      MountMonitor *mount_monitor = l->data;

      mount_monitor->mountpoints_changed (mount_monitor->user_data);
      mount_monitor->mounts_changed (mount_monitor->user_data);
    }
  
  return TRUE;
}

gpointer
_g_monitor_unix_mounts (GUnixMountCallback mountpoints_changed,
			GUnixMountCallback mounts_changed,
			gpointer user_data)
{
  char *fstab_file;
  char *mtab_file;
  MountMonitor *mount_monitor;

  mount_monitor = g_new0 (MountMonitor, 1);
  mount_monitor->mountpoints_changed = mountpoints_changed;
  mount_monitor->mounts_changed = mounts_changed;
  mount_monitor->user_data = user_data;

  if (mount_monitors == NULL)
    {
      fstab_file = get_fstab_file ();
      mtab_file = get_mtab_monitor_file ();

#if 0
      if (fstab_file != NULL)
	{
	  char *fstab_uri;
	  fstab_uri = g_get_uri_from_local_path (fstab_file);
	  g_monitor_add (&fstab_monitor,
			 fstab_uri,
			 G_MONITOR_FILE,
			 fstab_monitor_callback,
			 mount_table_changed_user_data);
	  g_free (fstab_uri);
	}
      
      if (mtab_file != NULL)
	{
	  char *mtab_uri;
	  mtab_uri = g_get_uri_from_local_path (mtab_file);
	  g_monitor_add (&mtab_monitor,
			 mtab_uri,
			 G_MONITOR_FILE,
			 mtab_monitor_callback,
			 current_mounts_user_data);
	  g_free (mtab_uri);
	}
#endif

      /* Fallback to polling */
      
      poll_tag = g_timeout_add (MOUNT_POLL_INTERVAL,
				poll_mounts,
				NULL);
    }

  mount_monitors = g_list_prepend (mount_monitors, mount_monitor);
  return mount_monitor;
}

void
_g_stop_monitoring_unix_mounts (gpointer tag)
{
  MountMonitor *mount_monitor = tag;
  
  if (g_list_find (mount_monitors, mount_monitor) == NULL)
    {
      g_warning ("Could not stop mount monitor %p", tag);
      return;
    }

  mount_monitors = g_list_remove (mount_monitors, mount_monitor);
  g_free (mount_monitor);

  if (mount_monitors == NULL)
    {
      /*
	if (fstab_monitor != NULL)
	{
	g_monitor_cancel (fstab_monitor);
	fstab_monitor = NULL;
	}
	if (mtab_monitor != NULL)
	{
	g_monitor_cancel (mtab_monitor);
	mtab_monitor = NULL;
	}
      */
  
      if (poll_tag != 0)
	{
	  g_source_remove (poll_tag);
	  poll_tag = 0;
	}
    }
}

void
_g_unix_mount_free (GUnixMount *mount_entry)
{
  g_free (mount_entry->mount_path);
  g_free (mount_entry->device_path);
  g_free (mount_entry->filesystem_type);
  g_free (mount_entry);
}

void
_g_unix_mount_point_free (GUnixMountPoint *mount_point)
{
  g_free (mount_point->mount_path);
  g_free (mount_point->device_path);
  g_free (mount_point->filesystem_type);
  g_free (mount_point->dev_opt);
  g_free (mount_point);
}

static gboolean
strcmp_null (const char *str1,
	     const char *str2)
{
  if (str1 == str2)
    return 0;
  if (str1 == NULL && str2 != NULL) 
    return -1;
  if (str1 != NULL && str2 == NULL)
    return 1;
  return strcmp (str1, str2);
}

gint
_g_unix_mount_compare (GUnixMount      *mount1,
		       GUnixMount      *mount2)
{
  int res;
  
  res = strcmp_null (mount1->mount_path, mount2->mount_path);
  if (res != 0)
    return res;
	
  res = strcmp_null (mount1->device_path, mount2->device_path);
  if (res != 0)
    return res;
	
  res = strcmp_null (mount1->filesystem_type, mount2->filesystem_type);
  if (res != 0)
    return res;

  res =  mount1->is_read_only - mount2->is_read_only;
  if (res != 0)
    return res;
  
  return 0;
}

gint
_g_unix_mount_point_compare (GUnixMountPoint *mount1,
			     GUnixMountPoint *mount2)
{
  int res;

  res = strcmp_null (mount1->mount_path, mount2->mount_path);
  if (res != 0) 
    return res;
	
  res = strcmp_null (mount1->device_path, mount2->device_path);
  if (res != 0) 
    return res;
	
  res = strcmp_null (mount1->filesystem_type, mount2->filesystem_type);
  if (res != 0) 
    return res;

  res = strcmp_null (mount1->dev_opt, mount2->dev_opt);
  if (res != 0)
    return res;

  res =  mount1->is_read_only - mount2->is_read_only;
  if (res != 0) 
    return res;

  res = mount1->is_user_mountable - mount2->is_user_mountable;
  if (res != 0) 
    return res;

  res = mount1->is_loopback - mount2->is_loopback;
  if (res != 0)
    return res;
  
  return 0;
}


GUnixMountType
_g_guess_type_for_mount (const char     *mount_path,
			 const char     *device_path,
			 const char     *filesystem_type)
{
  GUnixMountType type;
  char *basename;

  type = G_UNIX_MOUNT_TYPE_UNKNOWN;
  
  if ((strcmp (filesystem_type, "udf") == 0) ||
      (strcmp (filesystem_type, "iso9660") == 0) ||
      (strcmp (filesystem_type, "cd9660") == 0))
    type = G_UNIX_MOUNT_TYPE_CDROM;
  else if (strcmp (filesystem_type, "nfs") == 0)
    type = G_UNIX_MOUNT_TYPE_NFS;
  else if (g_str_has_prefix (device_path, "/vol/dev/diskette/") ||
	   g_str_has_prefix (device_path, "/dev/fd") ||
	   g_str_has_prefix (device_path, "/dev/floppy"))
    type = G_UNIX_MOUNT_TYPE_FLOPPY;
  else if (g_str_has_prefix (device_path, "/dev/cdrom") ||
	   g_str_has_prefix (device_path, "/dev/acd") ||
	   g_str_has_prefix (device_path, "/dev/cd"))
    type = G_UNIX_MOUNT_TYPE_CDROM;
  else if (g_str_has_prefix (device_path, "/vol/"))
    {
      const char *name = mount_path + strlen ("/");
      
      if (g_str_has_prefix (name, "cdrom"))
	type = G_UNIX_MOUNT_TYPE_CDROM;
      else if (g_str_has_prefix (name, "floppy") ||
	       g_str_has_prefix (device_path, "/vol/dev/diskette/")) 
	type = G_UNIX_MOUNT_TYPE_FLOPPY;
      else if (g_str_has_prefix (name, "rmdisk")) 
	type = G_UNIX_MOUNT_TYPE_ZIP;
      else if (g_str_has_prefix (name, "jaz"))
	type = G_UNIX_MOUNT_TYPE_JAZ;
      else if (g_str_has_prefix (name, "memstick"))
	type = G_UNIX_MOUNT_TYPE_MEMSTICK;
    }
  else
    {
      basename = g_path_get_basename (mount_path);
      
      if (g_str_has_prefix (basename, "cdrom") ||
	  g_str_has_prefix (basename, "cdwriter") ||
	  g_str_has_prefix (basename, "burn") ||
	  g_str_has_prefix (basename, "cdr") ||
	  g_str_has_prefix (basename, "cdrw") ||
	  g_str_has_prefix (basename, "dvdrom") ||
	  g_str_has_prefix (basename, "dvdram") ||
	  g_str_has_prefix (basename, "dvdr") ||
	  g_str_has_prefix (basename, "dvdrw") ||
	  g_str_has_prefix (basename, "cdrom_dvdrom") ||
	  g_str_has_prefix (basename, "cdrom_dvdram") ||
	  g_str_has_prefix (basename, "cdrom_dvdr") ||
	  g_str_has_prefix (basename, "cdrom_dvdrw") ||
	  g_str_has_prefix (basename, "cdr_dvdrom") ||
	  g_str_has_prefix (basename, "cdr_dvdram") ||
	  g_str_has_prefix (basename, "cdr_dvdr") ||
	  g_str_has_prefix (basename, "cdr_dvdrw") ||
	  g_str_has_prefix (basename, "cdrw_dvdrom") ||
	  g_str_has_prefix (basename, "cdrw_dvdram") ||
	  g_str_has_prefix (basename, "cdrw_dvdr") ||
	  g_str_has_prefix (basename, "cdrw_dvdrw"))
	type = G_UNIX_MOUNT_TYPE_CDROM;
      else if (g_str_has_prefix (basename, "floppy"))
	type = G_UNIX_MOUNT_TYPE_FLOPPY;
      else if (g_str_has_prefix (basename, "zip"))
	type = G_UNIX_MOUNT_TYPE_ZIP;
      else if (g_str_has_prefix (basename, "jaz"))
	type = G_UNIX_MOUNT_TYPE_JAZ;
      else if (g_str_has_prefix (basename, "camera"))
	type = G_UNIX_MOUNT_TYPE_CAMERA;
      else if (g_str_has_prefix (basename, "memstick") ||
	       g_str_has_prefix (basename, "memory_stick") ||
	       g_str_has_prefix (basename, "ram"))
	type = G_UNIX_MOUNT_TYPE_MEMSTICK;
      else if (g_str_has_prefix (basename, "compact_flash"))
	type = G_UNIX_MOUNT_TYPE_CF;
      else if (g_str_has_prefix (basename, "smart_media"))
	type = G_UNIX_MOUNT_TYPE_SM;
      else if (g_str_has_prefix (basename, "sd_mmc"))
	type = G_UNIX_MOUNT_TYPE_SDMMC;
      else if (g_str_has_prefix (basename, "ipod"))
	type = G_UNIX_MOUNT_TYPE_IPOD;
      
      g_free (basename);
    }
  
  if (type == G_UNIX_MOUNT_TYPE_UNKNOWN)
    type = G_UNIX_MOUNT_TYPE_HD;
  
  return type;
}
