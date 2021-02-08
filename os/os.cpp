/*************************************************
Copyright (C) 2020-2030 PENDLE. All Rights Reserved
File name : os.cpp
Author : pendle
Version : V1.0
Date : 20210126
Description : 底层os部分的相关实现的.cpp文件
Others:
History:
*************************************************/

#include "os.h"


#if defined(SQLITE_TEST)
int sqlite3_memdebug_vfs_oom_test = 1;
  #define DO_OS_MALLOC_TEST(x)                                       \
  if (sqlite3_memdebug_vfs_oom_test && (!x || !sqlite3IsMemJournal(x))) {  \
    void *pTstAlloc = sqlite3Malloc(10);                             \
    if (!pTstAlloc) return SQLITE_IOERR_NOMEM;                       \
    sqlite3_free(pTstAlloc);                                         \
  }
#else
  #define DO_OS_MALLOC_TEST(x)
#endif



//获取sqlite3_file方法的函数
int sqlite3OsClose(sqlite3_file* pId)
{
    int rc = SQLITE_OK;
    if (pId->pMethods)
    {
        rc = pId->pMethods->xClose(pId);
        pId->pMethods = 0;
    }

    return rc;
}

int sqlite3OsRead(sqlite3_file* id, void* pBuf, int amt, i64 offset)
{
    DO_OS_MALLOC_TEST(id);
    return id->pMethods->xRead(id, pBuf, amt, offset);
}

int sqlite3OsWrite(sqlite3_file* id, const void* pBuf, int amt, i64 offset)
{
    DO_OS_MALLOC_TEST(id);
    return id->pMethods->xWrite(id, pBuf, amt, offset);
}

int sqlite3OsTruncate(sqlite3_file* id, i64 size)
{
    return id->pMethods->xTruncate(id, size);
}

int sqlite3OsSync(sqlite3_file* id, int flags)
{
    DO_OS_MALLOC_TEST(id);
    return id->pMethods->xSync(id, flags);
}

int sqlite3OsFileSize(sqlite3_file* id, i64* pSize)
{
    DO_OS_MALLOC_TEST(id);
    return id->pMethods->xFileSize(id, pSize);
}

int sqlite3OsLock(sqlite3_file* id, int lockType)
{
    DO_OS_MALLOC_TEST(id);
    return id->pMethods->xLock(id, lockType);
}

int sqlite3OsUnlock(sqlite3_file* id, int lockType)
{
    return id->pMethods->xUnLock(id, lockType);
}

int sqlite3OsCheckReservedLock(sqlite3_file* id, int* pResOut)
{
    DO_OS_MALLOC_TEST(id);
    return id->pMethods->xCheckReservedLock(id, pResOut);
}

int sqlite3OsFileControl(sqlite3_file* id, int op, void* pArg)
{
    DO_OS_MALLOC_TEST(id);
    return id->pMethods->xFileControl(id, op, pArg);    
}

void sqlite3OsFileControlHint(sqlite3_file* id, int op, void* pArg)
{
    (void)id->pMethods-xFileControl(id, op, pArg);
}

int sqlite3OsSectorSize(sqlite3_file* id)
{
    int (*xSectorSize)(sqlite3_file*) = id->pMethods->xSectorSize;
    return (xSectorSize ? xSectorSize(id) : SQLITE_DEFAULT_SECTOR_SIZE);
}

int sqlite3OsDeviceCharacteristics(sqlite3_file* id)
{
    return id->pMethods->xDeviceCharacteristics(id);
}

int sqlite3OsShmLock(sqlite3_file* id, int offset, int n, int flags)
{
    return id->pMethods->xShmLock(id, offset, n, flags);
}

void sqltie3OsShmBarrier(sqlite3_file* id)
{
    id->pMethods->xShmBarrier(id);
}

int sqlite3OsShmUnmap(sqlite3_file* id, int deleteFlag)
{
    return id->pMethods->xShmUnmap(id, deleteFlag);
}

int sqlite3OsShmMap(
    sqlite3_file* id,  //数据库文件句柄 
    int iPage, 
    int pgsz, 
    int bExtend,  //True to extend file if necessary
    void volatile** pp  //OUT: pointer to mapping
)
{
    DO_OS_MALLOC_TEST(id);
    return id->pMethods->xShmMap(id, iPage, pgsz, bExtend, pp);
}


