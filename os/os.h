/*************************************************
Copyright (C) 2020-2030 PENDLE. All Rights Reserved
File name : os.h
Author : pendle
Version : V1.0
Date : 20210126
Description : 底层os部分的相关实现的头文件
Others:
History:
*************************************************/

#ifndef OS_OS_H_
#define OS_OS_H_


#if defined(SQLITE_OS_OTHER)
# if SQLITE_OS_OTHER==1
#   undef SQLITE_OS_UNIX
#   define SQLITE_OS_UNIX 0
#   undef SQLITE_OS_WIN
#   define SQLITE_OS_WIN 0
# else
#   undef SQLITE_OS_OTHER
# endif
#endif
#if !defined(SQLITE_OS_UNIX) && !defined(SQLITE_OS_OTHER)
# define SQLITE_OS_OTHER 0
# ifndef SQLITE_OS_WIN
#   if defined(_WIN32) || defined(WIN32) || defined(__CYGWIN__) || defined(__MINGW32) || defined(__BORLANDC__)
#     define SQLITE_OS_WIN 1
#     define SQLITE_OS_UNIX 0
#   else
#     define SQLITE_OS_WIN 0
#     define SQLITE_OS_UNIX 1
#   endif
# else
#  define  SQLITE_OS_UNIX 0
# endif
#else
# ifndef SQLITE_OS_WIN
#   define SQLITE_OS_WIN 0
# endif
#endif

#if SQLITE_OS_WIN
# include <windows.h>
#endif

#if SQLITE_OS_WIN && !defined(SQLITE_OS_WINNT)
# define SQLITE_OS_WINNT 1
#endif

#if defined(_WIN32_WCE)
# define SQLITE_OS_WINCE 1
#else
# define SQLITE_OS_WINCE 0
#endif

#if !defined(SQLITE_OS_WINRT)
# define SQLITE_OS_WINRT 0
#endif

#if !SQLITE_OS_WINCE && !SQLITE_OS_WINRT
# define SQLITE_CURDIR 1
#endif

#ifndef SET_FULLSYNC
# define SET_FULLSYNC(x,y)
#endif

#ifndef SQLITE_DEFAULT_SECTOR_SIZE
# define SQLITE_DEFAULT_SECTOR_SIZE 4096
#endif

#ifndef SQLITE_TEMP_FILE_PREFIX
# define SQLITE_TEMP_FILE_PREFIX "etilqs_"
#endif

#define NO_LOCK         0
#define SHARED_LOCK     1
#define RESERVED_LOCK   2
#define PENDING_LOCK    3
#define EXCLUSIVE_LOCK  4

#ifndef SQLITE_OMIT_WSD
# define PENDING_BYTE      (0x40000000)
#else
# define PENDING_BYTE      sqlite3PendingByte
#endif
#define RESERVED_BYTE      (PENDING_BYTE+1)
#define SHARED_FIRST       (PENDING_BYTE+2)
#define SHARED_SIZE        510


//sqlite_os_init()函数特定的包装器
int sqlite3OsInit(void);

//获取sqlite3_file方法的函数
int sqlite3OsClose(sqlite3_file*)
int sqlite3OsRead(sqlite3_file*, void*, int amt, i64 offset)
int sqlite3OsWrite(sqlite3_file*, const void*, int amt, i64 offset);
int sqlite3OsTruncate(sqlite3_file*, i64 size);
int sqlite3OsSync(sqlite3_file*, int);
int sqlite3OsFileSize(sqlite3_file*, i64* pSize);
int sqlite3OsLock(sqlite3_file*, int);
int sqlite3OsUnlock(sqlite3_file*, int);
int sqlite3OsCheckReservedLock(sqlite3_file* id, int* pResOut);
int sqlite3OsFileControl(sqlite3_file*, int, void*);
int sqlite3OsFileControlHint(sqlite3_file*, int, void*);
#define SQLITE_FCNTL_DB_UNCHANGED 0xca093fa0
int sqlite3OsSectorSize(sqlite3_file* id);
int sqlite3OsDeviceCharacteristics(sqlite3_file* id);
int sqlite3OsShmMap(sqlite3_file*, int, int, int);
int sqlite3OsShmLock(sqlite3_file* id, int, int, int);
void sqltie3OsShmBarrier(sqlite3_file* id);
int sqlite3OsShmUnmap(sqlite3_file* id, int);


//获取sqlite_vfs方法的函数
int sqlite3OsOpen(sqlite3_vfs*, const char*, sqlite3_file*, int, int*);
int sqlite3OsDelete(sqlite3_vfs*, const char*, int);
int sqlite3OsAccess(sqlite3_vfs*, const char*, int , int* pResOut);
int sqlite3OsFullPathname(sqlite3_vfs*, const char*, int, char*);
#ifndef SQLTIE_OMIT_LOAD_EXTENSION
void* sqlite3OsDlOpen(sqlite3_vfs*, const char*);
void sqlite3OsDlError(sqlite3_vfs*, int, char*);
void (*sqlite3OsDlSym(sqlite3_vfs*, void*, const char*))(void);
void sqlite3OsDlClose(sqlite3_vfs*, void*);
#endif  //SQLTIE_OMIT_LOAD_EXTENSION
int sqlite3OsRandomness(sqlite3_vfs*, int, char*);
int sqlite3OsSleep(sqlite3_vfs*, int);
int sqlite3OsCurrentTimeInt64(sqlite3_vfs*, sqlite3_int64*);

//为sqlite3_melloc()所写的用来申请和释放内存空间的函数
int sqlite3OsOpenMalloc(sqlite3_vfs*, const char*, sqlite3_file**, int, int*);
int sqlite3OsCloseFree(sqlite3_file*);


#endif  //OS_OS_H_