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

#include "../sqlite3.h"


//sqlite_os_init()函数特定的包装器
int sqlite3OsInit(void);


//获取sqlite3_file方法的函数
int sqlite3OsClose(sqlite3_file*);
int sqlite3OsRead(sqlite3_file*, void*, int amt, i64 offset);
int sqlite3OsWrite(sqlite3_file*, const void*, int amt, i64 offset);
int sqlite3OsTruncate(sqlite3_file*, i64 size);
int sqlite3OsSync(sqlite3_file*, int);
int sqlite3OsFileSize(sqlite3_file*, i64* pSize);
int sqlite3OsLock(sqlite3_file*, int);
int sqlite3OsUnlock(sqlite3_file*, int);
int sqlite3OsCheckReservedLock(sqlite3_file* id, int* pResOut);
int sqlite3OsFileControl(sqlite3_file*, int, void*);
void sqlite3OsFileControlHint(sqlite3_file*, int, void*);
#define SQLITE_FCNTL_DB_UNCHANGED 0xca093fa0
int sqlite3OsSectorSize(sqlite3_file* id);
int sqlite3OsDeviceCharacteristics(sqlite3_file* id);
int sqlite3OsShmMap(sqlite3_file*, int, int, int, void volatile **);
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