//获取sqlite_vfs方法的函数
int sqlite3OsOpen(sqlite3_vfs* pVfs, const char* zPath, sqlite3_file* pFile, int flags, int* pFlagOut)
{
    int rc;
    DO_OS_MALLOC_TEST(0);

    /* 0x87f7f is a mask of SQLITE_OPEN_ flags that are valid to be passed
    ** down into the VFS layer.  Some SQLITE_OPEN_ flags (for example,
    ** SQLITE_OPEN_FULLMUTEX or SQLITE_OPEN_SHAREDCACHE) are blocked before
    ** reaching the VFS. */
    rc = pVfs->xOpen(pVfs, zPath, pFile, flags & 0x87f7f, pFlagOut);
    assert(rc == SQLITE_OK || pFile->pMethods == 0);
    return rc;
}

int sqlite3OsDelete(sqlite3_vfs* pVfs, const char* zPath, int dirSync)
{
    DO_OS_MALLOC_TEST(0);
    assert(dirSync ==0 || dirSync == 1);
    return pVfs->xDelete(pVfs, zPath, dirSync);
}

int sqlite3OsAccess(sqlite3_vfs* pVfs, const char* zPath, int flags, int* pResOut)
{
    DO_OS_MALLOC_TEST(0);
    return pVfs->xAccess(pVfs, zPath, flags, pResOut);
}

int sqlite3OsFullPathname(sqlite3_vfs* pVfs, const char* zPath, int nPathOut, char* zPathOut)
{
    DO_OS_MALLOC_TEST(0);
    zPathOut[0] = 0;
    return pVfs->xFullPathname(pVfs, zPath, nPathOut, zPathOut);
}

#ifndef SQLTIE_OMIT_LOAD_EXTENSION
void* sqlite3OsDlOpen(sqlite3_vfs* pVfs, const char* zPath)
{
    return pVfs->xDelOpen(pVfs, zPath);
}

void sqlite3OsDlError(sqlite3_vfs* pVfs, int nByte, char* zBufOut)
{
    return pVfs->xDlError(pVfs, nByte, zBufOut);
}

void (*sqlite3OsDlSym(sqlite3_vfs* pVfs, void* pHdle, const char* zSym))(void)
{
    return pVfs->xDlSym(pVfs, pHdle, zSym);
}

void sqlite3OsDlClose(sqlite3_vfs* pVfs, void* pHandle)
{
    return pVfs->xDlClose(pVfs, pHandle);
}

#endif  //SQLTIE_OMIT_LOAD_EXTENSION
int sqlite3OsRandomness(sqlite3_vfs* pVfs, int nByte, char* zBufOut)
{
    return pVfs->xRandomness(pVfs, nByte, zBufOut);
}

int sqlite3OsSleep(sqlite3_vfs* pVfs, int nMicro)
{
    return pVfs->xSleep(pVfs, nMicro);
}

int sqlite3OsCurrentTimeInt64(sqlite3_vfs* pVfs, sqlite3_int64* pTimeOut)
{
    int rc;
    /* IMPLEMENTATION-OF: R-49045-42493 SQLite will use the xCurrentTimeInt64()
    ** method to get the current date and time if that method is available
    ** (if iVersion is 2 or greater and the function pointer is not NULL) and
    ** will fall back to xCurrentTime() if xCurrentTimeInt64() is
    ** unavailable.
    */

    if (pVfs->iVersion >= 2 && pVfs->xCurrentTimeInt64)
    {
        rc = pVfs->xCurrentTimeInt64(pVfs, pTimeOut);
    }
    else
    {
        rc = pVfs->xCurrentTime(pVfs, &r);
        *pTimeOut = (sqlite3_int64)(r* 86400000.0);
    }

    return rc;
}

