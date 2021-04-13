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

typedef struct
{
    char name[MAX_PATH];
    HyperVStat stat;
} HyperVFile;


enum
{
    HYPERV_OK = 0,
    HYPERV_NOENT = 1
};

void Log(int ret, const char* function)
{
    if (ret == 0)
        printf("%s success\n", function);
    else
        printf("%s error: %d\n", function, WSAGetLastError());
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

HyperVStat* getAttr(const char *path)
{
    HyperVStat* stat = (HyperVStat*) calloc(1, sizeof(HyperVStat));

    uint32 fileAttr = GetFileAttributes(path);
    // TODO: check for errors

    stat->type = 0;
    uint32 dwFlagsAndAttributes = 0;

    if (fileAttr & FILE_ATTRIBUTE_DIRECTORY) {
        stat->type = 1;
        dwFlagsAndAttributes = FILE_ATTRIBUTE_DIRECTORY | FILE_FLAG_BACKUP_SEMANTICS;
    }
    else {
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

    stat->nlink = lpFileInformation.nNumberOfLinks;
    stat->size = makeLong(lpFileInformation.nFileSizeHigh, lpFileInformation.nFileSizeLow);
    stat->used = stat->size;
    stat->fsid = (uint64)lpFileInformation.dwVolumeSerialNumber;
    stat->fileid = makeLong(lpFileInformation.nFileIndexHigh, lpFileInformation.nFileIndexLow);
    stat->atime = fileTimeToUnix(lpFileInformation.ftLastAccessTime);
    stat->mtime = fileTimeToUnix(lpFileInformation.ftLastWriteTime);
    stat->ctime = fileTimeToUnix(lpFileInformation.ftLastWriteTime);

    return stat;
}

char* makePath(char* path, char* name)
{
    char *filePath = (char*) calloc(MAX_PATH, sizeof(char));
    strcpy(filePath, path);
    strcat(filePath, "\\");
    strcat(filePath, name);

    return filePath;
}

int readDir(char* path, int bufferSize, char *buffer)
{
    char fPath[MAX_PATH];
    strcpy(fPath, path);
    strcat(fPath, "\\*");

    WIN32_FIND_DATA fileinfo;
    HANDLE handle = FindFirstFile(fPath, &fileinfo);
    // TODO: error handling
    short status = HYPERV_OK;

    // prefix with status code
    bufferSize += sizeof(short);
    buffer = (char*)realloc(buffer, bufferSize);
    memcpy(buffer, &status, sizeof(short));

    int size = bufferSize;

    do {
        // skip . and ..
        if (strcmp(fileinfo.cFileName, ".") == 0 || strcmp(fileinfo.cFileName, "..") == 0) {
            continue;
        }

       
        // get file stat
        char* filePath = makePath(path, fileinfo.cFileName);
        HyperVStat* stat = getAttr(filePath);
        free(filePath);

        if (!stat) {
            // TODO: error handling
            continue;
        }

        short nameLength = strlen(fileinfo.cFileName) + 1;

        // allocate space for the file info
        size += sizeof(short) + nameLength + sizeof(HyperVStat);
        buffer = (char*) realloc(buffer , size);

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
        short *len = (short*)calloc(1, sizeof(short));
        memcpy(len, buffer + bufferSize, sizeof(short));

        // read name
        bufferSize += sizeof(short);
        char* name = (char*) calloc(1, *len);
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

    return bufferSize;
}

int main(void)
{
    char* buffer = NULL;
    readDir((char*) "H:\\WORK\\vhosts-cifs\\load-test", 0, buffer);
    return 0;


    WSADATA wdata;
    int ret = WSAStartup(MAKEWORD(2,2), &wdata);
    if (ret != 0)
        printf("WSAStartup error: %d\n", ret);

    SOCKET sServer = socket(AF_HYPERV, SOCK_STREAM, HV_PROTOCOL_RAW);
    if (sServer > 0)
        printf("server socket: %lld\n", sServer);
    else
        printf("socket error: %d\n", WSAGetLastError());

    SOCKADDR_HV addr = { 0 };
    addr.Family = AF_HYPERV;
    addr.ServiceId = HV_GUID_VSOCK_TEMPLATE;
    addr.ServiceId.Data1 = PORT_NUM;

    ret = bind(sServer, (struct sockaddr*)&addr, sizeof addr);
    Log(ret, "bind");

    ret = listen(sServer, 1);
    Log(ret, "listen");

    SOCKET sClient = accept(sServer, NULL, NULL);
    if (sClient > 0)
        printf("client socket: %lld\n", sClient);
    else
        printf("accept error: %d\n", WSAGetLastError());

    unsigned long long size = 0;
    char sizeBuffer[sizeof(size)];

    do
    {
        ret = recv(sClient, sizeBuffer, sizeof(size), 0);
        memcpy(&size, &sizeBuffer, sizeof(size));
        printf("Message size: %d %d\n", size, sizeof(size));

        char *msg = (char*) calloc(size + 1, sizeof(char));
        ret = recv(sClient, msg, size, 0);
        printf("Message: %s\n", msg);
        free(msg);

        // send data back
        printf("Sent size of %d\n", sizeof(HyperVStat));
        HyperVStat* stat = getAttr("H:\\WORK\\vhosts-cifs\\load-test");
        send(sClient, (char*) stat, sizeof(HyperVStat), 0);
        printf("Sent size of %d\n", stat->size);
        free(stat);

    } while (ret > 0);

/* cleanup */
    closesocket(sClient);
    closesocket(sServer);
    WSACleanup();
    return 0;
}