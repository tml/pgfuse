/*
    Copyright (C) 2012 Andreas Baumann <abaumann@yahoo.com>

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <string.h>		/* for strdup, strlen, memset */
#include <libgen.h>		/* for POSIX compliant basename */
#include <unistd.h>		/* for exit */
#include <stdlib.h>		/* for EXIT_FAILURE, EXIT_SUCCESS */
#include <stdio.h>		/* for fprintf */
#include <stddef.h>		/* for offsetof */
#include <syslog.h>		/* for openlog, syslog */
#include <errno.h>		/* for ENOENT and friends */
#include <sys/types.h>		/* size_t */
#include <sys/stat.h>		/* mode_t */
#include <values.h>		/* for INT_MAX */
#include <stdint.h>		/* for uint64_t */
#include <inttypes.h>		/* for PRIxxx macros */
#include <mntent.h>		/* for iterating mount entries */
#include <sys/vfs.h>		/* for statfs */
#include <limits.h>

#include <fuse.h>		/* for user-land filesystem */
#include <fuse_opt.h>		/* fuse command line parser */

#include <pthread.h>		/* for pthread_self */

#if FUSE_VERSION < 21
#error Currently only written for newer FUSE API (FUSE_VERSION at least 21)
#endif

#include "config.h"		/* compiled in defaults */
#include "pgsql.h"		/* implements Postgresql accessers */
#include "pool.h"		/* implements the connection pool */

/* --- FUSE private context data --- */

typedef struct PgFuseData {
	int verbose;		/* whether we should be verbose */
	char *conninfo;		/* connection info as used in PQconnectdb */
	char *mountpoint;	/* where we mount the virtual filesystem */
	PGconn *conn;		/* the database handle to operate on (single-thread only) */
	PgConnPool pool;	/* the database pool to operate on (multi-thread only) */
	int read_only;		/* whether the mount point is read-only */
	int multi_threaded;	/* whether we run multi-threaded */
	size_t block_size;	/* block size to use for storage of data in bytea fields */
} PgFuseData;

/* --- timestamp helpers --- */

static struct timespec now( void )
{
	int res;
	struct timeval t;
	struct timezone tz;
	struct timespec s;
	
	res = gettimeofday( &t, &tz );
	if( res != 0 ) {
		s.tv_sec = 0;
		s.tv_nsec = 0;
		return s;
	}
	
	s.tv_sec = t.tv_sec;
	s.tv_nsec = t.tv_usec * 1000;
	
	return s;
}

/* --- pool helpers --- */

static PGconn *psql_acquire( PgFuseData *data )
{
	if( !data->multi_threaded ) {
		return data->conn;
	}
	
	return psql_pool_acquire( &data->pool );
}

static int psql_release( PgFuseData *data, PGconn *conn )
{
	if( !data->multi_threaded ) return 0;
	
	return psql_pool_release( &data->pool, conn );
}

#define ACQUIRE( C ) \
	C = psql_acquire( data ); \
	if( C == NULL ) return -EIO;
	
#define RELEASE( C ) \
	if( psql_release( data, C ) < 0 ) return -EIO;

#define THREAD_ID (unsigned int)pthread_self( )

/* --- implementation of FUSE hooks --- */

static void *pgfuse_init( struct fuse_conn_info *conn )
{
	PgFuseData *data = (PgFuseData *)fuse_get_context( )->private_data;
	
	syslog( LOG_INFO, "Mounting file system on '%s' ('%s', %s), thread #%u",
		data->mountpoint, data->conninfo,
		data->read_only ? "read-only" : "read-write",
		THREAD_ID );
	
	/* in single-threaded case we just need one shared PostgreSQL connection */
	if( !data->multi_threaded ) {
		data->conn = PQconnectdb( data->conninfo );
		if( PQstatus( data->conn ) != CONNECTION_OK ) {
			syslog( LOG_ERR, "Connection to database failed: %s",
				PQerrorMessage( data->conn ) );
			PQfinish( data->conn );
			exit( EXIT_FAILURE );
		}
	} else {
		int res;

		res = psql_pool_init( &data->pool, data->conninfo, MAX_DB_CONNECTIONS );
		if( res < 0 ) {
			syslog( LOG_ERR, "Allocating database connection pool failed!" );
			exit( EXIT_FAILURE );
		}
	}
	
	return data;
}

static void pgfuse_destroy( void *userdata )
{
	PgFuseData *data = (PgFuseData *)userdata;
	
	syslog( LOG_INFO, "Unmounting file system on '%s' (%s), thread #%u",
		data->mountpoint, data->conninfo, THREAD_ID );

	if( !data->multi_threaded ) {
		PQfinish( data->conn );
	} else {
		(void)psql_pool_destroy( &data->pool );
	}
}

static int pgfuse_fgetattr( const char *path, struct stat *stbuf, struct fuse_file_info *fi )
{
	PgFuseData *data = (PgFuseData *)fuse_get_context( )->private_data;
	int64_t id;
	PgMeta meta;
	PGconn *conn;
	
	if( data->verbose ) {
		syslog( LOG_INFO, "FgetAttrs '%s' on '%s', thread #%u",
			path, data->mountpoint, THREAD_ID );
	}

	ACQUIRE( conn );
	PSQL_BEGIN( conn );
	
	memset( stbuf, 0, sizeof( struct stat ) );

	id = psql_read_meta( conn, fi->fh, path, &meta );
	if( id < 0 ) {
		PSQL_ROLLBACK( conn ); RELEASE( conn );
		return id;
	}

	if( data->verbose ) {
		syslog( LOG_DEBUG, "Id for %s '%s' is %"PRIi64", thread #%u",
			S_ISDIR( meta.mode ) ? "dir" : "file", path, id,
			THREAD_ID );
	}
	
	/* TODO: check bits of inodes of the kernel */
	stbuf->st_ino = id;
	stbuf->st_blocks = 0;
	stbuf->st_mode = meta.mode;
	stbuf->st_size = meta.size;
	stbuf->st_blksize = data->block_size;
	stbuf->st_blocks = ( meta.size + data->block_size - 1 ) / data->block_size;
	/* TODO: set correctly from table */
	stbuf->st_nlink = 1;
	stbuf->st_uid = meta.uid;
	stbuf->st_gid = meta.gid;

	PSQL_COMMIT( conn ); RELEASE( conn );
	
	return 0;
}

