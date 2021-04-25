/**
 * Compile with
 *
 * gcc -Wall hypervfs.c `pkg-config fuse3 --cflags --libs` -o hypervfs
 *
 */

#define FUSE_USE_VERSION 34
#define _GNU_SOURCE

#define PORT_NUM 5001
#define SOCKET_NUM 5

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
#include <sys/socket.h>
#include <linux/vm_sockets.h>
#include <pthread.h>

typedef uint64_t uint64;
typedef uint32_t uint32;
typedef int64_t int64;

typedef struct
{
	uint64 fsid;
	uint64 fileid;
	uint64 size;
	uint64 used;
	uint32 type;
	uint32 mode;
	uint32 nlink;
	uint32 uid;
	uint32 gid;
	uint32 atime;
	uint32 mtime;
	uint32 ctime;
} HyperVStat;

enum
{
	HYPERV_OK = 0,
	HYPERV_NOENT = ENOENT,
	HYPERV_EXIST = EEXIST,

	// op codes
	HYPERV_ATTR = 10,
	HYPERV_READDIR = 20,
	HYPERV_READ = 30,
	HYPERV_CREATE = 40,
	HYPERV_WRITE = 50,
	HYPERV_UNLINK = 60,
	HYPERV_TRUNCATE = 70,
	HYPERV_MKDIR = 80,
	HYPERV_RMDIR = 90,
	HYPERV_RENAME = 100,
	HYPERV_SYMLINK = 110,
	HYPERV_LINK = 120,
	HYPERV_READLINK = 130
};

struct xmp_dirp {
	void* entry;
	uint64 readSize;
	int64 offset;
};

int sSockets[SOCKET_NUM] = { 0 };
pthread_mutex_t sSocketLock = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t sSocketCond = PTHREAD_COND_INITIALIZER;

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
	struct sockaddr_vm addr = { 0 };
	addr.svm_family = AF_VSOCK;
	addr.svm_port = PORT_NUM;
	addr.svm_cid = VMADDR_CID_HOST;

	for (int i = 0; i < SOCKET_NUM; i++) {
		sSockets[i] = socket(AF_VSOCK, SOCK_STREAM, 0);
		connect(sSockets[i], (struct sockaddr*)&addr, sizeof addr);
	}
}

char* opReadAttr(const char* path)
{
	short opCode = HYPERV_ATTR;
	short pathLength = strlen(path) + 1;
	uint64 size = sizeof(uint64) + sizeof(short) + sizeof(short) + pathLength;
	char* request = (char*)malloc(size);

	memcpy(request, &size, sizeof(uint64));
	memcpy(request + sizeof(uint64), &opCode, sizeof(short));
	memcpy(request + sizeof(uint64) + sizeof(short), &pathLength, sizeof(short));
	memcpy(request + sizeof(uint64) + sizeof(short) + sizeof(short), path, pathLength);

	return request;
}

char* opReadDir(const char* path)
{
	short opCode = HYPERV_READDIR;
	short pathLength = strlen(path) + 1;
	uint64 size = sizeof(uint64) + sizeof(short) + sizeof(short) + pathLength;
	char* request = (char*)malloc(size);

	memcpy(request, &size, sizeof(uint64));
	memcpy(request + sizeof(uint64), &opCode, sizeof(short));
	memcpy(request + sizeof(uint64) + sizeof(short), &pathLength, sizeof(short));
	memcpy(request + sizeof(uint64) + sizeof(short) + sizeof(short), path, pathLength);

	return request;
}