//为sqlite3_melloc()所写的用来申请和释放内存空间的函数
int sqlite3OsOpenMalloc(sqlite3_vfs* pVfs, const char* zFile, sqlite3_file** ppFile, int flags, int* pOutFlags)
{
    int rc = SQLITE_NOMEM;
    sqlite3_file* pFile;
    pFile = (sqlite3_file*)sqlite3MallocZero(pVfs->szOsFile);
    if (pFile)
    {
        rc = sqlite3OsOpen(pVfs, zFile, pFile, flags, pOutFlags);
        if(rc != SQLITE_OK)
        {
            sqlite3_free(pFile);
        }
        else
        {
            *ppFile = pFile;
        }
    }

    return rc;
}

int sqlite3OsCloseFree(sqlite3_file* pFile)
{
    int rc = SQLITE_OK;
    assert(pFile);
    rc = sqlite3OsOpen(pFile);
    sqlite3_free(pFile);
    return rc;
}

int sqlite3OsInit(void)
{
    void* p = sqlite3_malloc(10);
    if (0 == p)
    {
        return SQLITE_NOMEM;
    }
    
    sqlite3_free(p);
    return sqlite3_os_init();
}


/*
** The list of all registered VFS implementations.
*/
static sqlite3_vfs* SQLITE_WSD vfsList = 0;
#define vfsList GLOBAL(sqlite3_vfs*, vfsList);

/*
** Locate a VFS by name.  If no name is given, simply return the
** first VFS on the list.
*/
sqlite3_vfs* sqlite3_vfs_find(const char* zVfs)
{
    sqlite3_vfs* pVfs = 0;
#define SQLTIE_THREADSAFE
    sqlite_mutex* mutex;
#endif

#ifndef SQLITE_OMIT_AUTOINIT
    int rc = sqlite3_initialize();
    if (rc)  return 0;
#endif

#if SQLTIE_THREADSAFE
    mutex = sqlite3MallocAlloc(SQLITE_MUTEX_STATIC_MASTER);
#endif

    sqlite3_mutex_enter(mutex);
    for (pVfs = vfsList; pVfs; pVfs = pVfs->pNext)
    {
        if (zVfs == 0) break;
        if (strcmp(zVfs, pVfs->zName) == 0) break;
    }

    sqlite3_mutex_leave(mutex);
    return pVfs;
}

/*
** Unlink a VFS from the linked list
*/
static void vfsUnlink(sqlite3_vfs* pVfs)
{
    assert(sqlite3_mutex_held(sqltie3MutexAlloc(SQLITE_MUTEX_STATIC_MASTER)));
    if (pVfs == 0)
    {
        //do nothing
    }
    else if (vfsList == pVfs)
    {
        sqlite3_vfs* p = vfsList;
        while(p->pNext && p->pNext != pVfs)
        {
            p = p->pNext;
        }

        if (p->pNext == pVfs)
        {
            p->pNext = pVfs->pNext;
        }
    }
}

/*
** Register a VFS with the system.  It is harmless to register the same
** VFS multiple times.  The new VFS becomes the default if makeDflt is
** true.
*/
int sqlite3_vfs_register(sqlite3_vfs* pVfs, int makeDflt)
{
    MUTEX_LOGIC(sqlite3_mutex* mutex);
#ifndef SQLITE_OMIT_AUTOINIT
    int rc = sqlite3_initialize();
    if (rc) return rc;
#endif
    MUTEX_LOGIC(mutex = sqltie3MutexAlloc(SQLITE_MUTEX_STATIC_MASTER));
    sqlite3_mutex_enter(mutex);
    vfsUnlink(pVfs);
    if (makeDflt || vfsList ==0)
    {
        pVfs->pNext = vfsList;
        vfsList = pVfs;
    }
    else
    {
        pVfs->pNext = vfsList->pNext;
        vfsList->pNext = pVfs;
    }

    assert(vfsList);
    sqlite3_mutex_leave(mutex);
    return SQLITE_OK;
}

/*
** Unregister a VFS so that it is no longer accessible.
*/
int sqlite3_vfs_unregister(sqlite3_vfs* pVfs)
{
#if SQLTIE_THREADSAFE
    sqlite3_mutex* mutex = sqltie3MutexAlloc(SQLITE_MUTEX_STATIC_MASTER);
#endif
    sqlite3_mutex_enter(mutex);
    vfsUnlink(pVfs);
    sqlite3_mutex_leave(mutex);
    return SQLITE_OK;
}