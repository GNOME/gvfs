/* GIO - GLib Input, Output and Streaming Library
 *  libsmb-compat.h: compatibility macros for libsmbclient < 3.2.0-pre2
 * 
 * Copyright (C) 2006-2008 Red Hat, Inc.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General
 * Public License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place, Suite 330,
 * Boston, MA 02111-1307, USA.
 *
 * Author: Tomas Bzatek <tbzatek@redhat.com>
 */

#include <config.h>

#include <libsmbclient.h>



#ifndef DEPRECATED_SMBC_INTERFACE


typedef SMBCFILE * (*smbc_opendir_fn)(SMBCCTX *c,
                                      const char *fname);

typedef int (*smbc_closedir_fn)(SMBCCTX *c,
                                SMBCFILE *dir);

typedef struct smbc_dirent * (*smbc_readdir_fn)(SMBCCTX *c,
                                                SMBCFILE *dir);

typedef int (*smbc_getdents_fn)(SMBCCTX *c,
                                SMBCFILE *dir,
                                struct smbc_dirent *dirp,
                                int count);

typedef int (*smbc_stat_fn)(SMBCCTX *c,
                            const char *fname,
                            struct stat *st);

typedef SMBCFILE * (*smbc_open_fn)(SMBCCTX *c,
                                   const char *fname,
                                   int flags,
                                   mode_t mode);

typedef SMBCFILE * (*smbc_creat_fn)(SMBCCTX *c,
                                    const char *path,
                                    mode_t mode);

typedef ssize_t (*smbc_read_fn)(SMBCCTX *c,
                                SMBCFILE *file,
                                void *buf,
                                size_t count);

typedef ssize_t (*smbc_write_fn)(SMBCCTX *c,
                                 SMBCFILE *file,
                                 void *buf,
                                 size_t count);

typedef int (*smbc_unlink_fn)(SMBCCTX *c,
                              const char *fname);

typedef int (*smbc_rename_fn)(SMBCCTX *ocontext,
                              const char *oname,
                              SMBCCTX *ncontext,
                              const char *nname);

typedef off_t (*smbc_lseek_fn)(SMBCCTX *c,
                               SMBCFILE * file,
                               off_t offset,
                               int whence);

typedef int (*smbc_close_fn)(SMBCCTX *c,
                             SMBCFILE *file);

typedef int (*smbc_fstat_fn)(SMBCCTX *c,
                             SMBCFILE *file,
                             struct stat *st);

typedef int (*smbc_rmdir_fn)(SMBCCTX *c,
                             const char *fname);

typedef int (*smbc_mkdir_fn)(SMBCCTX *c,
                             const char *fname,
                             mode_t mode);

typedef int (*smbc_chmod_fn)(SMBCCTX *c,
                             const char *fname,
                             mode_t mode);

typedef int (*smbc_utimes_fn)(SMBCCTX *c,
                              const char *fname,
                              struct timeval *tbuf);



#define smbc_getOptionUserData(ctx)		\
			smbc_option_get (ctx, "user_data")

#define smbc_setOptionUserData(ctx, data)	\
			smbc_option_set (ctx, "user_data", data)

#define smbc_setDebug(ctx, d)	\
			ctx->debug = d

#define smbc_setFunctionAuthDataWithContext(ctx, func) 	{ \
			ctx->callbacks.auth_fn = NULL; \
  			smbc_option_set (ctx, "auth_function", \
  					(void *) func); \
			}

#define smbc_setFunctionAddCachedServer(ctx, func)		\
			ctx->callbacks.add_cached_srv_fn = func

#define smbc_setFunctionGetCachedServer(ctx, func)		\
			ctx->callbacks.get_cached_srv_fn = func

#define smbc_setFunctionRemoveCachedServer(ctx, func)	\
			ctx->callbacks.remove_cached_srv_fn = func

#define smbc_setFunctionPurgeCachedServers(ctx, func)	\
			ctx->callbacks.purge_cached_fn = func

/* libsmbclient frees this on it's own, so make sure 
 * to use simple system malloc */
#define smbc_setWorkgroup(ctx, data)	\
			ctx->workgroup = strdup (data)

#define smbc_getWorkgroup(ctx)	\
			ctx->workgroup

#if defined(HAVE_SAMBA_FLAGS) && defined(SMB_CTX_FLAG_USE_KERBEROS) && defined(SMB_CTX_FLAG_FALLBACK_AFTER_KERBEROS)
	#define smbc_setOptionUseKerberos(ctx, val)		\
					ctx->flags |= SMB_CTX_FLAG_USE_KERBEROS
#else 					
	#define smbc_setOptionUseKerberos(ctx, val)		{  }	
#endif	

#if defined(HAVE_SAMBA_FLAGS) && defined(SMB_CTX_FLAG_USE_KERBEROS) && defined(SMB_CTX_FLAG_FALLBACK_AFTER_KERBEROS)
	#define smbc_setOptionFallbackAfterKerberos(ctx, val)	\
			ctx->flags |= SMB_CTX_FLAG_FALLBACK_AFTER_KERBEROS
#else 					
	#define smbc_setOptionFallbackAfterKerberos(ctx, val)	{  }	
#endif	
		
#if defined(HAVE_SAMBA_FLAGS) && defined(SMBCCTX_FLAG_NO_AUTO_ANONYMOUS_LOGON)
	#define smbc_setOptionNoAutoAnonymousLogin(ctx, val)	\
			ctx->flags |= SMBCCTX_FLAG_NO_AUTO_ANONYMOUS_LOGON
#else 					
	#define smbc_setOptionNoAutoAnonymousLogin(ctx, val)	{  }	
#endif	


#define smbc_setOptionDebugToStderr(ctx, val)	\
			smbc_option_set(ctx, "debug_stderr", (void *) val)

#define smbc_getFunctionStat(ctx)	ctx->stat

#define smbc_getFunctionFstat(ctx)	ctx->fstat

#define smbc_getFunctionOpen(ctx)	ctx->open

#define smbc_getFunctionRead(ctx)	ctx->read

#define smbc_getFunctionWrite(ctx)	ctx->write

#define smbc_getFunctionLseek(ctx)	ctx->lseek

#define smbc_getFunctionClose(ctx)	ctx->close_fn

#define smbc_getFunctionUnlink(ctx)	ctx->unlink

#define smbc_getFunctionRename(ctx)	ctx->rename

#define smbc_getFunctionOpendir(ctx)	ctx->opendir

#define smbc_getFunctionGetdents(ctx)	ctx->getdents

#define smbc_getFunctionClosedir(ctx)	ctx->closedir

#define smbc_getFunctionRmdir(ctx)	ctx->rmdir

#define smbc_getFunctionMkdir(ctx)	ctx->mkdir

#define smbc_getFunctionChmod(ctx)	ctx->chmod

#define smbc_getFunctionUtimes(ctx)	ctx->utimes


#endif