char* opRead(const char* path, uint64 rSize, int64 rOffset)
{
	short opCode = HYPERV_READ;
	short pathLength = strlen(path) + 1;
	uint64 size = sizeof(uint64) + sizeof(short) + sizeof(short) + pathLength + sizeof(uint64) + sizeof(int64);
	char* request = (char*)malloc(size);

	int offset = 0;
	memcpy(request + offset, &size, sizeof(uint64));

	offset += sizeof(uint64);
	memcpy(request + offset, &opCode, sizeof(short));

	offset += sizeof(short);
	memcpy(request + offset, &pathLength, sizeof(short));

	offset += sizeof(short);
	memcpy(request + offset, path, pathLength);

	offset += pathLength;
	memcpy(request + offset, &rSize, sizeof(uint64));

	offset += sizeof(uint64);
	memcpy(request + offset, &rOffset, sizeof(int64));

	return request;
}

char* opCreate(const char* path, uint32 mode)
{
	short opCode = HYPERV_CREATE;
	short pathLength = strlen(path) + 1;
	uint64 size = sizeof(uint64) + sizeof(short) + sizeof(short) + pathLength + sizeof(uint32);
	char* request = (char*)malloc(size);

	int offset = 0;
	memcpy(request + offset, &size, sizeof(uint64));

	offset += sizeof(uint64);
	memcpy(request + offset, &opCode, sizeof(short));

	offset += sizeof(short);
	memcpy(request + offset, &pathLength, sizeof(short));

	offset += sizeof(short);
	memcpy(request + offset, path, pathLength);

	offset += pathLength;
	memcpy(request + offset, &mode, sizeof(uint32));

	return request;
}

char* opWrite(const char* path, uint64 wSize, int64 wOffset, const char* buffer)
{
	short opCode = HYPERV_WRITE;
	short pathLength = strlen(path) + 1;
	uint64 size = sizeof(uint64) + sizeof(short) + sizeof(short) + pathLength + sizeof(uint64) + sizeof(int64) + wSize;
	char* request = (char*)malloc(size);

	int offset = 0;
	memcpy(request + offset, &size, sizeof(uint64));

	offset += sizeof(uint64);
	memcpy(request + offset, &opCode, sizeof(short));

	offset += sizeof(short);
	memcpy(request + offset, &pathLength, sizeof(short));

	offset += sizeof(short);
	memcpy(request + offset, path, pathLength);

	offset += pathLength;
	memcpy(request + offset, &wSize, sizeof(uint64));

	offset += sizeof(uint64);
	memcpy(request + offset, &wOffset, sizeof(int64));

	offset += sizeof(int64);
	memcpy(request + offset, buffer, wSize);

	return request;
}

char* opUnlink(const char* path)
{
	short opCode = HYPERV_UNLINK;
	short pathLength = strlen(path) + 1;
	uint64 size = sizeof(uint64) + sizeof(short) + sizeof(short) + pathLength;
	char* request = (char*)malloc(size);

	int offset = 0;
	memcpy(request + offset, &size, sizeof(uint64));

	offset += sizeof(uint64);
	memcpy(request + offset, &opCode, sizeof(short));

	offset += sizeof(short);
	memcpy(request + offset, &pathLength, sizeof(short));

	offset += sizeof(short);
	memcpy(request + offset, path, pathLength);

	return request;
}

char* opTruncate(const char* path, int64 tOffset)
{
	short opCode = HYPERV_TRUNCATE;
	short pathLength = strlen(path) + 1;
	uint64 size = sizeof(uint64) + sizeof(short) + sizeof(short) + pathLength + sizeof(int64);
	char* request = (char*)malloc(size);

	int offset = 0;
	memcpy(request + offset, &size, sizeof(uint64));

	offset += sizeof(uint64);
	memcpy(request + offset, &opCode, sizeof(short));

	offset += sizeof(short);
	memcpy(request + offset, &pathLength, sizeof(short));

	offset += sizeof(short);
	memcpy(request + offset, path, pathLength);

	offset += pathLength;
	memcpy(request + offset, &tOffset, sizeof(int64));

	return request;
}

