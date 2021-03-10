//自己写的部分接口，暂时放在这个文件中

/* //自己写的部分

//sqlite_os_init()函数特定的包装器
int sqlite3OsInit(void);


//获取sqlite3_file方法的函数
int sqlite3OsClose(sqlite3_file*);
int sqlite3OsRead(sqlite3_file*, void*, int amt, sqlite3_int64 offset);
int sqlite3OsWrite(sqlite3_file*, const void*, int amt, sqlite3_int64 offset);
int sqlite3OsTruncate(sqlite3_file*, sqlite3_int64 size);
int sqlite3OsSync(sqlite3_file*, int);
int sqlite3OsFileSize(sqlite3_file*, sqlite3_int64* pSize);
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


//获取file和vfs句柄的函数
sqlite3_vfs* osGetVFSHandle(sqlite3_vfs* vfs_name);
sqlite3_file* osGetFileHandle(const char* vfs); */