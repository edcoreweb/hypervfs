#define WIN32_LEAN_AND_MEAN
#define _CRT_SECURE_NO_WARNINGS

#include <winsock2.h>
#include <ws2tcpip.h>
#include <hvsocket.h>
#include <stdio.h>
#include <stdint.h>
#include <combaseapi.h>
#include <windows.h>

// link with Ws2_32.lib
#pragma comment(lib, "Ws2_32.lib")

#define PORT_NUM 5001
#define BUFF_SIZE 512
#define MAX_PATH 260

#define TICKS_PER_SECOND 10000000
#define EPOCH_DIFFERENCE 11644473600

/* File types.  */
#define S_IFDIR	0040000 /* Directory.  */
#define S_IFREG	0100000 /* Regular file.  */
#define S_IFLNK	0120000 /* Symbolic link.  */

#define ROOT "H:\\WORK\\vhosts-cifs\\load-test"

typedef uint64_t uint64;
typedef uint32_t uint32;
typedef int64_t int64;

// this needs to be aligned
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
    HYPERV_NOENT = 1,

    // op codes
    HYPERV_ATTR = 10,
    HYPERV_READDIR = 20,
    HYPERV_READ = 30,
    HYPERV_CREATE = 40,
    HYPERV_WRITE = 50,
    HYPERV_UNLINK = 60
};

void Log(int ret, const char* function, int retZeroSuccess = 1)
{
    int success = (ret == 0 && retZeroSuccess == 1) || (ret != 0 && retZeroSuccess == 0);

    if (success) {
        printf("%s success\n", function);
    }
    else {
        printf("%s error: %d\n", function, WSAGetLastError());
    }
}

uint64 makeLong(uint32 high, uint32 low)
{
    return (uint64) high << 32 | low;
}

uint32 fileTimeToUnix(FILETIME ft)
{
    uint64 ticks = makeLong(ft.dwHighDateTime, ft.dwLowDateTime);

    return (uint32)(ticks / TICKS_PER_SECOND - EPOCH_DIFFERENCE);
}

HyperVStat* getPathAttr(const char *path)
{
    HyperVStat* stat = (HyperVStat*) calloc(1, sizeof(HyperVStat));

    uint32 fileAttr = GetFileAttributes(path);
    // TODO: check for errors
    
    uint32 dwFlagsAndAttributes = 0;

    if (fileAttr & FILE_ATTRIBUTE_DIRECTORY) {
        stat->type = 0;
        stat->mode = S_IFDIR | 0755;
        dwFlagsAndAttributes = FILE_ATTRIBUTE_DIRECTORY | FILE_FLAG_BACKUP_SEMANTICS;
    }
    else {
        stat->type = 1;
        stat->mode = S_IFREG | 0644;
        dwFlagsAndAttributes = FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED;
    }

    HANDLE hFile = CreateFile(path, FILE_READ_EA, FILE_SHARE_READ, NULL, OPEN_EXISTING, dwFlagsAndAttributes, NULL);

    if (hFile == INVALID_HANDLE_VALUE) {
        free(stat);
        return NULL;
    }

    BY_HANDLE_FILE_INFORMATION lpFileInformation;
    GetFileInformationByHandle(hFile, &lpFileInformation);
    CloseHandle(hFile);

    stat->fsid = (uint64)lpFileInformation.dwVolumeSerialNumber;
    stat->fileid = makeLong(lpFileInformation.nFileIndexHigh, lpFileInformation.nFileIndexLow);
    stat->size = makeLong(lpFileInformation.nFileSizeHigh, lpFileInformation.nFileSizeLow);
    stat->used = stat->size;
    stat->nlink = lpFileInformation.nNumberOfLinks;
    stat->atime = fileTimeToUnix(lpFileInformation.ftLastAccessTime);
    stat->mtime = fileTimeToUnix(lpFileInformation.ftLastWriteTime);
    stat->ctime = fileTimeToUnix(lpFileInformation.ftCreationTime);

    return stat;
}

char* cleanPath(const char* name)
{
    int len = strlen(name);

    // root /
    if (len == 1) {
        return NULL;
    }

    // all parts start with /, which gets removed
    char* cleaned = (char*) calloc(1, len);
    int next = 0;

    // skip starting slash
    for (int i = 1; i < strlen(name) + 1; i++)
    {
        // find "/" and replace them with "\\"
        if (name[i] == '/')
        {
            cleaned[next++] = '\\';
            continue;
        }

        cleaned[next++] = name[i];
    }


    return cleaned;
}