char* opMkdir(const char* path, uint32 mode)
{
	short opCode = HYPERV_MKDIR;
	short pathLength = strlen(path) + 1;
	uint64 size = sizeof(uint64) + sizeof(short) + sizeof(short) + pathLength + sizeof(uint32);
	char* request = (char*)malloc(size);

	int offset = 0;
	memcpy(request + offset, &size, sizeof(uint64));

	offset += sizeof(uint64);
	memcpy(request + offset, &opCode, sizeof(short));

	offset += sizeof(short);
	memcpy(request + offset, &pathLength, sizeof(short));

	offset += sizeof(short);
	memcpy(request + offset, path, pathLength);

	offset += pathLength;
	memcpy(request + offset, &mode, sizeof(uint32));

	return request;
}

char* opRmdir(const char* path)
{
	short opCode = HYPERV_RMDIR;
	short pathLength = strlen(path) + 1;
	uint64 size = sizeof(uint64) + sizeof(short) + sizeof(short) + pathLength;
	char* request = (char*)malloc(size);

	int offset = 0;
	memcpy(request + offset, &size, sizeof(uint64));

	offset += sizeof(uint64);
	memcpy(request + offset, &opCode, sizeof(short));

	offset += sizeof(short);
	memcpy(request + offset, &pathLength, sizeof(short));

	offset += sizeof(short);
	memcpy(request + offset, path, pathLength);

	return request;
}

char* opRename(const char* from, const char* to)
{
	short opCode = HYPERV_RENAME;
	short fromLength = strlen(from) + 1;
	short toLength = strlen(to) + 1;
	uint64 size = sizeof(uint64) + sizeof(short) + sizeof(short) + fromLength + sizeof(short) + toLength;
	char* request = (char*)malloc(size);

	int offset = 0;
	memcpy(request + offset, &size, sizeof(uint64));

	offset += sizeof(uint64);
	memcpy(request + offset, &opCode, sizeof(short));

	offset += sizeof(short);
	memcpy(request + offset, &fromLength, sizeof(short));

	offset += sizeof(short);
	memcpy(request + offset, from, fromLength);

	offset += fromLength;
	memcpy(request + offset, &toLength, sizeof(short));

	offset += sizeof(short);
	memcpy(request + offset, to, toLength);

	return request;
}

char* opSymlink(const char* from, const char* to, short ext)
{
	short opCode = HYPERV_SYMLINK;
	short fromLength = strlen(from) + 1;
	short toLength = strlen(to) + 1;
	uint64 size = sizeof(uint64) + sizeof(short) + sizeof(short) + fromLength + sizeof(short) + toLength + sizeof(short);
	char* request = (char*)malloc(size);

	int offset = 0;
	memcpy(request + offset, &size, sizeof(uint64));

	offset += sizeof(uint64);
	memcpy(request + offset, &opCode, sizeof(short));

	offset += sizeof(short);
	memcpy(request + offset, &fromLength, sizeof(short));

	offset += sizeof(short);
	memcpy(request + offset, from, fromLength);

	offset += fromLength;
	memcpy(request + offset, &toLength, sizeof(short));

	offset += sizeof(short);
	memcpy(request + offset, to, toLength);

	offset += toLength;
	memcpy(request + offset, &ext, sizeof(short));

	return request;
}

char* opLink(const char* from, const char* to)
{
	short opCode = HYPERV_LINK;
	short fromLength = strlen(from) + 1;
	short toLength = strlen(to) + 1;
	uint64 size = sizeof(uint64) + sizeof(short) + sizeof(short) + fromLength + sizeof(short) + toLength;
	char* request = (char*)malloc(size);

	int offset = 0;
	memcpy(request + offset, &size, sizeof(uint64));

	offset += sizeof(uint64);
	memcpy(request + offset, &opCode, sizeof(short));

	offset += sizeof(short);
	memcpy(request + offset, &fromLength, sizeof(short));

	offset += sizeof(short);
	memcpy(request + offset, from, fromLength);

	offset += fromLength;
	memcpy(request + offset, &toLength, sizeof(short));

	offset += sizeof(short);
	memcpy(request + offset, to, toLength);

	return request;
}

