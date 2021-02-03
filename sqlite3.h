/*************************************************
Copyright (C) 2020-2030 PENDLE. All Rights Reserved
File name : os.h
Author : pendle
Version : V1.0
Date : 20210126
Description : sqlite3相关的头文件
Others:
History:
*************************************************/

#ifndef SQLITE3_H_
#define SQLITE3_H_

//结构体sqlite3_io_method
typedef struct sqlite3_io_method sqlite3_io_method
struct sqlite3_io_method
{
    int iVersion;

    //版本1函数指针
    int (*xClose)(sqlite3_file*);
    int (*xRead)(sqlite3_file*, int iAmt, sqlite3_int64 iOfst);
    int (*xWrite)(sqlite3_file*, const void*, int iAmt, sqlite3_int64 iOfst);
    int (*xTruncate)(sqlite3_file*, sqlite3_int64 size);
    int (*xSync)(sqlite3_file*, int flags);
    int (*xFileSize)(sqlite3_file*, sqlite3_int64* pSize);
    int (*xLock)(sqlite3_file*, int);
    int (*xUnLock)(sqlite3_file*, int);
    int (*xCheckReservedLock)(sqlite3_file*, int* pResOut);
    int (*xFileControl)(sqlite3_file*, int op, void* pArg);
    int (*xSectorSize)(sqlite3_file*);
    int (*xDeviceCharacteristics)(sqlite3_file*);

    //版本2函数指针
    int (*xShmMap)(sqlite3_file*, int iPg, int pgsz, int, void volatile**);
    int (*xShmLock)(sqlite3_file*, int offset, int n, int flags);
    int (*xShmBarrier)(sqlite3_file*);
    int (*xShmUnmap)(sqlite3_file*, int deleteFlag);

    //版本3函数指针
    int (*xFetch)(sqlite3_file*, sqlite3_int64 iOfst, int iAmt, void** pp);
    int (*xUnfetch)(sqlite3_file*, sqlite3_int64 iOfst, void* p);
};

//结构体sqlite3_file
typedef struct sqlite3_file sqlite3_file;
struct sqlite3_file
{
    const struct sqlite3_io_method* pMethods;  //打开文件的方法
};


//结构体sqlite3_vfs
typedef struct sqlite3_vfs sqlites_vfs;
typedef void (*sqlite3_syscall_ptr)(void);
struct sqlite3_vfs
{
    int iVersion;  //结构体版本号，当前版本是3
    int szOsFile;  //sqlite3文件的大小
    int mxPathname;  //最大的文件路径名称长度
    sqlite3_vfs* pNext;  //下一个注册的VFS
    const char* zName;  //虚拟文件系统的名称
    void* pAppData;  //指向程序特定应用的指针

    //版本1中的函数指针
    int (*xOpen)(sqlite3_vfs*, const char* zName, sqlite3_file*, int flags, int* pOutFlags);
    int (*xDelete)(sqlite3_vfs*, const char* zName, int syncDir);
    int (*xAccess)(sqlite3_vfs*, const char* zName, int flags, int* pResOut);
    int (*xFullPathname)(sqlite3_vfs*, const char* zName, int nOut, char* zOut);
    void* (*xDelOpen)(sqlite3_vfs*, const char* zFilename);
    void (*xDlError)(sqlite3_vfs*, int nByte, char* zErrMsg);
    void (*(*xDlSym)(sqlite3_vfs*, void*, const char* zSymbol))(void);
    void (*xDlClose)(sqlite3_vfs*, void*);
    void (*xRandomness)(sqlite3_vfs*, int nByte, char* zOut);
    int (*xSleep)(sqlite3_vfs*, int microseconds);
    int (*xCurrentTime)(sqlite3_vfs*, double*);
    int (*xGetLastError)(sqlite3_vfs*, int, char*);

    //版本2增加的函数指针
    int (*xCurrentTimeInt64)(sqlite3_vfs*, sqlite3_int64*);

    //版本3增加的函数指针
    int (*xSetSystemCall)(sqlite3_vfs*, const char* zName, sqlite3_syscall_ptr);
    sqlite3_syscall_ptr (*xGetSystemCall)(sqlite3_vfs*, const cahr* zName);
    const char* (*xNextSystemCall)(sqlite3_vfs*, const char* zName);
};



#endif  //SQLITE3_H_