static int pgfuse_getattr( const char *path, struct stat *stbuf )
{
	PgFuseData *data = (PgFuseData *)fuse_get_context( )->private_data;
	int64_t id;
	PgMeta meta;
	PGconn *conn;

	if( data->verbose ) {
		syslog( LOG_INFO, "GetAttrs '%s' on '%s', thread #%u",
			path, data->mountpoint, THREAD_ID );
	}

	ACQUIRE( conn );
	PSQL_BEGIN( conn );
	
	memset( stbuf, 0, sizeof( struct stat ) );

	id = psql_read_meta_from_path( conn, path, &meta );
	if( id < 0 ) {
		PSQL_ROLLBACK( conn ); RELEASE( conn );
		return id;
	}

	if( data->verbose ) {
		syslog( LOG_DEBUG, "Id for %s '%s' is %"PRIi64", thread #%u",
			S_ISDIR( meta.mode ) ? "dir" : "file", path, id,
			THREAD_ID );
	}
	
	/* TODO: check bits of inodes of the kernel */
	stbuf->st_ino = id;
	stbuf->st_blocks = 0;
	stbuf->st_mode = meta.mode;
	stbuf->st_size = meta.size;
	stbuf->st_blksize = data->block_size;
	stbuf->st_blocks = ( meta.size + data->block_size - 1 ) / data->block_size;
	/* TODO: set correctly from table */
	stbuf->st_nlink = 1;
	stbuf->st_uid = meta.uid;
	stbuf->st_gid = meta.gid;
	stbuf->st_atime = meta.atime.tv_sec;
	stbuf->st_mtime = meta.mtime.tv_sec;
	stbuf->st_ctime = meta.ctime.tv_sec;

	PSQL_COMMIT( conn ); RELEASE( conn );
	
	return 0;
}

static int pgfuse_access( const char *path, int mode )
{
	PgFuseData *data = (PgFuseData *)fuse_get_context( )->private_data;

	if( data->verbose ) {
		syslog( LOG_INFO, "Access on '%s' and mode '%o, thread #%u",
			path, (unsigned int)mode, THREAD_ID );
	}
	
	/* TODO: check access, but not now. grant always access */
	return 0;
}

static char *flags_to_string( int flags )
{
	char *s;
	char *mode_s = "";
	
	if( ( flags & O_ACCMODE ) == O_WRONLY ) mode_s = "O_WRONLY";
	else if( ( flags & O_ACCMODE ) == O_RDWR ) mode_s = "O_RDWR";
	else if( ( flags & O_ACCMODE ) == O_RDONLY ) mode_s = "O_RDONLY";
	
	s = (char *)malloc( 100 );
	if( s == NULL ) return "<memory allocation failed>";

	snprintf( s, 100, "access_mode=%s, flags=%s%s%s%s",
		mode_s,
		( flags & O_CREAT ) ? "O_CREAT " : "",
		( flags & O_TRUNC ) ? "O_TRUNC " : "",
		( flags & O_EXCL ) ? "O_EXCL " : "",
		( flags & O_APPEND ) ? "O_APPEND " : "");
	
	return s;
}

static int pgfuse_create( const char *path, mode_t mode, struct fuse_file_info *fi )
{
	PgFuseData *data = (PgFuseData *)fuse_get_context( )->private_data;
	int64_t id;
	PgMeta meta;
	char *copy_path;
	char *parent_path;
	char *new_file;
	int64_t parent_id;
	int64_t res;
	PGconn *conn;

	if( data->verbose ) {
		char *s = flags_to_string( fi->flags );
		syslog( LOG_INFO, "Create '%s' in mode '%o' on '%s' with flags '%s', thread #%u",
			path, mode, data->mountpoint, s, THREAD_ID );
		if( *s != '<' ) free( s );
	}
	
	ACQUIRE( conn );		
	PSQL_BEGIN( conn );
	
	if( data->read_only ) {
		PSQL_ROLLBACK( conn ); RELEASE( conn );
		return -EROFS;
	}
	
	id = psql_read_meta_from_path( conn, path, &meta );
	if( id < 0 && id != -ENOENT ) {
		PSQL_ROLLBACK( conn ); RELEASE( conn );
		return id;
	}
	
	if( id >= 0 ) {
		if( data->verbose ) {
			syslog( LOG_DEBUG, "Id for dir '%s' is %"PRIi64", thread #%u",
				path, id, THREAD_ID );
		}
		
		if( S_ISDIR(meta.mode ) ) {
			PSQL_ROLLBACK( conn ); RELEASE( conn );
			return -EISDIR;
		}
		
		PSQL_ROLLBACK( conn ); RELEASE( conn );
		return -EEXIST;
	}
	
	copy_path = strdup( path );
	if( copy_path == NULL ) {
		syslog( LOG_ERR, "Out of memory in Create '%s'!", path );
		PSQL_ROLLBACK( conn ); RELEASE( conn );
		return -ENOMEM;
	}
	
	parent_path = dirname( copy_path );

	parent_id = psql_read_meta_from_path( conn, parent_path, &meta );
	if( parent_id < 0 ) {
		PSQL_ROLLBACK( conn ); RELEASE( conn );
		return parent_id;
	}
	if( !S_ISDIR(meta.mode ) ) {
		PSQL_ROLLBACK( conn ); RELEASE( conn );
		return -ENOENT;
	}
	
	if( data->verbose ) {
		syslog( LOG_DEBUG, "Parent_id for new file '%s' in dir '%s' is %"PRIi64", thread #%u",
			path, parent_path, parent_id, THREAD_ID );
	}
	
	free( copy_path );
	copy_path = strdup( path );
	if( copy_path == NULL ) {
		free( parent_path );
		syslog( LOG_ERR, "Out of memory in Create '%s'!", path );
		PSQL_ROLLBACK( conn ); RELEASE( conn );
		return -ENOMEM;
	}
	
	new_file = basename( copy_path );
	
	meta.size = 0;
	meta.mode = mode;
	meta.uid = fuse_get_context( )->uid;
	meta.gid = fuse_get_context( )->gid;
	meta.ctime = now( );
	meta.mtime = meta.ctime;
	meta.atime = meta.ctime;
	
	res = psql_create_file( conn, parent_id, path, new_file, meta );
	if( res < 0 ) {
		free( copy_path );
		PSQL_ROLLBACK( conn ); RELEASE( conn );
		return res;
	}
	
	id = psql_read_meta_from_path( conn, path, &meta );
	if( id < 0 ) {
		free( copy_path );
		PSQL_ROLLBACK( conn ); RELEASE( conn );
		return res;
	}
	
	if( data->verbose ) {
		syslog( LOG_DEBUG, "Id for new file '%s' is %"PRIi64", thread #%u",
			path, id, THREAD_ID );
	}
	
	fi->fh = id;
	
	free( copy_path );

	PSQL_COMMIT( conn ); RELEASE( conn );
	
	return res;
}