char* opReadlink(const char* path)
{
	short opCode = HYPERV_READLINK;
	short pathLength = strlen(path) + 1;
	uint64 size = sizeof(uint64) + sizeof(short) + sizeof(short) + pathLength;
	char* request = (char*)malloc(size);

	int offset = 0;
	memcpy(request + offset, &size, sizeof(uint64));

	offset += sizeof(uint64);
	memcpy(request + offset, &opCode, sizeof(short));

	offset += sizeof(short);
	memcpy(request + offset, &pathLength, sizeof(short));

	offset += sizeof(short);
	memcpy(request + offset, path, pathLength);

	return request;
}

int aquireSocket()
{
	pthread_mutex_lock(&sSocketLock);

	// 	int socket = 0;

	// aquire:
	// 	for (int i = 0; i < SOCKET_NUM; i++) {
	// 		if (sSockets[i]) {
	// 			socket = sSockets[i];
	// 			sSockets[i] = 0;
	// 		}
	// 	}

	// 	if (!socket) {
	// 		pthread_cond_wait(&sSocketCond, &sSocketLock);
	// 		goto aquire;
	// 	}


	// 	pthread_mutex_unlock(&sSocketLock);

	return sSockets[0];
}

char* mountPath()
{
	return *((char**)fuse_get_session(fuse_get_context()->fuse));
}

char* makeLocalPath(const char* path, const char* name)
{
	int len = strlen(name);
	int size = len == 0 ? 1 : len + 1;

	char* filePath = (char*)calloc(1, strlen(path) + size);

	strcpy(filePath, path);

	if (len) {
		strcat(filePath, name);
	}

	return filePath;
}

int relativeToMountpoint(const char* mountpoint, const char* path)
{
	int mountLen = strlen(mountpoint);

	// does not start with the mountpoint
	if (strncmp(mountpoint, path, mountLen) != 0) {
		return 0;
	}

	const char* start = path + mountLen;
	const char* end = start;

	while (*end)
	{
		// find next segment
		while (*end && *end != '/') {
			++end;
		}

		// check for . or ..
		if (end - start == 2 && start[1] == '.') {
			return 0;
		}

		if (end - start == 3 && start[1] == '.' && start[2] == '.') {
			return 0;
		}

		start = end++;
	}

	return mountLen;
}

void releaseSocket(int socket)
{
	// pthread_mutex_lock(&sSocketLock);

	// // find first empty position
	// for (int i = 0; i < SOCKET_NUM; i++) {
	// 	if (!sSockets[i]) {
	// 		sSockets[i] = socket;
	// 	}
	// }

	// pthread_cond_signal(&sSocketCond);

	pthread_mutex_unlock(&sSocketLock);
}

int readMessage(int socket, char** buffer)
{
	uint64 size = 0;
	char sizeBuffer[sizeof(uint64)];

	int ret = recv(socket, sizeBuffer, sizeof(uint64), MSG_WAITALL);

	// if we read 0 bytes, connection might be closed, return
	if (ret <= 0) {
		return 0;
	}

	memcpy(&size, sizeBuffer, sizeof(uint64));

	*buffer = (char*)malloc(size);
	memcpy(*buffer, sizeBuffer, sizeof(uint64));

	uint64 rOffset = sizeof(uint64);
	uint64 rSize = size - rOffset;

	while (rSize > 0)
	{
		ret = recv(socket, *buffer + rOffset, rSize, 0);

		// if we read 0 bytes, connection might be closed, return
		if (ret <= 0) {
			free(buffer);
			return 0;
		}

		rSize -= ret;
		rOffset += ret;
	}

	return size;
}

int sendMessage(int socket, char* buffer)
{
	uint64* size = (uint64*)buffer;

	return send(socket, buffer, *size, 0);
}

