/**
 * Compile with
 *
 * gcc -Wall hypervfs.c `pkg-config fuse3 --cflags --libs` -o hypervfs
 *
 */

#define FUSE_USE_VERSION 31
#define _GNU_SOURCE

#include <fuse.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <dirent.h>
#include <errno.h>
#include <sys/time.h>

#include "passthrough_helpers.h"

#include <sys/socket.h>
#include <linux/vm_sockets.h>


#define PORT_NUM 5001
#define BUFF_SIZE 512

typedef uint64_t uint64;
typedef uint32_t uint32;

typedef struct
{
	uint32 type;
	uint32 mode;
	uint32 nlink;
	uint32 uid;
	uint32 gid;
	uint64 size;
	uint64 used;
	uint64 fsid;
	uint64 fileid;
	uint32 atime;
	uint32 mtime;
	uint32 ctime;
} HyperVStat;

enum
{
	HYPERV_OK = 0,
	HYPERV_NOENT = 1,

	// op codes
	HYPERV_ATTR = 10,
	HYPERV_READDIR = 20
};

int sServer = -1;

struct timespec toTimeSpec(uint32 sec)
{
	struct timespec time = {
		.tv_sec = sec,
		.tv_nsec = 0
	};

	return time;
}

void opConnect()
{

	sServer = socket(AF_VSOCK, SOCK_STREAM, 0);
	if (sServer < 0) {
		printf("socket error: %s\n", strerror(errno));
	}

	struct sockaddr_vm addr = { 0 };
	addr.svm_family = AF_VSOCK;
	addr.svm_port = PORT_NUM;
	addr.svm_cid = VMADDR_CID_HOST;

	int ret = connect(sServer, (struct sockaddr*)&addr, sizeof addr);

	if (ret < 0) {
		printf("connect error: %s\n", strerror(errno));
	}
}

int opReadAttr(const char* path, char** outBuffer)
{
	short opCode = HYPERV_ATTR;
	short pathLength = strlen(path) + 1;
	uint64 size = sizeof(uint64) + sizeof(short) + sizeof(short) + pathLength;
	*outBuffer = (char*)malloc(size);

	memcpy(*outBuffer, &size, sizeof(uint64));
	memcpy(*outBuffer + sizeof(uint64), &opCode, sizeof(short));
	memcpy(*outBuffer + sizeof(uint64) + sizeof(short), &pathLength, sizeof(short));
	memcpy(*outBuffer + sizeof(uint64) + sizeof(short) + sizeof(short), path, pathLength);

	return size;
}

int readMessage(int socket, char** buffer)
{
	uint64 size = 0;
	char sizeBuffer[sizeof(uint64)];

	int ret = recv(socket, sizeBuffer, sizeof(uint64), 0);

	// if we read 0 bytes, connection might be closed, return
	if (ret <= 0) {
		return 0;
	}

	memcpy(&size, &sizeBuffer, sizeof(uint64));
	printf("Got message of size: %l\n", size);

	*buffer = (char*)malloc(size);
	memcpy(*buffer, sizeBuffer, sizeof(uint64));
	ret = recv(socket, *buffer + sizeof(uint64), size - sizeof(uint64), 0);

	// if we read 0 bytes, connection might be closed, return
	if (ret <= 0) {
		free(buffer);
		return 0;
	}

	printf("Message: %s\n", buffer);

	return size;
}

int sendMessage(int socket, char* buffer)
{
	uint64* size = (uint64*)buffer;

	return send(socket, buffer, *size, 0);
}



static void* xmp_init(struct fuse_conn_info* conn,
	struct fuse_config* cfg)
{
	printf("Function call [xmp_init]\n");
	(void)conn;
	cfg->use_ino = 1;

	/* Pick up changes from lower filesystem right away. This is
	   also necessary for better hardlink support. When the kernel
	   calls the unlink() handler, it does not know the inode of
	   the to-be-removed entry and can therefore not invalidate
	   the cache of the associated inode - resulting in an
	   incorrect st_nlink value being reported for any remaining
	   hardlinks to this inode. */
	cfg->entry_timeout = 0;
	cfg->attr_timeout = 0;
	cfg->negative_timeout = 0;

	opConnect();

	return NULL;
}


static int xmp_getattr(const char* path, struct stat* stbuf,
	struct fuse_file_info* fi)
{
	printf("Function call [xmp_getattr] on path %s\n", path);
	(void)fi;

	int ret;
	char* inBuffer = NULL;
	char* outBuffer = NULL;