static int pgfuse_open( const char *path, struct fuse_file_info *fi )
{
	PgFuseData *data = (PgFuseData *)fuse_get_context( )->private_data;
	PgMeta meta;
	int64_t id;
	int64_t res;
	PGconn *conn;

	if( data->verbose ) {
		char *s = flags_to_string( fi->flags );
		syslog( LOG_INFO, "Open '%s' on '%s' with flags '%s', thread #%u",
			path, data->mountpoint, s, THREAD_ID );
		if( *s != '<' ) free( s );
	}

	ACQUIRE( conn );
	PSQL_BEGIN( conn );

	id = psql_read_meta_from_path( conn, path, &meta );
	if( id < 0 ) {
		PSQL_ROLLBACK( conn ); RELEASE( conn );
		return id;
	}
	
	if( data->verbose ) {
		syslog( LOG_DEBUG, "Id for file '%s' to open is %"PRIi64", thread #%u",
			path, id, THREAD_ID );
	}
		
	if( S_ISDIR( meta.mode ) ) {
		PSQL_ROLLBACK( conn ); RELEASE( conn );
		return -EISDIR;
	}
	
	if( data->read_only ) {
		if( ( fi->flags & O_ACCMODE ) != O_RDONLY ) {
			PSQL_ROLLBACK( conn ); RELEASE( conn );
			return -EROFS;
		}
	}
	
	res = psql_write_meta( conn, id, path, meta );
	if( res < 0 ) {
		PSQL_ROLLBACK( conn ); RELEASE( conn );
		return res;
	}	
		
	fi->fh = id;

	PSQL_COMMIT( conn ); RELEASE( conn );
	
	return 0;
}

static int pgfuse_opendir( const char *path, struct fuse_file_info *fi )
{
	/* nothing to do, everything is done in pgfuse_readdir currently */
	return 0;
}

static int pgfuse_readdir( const char *path, void *buf, fuse_fill_dir_t filler,
                           off_t offset, struct fuse_file_info *fi )
{
	PgFuseData *data = (PgFuseData *)fuse_get_context( )->private_data;
	int id;
	int res;
	PgMeta meta;
	PGconn *conn;

	if( data->verbose ) {
		syslog( LOG_INFO, "Readdir '%s' on '%s', thread #%u",
			path, data->mountpoint, THREAD_ID );
	}
	
	ACQUIRE( conn );	
	PSQL_BEGIN( conn );
	
	filler( buf, ".", NULL, 0 );
	filler( buf, "..", NULL, 0 );
	
	id = psql_read_meta_from_path( conn, path, &meta );
	if( id < 0 ) {
		PSQL_ROLLBACK( conn ); RELEASE( conn );
		return id;
	}
	
	res = psql_readdir( conn, id, buf, filler );
	if( res < 0 ) {
		PSQL_ROLLBACK( conn ); RELEASE( conn );
		return res;
	}
	
	PSQL_COMMIT( conn ); RELEASE( conn );
	
	return 0;
}

static int pgfuse_releasedir( const char *path, struct fuse_file_info *fi )
{
	/* nothing to do, everything is done in pgfuse_readdir currently */
	return 0;
}

static int pgfuse_fsyncdir( const char *path, int datasync, struct fuse_file_info *fi )
{
	/* nothing to do, everything is done in pgfuse_readdir currently */
	return 0;
}

static int pgfuse_mkdir( const char *path, mode_t mode )
{
	PgFuseData *data = (PgFuseData *)fuse_get_context( )->private_data;
	char *copy_path;
	char *parent_path;
	char *new_dir;
	int64_t parent_id;
	int res;
	PgMeta meta;
	PGconn *conn;

	if( data->verbose ) {
		syslog( LOG_INFO, "Mkdir '%s' in mode '%o' on '%s', thread #%u",
			path, (unsigned int)mode, data->mountpoint,
			THREAD_ID );
	}

	ACQUIRE( conn );
	PSQL_BEGIN( conn );
	
	if( data->read_only ) {
		PSQL_ROLLBACK( conn ); RELEASE( conn );
		return -EROFS;
	}
	
	copy_path = strdup( path );
	if( copy_path == NULL ) {
		syslog( LOG_ERR, "Out of memory in Mkdir '%s'!", path );
		PSQL_ROLLBACK( conn ); RELEASE( conn );
		return -ENOMEM;
	}
	
	parent_path = dirname( copy_path );

	parent_id = psql_read_meta_from_path( conn, parent_path, &meta );
	if( parent_id < 0 ) {
		PSQL_ROLLBACK( conn ); RELEASE( conn );
		return parent_id;
	}
	if( !S_ISDIR( meta.mode ) ) {
		PSQL_ROLLBACK( conn ); RELEASE( conn );
		return -ENOENT;
	}
	
	if( data->verbose ) {
		syslog( LOG_DEBUG, "Parent_id for new dir '%s' is %"PRIi64", thread #%u",
			path, parent_id, THREAD_ID );
	}
	
	free( copy_path );
	copy_path = strdup( path );
	if( copy_path == NULL ) {
		free( parent_path );
		syslog( LOG_ERR, "Out of memory in Mkdir '%s'!", path );
		PSQL_ROLLBACK( conn ); RELEASE( conn );
		return -ENOMEM;
	}
	
	new_dir = basename( copy_path );

	meta.size = 0;
	meta.mode = mode | S_IFDIR; /* S_IFDIR is not set by fuse */
	meta.uid = fuse_get_context( )->uid;
	meta.gid = fuse_get_context( )->gid;
	meta.ctime = now( );
	meta.mtime = meta.ctime;
	meta.atime = meta.ctime;
	
	res = psql_create_dir( conn, parent_id, path, new_dir, meta );
	if( res < 0 ) {
		free( copy_path );
		PSQL_ROLLBACK( conn ); RELEASE( conn );
		return res;
	}

	free( copy_path );

	PSQL_COMMIT( conn ); RELEASE( conn );
	
	return 0;
}

static int pgfuse_rmdir( const char *path )
{
	PgFuseData *data = (PgFuseData *)fuse_get_context( )->private_data;
	int64_t id;
	int res;
	PgMeta meta;
	PGconn *conn;

	if( data->verbose ) {
		syslog( LOG_INFO, "Rmdir '%s' on '%s', thread #%u",
			path, data->mountpoint, THREAD_ID );
	}

	ACQUIRE( conn );	
	PSQL_BEGIN( conn );
	
	id = psql_read_meta_from_path( conn, path, &meta );
	if( id < 0 ) {
		PSQL_ROLLBACK( conn ); RELEASE( conn );
		return id;
	}
	if( !S_ISDIR( meta.mode ) ) {
		PSQL_ROLLBACK( conn ); RELEASE( conn );
		return -ENOTDIR;
	}
	
	if( data->verbose ) {
		syslog( LOG_DEBUG, "Id of dir '%s' to be removed is %"PRIi64", thread #%u",
			path, id, THREAD_ID );
	}

	if( data->read_only ) {
		PSQL_ROLLBACK( conn ); RELEASE( conn );
		return -EROFS;
	}
				
	res = psql_delete_dir( conn, id, path );
	if( res < 0 ) {
		PSQL_ROLLBACK( conn ); RELEASE( conn );
		return res;
	}
	
	PSQL_COMMIT( conn ); RELEASE( conn );
	
	return 0;
}