char* requestOp(char* request, int* err)
{
	int socket = aquireSocket();
	char* response = NULL;
	*err = 0;

	if (!sendMessage(socket, request)) {
		*err = ENOTCONN;
		goto out;
	}

	free(request);

	if (!readMessage(socket, &response)) {
		*err = ENOTCONN;
		goto out;
	}

	short status = *(short*)(response + sizeof(uint64));

	if (status != HYPERV_OK) {
		free(response);
		*err = (int)status;
		goto out;
	}

out:
	releaseSocket(socket);
	return *err ? NULL : response;
}



static void* xmp_init(struct fuse_conn_info* conn,
	struct fuse_config* cfg)
{
	printf("Function call [init]\n");
	(void)conn;
	cfg->use_ino = 1;

	/* Pick up changes from lower filesystem right away. This is
	   also necessary for better hardlink support. When the kernel
	   calls the unlink() handler, it does not know the inode of
	   the to-be-removed entry and can therefore not invalidate
	   the cache of the associated inode - resulting in an
	   incorrect st_nlink value being reported for any remaining
	   hardlinks to this inode. */
	   // cfg->entry_timeout = 500;
	   // cfg->attr_timeout = 500;
	   // cfg->negative_timeout = 500;

	opConnect();

	return NULL;
}

static int xmp_getattr(const char* path, struct stat* stbuf,
	struct fuse_file_info* fi)
{
	printf("Function call [getattr] on path %s\n", path);

	(void)fi;
	int err;

	char* inBuffer = requestOp(
		opReadAttr(path),
		&err
	);

	if (err) {
		return -err;
	}

	HyperVStat* stat = (HyperVStat*)(inBuffer + sizeof(uint64) + sizeof(short));

	// stbuf->st_dev = stat->fsid;
	stbuf->st_ino = stat->fileid;
	stbuf->st_nlink = stat->nlink;
	stbuf->st_mode = stat->mode;
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
	fprintf(stderr, "UNIMPLEMENTED: Function call [access] on path %s\n", path);

	return 0;
}

static int xmp_readlink(const char* path, char* buf, size_t size)
{
	printf("Function call [readlink] on path %s\n", path);

	int err;

	char* inBuffer = requestOp(
		opReadlink(path),
		&err
	);

	if (err) {
		return -err;
	}

	// fill the buffer with the path
	int offset = sizeof(uint64) + sizeof(short);
	short* ext = (short*)(inBuffer + offset);

	offset += sizeof(short) + sizeof(short);
	char* lPath = (char*)(inBuffer + offset);
	char* linkPath = !*ext ? makeLocalPath(mountPath(), lPath) : strdup(lPath);
	int len = strlen(linkPath) + 1;

	int bufSize = size > len ? len : size;
	memcpy(buf, linkPath, bufSize);
	buf[bufSize - 1] = '\0';

	free(linkPath);
	free(inBuffer);

	return 0;
}

static int xmp_opendir(const char* path, struct fuse_file_info* fi)
{
	struct xmp_dirp* d = malloc(sizeof(struct xmp_dirp));

	if (d == NULL) {
		return -ENOMEM;
	}

	d->entry = NULL;
	d->offset = 0;
	d->readSize = 0;

	fi->fh = (uint64)d;
	return 0;
}

static int xmp_readdir(const char* path, void* buf, fuse_fill_dir_t filler,
	off_t offset, struct fuse_file_info* fi,
	enum fuse_readdir_flags flags)
{
	printf("Function call [readdir] on path %s\n", path);

	(void)flags;

	char* inBuffer = NULL;
	int64 dOffset = 1;
	uint64 readSize = sizeof(uint64) + sizeof(short);
	struct xmp_dirp* d = (struct xmp_dirp*)fi->fh;

	// if we have a offset, we don't need to fetch again
	if (d->offset) {
		dOffset = d->offset;
		inBuffer = (char*)d->entry;
		readSize = d->readSize;
		goto fill;
	}