char* makePath(const char* path, const char* name)
{
    int len = strlen(name);
    int size = len == 0 ? 1 : len + 2;

    char *filePath = (char*) calloc(1, strlen(path) + size);

    strcpy(filePath, path);

    if (len) {
        filePath[strlen(path)] = '\\';
        strcat(filePath, name);
    }

    return filePath;
}

char* makeCleanPath(const char* path, const char* name)
{
    char* cleaned = cleanPath(name);

    if (!cleaned)
    {
        return makePath(path, "");
    }

    char* filePath = makePath(path, cleaned);
    free(cleaned);

    return filePath;
}

int opError(short err, char** outBuffer)
{
    uint64 size = sizeof(uint64) + sizeof(short);
    *outBuffer = (char*)malloc(size);

    memcpy(*outBuffer, &size, sizeof(uint64));
    memcpy(*outBuffer + sizeof(uint64), &err, sizeof(short));

    return (int)size;
}

int opOk(char** outBuffer)
{
    uint64 size = sizeof(uint64) + sizeof(short);
    *outBuffer = (char*)malloc(size);

    short status = HYPERV_OK;
    memcpy(*outBuffer, &size, sizeof(uint64));
    memcpy(*outBuffer + sizeof(uint64), &status, sizeof(short));

    return (int)size;
}

int opReadAttr(char* inBuffer, char** outBuffer)
{
    int offset = sizeof(uint64) + sizeof(short) + sizeof(short);
    char* path = inBuffer + offset;

    // prefix the path
    char* filePath = makeCleanPath(ROOT, path);
    HyperVStat* stat = getPathAttr(filePath);

    free(filePath);

    if (!stat) {
        return opError(HYPERV_NOENT, outBuffer);
    }

    int status = HYPERV_OK;
    uint64 size = sizeof(uint64) + sizeof(short) + sizeof(HyperVStat);
    *outBuffer = (char*) malloc(size);

    memcpy(*outBuffer, &size, sizeof(uint64));
    memcpy(*outBuffer + sizeof(uint64), &status, sizeof(short));
    memcpy(*outBuffer + sizeof(uint64) + sizeof(short), stat, sizeof(HyperVStat));

    free(stat);

    return size;
}

int opReadDir(char* inBuffer, char** outBuffer)
{
    int offset = sizeof(uint64) + sizeof(short) + sizeof(short);
    char* path = inBuffer + offset;

    // prefix the path
    char* dirPath = makeCleanPath(ROOT, path);

    char *findPath = (char*) calloc(1, strlen(dirPath) + 3 + 1);
    strcpy(findPath, dirPath);
    strcat(findPath, "\\*");

    WIN32_FIND_DATA fileinfo;
    HANDLE handle = FindFirstFile(findPath, &fileinfo);
    free(findPath);

    if (handle == INVALID_HANDLE_VALUE) {
        free(dirPath);
        return opError(HYPERV_NOENT, outBuffer);
    }

    // allocate for average file length
    int blockSize = sizeof(short) + MAX_PATH + sizeof(HyperVStat);
    int blockNum = 5;
    int allocatedBlocks = 0;

    int bufferSize = 0;
    int realSize = bufferSize;
    char* buffer = NULL;

    do {
        // get file stat
        char* filePath = makePath(dirPath, fileinfo.cFileName);
        HyperVStat* stat = getPathAttr(filePath);
        free(filePath);

        if (!stat) {
            continue;
        }

        short nameLength = strlen(fileinfo.cFileName) + 1;

        // allocate space if blocks are needed
        realSize += sizeof(short) + nameLength + sizeof(HyperVStat);
        int requestedSize = realSize / blockSize + (realSize % blockSize != 0);

        if (requestedSize > allocatedBlocks) {
            allocatedBlocks += blockNum;
            buffer = (char*) realloc(buffer, blockSize * allocatedBlocks);
        }

        // copy file name length
        memcpy(buffer + bufferSize, &nameLength, nameLength);

        // copy file name
        bufferSize += sizeof(short);
        memcpy(buffer + bufferSize, fileinfo.cFileName, nameLength);

        // copy file stat
        bufferSize += nameLength;
        memcpy(buffer + bufferSize, stat, sizeof(HyperVStat));

        free(stat);

        bufferSize += sizeof(HyperVStat);
    } while (FindNextFile(handle, &fileinfo) != 0);


    // prefix it with the status and final size
    short status = HYPERV_OK;
    uint64 size = sizeof(uint64) + sizeof(short) + realSize;
    *outBuffer = (char*) malloc(size);

    memcpy(*outBuffer, &size, sizeof(uint64));
    memcpy(*outBuffer + sizeof(uint64), &status, sizeof(short));
    memcpy(*outBuffer + sizeof(uint64) + sizeof(short), buffer, realSize);

    free(buffer);

    return size;
}