static int pgfuse_unlink( const char *path )
{
	PgFuseData *data = (PgFuseData *)fuse_get_context( )->private_data;
	int64_t id;
	int res;
	PgMeta meta;
	PGconn *conn;

	if( data->verbose ) {
		syslog( LOG_INFO, "Remove file '%s' on '%s', thread #%u",
			path, data->mountpoint, THREAD_ID );
	}
	
	ACQUIRE( conn );
	PSQL_BEGIN( conn );
	
	id = psql_read_meta_from_path( conn, path, &meta );
	if( id < 0 ) {
		PSQL_ROLLBACK( conn ); RELEASE( conn );
		return id;
	}
	if( S_ISDIR( meta.mode ) ) {
		PSQL_ROLLBACK( conn ); RELEASE( conn );
		return -EPERM;
	}
	
	if( data->verbose ) {
		syslog( LOG_DEBUG, "Id of file '%s' to be removed is %"PRIi64", thread #%u",
			path, id, THREAD_ID );
	}

	if( data->read_only ) {
		PSQL_ROLLBACK( conn ); RELEASE( conn );
		return -EROFS;
	}

	res = psql_delete_file( conn, id, path );
	if( res < 0 ) {
		PSQL_ROLLBACK( conn ); RELEASE( conn );
		return res;
	}
	
	PSQL_COMMIT( conn ); RELEASE( conn );
	
	return 0;
}

static int pgfuse_flush( const char *path, struct fuse_file_info *fi )
{
	/* nothing to do, data is always persistent in database */

	return 0;
}

static int pgfuse_fsync( const char *path, int isdatasync, struct fuse_file_info *fi )
{
	PgFuseData *data = (PgFuseData *)fuse_get_context( )->private_data;
	
	if( data->verbose ) {
		syslog( LOG_INFO, "%s on file '%s' on '%s', thread #%u",
			isdatasync ? "FDataSync" : "FSync", path, data->mountpoint,
			THREAD_ID );
	}

	if( data->read_only ) {
		return -EROFS;
	}

	if( fi->fh == 0 ) {
		return -EBADF;
	}
	
	/* nothing to do, data is always persistent in database */
	
	/* TODO: if we have a per transaction/file transaction policy, we must change this here! */
	
	return 0;
}

static int pgfuse_release( const char *path, struct fuse_file_info *fi )
{
	PgFuseData *data = (PgFuseData *)fuse_get_context( )->private_data;

	/* nothing to do given the simple transaction model */
	
	if( data->verbose ) {
		syslog( LOG_INFO, "Releasing '%s' on '%s', thread #%u",
			path, data->mountpoint, THREAD_ID );
	}

	return 0;
}

static int pgfuse_write( const char *path, const char *buf, size_t size,
                         off_t offset, struct fuse_file_info *fi )
{
	PgFuseData *data = (PgFuseData *)fuse_get_context( )->private_data;
	int64_t tmp;
	int res;
	PgMeta meta;
	PGconn *conn;

	if( data->verbose ) {
		syslog( LOG_INFO, "Write to '%s' from offset %jd, size %zu on '%s', thread #%u",
			path, offset, size, data->mountpoint,
			THREAD_ID );
	}

	ACQUIRE( conn );
	PSQL_BEGIN( conn );
	
	if( fi->fh == 0 ) {
		PSQL_ROLLBACK( conn ); RELEASE( conn );
		return -EBADF;
	}

	if( data->read_only ) {
		PSQL_ROLLBACK( conn ); RELEASE( conn );
		return -EBADF;
	}
		
	tmp = psql_read_meta( conn, fi->fh, path, &meta );
	if( tmp < 0 ) {
		PSQL_ROLLBACK( conn ); RELEASE( conn );
		return tmp;
	}
	
	if( offset + size > meta.size ) {
		meta.size = offset + size;
	}
	
	res = psql_write_buf( conn, data->block_size, fi->fh, path, buf, offset, size, data->verbose );
	if( res < 0 ) {
		PSQL_ROLLBACK( conn ); RELEASE( conn );
		return res;
	}
	if( res != size ) {
		syslog( LOG_ERR, "Write size mismatch in file '%s' on mountpoint '%s', expected '%d' to be written, but actually wrote '%d' bytes! Data inconistency!",
			path, data->mountpoint, (unsigned int)size, res );
		PSQL_ROLLBACK( conn ); RELEASE( conn );
		return -EIO;
	}
	
	res = psql_write_meta( conn, fi->fh, path, meta );
	if( res < 0 ) {
		PSQL_ROLLBACK( conn ); RELEASE( conn );
		return res;
	}

	PSQL_COMMIT( conn ); RELEASE( conn );
	
	return size;
}

static int pgfuse_read( const char *path, char *buf, size_t size,
                        off_t offset, struct fuse_file_info *fi )
{
	PgFuseData *data = (PgFuseData *)fuse_get_context( )->private_data;
	int res;
	PGconn *conn;

	if( data->verbose ) {
		syslog( LOG_INFO, "Read to '%s' from offset %jd, size %zu on '%s', thread #%u",
			path, offset, size, data->mountpoint,
			THREAD_ID );
	}

	ACQUIRE( conn );
	PSQL_BEGIN( conn );

	if( fi->fh == 0 ) {
		PSQL_ROLLBACK( conn ); RELEASE( conn );
		return -EBADF;
	}

	res = psql_read_buf( conn, data->block_size, fi->fh, path, buf, offset, size, data->verbose );
	if( res < 0 ) {
		PSQL_ROLLBACK( conn ); RELEASE( conn );
		return res;
	}

	PSQL_COMMIT( conn ); RELEASE( conn );
		
	return res;
}

static int pgfuse_truncate( const char* path, off_t offset )
{
	PgFuseData *data = (PgFuseData *)fuse_get_context( )->private_data;
	int64_t id;
	PgMeta meta;
	int res;
	PGconn *conn;

	if( data->verbose ) {
		syslog( LOG_INFO, "Truncate of '%s' to size '%jd' on '%s', thread #%u",
			path, offset, data->mountpoint, THREAD_ID );
	}

	ACQUIRE( conn );
	PSQL_BEGIN( conn );

	id = psql_read_meta_from_path( conn, path, &meta );
	if( id < 0 ) {
		PSQL_ROLLBACK( conn ); RELEASE( conn );
		return id;
	}
	
	if( S_ISDIR( meta.mode ) ) {
		PSQL_ROLLBACK( conn ); RELEASE( conn );
		return -EISDIR;
	}
	
	if( data->verbose ) {
		syslog( LOG_DEBUG, "Id of file '%s' to be truncated is %"PRIi64", thread #%u",
			path, id, THREAD_ID );
	}

	if( data->read_only ) {
		PSQL_ROLLBACK( conn ); RELEASE( conn );
		return -EROFS;
	}

	res = psql_truncate( conn, data->block_size, id, path, offset );
	if( res < 0 ) {
		PSQL_ROLLBACK( conn ); RELEASE( conn );
		return res;
	}
	
	meta.size = offset;
	res = psql_write_meta( conn, id, path, meta );
	if( res < 0 ) {
		PSQL_ROLLBACK( conn ); RELEASE( conn );
		return res;
	}
	
	PSQL_COMMIT( conn ); RELEASE( conn );
	
	return 0;
}