	int err;

	inBuffer = requestOp(
		opReadDir(path),
		&err
	);

	if (err) {
		return -err;
	}

fill:;
	uint64* size = (uint64*)inBuffer;

	while (*size > readSize) {
		// we also have the name size, but names are NULL terminated
		short* nameLength = (short*)(inBuffer + readSize);
		readSize += sizeof(short);

		// read name
		char* name = (char*)(inBuffer + readSize);
		readSize += *nameLength;

		// read stat
		HyperVStat* stat = (HyperVStat*)(inBuffer + readSize);
		readSize += sizeof(HyperVStat);

		// convert
		struct stat st = { 0 };
		st.st_dev = stat->fsid;
		st.st_ino = stat->fileid;
		st.st_nlink = stat->nlink;
		st.st_mode = stat->mode;
		st.st_gid = getgid();
		st.st_uid = getuid();
		st.st_size = stat->size;
		st.st_atim = toTimeSpec(stat->atime);
		st.st_mtim = toTimeSpec(stat->mtime);
		st.st_ctim = toTimeSpec(stat->ctime);

		if (filler(buf, name, &st, dOffset, FUSE_FILL_DIR_PLUS)) {
			// buffer is full
			break;
		}

		dOffset++;
	}

	// save the offset and hope we don't have a memory leak
	d->entry = (void*)inBuffer;
	d->readSize = readSize;
	d->offset = dOffset;

	return 0;
}

static int xmp_releasedir(const char* path, struct fuse_file_info* fi)
{
	struct xmp_dirp* d = (struct xmp_dirp*)fi->fh;
	(void)path;
	free(d->entry);
	free(d);
	return 0;
}

static int xmp_mknod(const char* path, mode_t mode, dev_t rdev)
{
	fprintf(stderr, "UNIMPLEMENTED: Function call [mknod] on path %s\n", path);

	return -ENOSYS;
}

static int xmp_mkdir(const char* path, mode_t mode)
{
	printf("Function call [mkdir] on path %s\n", path);

	int err;

	char* inBuffer = requestOp(
		opMkdir(path, mode),
		&err
	);

	if (err) {
		return -err;
	}

	free(inBuffer);

	return 0;
}

static int xmp_unlink(const char* path)
{
	printf("Function call [unlink] on path %s\n", path);

	int err;

	char* inBuffer = requestOp(
		opUnlink(path),
		&err
	);

	if (err) {
		return -err;
	}

	free(inBuffer);

	return 0;
}

static int xmp_rmdir(const char* path)
{
	printf("Function call [rmdir] on path %s\n", path);

	int err;

	char* inBuffer = requestOp(
		opRmdir(path),
		&err
	);

	if (err) {
		return -err;
	}

	free(inBuffer);

	return 0;
}

static int xmp_symlink(const char* from, const char* to)
{
	printf("Function call [symlink] on path %s to %s\n", from, to);

	int offset = relativeToMountpoint(mountPath(), from);
	short ext = offset ? 0 : 1;

	int err;

	char* inBuffer = requestOp(
		// TODO: maybe send the a flag if from is external if it is a dir
		opSymlink(from + offset, to, ext),
		&err
	);

	if (err) {
		return -err;
	}

	free(inBuffer);

	return 0;
}

static int xmp_rename(const char* from, const char* to, unsigned int flags)
{
	printf("Function call [rename] on path %s\n", from);

	int err;

	char* inBuffer = requestOp(
		opRename(from, to),
		&err
	);

	if (err) {
		return -err;
	}

	free(inBuffer);

	return 0;
}

static int xmp_link(const char* from, const char* to)
{
	printf("Function call [link] on path %s to %s\n", from, to);

	int err;

	char* inBuffer = requestOp(
		opLink(from, to),
		&err
	);

	if (err) {
		return -err;
	}

	free(inBuffer);

	return 0;
}