int opRead(char* inBuffer, char** outBuffer)
{
    int offset = sizeof(uint64) + sizeof(short);
    short* pathLength = (short*) (inBuffer + offset);

    offset += sizeof(short);
    char* path = inBuffer + offset;
    char* fPath = makeCleanPath(ROOT, path);

    HANDLE hFile = CreateFile(fPath, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    free(fPath);

    if (hFile == INVALID_HANDLE_VALUE) {
        return opError(HYPERV_NOENT, outBuffer);
    }

    offset += *pathLength;
    uint64* rSize = (uint64*) (inBuffer + offset);

    offset += sizeof(uint64);
    int64* rOffset = (int64*) (inBuffer + offset);

    // TODO: maybe validate rSize
    char* buffer = (char*) calloc(1, *rSize);

    // set offset
    LARGE_INTEGER lrOffset;
    lrOffset.QuadPart = *rOffset;
    SetFilePointer(hFile, lrOffset.LowPart, &lrOffset.HighPart, FILE_BEGIN);

    unsigned long readBytes = 0;
    int success = ReadFile(hFile, buffer, *rSize, &readBytes, NULL);
    CloseHandle(hFile);

    if (!success)
    {
        return opError(HYPERV_NOENT, outBuffer);
    }

    int status = HYPERV_OK;
    uint64 lReadBytes = (uint64) readBytes;
    uint64 size = sizeof(uint64) + sizeof(short) + sizeof(uint64) + lReadBytes;
    *outBuffer = (char*) malloc(size);

    offset = 0;
    memcpy(*outBuffer + offset, &size, sizeof(uint64));

    offset += sizeof(uint64);
    memcpy(*outBuffer + offset, &status, sizeof(short));

    offset += sizeof(short);
    memcpy(*outBuffer + offset, &lReadBytes, sizeof(uint64));

    offset += sizeof(uint64);
    memcpy(*outBuffer + offset, buffer, lReadBytes);

    free(buffer);

    return size;
}

int opCreate(char* inBuffer, char** outBuffer)
{
    int offset = sizeof(uint64) + sizeof(short);
    short* pathLength = (short*) (inBuffer + offset);

    offset += sizeof(short);
    char* path = inBuffer + offset;
    char* fPath = makeCleanPath(ROOT, path);

    HANDLE hFile = CreateFile(fPath, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
    free(fPath);

    if (hFile == INVALID_HANDLE_VALUE) {
        return opError(HYPERV_NOENT, outBuffer);
    }

    CloseHandle(hFile);

    return opOk(outBuffer);
}

int opWrite(char* inBuffer, char** outBuffer)
{
    int offset = sizeof(uint64) + sizeof(short);
    short* pathLength = (short*)(inBuffer + offset);

    offset += sizeof(short);
    char* path = inBuffer + offset;
    char* fPath = makeCleanPath(ROOT, path);

    HANDLE hFile = CreateFile(fPath, GENERIC_WRITE, FILE_SHARE_READ, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    free(fPath);

    if (hFile == INVALID_HANDLE_VALUE) {
        return opError(HYPERV_NOENT, outBuffer);
    }

    offset += *pathLength;
    uint64* wSize = (uint64*)(inBuffer + offset);

    offset += sizeof(uint64);
    int64* wOffset = (int64*)(inBuffer + offset);

    // set offset
    LARGE_INTEGER lrOffset;
    lrOffset.QuadPart = *wOffset;
    SetFilePointer(hFile, lrOffset.LowPart, &lrOffset.HighPart, FILE_BEGIN);

    offset += sizeof(int64);
    unsigned long writtenBytes = 0;
    int success = WriteFile(hFile, inBuffer + offset, *wSize, &writtenBytes, NULL);
    CloseHandle(hFile);

    if (!success)
    {
        return opError(HYPERV_NOENT, outBuffer);
    }

    int status = HYPERV_OK;
    uint64 lWrittenBytes = (uint64)writtenBytes;
    uint64 size = sizeof(uint64) + sizeof(short) + sizeof(uint64);
    *outBuffer = (char*)malloc(size);

    offset = 0;
    memcpy(*outBuffer + offset, &size, sizeof(uint64));

    offset += sizeof(uint64);
    memcpy(*outBuffer + offset, &status, sizeof(short));

    offset += sizeof(short);
    memcpy(*outBuffer + offset, &lWrittenBytes, sizeof(uint64));

    return size;
}

int opUnlink(char* inBuffer, char** outBuffer)
{
    int offset = sizeof(uint64) + sizeof(short);
    short* pathLength = (short*)(inBuffer + offset);

    offset += sizeof(short);
    char* path = inBuffer + offset;
    char* fPath = makeCleanPath(ROOT, path);

    int success = DeleteFile(fPath);
    free(fPath);

    if (!success) {
        return opError(HYPERV_NOENT, outBuffer);
    }

    return opOk(outBuffer);
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
    printf("Got message of size: %d\n", size);

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
    uint64 *size = (uint64*) buffer;

    return send(socket, buffer, *size, 0);
}

int processMessage(char* inBuffer, char** outBuffer)
{
    // get the op code
    short *op = (short*) (inBuffer + sizeof(uint64));

    switch (*op)
    {
    case HYPERV_ATTR:
        return opReadAttr(inBuffer, outBuffer);
    case HYPERV_READDIR:
        return opReadDir(inBuffer, outBuffer);
    case HYPERV_READ:
        return opRead(inBuffer, outBuffer);
    case HYPERV_CREATE:
        return opCreate(inBuffer, outBuffer);
    case HYPERV_WRITE:
        return opWrite(inBuffer, outBuffer);
    case HYPERV_UNLINK:
        return opUnlink(inBuffer, outBuffer);
    default:
        return opError(HYPERV_NOENT, outBuffer);
    }
}

int main(void)
{
    /*char* buffer = NULL;
    opReadDir((char*) "H:\\WORK\\vhosts-cifs\\load-test", 0, buffer);
    return 0;

    // read it back to test
    size = bufferSize;
    bufferSize = 0;

    // read status
    bufferSize += sizeof(short);
    short* st = (short*)calloc(1, sizeof(short));
    memcpy(st, buffer, sizeof(short));
    free(st);


    while (bufferSize < size) {
        // read name length
        short* len = (short*)calloc(1, sizeof(short));
        memcpy(len, buffer + bufferSize, sizeof(short));

        // read name
        bufferSize += sizeof(short);
        char* name = (char*)calloc(1, *len);
        memcpy(name, buffer + bufferSize, *len);

        // read stat
        bufferSize += *len;
        HyperVStat* stat = (HyperVStat*)malloc(sizeof(HyperVStat));
        memcpy(stat, buffer + bufferSize, sizeof(HyperVStat));

        free(len);
        free(name);
        free(stat);

        bufferSize += sizeof(HyperVStat);
    }

    return bufferSize;*/


    WSADATA wdata;
    int ret = WSAStartup(MAKEWORD(2,2), &wdata);
    Log(ret, "WSAStartup");

    SOCKET sServer = socket(AF_HYPERV, SOCK_STREAM, HV_PROTOCOL_RAW);
    Log(ret, "socket", 0);

    SOCKADDR_HV addr = { 0 };
    addr.Family = AF_HYPERV;
    addr.ServiceId = HV_GUID_VSOCK_TEMPLATE;
    addr.ServiceId.Data1 = PORT_NUM;

    ret = bind(sServer, (struct sockaddr*)&addr, sizeof addr);
    Log(ret, "bind");

    ret = listen(sServer, 1);
    Log(ret, "listen");

    SOCKET sClient = accept(sServer, NULL, NULL);
    Log(ret, "socket", 0);

    char* inBuffer = NULL;
    char* outBuffer = NULL;

    do
    {
        ret = readMessage(sClient, &inBuffer);
        ret = processMessage(inBuffer, &outBuffer);
        ret = sendMessage(sClient, outBuffer);

        free(inBuffer);
        free(outBuffer);

    } while (ret > 0);


    /* cleanup */
    closesocket(sClient);
    closesocket(sServer);
    WSACleanup();
    return 0;
}