static int pgfuse_ftruncate( const char *path, off_t offset, struct fuse_file_info *fi )
{
	PgFuseData *data = (PgFuseData *)fuse_get_context( )->private_data;
	int64_t id;
	int res;
	PgMeta meta;
	PGconn *conn;

	if( data->verbose ) {
		syslog( LOG_INFO, "Truncate of '%s' to size '%jd' on '%s', thread #%u",
			path, offset, data->mountpoint,
			THREAD_ID );
	}

	ACQUIRE( conn );
	PSQL_BEGIN( conn );

	if( fi->fh == 0 ) {
		PSQL_ROLLBACK( conn ); RELEASE( conn );
		return -EBADF;
	}
	
	id = psql_read_meta( conn, fi->fh, path, &meta );
	if( id < 0 ) {
		PSQL_ROLLBACK( conn ); RELEASE( conn );
		return id;
	}

	if( data->read_only ) {
		PSQL_ROLLBACK( conn ); RELEASE( conn );
		return -EROFS;
	}
	
	res = psql_truncate( conn, data->block_size, fi->fh, path, offset );
	if( res < 0 ) {
		PSQL_ROLLBACK( conn ); RELEASE( conn );
		return res;
	}
	
	meta.size = offset;
	
	res = psql_write_meta( conn, fi->fh, path, meta );
	if( res < 0 ) {
		PSQL_ROLLBACK( conn ); RELEASE( conn );
		return res;
	}

	PSQL_COMMIT( conn ); RELEASE( conn );
	
	return 0;
}

static int pgfuse_statfs( const char *path, struct statvfs *buf )
{
	PgFuseData *data = (PgFuseData *)fuse_get_context( )->private_data;
	PGconn *conn;
	int64_t blocks_total, blocks_used, blocks_free, blocks_avail;
	int64_t files_total, files_used, files_free, files_avail;
	int res;
	int i;
	size_t nof_locations = MAX_TABLESPACE_OIDS;
	char *location[MAX_TABLESPACE_OIDS];
	FILE *mtab;
	struct mntent *m;
	struct mntent mnt;
	char strings[MTAB_BUFFER_SIZE];
	char *prefix;
	int prefix_len;

	if( data->verbose ) {
		syslog( LOG_INFO, "Statfs called on '%s', thread #%u",
			data->mountpoint, THREAD_ID );
	}
		
	memset( buf, 0, sizeof( struct statvfs ) );
	
	ACQUIRE( conn );
        PSQL_BEGIN( conn );

	/* blocks */

	res = psql_get_tablespace_locations( conn, location, &nof_locations, data->verbose );
	if( res < 0 ) {
		return res;
	}

	/* transform them and especially resolve symlinks */
	for( i = 0; i < nof_locations; i++ ) {
		char *old_path = location[i];
		char *new_path = realpath( old_path, NULL );
		if( new_path == NULL ) {
			/* do nothing, most likely a permission problem */
			syslog( LOG_ERR, "realpath for '%s' failed: %s,  pgfuse mount point '%s', thread #%u",
				old_path, strerror( errno ), data->mountpoint, THREAD_ID );
		} else {
			location[i] = new_path;
			free( old_path );
		}
	}
	
	blocks_free = INT64_MAX;
	blocks_avail = INT64_MAX;
	
	/* iterate over mount entries and try to match to the tablespace locations */
	mtab = setmntent( MTAB_FILE, "r" );
	while( ( m = getmntent_r( mtab, &mnt, strings, sizeof( strings ) ) ) != NULL ) {
		struct statfs fs;
		
		/* skip filesystems without mount point */
		if( mnt.mnt_dir == NULL ) continue;
		
		/* skip filesystems which are not a prefix of one of the tablespace locations */
		prefix = NULL;
		prefix_len = 0;
		for( i = 0; i < nof_locations; i++ ) {
			if( strncmp( mnt.mnt_dir, location[i], strlen( mnt.mnt_dir ) ) == 0 ) {
				if( strlen( mnt.mnt_dir ) > prefix_len ) {
					prefix_len = strlen( mnt.mnt_dir );
					prefix = strdup( mnt.mnt_dir );
					blocks_free = INT64_MAX;
					blocks_avail = INT64_MAX;
				}
			}
		}
		if( prefix == NULL ) continue;
		
		/* get data of file system */
		res = statfs( prefix, &fs );
		if( res < 0 ) {
			syslog( LOG_ERR, "statfs on '%s' failed: %s,  pgfuse mount point '%s', thread #%u",
				prefix,	strerror( errno ), data->mountpoint, THREAD_ID );
			return res;
		}

		if( data->verbose ) {
			syslog( LOG_DEBUG, "Checking mount point '%s' for free disk space, now %jd, was %jd, pgfuse mount point '%s', thread #%u",
				prefix,	fs.f_bfree, blocks_free, data->mountpoint, THREAD_ID );
		}

		/* take the smallest available disk space free (worst case the first one
		 * to overflow one of the tablespaces)
		 */
		if( fs.f_bfree * fs.f_frsize < blocks_free * data->block_size ) {
			blocks_free = fs.f_bfree * fs.f_frsize / data->block_size;
		}
		if( fs.f_bavail * fs.f_frsize < blocks_avail * data->block_size ) {
			blocks_avail = fs.f_bavail * fs.f_frsize / data->block_size;
		}
				
		if( prefix ) free( prefix );
	}
	endmntent( mtab );
	
	for( i = 0; i < nof_locations; i++ ) {
		if( location[i] ) free( location[i] );
	}
			
	blocks_used = psql_get_fs_blocks_used( conn );	
	if( blocks_used < 0 ) {
                PSQL_ROLLBACK( conn ); RELEASE( conn );
		return blocks_used;
	}
            
	blocks_total = blocks_avail + blocks_used;
	blocks_free = blocks_avail;
	
	/* inodes */

	/* no restriction on the number of files storable, we could
	   add some limits later */
	files_free = INT64_MAX;
	
	files_used = psql_get_fs_files_used( conn );
	if( files_used < 0 ) {
                PSQL_ROLLBACK( conn ); RELEASE( conn );
		return files_used;
	}
	
	files_total = files_free + files_used;
	files_avail = files_free;

	if( data->verbose ) {
		syslog( LOG_DEBUG, "Stats for '%s' are (%jd blocks total, %jd used, %jd free, "
			"%jd files total, %jd files used, %jd files free, thread #%u",
			data->mountpoint, 
			blocks_total, blocks_used, blocks_free,
			files_total, files_used, files_free,
			THREAD_ID );
	}
	
	/* fill statfs structure */
	
	/* Note: blocks have to be retrning as units of f_frsize
	 * f_favail, f_fsid and f_flag are currently ignored by FUSE ? */
	buf->f_bsize = data->block_size;
	buf->f_frsize = data->block_size;
	buf->f_blocks = blocks_total;
	buf->f_bfree = blocks_free;
	buf->f_bavail = blocks_avail;
	buf->f_files = files_total;
	buf->f_ffree = files_free;
	buf->f_favail = files_avail;
	buf->f_fsid =  0x4FE3A364;
	if( data->read_only ) {
		buf->f_flag |= ST_RDONLY;
	}
	buf->f_namemax = MAX_FILENAME_LENGTH;

	PSQL_COMMIT( conn ); RELEASE( conn );
	
	return 0;
}