static int xmp_chmod(const char* path, mode_t mode,
	struct fuse_file_info* fi)
{
	fprintf(stderr, "UNIMPLEMENTED: Function call [chmod] on path %s\n", path);

	return -ENOSYS;
}

static int xmp_chown(const char* path, uid_t uid, gid_t gid,
	struct fuse_file_info* fi)
{
	fprintf(stderr, "UNIMPLEMENTED: Function call [chown] on path %s\n", path);

	return -ENOSYS;
}

static int xmp_truncate(const char* path, off_t offset,
	struct fuse_file_info* fi)
{
	printf("Function call [truncate] on path %s\n", path);

	(void)fi;
	int err;

	char* inBuffer = requestOp(
		opTruncate(path, offset),
		&err
	);

	if (err) {
		return -err;
	}

	free(inBuffer);

	return 0;
}

static int xmp_create(const char* path, mode_t mode,
	struct fuse_file_info* fi)
{
	printf("Function call [create] on path %s\n", path);

	(void)fi;
	int err;

	char* inBuffer = requestOp(
		opCreate(path, mode),
		&err
	);

	if (err) {
		return -err;
	}

	free(inBuffer);

	return 0;
}

static int xmp_open(const char* path, struct fuse_file_info* fi)
{
	printf("Function call [open] on path %s\n", path);

	(void)fi;

	return 0;
}

static int xmp_read(const char* path, char* buf, size_t size, off_t offset,
	struct fuse_file_info* fi)
{
	printf("Function call [read] on path %s\n", path);

	(void)fi;
	int err;

	char* inBuffer = requestOp(
		opRead(path, size, offset),
		&err
	);

	if (err) {
		return -err;
	}

	uint64 bytesRead = 0;
	int iOffset = sizeof(uint64) + sizeof(short);
	memcpy(&bytesRead, inBuffer + iOffset, sizeof(uint64));

	iOffset += sizeof(uint64);
	memcpy(buf, inBuffer + iOffset, bytesRead);

	free(inBuffer);

	return bytesRead;
}

static int xmp_write(const char* path, const char* buf, size_t size,
	off_t offset, struct fuse_file_info* fi)
{
	printf("Function call [write] on path %s\n", path);

	(void)fi;
	int err;

	char* inBuffer = requestOp(
		opWrite(path, size, offset, buf),
		&err
	);

	if (err) {
		return -err;
	}

	uint64 bytesWritten = 0;
	int iOffset = sizeof(uint64) + sizeof(short);
	memcpy(&bytesWritten, inBuffer + iOffset, sizeof(uint64));

	free(inBuffer);

	return bytesWritten;
}

static int xmp_statfs(const char* path, struct statvfs* stbuf)
{
	fprintf(stderr, "UNIMPLEMENTED: Function call [statfs] on path %s\n", path);

	return -ENOSYS;
}

static int xmp_release(const char* path, struct fuse_file_info* fi)
{
	printf("Function call [release] on path %s\n", path);
	(void)path;
	(void)fi;
	return 0;
}

static int xmp_fsync(const char* path, int isdatasync,
	struct fuse_file_info* fi)
{
	printf("Function call [fsync] on path %s\n", path);
	/* Just a stub.	 This method is optional and can safely be left
	   unimplemented */

	(void)path;
	(void)isdatasync;
	(void)fi;
	return 0;
}

static off_t xmp_lseek(const char* path, off_t off, int whence, struct fuse_file_info* fi)
{
	fprintf(stderr, "UNIMPLEMENTED: Function call [lseek] on path %s\n", path);

	return -ENOSYS;
}

static const struct fuse_operations xmp_oper = {
	.init = xmp_init,
	.getattr = xmp_getattr,
	.access = xmp_access,
	.readlink = xmp_readlink,
	.opendir = xmp_opendir,
	.readdir = xmp_readdir,
	.releasedir = xmp_releasedir,
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