	ret = opReadAttr(path, &outBuffer);
	ret = sendMessage(sServer, outBuffer);
	free(outBuffer);

	ret = readMessage(sServer, &inBuffer);

	short* status = (short*)(inBuffer + sizeof(uint64));

	if (*status != HYPERV_OK) {
		free(inBuffer);
		// TODO: real error handling
		return -ENOENT;
	}

	HyperVStat* stat = (HyperVStat*)(inBuffer + sizeof(uint64) + sizeof(short));

	stbuf->st_ino = stat->fileid;
	stbuf->st_nlink = stat->nlink;
	stbuf->st_mode = S_IFDIR | 0755;
	stbuf->st_gid = getgid();
	stbuf->st_uid = getuid();
	stbuf->st_size = stat->size;
	stbuf->st_atim = toTimeSpec(stat->atime);
	stbuf->st_mtim = toTimeSpec(stat->mtime);
	stbuf->st_ctim = toTimeSpec(stat->ctime);

	free(inBuffer);

	return 0;
}

static int xmp_access(const char* path, int mask)
{
	printf("Function call [xmp_access] on path %s\n", path);
	// int res;

	// res = access(path, mask);
	// if (res == -1)
	// 	return -errno;

	return 0;
}

static int xmp_readlink(const char* path, char* buf, size_t size)
{
	printf("Function call [xmp_readlink] on path %s\n", path);
	int res;

	res = readlink(path, buf, size - 1);
	if (res == -1)
		return -errno;

	buf[res] = '\0';
	return 0;
}


static int xmp_readdir(const char* path, void* buf, fuse_fill_dir_t filler,
	off_t offset, struct fuse_file_info* fi,
	enum fuse_readdir_flags flags)
{
	printf("Function call [xmp_readdir] on path %s\n", path);
	DIR* dp;
	struct dirent* de;

	(void)offset;
	(void)fi;
	(void)flags;

	// send_vsock(path);
	// send_vsock("A very nice message, with a coma and a dot.");

	dp = opendir(path);
	if (dp == NULL)
		return -errno;

	while ((de = readdir(dp)) != NULL) {
		struct stat st;
		memset(&st, 0, sizeof(st));
		st.st_ino = de->d_ino;
		st.st_mode = de->d_type << 12;
		if (filler(buf, de->d_name, &st, 0, FUSE_FILL_DIR_PLUS))
			break;
	}

	closedir(dp);
	return 0;
}

static int xmp_mknod(const char* path, mode_t mode, dev_t rdev)
{
	printf("Function call [xmp_mknod] on path %s\n", path);
	int res;

	res = mknod_wrapper(AT_FDCWD, path, NULL, mode, rdev);
	if (res == -1)
		return -errno;

	return 0;
}

static int xmp_mkdir(const char* path, mode_t mode)
{
	printf("Function call [xmp_mkdir] on path %s\n", path);
	int res;

	res = mkdir(path, mode);
	if (res == -1)
		return -errno;

	return 0;
}

static int xmp_unlink(const char* path)
{
	printf("Function call [xmp_unlink] on path %s\n", path);
	int res;

	res = unlink(path);
	if (res == -1)
		return -errno;

	return 0;
}

static int xmp_rmdir(const char* path)
{
	printf("Function call [xmp_rmdir] on path %s\n", path);
	int res;

	res = rmdir(path);
	if (res == -1)
		return -errno;

	return 0;
}

static int xmp_symlink(const char* from, const char* to)
{
	printf("Function call [xmp_symlink] on path %s\n", from);
	int res;

	res = symlink(from, to);
	if (res == -1)
		return -errno;

	return 0;
}

static int xmp_rename(const char* from, const char* to, unsigned int flags)
{
	printf("Function call [xmp_rename] on path %s\n", from);
	int res;

	if (flags)
		return -EINVAL;

	res = rename(from, to);
	if (res == -1)
		return -errno;

	return 0;
}

static int xmp_link(const char* from, const char* to)
{
	printf("Function call [xmp_link] on path %s\n", from);
	int res;

	res = link(from, to);
	if (res == -1)
		return -errno;

	return 0;
}

static int xmp_chmod(const char* path, mode_t mode,
	struct fuse_file_info* fi)
{
	printf("Function call [xmp_chmod] on path %s\n", path);
	(void)fi;
	int res;

	res = chmod(path, mode);
	if (res == -1)
		return -errno;

	return 0;
}