static int pgfuse_chmod( const char *path, mode_t mode )
{
	PgFuseData *data = (PgFuseData *)fuse_get_context( )->private_data;
	int64_t id;
	PgMeta meta;	
	int res;
	PGconn *conn;

	if( data->verbose ) {
		syslog( LOG_INFO, "Chmod on '%s' to mode '%o' on '%s', thread #%u",
			path, (unsigned int)mode, data->mountpoint,
			THREAD_ID );
	}

	ACQUIRE( conn );
	PSQL_BEGIN( conn );
	
	id = psql_read_meta_from_path( conn, path, &meta );
	if( id < 0 ) {
		PSQL_ROLLBACK( conn ); RELEASE( conn );
		return id;
	}

	if( data->read_only ) {
		PSQL_ROLLBACK( conn ); RELEASE( conn );
		return -EROFS;
	}
		
	meta.mode = mode;
	
	res = psql_write_meta( conn, id, path, meta );
	if( res < 0 ) {
		PSQL_ROLLBACK( conn ); RELEASE( conn );
		return res;
	}

	PSQL_COMMIT( conn ); RELEASE( conn );

	return 0;
}

static int pgfuse_chown( const char *path, uid_t uid, gid_t gid )
{
	PgFuseData *data = (PgFuseData *)fuse_get_context( )->private_data;
	int64_t id;
	PgMeta meta;	
	int res;
	PGconn *conn;

	if( data->verbose ) {
		syslog( LOG_INFO, "Chown on '%s' to uid '%d' and gid '%d' on '%s', thread #%u",
			path, (unsigned int)uid, (unsigned int)gid, data->mountpoint,
			THREAD_ID );
	}
	
	ACQUIRE( conn );	
	PSQL_BEGIN( conn );
	
	id = psql_read_meta_from_path( conn, path, &meta );
	if( id < 0 ) {
		PSQL_ROLLBACK( conn ); RELEASE( conn );
		return id;
	}

	if( data->read_only ) {
		PSQL_ROLLBACK( conn ); RELEASE( conn );
		return -EROFS;
	}
	
	meta.uid = uid;
	meta.gid = gid;
	
	res = psql_write_meta( conn, id, path, meta );
	if( res < 0 ) {
		PSQL_ROLLBACK( conn ); RELEASE( conn );
		return res;
	}
	
	PSQL_COMMIT( conn ); RELEASE( conn );
	
	return res;
}

static int pgfuse_symlink( const char *from, const char *to )
{
	PgFuseData *data = (PgFuseData *)fuse_get_context( )->private_data;
	char *copy_to;
	char *parent_path;
	char *symlink;
	int64_t parent_id;
	int res;
	int64_t id;
	PgMeta meta;
	PGconn *conn;

	if( data->verbose ) {
		syslog( LOG_INFO, "Symlink from '%s' to '%s' on '%s', thread #%u",
			from, to, data->mountpoint, THREAD_ID );
	}

	ACQUIRE( conn );
	PSQL_BEGIN( conn );
	
	copy_to = strdup( to );
	if( copy_to == NULL ) {
		syslog( LOG_ERR, "Out of memory in Symlink '%s'!", to );
		PSQL_ROLLBACK( conn ); RELEASE( conn );
		return -ENOMEM;
	}
	
	parent_path = dirname( copy_to );

	parent_id = psql_read_meta_from_path( conn, parent_path, &meta );
	if( parent_id < 0 ) {
		PSQL_ROLLBACK( conn ); RELEASE( conn );
		return parent_id;
	}
	if( !S_ISDIR( meta.mode ) ) {
		PSQL_ROLLBACK( conn ); RELEASE( conn );
		return -ENOENT;
	}
	
	if( data->verbose ) {
		syslog( LOG_DEBUG, "Parent_id for symlink '%s' is %"PRIi64", thread #%u",
			to, parent_id, THREAD_ID );
	}
	
	free( copy_to );
	copy_to = strdup( to );
	if( copy_to == NULL ) {
		syslog( LOG_ERR, "Out of memory in Symlink '%s'!", to );
		PSQL_ROLLBACK( conn ); RELEASE( conn );
		return -ENOMEM;
	}

	if( data->read_only ) {
		free( copy_to );
		PSQL_ROLLBACK( conn ); RELEASE( conn );
		return -EROFS;
	}
		
	symlink = basename( copy_to );

	meta.size = strlen( from );	/* size = length of path */
	meta.mode = 0777 | S_IFLNK; 	/* symlinks have no modes per se */
	/* TODO: use FUSE context */
	meta.uid = fuse_get_context( )->uid;
	meta.gid = fuse_get_context( )->gid;
	meta.ctime = now( );
	meta.mtime = meta.ctime;
	meta.atime = meta.ctime;
	
	res = psql_create_file( conn, parent_id, to, symlink, meta );
	if( res < 0 ) {
		free( copy_to );
		PSQL_ROLLBACK( conn ); RELEASE( conn );
		return res;
	}
	
	id = psql_read_meta_from_path( conn, to, &meta );
	if( id < 0 ) {
		free( copy_to );
		PSQL_ROLLBACK( conn ); RELEASE( conn );
		return id;
	}

	res = psql_write_buf( conn, data->block_size, id, to, from, 0, strlen( from ), data->verbose );
	if( res < 0 ) {
		free( copy_to );
		PSQL_ROLLBACK( conn ); RELEASE( conn );
		return res;
	}
	
	if( res != strlen( from ) ) {
		free( copy_to );
		PSQL_ROLLBACK( conn ); RELEASE( conn );
		return -EIO;
	}

	free( copy_to );
	
	PSQL_COMMIT( conn ); RELEASE( conn );
	
	return 0;
}

