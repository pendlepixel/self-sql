/*************************************************
Copyright (C) 2020-2030 PENDLE. All Rights Reserved
File name : os.h
Author : pendle
Version : V1.0
Date : 20210126
Description : sqlite3相关的头文件__
Others:
History:
*************************************************/

#ifndef SQLITE3_H_
#define SQLITE3_H_


#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <assert.h>


#ifdef SQLITE_INT64_TYPE
  typedef SQLITE_INT64_TYPE sqlite_int64;
  typedef unsigned SQLITE_INT64_TYPE sqlite_uint64;
#elif defined(_MSC_VER) || defined(__BORLANDC__)
  typedef __int64 sqlite_int64;
  typedef unsigned __int64 sqlite_uint64;
#else
  typedef long long int sqlite_int64;
  typedef unsigned long long int sqlite_uint64;
#endif
typedef sqlite_int64 sqlite3_int64;
typedef sqlite_uint64 sqlite3_uint64;


/*
** CAPI3REF: Result Codes
** KEYWORDS: {result code definitions}
**
** Many SQLite functions return an integer result code from the set shown
** here in order to indicate success or failure.
**
** New error codes may be added in future versions of SQLite.
**
** See also: [extended result code definitions]
*/
#define SQLITE_OK           0   /* Successful result */
/* beginning-of-error-codes */
#define SQLITE_ERROR        1   /* SQL error or missing database */
#define SQLITE_INTERNAL     2   /* Internal logic error in SQLite */
#define SQLITE_PERM         3   /* Access permission denied */
#define SQLITE_ABORT        4   /* Callback routine requested an abort */
#define SQLITE_BUSY         5   /* The database file is locked */
#define SQLITE_LOCKED       6   /* A table in the database is locked */
#define SQLITE_NOMEM        7   /* A malloc() failed */
#define SQLITE_READONLY     8   /* Attempt to write a readonly database */
#define SQLITE_INTERRUPT    9   /* Operation terminated by sqlite3_interrupt()*/
#define SQLITE_IOERR       10   /* Some kind of disk I/O error occurred */
#define SQLITE_CORRUPT     11   /* The database disk image is malformed */
#define SQLITE_NOTFOUND    12   /* Unknown opcode in sqlite3_file_control() */
#define SQLITE_FULL        13   /* Insertion failed because database is full */
#define SQLITE_CANTOPEN    14   /* Unable to open the database file */
#define SQLITE_PROTOCOL    15   /* Database lock protocol error */
#define SQLITE_EMPTY       16   /* Database is empty */
#define SQLITE_SCHEMA      17   /* The database schema changed */
#define SQLITE_TOOBIG      18   /* String or BLOB exceeds size limit */
#define SQLITE_CONSTRAINT  19   /* Abort due to constraint violation */
#define SQLITE_MISMATCH    20   /* Data type mismatch */
#define SQLITE_MISUSE      21   /* Library used incorrectly */
#define SQLITE_NOLFS       22   /* Uses OS features not supported on host */
#define SQLITE_AUTH        23   /* Authorization denied */
#define SQLITE_FORMAT      24   /* Auxiliary database format error */
#define SQLITE_RANGE       25   /* 2nd parameter to sqlite3_bind out of range */
#define SQLITE_NOTADB      26   /* File opened that is not a database file */
#define SQLITE_NOTICE      27   /* Notifications from sqlite3_log() */
#define SQLITE_WARNING     28   /* Warnings from sqlite3_log() */
#define SQLITE_ROW         100  /* sqlite3_step() has another row ready */
#define SQLITE_DONE        101  /* sqlite3_step() has finished executing */


//////////////////////////////////////
//  define for vfs
//////////////////////////////////////
#define SQL_DEFAULT_FILE_CREATE_MODE  744



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
    sqlite3_syscall_ptr (*xGetSystemCall)(sqlite3_vfs*, const char* zName);
    const char* (*xNextSystemCall)(sqlite3_vfs*, const char* zName);
};



#endif  //SQLITE3_H_