static int xmp_chown(const char* path, uid_t uid, gid_t gid,
	struct fuse_file_info* fi)
{
	printf("Function call [xmp_chown] on path %s\n", path);
	(void)fi;
	int res;

	res = lchown(path, uid, gid);
	if (res == -1)
		return -errno;

	return 0;
}

static int xmp_truncate(const char* path, off_t size,
	struct fuse_file_info* fi)
{
	printf("Function call [xmp_truncate] on path %s\n", path);
	int res;

	if (fi != NULL)
		res = ftruncate(fi->fh, size);
	else
		res = truncate(path, size);
	if (res == -1)
		return -errno;

	return 0;
}

static int xmp_create(const char* path, mode_t mode,
	struct fuse_file_info* fi)
{
	printf("Function call [xmp_create] on path %s\n", path);
	int res;

	res = open(path, fi->flags, mode);
	if (res == -1)
		return -errno;

	fi->fh = res;
	return 0;
}

static int xmp_open(const char* path, struct fuse_file_info* fi)
{
	printf("Function call [xmp_open] on path %s\n", path);
	int res;

	res = open(path, fi->flags);
	if (res == -1)
		return -errno;

	fi->fh = res;
	return 0;
}

static int xmp_read(const char* path, char* buf, size_t size, off_t offset,
	struct fuse_file_info* fi)
{
	printf("Function call [xmp_read] on path %s\n", path);
	int fd;
	int res;

	if (fi == NULL)
		fd = open(path, O_RDONLY);
	else
		fd = fi->fh;

	if (fd == -1)
		return -errno;

	res = pread(fd, buf, size, offset);
	if (res == -1)
		res = -errno;

	if (fi == NULL)
		close(fd);
	return res;
}

static int xmp_write(const char* path, const char* buf, size_t size,
	off_t offset, struct fuse_file_info* fi)
{
	printf("Function call [xmp_write] on path %s\n", path);
	int fd;
	int res;

	(void)fi;
	if (fi == NULL)
		fd = open(path, O_WRONLY);
	else
		fd = fi->fh;

	if (fd == -1)
		return -errno;

	res = pwrite(fd, buf, size, offset);
	if (res == -1)
		res = -errno;

	if (fi == NULL)
		close(fd);
	return res;
}

static int xmp_statfs(const char* path, struct statvfs* stbuf)
{
	printf("Function call [xmp_statfs] on path %s\n", path);
	int res;

	res = statvfs(path, stbuf);
	if (res == -1)
		return -errno;

	return 0;
}

static int xmp_release(const char* path, struct fuse_file_info* fi)
{
	printf("Function call [xmp_release] on path %s\n", path);
	(void)path;
	close(fi->fh);
	return 0;
}

static int xmp_fsync(const char* path, int isdatasync,
	struct fuse_file_info* fi)
{
	printf("Function call [xmp_fsync] on path %s\n", path);
	/* Just a stub.	 This method is optional and can safely be left
	   unimplemented */

	(void)path;
	(void)isdatasync;
	(void)fi;
	return 0;
}

static off_t xmp_lseek(const char* path, off_t off, int whence, struct fuse_file_info* fi)
{
	printf("Function call [xmp_lseek] on path %s\n", path);
	int fd;
	off_t res;

	if (fi == NULL)
		fd = open(path, O_RDONLY);
	else
		fd = fi->fh;

	if (fd == -1)
		return -errno;

	res = lseek(fd, off, whence);
	if (res == -1)
		res = -errno;

	if (fi == NULL)
		close(fd);
	return res;
}

static const struct fuse_operations xmp_oper = {
	.init = xmp_init,
	.getattr = xmp_getattr,
	.access = xmp_access,
	.readlink = xmp_readlink,
	.readdir = xmp_readdir,
	.mknod = xmp_mknod,
	.mkdir = xmp_mkdir,
	.symlink = xmp_symlink,
	.unlink = xmp_unlink,
	.rmdir = xmp_rmdir,
	.rename = xmp_rename,
	.link = xmp_link,
	.chmod = xmp_chmod,
	.chown = xmp_chown,
	.truncate = xmp_truncate,
	.open = xmp_open,
	.create = xmp_create,
	.read = xmp_read,
	.write = xmp_write,
	.statfs = xmp_statfs,
	.release = xmp_release,
	.fsync = xmp_fsync,
	.lseek = xmp_lseek,
};

int main(int argc, char* argv[])
{
	umask(0);
	return fuse_main(argc, argv, &xmp_oper, NULL);
}