static int pgfuse_rename( const char *from, const char *to )
{
	PgFuseData *data = (PgFuseData *)fuse_get_context( )->private_data;
	PGconn *conn;
	int res;
	int64_t from_id;
	int64_t to_id;
	PgMeta from_meta;
	PgMeta to_meta;
	char *copy_to;
	char *parent_path;
	int64_t to_parent_id;
	PgMeta to_parent_meta;
	char *rename_to;
	
	if( data->verbose ) {
		syslog( LOG_INFO, "Renaming '%s' to '%s' on '%s', thread #%u",
			from, to, data->mountpoint, THREAD_ID );
	}

	ACQUIRE( conn );	
	PSQL_BEGIN( conn );
		
	from_id = psql_read_meta_from_path( conn, from, &from_meta );
	if( from_id < 0 ) {
		return from_id;
	}
		
	to_id = psql_read_meta_from_path( conn, to, &to_meta );
	if( to_id < 0 && to_id != -ENOENT ) {
		return to_id;
	}
	
	/* destination already exists */
	if( to_id > 0 ) {
		/* destination is a file */
		if( S_ISREG( to_meta.mode ) ) {
			if( strcmp( from, to ) == 0 ) {
				/* source equal to destination? This should succeed */
				return 0;
			} else {
				/* otherwise bail out */
				return -EEXIST;
			}
		}
		/* TODO: handle all other cases */
		return -EINVAL;
	}
	
	copy_to = strdup( to );
	if( copy_to == NULL ) {
		syslog( LOG_ERR, "Out of memory in Rename '%s'!", to );
		PSQL_ROLLBACK( conn ); RELEASE( conn );
		return -ENOMEM;
	}
	
	parent_path = dirname( copy_to );

	to_parent_id = psql_read_meta_from_path( conn, parent_path, &to_parent_meta );
	if( to_parent_id < 0 ) {
		free( copy_to );
		PSQL_ROLLBACK( conn ); RELEASE( conn );
		return to_parent_id;
	}
	
	if( !S_ISDIR( to_parent_meta.mode ) ) {
		syslog( LOG_ERR, "Weird situation in Rename, '%s' expected to be a directory!",
			parent_path );
		free( copy_to );
		PSQL_ROLLBACK( conn ); RELEASE( conn );
		return -EIO;
	}
	
	free( copy_to );
	copy_to = strdup( to );
	if( copy_to == NULL ) {
		syslog( LOG_ERR, "Out of memory in Rename '%s'!", to );
		PSQL_ROLLBACK( conn ); RELEASE( conn );
		return -ENOMEM;
	}

	if( data->read_only ) {
		free( copy_to );
		PSQL_ROLLBACK( conn ); RELEASE( conn );
		return -EROFS;
	}
		
	rename_to = basename( copy_to );
		
	res = psql_rename( conn, from_id, from_meta.parent_id, to_parent_id, rename_to, from, to );
	
	free( copy_to );
	
	PSQL_COMMIT( conn ); RELEASE( conn );

	return res;
}

static int pgfuse_readlink( const char *path, char *buf, size_t size )
{
	PgFuseData *data = (PgFuseData *)fuse_get_context( )->private_data;
	int64_t id;
	PgMeta meta;
	int res;
	PGconn *conn;

	if( data->verbose ) {
		syslog( LOG_INFO, "Dereferencing symlink '%s' on '%s', thread #%u",
			path, data->mountpoint, THREAD_ID );
	}
	
	ACQUIRE( conn );	
	PSQL_BEGIN( conn );

	id = psql_read_meta_from_path( conn, path, &meta );
	if( id < 0 ) {
		PSQL_ROLLBACK( conn ); RELEASE( conn );
		return id;
	}
	if( !S_ISLNK( meta.mode ) ) {
		PSQL_ROLLBACK( conn ); RELEASE( conn );
		return -ENOENT;
	}
	
	if( size < meta.size + 1 ) {
		PSQL_ROLLBACK( conn ); RELEASE( conn );
		return -ENOMEM;
	}
	
	res = psql_read_buf( conn, data->block_size, id, path, buf, 0, meta.size, data->verbose );
	if( res < 0 ) {
		PSQL_ROLLBACK( conn ); RELEASE( conn );
		return res;
	}
	
	buf[meta.size] = '\0';

	PSQL_COMMIT( conn ); RELEASE( conn );
	
	return 0;
}

static int pgfuse_utimens( const char *path, const struct timespec tv[2] )
{
	PgFuseData *data = (PgFuseData *)fuse_get_context( )->private_data;
	int64_t id;
	PgMeta meta;	
	int res;
	PGconn *conn;

	if( data->verbose ) {
		syslog( LOG_INFO, "Utimens on '%s' to access time '%d' and modification time '%d' on '%s', thread #%u",
			path, (unsigned int)tv[0].tv_sec, (unsigned int)tv[1].tv_sec, data->mountpoint,
			THREAD_ID );
	}
	
	ACQUIRE( conn );
	PSQL_BEGIN( conn );
	
	id = psql_read_meta_from_path( conn, path, &meta );
	if( id < 0 ) {
		PSQL_ROLLBACK( conn ); RELEASE( conn );
		return id;
	}
		
	meta.atime = tv[0];
	meta.mtime = tv[1];
	
	res = psql_write_meta( conn, id, path, meta );
	if( res < 0 ) {
		PSQL_ROLLBACK( conn ); RELEASE( conn );
		return res;
	}
	
	PSQL_COMMIT( conn ); RELEASE( conn );
	
	return 0;
}


static struct fuse_operations pgfuse_oper = {
	.getattr	= pgfuse_getattr,
	.readlink	= pgfuse_readlink,
	.mknod		= NULL,		/* not used, we use 'create' */
	.mkdir		= pgfuse_mkdir,
	.unlink		= pgfuse_unlink,
	.rmdir		= pgfuse_rmdir,
	.symlink	= pgfuse_symlink,
	.rename		= pgfuse_rename,
	.link		= NULL,
	.chmod		= pgfuse_chmod,
	.chown		= pgfuse_chown,
	.utime		= NULL,		/* deprecated in favour of 'utimes' */
	.open		= pgfuse_open,
	.read		= pgfuse_read,
	.write		= pgfuse_write,
	.statfs		= pgfuse_statfs,
	.flush		= pgfuse_flush,
	.release	= pgfuse_release,
	.fsync		= pgfuse_fsync,
	.setxattr	= NULL,
	.listxattr	= NULL,
	.removexattr	= NULL,
	.opendir	= pgfuse_opendir,
	.readdir	= pgfuse_readdir,
	.releasedir	= pgfuse_releasedir,
	.fsyncdir	= pgfuse_fsyncdir,
	.init		= pgfuse_init,
	.destroy	= pgfuse_destroy,
	.access		= pgfuse_access,
	.create		= pgfuse_create,
	.truncate	= pgfuse_truncate,
	.ftruncate	= pgfuse_ftruncate,
	.fgetattr	= pgfuse_fgetattr,
	.lock		= NULL,
	.utimens	= pgfuse_utimens,
	.bmap		= NULL,
#if FUSE_VERSION >= 28
	.ioctl		= NULL,
	.poll		= NULL
#endif
};

/* --- parse arguments --- */

typedef struct PgFuseOptions {
	int print_help;		/* whether we should print a help page */
	int print_version;	/* whether we should print the version */
	int verbose;		/* whether we should be verbose */
	char *conninfo;		/* connection info as used in PQconnectdb */
	char *mountpoint;	/* where we mount the virtual filesystem */
	int read_only;		/* whether to mount read-only */
	int multi_threaded;	/* whether we run multi-threaded */
	size_t block_size;	/* block size to use to store data in BYTEA fields */
} PgFuseOptions;

#define PGFUSE_OPT( t, p, v ) { t, offsetof( PgFuseOptions, p ), v }

enum {
	KEY_HELP,
	KEY_VERBOSE,
	KEY_VERSION
};

static struct fuse_opt pgfuse_opts[] = {
	PGFUSE_OPT( 	"ro",		read_only, 1 ),
	PGFUSE_OPT(     "blocksize=%d",	block_size, DEFAULT_BLOCK_SIZE ),
	FUSE_OPT_KEY( 	"-h",		KEY_HELP ),
	FUSE_OPT_KEY( 	"--help",	KEY_HELP ),
	FUSE_OPT_KEY( 	"-v",		KEY_VERBOSE ),
	FUSE_OPT_KEY( 	"--verbose",	KEY_VERBOSE ),
	FUSE_OPT_KEY( 	"-V",		KEY_VERSION ),
	FUSE_OPT_KEY( 	"--version",	KEY_VERSION ),
	FUSE_OPT_END
};

static int pgfuse_opt_proc( void* data, const char* arg, int key,
                            struct fuse_args* outargs )
{
	PgFuseOptions *pgfuse = (PgFuseOptions *)data;

	switch( key ) {
		case FUSE_OPT_KEY_OPT:
			if( strcmp( arg, "-s" ) == 0 ) {
				pgfuse->multi_threaded = 0;
			}
			return 1;
		
		case FUSE_OPT_KEY_NONOPT:
			if( pgfuse->conninfo == NULL ) {
				pgfuse->conninfo = strdup( arg );
				return 0;
			} else if( pgfuse->mountpoint == NULL ) {
				pgfuse->mountpoint = strdup( arg );
				return 1;
			} else {
				fprintf( stderr, "%s, only two arguments allowed: Postgresql connection data and mountpoint\n", basename( outargs->argv[0] ) );
				return -1;
			}
			
		case KEY_HELP:
			pgfuse->print_help = 1;
			return -1;
		
		case KEY_VERBOSE:
			pgfuse->verbose = 1;
			return 0;
			
		case KEY_VERSION:
			pgfuse->print_version = 1;
			return -1;
		
		default:
			return -1;
	}
}
	
static void print_usage( char* progname )
{
	printf(
		"Usage: %s <Postgresql Connection String> <mountpoint>\n"
		"\n"
		"Postgresql Connection String (key=value separated with whitespaces) :\n"
		"\n"
		"    host                   optional (ommit for Unix domain sockets), e.g. 'localhost'\n"
		"    port                   default is 5432\n"
		"    dbname                 database to connect to\n"
		"    user                   database user to connect with\n"
		"    password               for password credentials (or rather use ~/.pgpass)\n"
		"    ...\n"
		"    for more options see libpq, PQconnectdb\n"
		"\n"
		"Example: \"dbname=test user=test password=xx\"\n"
		"\n"
		"Options:\n"
		"    -o opt,[opt...]        pgfuse options\n"
		"    -v   --verbose         make libcurl print verbose debug\n"
		"    -h   --help            print help\n"
		"    -V   --version         print version\n"
		"\n"
		"PgFuse options:\n"
		"    ro                     mount filesystem read-only, do not change data in database\n"
		"    blocksize=<bytes>      block size to use for storage of data\n"
		"\n",
		progname
	);
}
		
/* --- main --- */

int main( int argc, char *argv[] )
{		
	int res;
	PGconn *conn;
	struct fuse_args args = FUSE_ARGS_INIT( argc, argv );
	PgFuseOptions pgfuse;
	PgFuseData userdata;
	const char *value;
	
	memset( &pgfuse, 0, sizeof( pgfuse ) );
	pgfuse.multi_threaded = 1;
	pgfuse.block_size = DEFAULT_BLOCK_SIZE;
	
	if( fuse_opt_parse( &args, &pgfuse, pgfuse_opts, pgfuse_opt_proc ) == -1 ) {
		if( pgfuse.print_help ) {
			/* print our options */
			print_usage( basename( argv[0] ) );
			fflush( stdout );
			/* print options of FUSE itself */
			argv[1] = "-ho";
			argv[2] = "mountpoint";
			(void)dup2( STDOUT_FILENO, STDERR_FILENO ); /* force fuse help to stdout */
			fuse_main( 2, argv, &pgfuse_oper, NULL);
			exit( EXIT_SUCCESS );
		}
		if( pgfuse.print_version ) {
			printf( "%s\n", PGFUSE_VERSION );
			exit( EXIT_SUCCESS );
		}
		exit( EXIT_FAILURE );
	}
		
	if( pgfuse.conninfo == NULL ) {
		fprintf( stderr, "Missing Postgresql connection data\n" );
		fprintf( stderr, "See '%s -h' for usage\n", basename( argv[0] ) );
		exit( EXIT_FAILURE );
	}
		
	/* just test if the connection can be established, do the
	 * real connection in the fuse init function!
	 */
	conn = PQconnectdb( pgfuse.conninfo );
	if( PQstatus( conn ) != CONNECTION_OK ) {
		fprintf( stderr, "Connection to database failed: %s",
			PQerrorMessage( conn ) );
		PQfinish( conn );
		exit( EXIT_FAILURE );
	}

	/* test storage of timestamps (expecting uint64 as it is the
	 * standard for PostgreSQL 8.4 or newer). Otherwise bail out
	 * currently..
	 */

	value = PQparameterStatus( conn, "integer_datetimes" );
	if( value == NULL ) {
		fprintf( stderr, "PQ param integer_datetimes not available?\n"
		         "You use a too old version of PostgreSQL..can't continue.\n" );
		PQfinish( conn );
		return 1;
	}
	
	if( strcmp( value, "on" ) != 0 ) {
		fprintf( stderr, "Expecting UINT64 for timestamps, not doubles. You may use an old version of PostgreSQL (<8.4)\n"
		         "or PostgreSQL has been compiled with the deprecated compile option '--disable-integer-datetimes'\n" );
		PQfinish( conn );
		return 1;
	}

	openlog( basename( argv[0] ), LOG_PID, LOG_USER );	
		
	/* Compare blocksize given as parameter and blocksize in database */
	res = psql_get_block_size( conn, pgfuse.block_size );
	if( res < 0 ) {
		PQfinish( conn );
		return 1;
	}
	if( res != pgfuse.block_size ) {
		fprintf( stderr, "Blocksize parameter mismatch (is '%zu', in database we have '%zu') taking the later one!\n",
			pgfuse.block_size, (size_t)res );
		PQfinish( conn );
		return 1;
	}
	
	PQfinish( conn );
	
	memset( &userdata, 0, sizeof( PgFuseData ) );
	userdata.conninfo = pgfuse.conninfo;
	userdata.mountpoint = pgfuse.mountpoint;
	userdata.verbose = pgfuse.verbose;
	userdata.read_only = pgfuse.read_only;
	userdata.multi_threaded = pgfuse.multi_threaded;
	userdata.block_size = pgfuse.block_size;
	
	res = fuse_main( args.argc, args.argv, &pgfuse_oper, &userdata );
	
	closelog( );
	
	exit( res );
}
