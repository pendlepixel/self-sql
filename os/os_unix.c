/*
** 2004 May 22
**
** The author disclaims copyright to this source code.  In place of
** a legal notice, here is a blessing:
**
**    May you do good and not evil.
**    May you find forgiveness for yourself and forgive others.
**    May you share freely, never taking more than you give.
**
******************************************************************************
**
** This file contains the VFS implementation for unix-like operating systems
** include Linux, MacOSX, *BSD, QNX, VxWorks, AIX, HPUX, and others.
** 这个文件包含包括Linux,MacOSX,* BSD,QNX,VxWorks,AIX,HPUX等类unix操作系统的VFS实现。
**
** There are actually several different VFS implementations in this file.
** The differences are in the way that file locking is done.  The default
** implementation uses Posix Advisory Locks.  Alternative implementations
** use flock(), dot-files, various proprietary locking schemas, or simply
** skip locking all together.
** 实际上在这个文件中有几种不同的VFS实现(VFS的作用就是采用标准的Unix系统调
** 用读写位于不同物理介质上的不同文件系统。VFS是一个可以让open()、read()、write()
** 等系统调用不用关心底层的存储介质和文件系统类型就可以工作的粘合层。)
** 差异在于文件加锁的方式。默认的实现是使用Posix咨询锁。替代实现使用flock(),dot文件,
** 各种专有锁定模式,或者只是跳过锁定在一起.
**
** This source file is organized into divisions where the logic for various
** subfunctions is contained within the appropriate division.  PLEASE
** KEEP THE STRUCTURE OF THIS FILE INTACT.  New code should be placed
** in the correct division and should be clearly labeled.
** 这些源文件组成了包含了各种逻辑子函数的分区.请保持这个文件结构的完整
** 新代码应放置在正确的分区,并且都应有明确的标示。
**
** The layout of divisions is as follows:
**
**   *  General-purpose declarations and utility functions.  //通用的声明和实用功能
**   *  Unique file ID logic used by VxWorks.  //VxWorks所使用的唯一的文件ID逻辑
**   *  Various locking primitive implementations (all except proxy locking):  //锁定原语的实现（除了代理锁定）
**      + for Posix Advisory Locks  //Posix咨询锁
**      + for no-op locks  //无操作锁
**      + for dot-file locks  //点文件锁
**      + for flock() locking  //聚集锁
**      + for named semaphore locks (VxWorks only)  //命名信号锁（只存在于VxWorks）
**      + for AFP filesystem locks (MacOSX only)  //AFP系统文件锁（只存在于MacOSX）
**   *  sqlite3_file methods not associated with locking.  //sqlite_file方法和加锁没有关系
**   *  Definitions of sqlite3_io_methods objects for all locking
**      methods plus "finder" functions for each locking method.
**   //sqlite3_io_methods对象的定义为所有锁定方法增加"finder"功能
**   *  sqlite3_vfs method implementations.  //sqlite3_vfs方法实现
**   *  Locking primitives for the proxy uber-locking-method. (MacOSX only)
**   //proxy uber-locking-method的锁定原语（只在MacOSX上有）
**   *  Definitions of sqlite3_vfs objects for all locking methods
**      plus implementations of sqlite3_os_init() and sqlite3_os_end().
**   //为所有加锁方法定义了sqlite3_vfs对象，增加了sqlite3_os_init() 和 sqlite3_os_end()的实现.
*/
#include "../sqliteInt.h"
#if SQLITE_OS_UNIX              /* This file is used on unix only */

#define OS_VXWORKS 0

/*
** There are various methods for file locking used for concurrency  //多种用于并发控制的文件加锁的方法
** control:
**
**   1. POSIX locking (the default),
**   2. No locking,
**   3. Dot-file locking,
**   4. flock() locking,
**   5. AFP locking (OSX only),
**   6. Named POSIX semaphores (VXWorks only),
**   7. proxy locking. (OSX only)
**
** Styles 4, 5, and 7 are only available of SQLITE_ENABLE_LOCKING_STYLE
** is defined to 1.  The SQLITE_ENABLE_LOCKING_STYLE also enables automatic
** selection of the appropriate locking style based on the filesystem
** where the database is located.  
** 4、5、7只可用定义为1的SQLITE_ENABLE_LOCKING_STYLE。SQLITE_ENABLE_LOCKING_STYLE
** 还支持基于数据库所在的文件系统自动选择适当的加锁风格。
*/
#if !defined(SQLITE_ENABLE_LOCKING_STYLE)
#    define SQLITE_ENABLE_LOCKING_STYLE 0
#endif

/* Use pread() and pwrite() if they are available */
#if defined(HAVE_PREAD64) && defined(HAVE_PWRITE64)
# undef USE_PREAD
# define USE_PREAD64 1
#elif defined(HAVE_PREAD) && defined(HAVE_PWRITE)
# undef USE_PREAD64
# define USE_PREAD 1
#endif

/*
** standard include files.
*/
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <time.h>
#include <sys/time.h>
#include <errno.h>
#if !defined(SQLITE_OMIT_WAL) || SQLITE_MAX_MMAP_SIZE>0
# include <sys/mman.h>
#endif

#if SQLITE_ENABLE_LOCKING_STYLE
# include <sys/ioctl.h>
# include <sys/file.h>
# include <sys/param.h>
#endif /* SQLITE_ENABLE_LOCKING_STYLE */

/*
** Try to determine if gethostuuid() is available based on standard
** macros.  This might sometimes compute the wrong value for some
** obscure platforms.  For those cases, simply compile with one of
** the following:
**
**    -DHAVE_GETHOSTUUID=0
**    -DHAVE_GETHOSTUUID=1
**
** None if this matters except when building on Apple products with
** -DSQLITE_ENABLE_LOCKING_STYLE.
*/
#ifndef HAVE_GETHOSTUUID
# define HAVE_GETHOSTUUID 0


#ifdef HAVE_UTIME
# include <utime.h>
#endif

/*
** Allowed values of unixFile.fsFlags  //unixFile.fsFlags允许的值
*/
#define SQLITE_FSFLAGS_IS_MSDOS     0x1

/*
** If we are to be thread-safe, include the pthreads header.
** //如果希望线程安全，包裹线程头部
*/
#if SQLITE_THREADSAFE
# include <pthread.h>
#endif

/*
** Default permissions when creating a new file
** //创建一个新文件时的默认权限
*/
#ifndef SQLITE_DEFAULT_FILE_PERMISSIONS
# define SQLITE_DEFAULT_FILE_PERMISSIONS 0644
#endif

/*
** Default permissions when creating auto proxy dir
** //创建自动代理路径时的默认权限
*/
#ifndef SQLITE_DEFAULT_PROXYDIR_PERMISSIONS
# define SQLITE_DEFAULT_PROXYDIR_PERMISSIONS 0755
#endif

/*
** Maximum supported path-length.
** //路径长度的最大支持值
*/
#define MAX_PATHNAME 512

/*
** Maximum supported symbolic links
** //最大支持的软链接数
*/
#define SQLITE_MAX_SYMLINKS 100

/* Always cast the getpid() return type for compatibility with
** kernel modules in VxWorks. */
#define osGetpid(X) (pid_t)getpid()

/*
** Only set the lastErrno if the error code is a real error and not 
** a normal expected return code of SQLITE_BUSY or SQLITE_OK
** //如果错误代码是一个真正的错误，而不是一个正常的预期的SQLITE_BUSY或SQLITE_OK返回代码
** //则，只能设置lastErrno
*/
#define IS_LOCK_ERROR(x)  ((x != SQLITE_OK) && (x != SQLITE_BUSY))

/* Forward references */  //前向引用
typedef struct unixShm unixShm;               /* Connection shared memory */
typedef struct unixShmNode unixShmNode;       /* Shared memory instance */
typedef struct unixInodeInfo unixInodeInfo;   /* An i-node */
typedef struct UnixUnusedFd UnixUnusedFd;     /* An unused file descriptor */

/*
** Sometimes, after a file handle is closed by SQLite, the file descriptor
** cannot be closed immediately. In these cases, instances of the following
** structure are used to store the file descriptor while waiting for an
** opportunity to either close or reuse it.
** //有时，sqlite关闭文件句柄后，不能立即关闭文件描述符。在这些情况下，下面的结构实例用来
** //存储文件描述符，同时等待机会，要么关闭或重用它。
*/
struct UnixUnusedFd {
  int fd;                   /* File descriptor to close */  //要关闭的文件描述符
  int flags;                /* Flags this file descriptor was opened with */  //打开此文件描述符的标志
  UnixUnusedFd *pNext;      /* Next unused file descriptor on same file */  //同一文件的下一个未使用的文件描述符
};

/*
** The unixFile structure is subclass of sqlite3_file specific to the unix
** VFS implementations.
** //unixFile结构体是sqlite3_file特定于unix VFS的子类实现
*/
typedef struct unixFile unixFile;
struct unixFile {
  sqlite3_io_methods const *pMethod;  /* Always the first entry */  //总是第一个进入
  sqlite3_vfs *pVfs;                  /* The VFS that created this unixFile */  //创建这个unixFile的VFS
  unixInodeInfo *pInode;              /* Info about locks on this inode */  //关于这个i节点上的锁的消息
  int h;                              /* The file descriptor */  //文件描述符
  unsigned char eFileLock;            /* The type of lock held on this fd */  //fd（上面定义的文件描述符）上加锁的类型
  unsigned short int ctrlFlags;       /* Behavioral bits.  UNIXFILE_* flags */  //行为位
  int lastErrno;                      /* The unix errno from last I/O error */  //最后一个输入/删除错误的unix错误
  void *lockingContext;               /* Locking style specific state */  //加锁类型特定的状态
  UnixUnusedFd *pPreallocatedUnused;  /* Pre-allocated UnixUnusedFd */  //预分配的unixunusedFd
  const char *zPath;                  /* Name of the file */  //文件名
  unixShm *pShm;                      /* Shared memory segment information */  //共享内存段的信息  
  int szChunk;                        /* Configured by FCNTL_CHUNK_SIZE */  //由FCNTL_CHUNK_SIZE配置
#if SQLITE_MAX_MMAP_SIZE>0
  int nFetchOut;                      /* Number of outstanding xFetch refs */
  sqlite3_int64 mmapSize;             /* Usable size of mapping at pMapRegion */
  sqlite3_int64 mmapSizeActual;       /* Actual size of mapping at pMapRegion */
  sqlite3_int64 mmapSizeMax;          /* Configured FCNTL_MMAP_SIZE value */
  void *pMapRegion;                   /* Memory mapped region */
#endif
  int sectorSize;                     /* Device sector size */
  int deviceCharacteristics;          /* Precomputed device characteristics */
#if SQLITE_ENABLE_LOCKING_STYLE
  int openFlags;                      /* The flags specified at open() */  //指定的open()标志
#endif
#if SQLITE_ENABLE_LOCKING_STYLE || defined(__APPLE__)
  unsigned fsFlags;                   /* cached details from statfs() */  //statfs()的缓存的详细信息
#endif
#ifdef SQLITE_ENABLE_SETLK_TIMEOUT
  unsigned iBusyTimeout;              /* Wait this many millisec on locks */
#endif

#ifdef SQLITE_TEST
  /* In test mode, increase the size of this structure a bit so that 
  ** it is larger than the struct CrashFile defined in test6.c.
  ** //在测试模式下，增加一点此结构的大小，让它大于CrashFile在test6.c中定义的结构。
  */
  char aPadding[32];
#endif
};

/* This variable holds the process id (pid) from when the xRandomness()
** method was called.  If xOpen() is called from a different process id,
** indicating that a fork() has occurred, the PRNG will be reset.
*/
static pid_t randomnessPid = 0;

/*
** Allowed values for the unixFile.ctrlFlags bitmask:
** //unixFile.ctrlFlags位掩码允许的值
*/
#define UNIXFILE_EXCL        0x01     /* Connections from one process only */  //仅来自一个进程的连接
#define UNIXFILE_RDONLY      0x02     /* Connection is read only */  //仅读连接
#define UNIXFILE_PERSIST_WAL 0x04     /* Persistent WAL mode */  //持续的WAL模式
#ifndef SQLITE_DISABLE_DIRSYNC
# define UNIXFILE_DIRSYNC    0x08     /* Directory sync needed */  //所需的目录同步
#else
# define UNIXFILE_DIRSYNC    0x00
#endif
#define UNIXFILE_PSOW        0x10     /* SQLITE_IOCAP_POWERSAFE_OVERWRITE */
#define UNIXFILE_DELETE      0x20     /* Delete on close */  //关闭后删除
#define UNIXFILE_URI         0x40     /* Filename might have query parameters */  //文件名可能有查询参数
#define UNIXFILE_NOLOCK      0x80     /* Do no file locking */   //没有文件锁定

/*
** Include code that is common to all os_*.c files  //包含了所有os_*.c文件通用的代码
*/
#include "os_common.h"

/*
** Define various macros that are missing from some systems.  //定义了许多系统都缺少的各种宏
*/
#ifndef O_LARGEFILE
# define O_LARGEFILE 0
#endif
#ifdef SQLITE_DISABLE_LFS
# undef O_LARGEFILE
# define O_LARGEFILE 0
#endif
#ifndef O_NOFOLLOW
# define O_NOFOLLOW 0
#endif
#ifndef O_BINARY
# define O_BINARY 0
#endif

/*
** The threadid macro resolves to the thread-id or to 0.  Used for
** testing and debugging only.
** //threadid宏解析到线程id或0，只用于测试和调试
*/
#if SQLITE_THREADSAFE
#define threadid pthread_self()
#else
#define threadid 0
#endif

/*
** HAVE_MREMAP defaults to true on Linux and false everywhere else.
*/
#if !defined(HAVE_MREMAP)
# if defined(__linux__) && defined(_GNU_SOURCE)
#  define HAVE_MREMAP 1
# else
#  define HAVE_MREMAP 0
# endif
#endif


#ifdef __linux__
/*
** Linux-specific IOCTL magic numbers used for controlling F2FS
*/
#define F2FS_IOCTL_MAGIC        0xf5
#define F2FS_IOC_START_ATOMIC_WRITE     _IO(F2FS_IOCTL_MAGIC, 1)
#define F2FS_IOC_COMMIT_ATOMIC_WRITE    _IO(F2FS_IOCTL_MAGIC, 2)
#define F2FS_IOC_START_VOLATILE_WRITE   _IO(F2FS_IOCTL_MAGIC, 3)
#define F2FS_IOC_ABORT_VOLATILE_WRITE   _IO(F2FS_IOCTL_MAGIC, 5)
#define F2FS_IOC_GET_FEATURES           _IOR(F2FS_IOCTL_MAGIC, 12, u32)
#define F2FS_FEATURE_ATOMIC_WRITE 0x0004
#endif /* __linux__ */


/*
** Different Unix systems declare open() in different ways.  Same use
** open(const char*,int,mode_t).  Others use open(const char*,int,...).
** The difference is important when using a pointer to the function.
** //不同的unix系统已不同的方式声明open().同样适用open(const char*, int mode_t).
** //其他使用open(const char*, int, ...)。当使用指向函数的指针时，这些区别是很重要的。
**
** The safest way to deal with the problem is to always use this wrapper
** which always has the same well-defined interface.
** //处理这个问题最安全的方式是始终使用这个总是具有相同的定义良好的接口的封装
*/
static int posixOpen(const char *zFile, int flags, int mode){
  return open(zFile, flags, mode);
}

/* Forward reference */
static int openDirectory(const char*, int*);
static int unixGetpagesize(void);

/*
** Many system calls are accessed through pointer-to-functions so that
** they may be overridden at runtime to facilitate fault injection during
** testing and sandboxing.  The following array holds the names and pointers
** to all overrideable system calls.
** //可以通过指针函数访问很多的系统调用，因为，他们可能会在运行时被重写，以便在测试和沙盒中故障注入。
** //下面的数组保存着有可重写的系统调用的名称和指针
*/
static struct unix_syscall {
  const char *zName;            /* Name of the system call */  //系统调用名称
  sqlite3_syscall_ptr pCurrent; /* Current value of the system call */  //系统调用的当前值
  sqlite3_syscall_ptr pDefault; /* Default value */  //默认值
} aSyscall[] = {
  { "open",         (sqlite3_syscall_ptr)posixOpen,  0  },
#define osOpen      ((int(*)(const char*,int,int))aSyscall[0].pCurrent)

  { "close",        (sqlite3_syscall_ptr)close,      0  },
#define osClose     ((int(*)(int))aSyscall[1].pCurrent)

  { "access",       (sqlite3_syscall_ptr)access,     0  },
#define osAccess    ((int(*)(const char*,int))aSyscall[2].pCurrent)

  { "getcwd",       (sqlite3_syscall_ptr)getcwd,     0  },
#define osGetcwd    ((char*(*)(char*,size_t))aSyscall[3].pCurrent)

  { "stat",         (sqlite3_syscall_ptr)stat,       0  },
#define osStat      ((int(*)(const char*,struct stat*))aSyscall[4].pCurrent)

/*
** The DJGPP compiler environment looks mostly like Unix, but it
** lacks the fcntl() system call.  So redefine fcntl() to be something
** that always succeeds.  This means that locking does not occur under
** DJGPP.  But it is DOS - what did you expect?
** //DJGPP编译器环境看起来像unix，但他缺少fcntl()系统调用。所以重新定义fcntl()的时候总是会成功。
** //这意味着锁定不会出现在DJGPP之下。但它可以是DOS-
*/
#ifdef __DJGPP__
  { "fstat",        0,                 0  },
#define osFstat(a,b,c)    0
#else     
  { "fstat",        (sqlite3_syscall_ptr)fstat,      0  },
#define osFstat     ((int(*)(int,struct stat*))aSyscall[5].pCurrent)
#endif

  { "ftruncate",    (sqlite3_syscall_ptr)ftruncate,  0  },
#define osFtruncate ((int(*)(int,off_t))aSyscall[6].pCurrent)

  { "fcntl",        (sqlite3_syscall_ptr)fcntl,      0  },
#define osFcntl     ((int(*)(int,int,...))aSyscall[7].pCurrent)

  { "read",         (sqlite3_syscall_ptr)read,       0  },
#define osRead      ((ssize_t(*)(int,void*,size_t))aSyscall[8].pCurrent)

#if defined(USE_PREAD) || SQLITE_ENABLE_LOCKING_STYLE
  { "pread",        (sqlite3_syscall_ptr)pread,      0  },
#else
  { "pread",        (sqlite3_syscall_ptr)0,          0  },
#endif
#define osPread     ((ssize_t(*)(int,void*,size_t,off_t))aSyscall[9].pCurrent)

#if defined(USE_PREAD64)
  { "pread64",      (sqlite3_syscall_ptr)pread64,    0  },
#else
  { "pread64",      (sqlite3_syscall_ptr)0,          0  },
#endif
#define osPread64 ((ssize_t(*)(int,void*,size_t,off64_t))aSyscall[10].pCurrent)

  { "write",        (sqlite3_syscall_ptr)write,      0  },
#define osWrite     ((ssize_t(*)(int,const void*,size_t))aSyscall[11].pCurrent)

#if defined(USE_PREAD) || SQLITE_ENABLE_LOCKING_STYLE
  { "pwrite",       (sqlite3_syscall_ptr)pwrite,     0  },
#else
  { "pwrite",       (sqlite3_syscall_ptr)0,          0  },
#endif
#define osPwrite    ((ssize_t(*)(int,const void*,size_t,off_t))\
                    aSyscall[12].pCurrent)

#if defined(USE_PREAD64)
  { "pwrite64",     (sqlite3_syscall_ptr)pwrite64,   0  },
#else
  { "pwrite64",     (sqlite3_syscall_ptr)0,          0  },
#endif
#define osPwrite64  ((ssize_t(*)(int,const void*,size_t,off64_t))\
                    aSyscall[13].pCurrent)

  { "fchmod",       (sqlite3_syscall_ptr)fchmod,          0  },
#define osFchmod    ((int(*)(int,mode_t))aSyscall[14].pCurrent)

#if defined(HAVE_POSIX_FALLOCATE) && HAVE_POSIX_FALLOCATE
  { "fallocate",    (sqlite3_syscall_ptr)posix_fallocate,  0 },
#else
  { "fallocate",    (sqlite3_syscall_ptr)0,                0 },
#endif
#define osFallocate ((int(*)(int,off_t,off_t))aSyscall[15].pCurrent)

  { "unlink",       (sqlite3_syscall_ptr)unlink,           0 },
#define osUnlink    ((int(*)(const char*))aSyscall[16].pCurrent)

  { "openDirectory",    (sqlite3_syscall_ptr)openDirectory,      0 },
#define osOpenDirectory ((int(*)(const char*,int*))aSyscall[17].pCurrent)

  { "mkdir",        (sqlite3_syscall_ptr)mkdir,           0 },
#define osMkdir     ((int(*)(const char*,mode_t))aSyscall[18].pCurrent)

  { "rmdir",        (sqlite3_syscall_ptr)rmdir,           0 },
#define osRmdir     ((int(*)(const char*))aSyscall[19].pCurrent)

#if defined(HAVE_FCHOWN)
  { "fchown",       (sqlite3_syscall_ptr)fchown,          0 },
#else
  { "fchown",       (sqlite3_syscall_ptr)0,               0 },
#endif
#define osFchown    ((int(*)(int,uid_t,gid_t))aSyscall[20].pCurrent)

#if defined(HAVE_FCHOWN)
  { "geteuid",      (sqlite3_syscall_ptr)geteuid,         0 },
#else
  { "geteuid",      (sqlite3_syscall_ptr)0,               0 },
#endif
#define osGeteuid   ((uid_t(*)(void))aSyscall[21].pCurrent)

#if !defined(SQLITE_OMIT_WAL) || SQLITE_MAX_MMAP_SIZE>0
  { "mmap",         (sqlite3_syscall_ptr)mmap,            0 },
#else
  { "mmap",         (sqlite3_syscall_ptr)0,               0 },
#endif
#define osMmap ((void*(*)(void*,size_t,int,int,int,off_t))aSyscall[22].pCurrent)

#if !defined(SQLITE_OMIT_WAL) || SQLITE_MAX_MMAP_SIZE>0
  { "munmap",       (sqlite3_syscall_ptr)munmap,          0 },
#else
  { "munmap",       (sqlite3_syscall_ptr)0,               0 },
#endif
#define osMunmap ((int(*)(void*,size_t))aSyscall[23].pCurrent)

#if HAVE_MREMAP && (!defined(SQLITE_OMIT_WAL) || SQLITE_MAX_MMAP_SIZE>0)
  { "mremap",       (sqlite3_syscall_ptr)mremap,          0 },
#else
  { "mremap",       (sqlite3_syscall_ptr)0,               0 },
#endif
#define osMremap ((void*(*)(void*,size_t,size_t,int,...))aSyscall[24].pCurrent)

#if !defined(SQLITE_OMIT_WAL) || SQLITE_MAX_MMAP_SIZE>0
  { "getpagesize",  (sqlite3_syscall_ptr)unixGetpagesize, 0 },
#else
  { "getpagesize",  (sqlite3_syscall_ptr)0,               0 },
#endif
#define osGetpagesize ((int(*)(void))aSyscall[25].pCurrent)

#if defined(HAVE_READLINK)
  { "readlink",     (sqlite3_syscall_ptr)readlink,        0 },
#else
  { "readlink",     (sqlite3_syscall_ptr)0,               0 },
#endif
#define osReadlink ((ssize_t(*)(const char*,char*,size_t))aSyscall[26].pCurrent)

#if defined(HAVE_LSTAT)
  { "lstat",         (sqlite3_syscall_ptr)lstat,          0 },
#else
  { "lstat",         (sqlite3_syscall_ptr)0,              0 },
#endif
#define osLstat      ((int(*)(const char*,struct stat*))aSyscall[27].pCurrent)

  { "ioctl",         (sqlite3_syscall_ptr)0,              0 },

}; /* End of the overrideable system calls */  //可重写系统调用结束


/*
** On some systems, calls to fchown() will trigger a message in a security
** log if they come from non-root processes.  So avoid calling fchown() if
** we are not running as root.
*/
static int robustFchown(int fd, uid_t uid, gid_t gid){
#if defined(HAVE_FCHOWN)
  return osGeteuid() ? 0 : osFchown(fd,uid,gid);
#else
  return 0;
#endif
}

/*
** This is the xSetSystemCall() method of sqlite3_vfs for all of the
** "unix" VFSes.  Return SQLITE_OK opon successfully updating the
** system call pointer, or SQLITE_NOTFOUND if there is no configurable
** system call named zName.
** //这是sqlite3_vfs中所有“unix” VFSes的xSetSystemCall()方法。成功地更新系统调用指针并返回SQLTIE_OK,
** //如果存在没有名为zName的可配置的系统调用就返回SQLITE_NOTFOUND。
*/
static int unixSetSystemCall(
  sqlite3_vfs *pNotUsed,        /* The VFS pointer.  Not used */  //VFS的指针，不使用
  const char *zName,            /* Name of system call to override */  //系统调用重写的名称
  sqlite3_syscall_ptr pNewFunc  /* Pointer to new system call value */  //指向新的系统调用值的指针
){
  unsigned int i;
  int rc = SQLITE_NOTFOUND;

  UNUSED_PARAMETER(pNotUsed);
  if( zName==0 ){
    /* If no zName is given, restore all system calls to their default
    ** settings and return NULL
    ** //如果没有给出zName（系统调用的名字）,所有的系统调用还原为其默认位置并返回NULL
    */
    rc = SQLITE_OK;
    for(i=0; i<sizeof(aSyscall)/sizeof(aSyscall[0]); i++){
      if( aSyscall[i].pDefault ){
        aSyscall[i].pCurrent = aSyscall[i].pDefault;
      }
    }
  }else{
    /* If zName is specified, operate on only the one system call
    ** specified.
    ** //如果指定了zName, 只能运行在一个指定的系统调用上
    */
    for(i=0; i<sizeof(aSyscall)/sizeof(aSyscall[0]); i++){
      if( strcmp(zName, aSyscall[i].zName)==0 ){
        if( aSyscall[i].pDefault==0 ){
          aSyscall[i].pDefault = aSyscall[i].pCurrent;
        }
        rc = SQLITE_OK;
        if( pNewFunc==0 ) pNewFunc = aSyscall[i].pDefault;
        aSyscall[i].pCurrent = pNewFunc;
        break;
      }
    }
  }
  return rc;
}

/*
** Return the value of a system call.  Return NULL if zName is not a
** recognized system call name.  NULL is also returned if the system call
** is currently undefined.
** //返回系统调用的值。如果zName不是一个被认可的系统调用名称，返回NULL。如果系统调用目前未定义，也返回NULL。
*/
static sqlite3_syscall_ptr unixGetSystemCall(
  sqlite3_vfs *pNotUsed,
  const char *zName
){
  unsigned int i;

  UNUSED_PARAMETER(pNotUsed);
  for(i=0; i<sizeof(aSyscall)/sizeof(aSyscall[0]); i++){
    if( strcmp(zName, aSyscall[i].zName)==0 ) return aSyscall[i].pCurrent;
  }
  return 0;
}

/*
** Return the name of the first system call after zName.  If zName==NULL
** then return the name of the first system call.  Return NULL if zName
** is the last system call or if zName is not the name of a valid
** system call.
** //在zName后返回的第一次系统调用的名称。如果zName==NULL, 则返回第一次系统调用的名称。如果zName是最后一次的系统调用。
** //或者zName不是一个有效的系统调用的名称，请返回NULL。
*/
static const char *unixNextSystemCall(sqlite3_vfs *p, const char *zName){
  int i = -1;

  UNUSED_PARAMETER(p);
  if( zName ){
    for(i=0; i<ArraySize(aSyscall)-1; i++){
      if( strcmp(zName, aSyscall[i].zName)==0 ) break;
    }
  }
  for(i++; i<ArraySize(aSyscall); i++){
    if( aSyscall[i].pCurrent!=0 ) return aSyscall[i].zName;
  }
  return 0;
}

/*
** Do not accept any file descriptor less than this value, in order to avoid
** opening database file using file descriptors that are commonly used for 
** standard input, output, and error.
*/
#ifndef SQLITE_MINIMUM_FILE_DESCRIPTOR
# define SQLITE_MINIMUM_FILE_DESCRIPTOR 3
#endif

/*
** Invoke open().  Do so multiple times, until it either succeeds or
** fails for some reason other than EINTR.
** //调用open()。重复做多次，知道它不是成功就是因为EINTR以外的某种原因失败（EINTR:linux中函数的返回状态，在不同的函数中意义不同）
**
** If the file creation mode "m" is 0 then set it to the default for
** SQLite.  The default is SQLITE_DEFAULT_FILE_PERMISSIONS (normally
** 0644) as modified by the system umask.  If m is not 0, then
** make the file creation mode be exactly m ignoring the umask.
** //如果文件的创建模式"m"为0，那么将它设置为SQLITE默认的。按照系统umask（umask：设置了用户创建文件的默认权限）
** //将默认值修改为SQLITE_DEFAULT_FILE_PERMISSIONS（通常是0644）。如果m不为0，然后将文件的创建模式设为完全忽略umask的m。
**
** The m parameter will be non-zero only when creating -wal, -journal,
** and -shm files.  We want those files to have *exactly* the same
** permissions as their original database, unadulterated by the umask.
** In that way, if a database file is -rw-rw-rw or -rw-rw-r-, and a
** transaction crashes and leaves behind hot journals, then any
** process that is able to write to the database will also be able to
** recover the hot journals.
** //只有在创建后缀名为-wal,-journal和-shm文件时，m参数将为非零。我们希望这些文件能有和他们原始的数据库“完全”相同的权限，
** //纯粹由umask设置。在这种方式中，如果数据库文件权限是-rw-rw-rw或-rw-rw-r-和交易崩溃留下的热日志，则任何能写入到数据库的
** //程序也能恢复热日志
*/
static int robust_open(const char *z, int f, mode_t m){
  int fd;
  mode_t m2 = m ? m : SQLITE_DEFAULT_FILE_PERMISSIONS;
  while(1){
#if defined(O_CLOEXEC)
    fd = osOpen(z,f|O_CLOEXEC,m2);
#else
    fd = osOpen(z,f,m2);
#endif
    if( fd<0 ){
      if( errno==EINTR ) continue;
      break;
    }
    if( fd>=SQLITE_MINIMUM_FILE_DESCRIPTOR ) break;
    osClose(fd);
    sqlite3_log(SQLITE_WARNING, 
                "attempt to open \"%s\" as file descriptor %d", z, fd);
    fd = -1;
    if( osOpen("/dev/null", O_RDONLY, m)<0 ) break;
  }
  if( fd>=0 ){
    if( m!=0 ){
      struct stat statbuf;
      if( osFstat(fd, &statbuf)==0 
       && statbuf.st_size==0
       && (statbuf.st_mode&0777)!=m 
      ){
        osFchmod(fd, m);
      }
    }
#if defined(FD_CLOEXEC) && (!defined(O_CLOEXEC) || O_CLOEXEC==0)
    osFcntl(fd, F_SETFD, osFcntl(fd, F_GETFD, 0) | FD_CLOEXEC);
#endif
  }
  return fd;
}

/*
** Helper functions to obtain and relinquish the global mutex. The
** global mutex is used to protect the unixInodeInfo and
** vxworksFileId objects used by this file, all of which may be 
** shared by multiple threads.
** //获得和放弃全局互斥锁的helper函数。全局互斥锁用户保护使用此文件中，所有的一切都可以由多个线程共享的unixInodeInfo
** //和vxwordsFileId的对象。
**
** Function unixMutexHeld() is used to assert() that the global mutex 
** is held when required. This function is only used as part of assert() 
** statements. e.g.
** //当有需要时，函数unixMutexHeld()用于使用全局互斥锁是所需的assert()函数。此函数仅用作assert()语句的一部分，例如：
**
**   unixEnterMutex()
**     assert( unixMutexHeld() );
**   unixEnterLeave()
**
** To prevent deadlock, the global unixBigLock must must be acquired
** before the unixInodeInfo.pLockMutex mutex, if both are held.  It is
** OK to get the pLockMutex without holding unixBigLock first, but if
** that happens, the unixBigLock mutex must not be acquired until after
** pLockMutex is released.
**
**      OK:     enter(unixBigLock),  enter(pLockInfo)
**      OK:     enter(unixBigLock)
**      OK:     enter(pLockInfo)
**   ERROR:     enter(pLockInfo), enter(unixBigLock)
*/
static sqlite3_mutex *unixBigLock = 0;
static void unixEnterMutex(void){
  assert( sqlite3_mutex_notheld(unixBigLock) );  /* Not a recursive mutex */
  sqlite3_mutex_enter(unixBigLock);
}
static void unixLeaveMutex(void){
  assert( sqlite3_mutex_held(unixBigLock) );
  sqlite3_mutex_leave(unixBigLock);
}


#ifdef SQLITE_HAVE_OS_TRACE
/*
** Helper function for printing out trace information from debugging
** binaries. This returns the string representation of the supplied
** integer lock-type.
** //Helper函数打印来自调试二进制文件的追踪信息。这里的返回值是提供的整型的加锁类型的字符串表示形式
*/
static const char *azFileLock(int eFileLock){
  switch( eFileLock ){
    case NO_LOCK: return "NONE";
    case SHARED_LOCK: return "SHARED";
    case RESERVED_LOCK: return "RESERVED";
    case PENDING_LOCK: return "PENDING";
    case EXCLUSIVE_LOCK: return "EXCLUSIVE";
  }
  return "ERROR";
}
#endif

#ifdef SQLITE_LOCK_TRACE
/*
** Print out information about all locking operations.
** //打印出所有加锁操作的信息
**
** This routine is used for troubleshooting locks on multithreaded
** platforms.  Enable by compiling with the -DSQLITE_LOCK_TRACE
** command-line option on the compiler.  This code is normally
** turned off.
** //这个程序用户在多线程的平台上的疑难解答锁。通过编译编译器的-DSQLITE_LOCK_TRACE命令行选项启用。此代码通常是关闭的。
*/
static int lockTrace(int fd, int op, struct flock *p){
  char *zOpName, *zType;
  int s;
  int savedErrno;
  if( op==F_GETLK ){
    zOpName = "GETLK";
  }else if( op==F_SETLK ){
    zOpName = "SETLK";
  }else{
    s = osFcntl(fd, op, p);
    sqlite3DebugPrintf("fcntl unknown %d %d %d\n", fd, op, s);
    return s;
  }
  if( p->l_type==F_RDLCK ){
    zType = "RDLCK";
  }else if( p->l_type==F_WRLCK ){
    zType = "WRLCK";
  }else if( p->l_type==F_UNLCK ){
    zType = "UNLCK";
  }else{
    assert( 0 );
  }
  assert( p->l_whence==SEEK_SET );
  s = osFcntl(fd, op, p);
  savedErrno = errno;
  sqlite3DebugPrintf("fcntl %d %d %s %s %d %d %d %d\n",
     threadid, fd, zOpName, zType, (int)p->l_start, (int)p->l_len,
     (int)p->l_pid, s);
  if( s==(-1) && op==F_SETLK && (p->l_type==F_RDLCK || p->l_type==F_WRLCK) ){
    struct flock l2;
    l2 = *p;
    osFcntl(fd, F_GETLK, &l2);
    if( l2.l_type==F_RDLCK ){
      zType = "RDLCK";
    }else if( l2.l_type==F_WRLCK ){
      zType = "WRLCK";
    }else if( l2.l_type==F_UNLCK ){
      zType = "UNLCK";
    }else{
      assert( 0 );
    }
    sqlite3DebugPrintf("fcntl-failure-reason: %s %d %d %d\n",
       zType, (int)l2.l_start, (int)l2.l_len, (int)l2.l_pid);
  }
  errno = savedErrno;
  return s;
}
#undef osFcntl
#define osFcntl lockTrace
#endif /* SQLITE_LOCK_TRACE */

/*
** Retry ftruncate() calls that fail due to EINTR
** //重试由于EINTR失败的ftruncate()调用
**
** All calls to ftruncate() within this file should be made through
** this wrapper.  On the Android platform, bypassing the logic below
** could lead to a corrupt database.
*/
static int robust_ftruncate(int h, sqlite3_int64 sz){
  int rc;
  do{ rc = osFtruncate(h,sz); }while( rc<0 && errno==EINTR );
  return rc;
}

/*
** This routine translates a standard POSIX errno code into something
** useful to the clients of the sqlite3 functions.  Specifically, it is
** intended to translate a variety of "try again" errors into SQLITE_BUSY
** and a variety of "please close the file descriptor NOW" errors into 
** SQLITE_IOERR
** //这个程序将标准的POSIX错误代码转化成sqlite3功能的客户端有用的东西。具体来说，
** //它将被翻译成各种“重试”错误为SQLITE_BUSY和各种“请立即关闭文件描述符”错误为SQLITE_IOERR
** 
** Errors during initialization of locks, or file system support for locks,
** should handle ENOLCK, ENOTSUP, EOPNOTSUPP separately.
** //锁或文件系统支持的锁的初始化期间发生的错误应分开成ENOLCK, ENOTSUP, ENPNOTSUPP来处理。
*/
static int sqliteErrorFromPosixError(int posixError, int sqliteIOErr) {
  assert( (sqliteIOErr == SQLITE_IOERR_LOCK) || 
          (sqliteIOErr == SQLITE_IOERR_UNLOCK) || 
          (sqliteIOErr == SQLITE_IOERR_RDLOCK) ||
          (sqliteIOErr == SQLITE_IOERR_CHECKRESERVEDLOCK) );
  switch (posixError) {
  case EACCES: 
  case EAGAIN:
  case ETIMEDOUT:
  case EBUSY:
  case EINTR:
  case ENOLCK:  
    /* random NFS retry error, unless during file system support 
     * introspection, in which it actually means what it says */
    //随机NFS重试错误，除非在文件系统支持自省期间，否则这实际上就意味着错误所说的内容
    return SQLITE_BUSY;
    
  case EPERM: 
    return SQLITE_PERM;
    
  default: 
    return sqliteIOErr;
  }
}


/******************************************************************************
****************** Begin Unique File ID Utility Used By VxWorks ***************
**
** On most versions of unix, we can get a unique ID for a file by concatenating
** the device number and the inode number.  But this does not work on VxWorks.
** On VxWorks, a unique file id must be based on the canonical filename.
** //对大多数版本的unix中，我们可以得到一个唯一的文件ID，通过串联设备数量和i节点数量。
** //但这不能用在VxWorks.在VxWorks上，一个唯一的文件id必须基于规范的文件名
**
** A pointer to an instance of the following structure can be used as a
** unique file ID in VxWorks.  Each instance of this structure contains
** a copy of the canonical filename.  There is also a reference count.  
** The structure is reclaimed when the number of pointers to it drops to
** zero.
** //一个指向下列结构体的一个实例指针可以用作VxWorks中的一个唯一的文件ID。这种结构体的每个实例包含一份规范的文件名。
** //还有一个引用计数。当指针指向它的数目降到零时回收结构体。
**
** There are never very many files open at one time and lookups are not
** a performance-critical path, so it is sufficient to put these
** structures on a linked list.
** //有很多文件从来不会打开一次，查找不是性能关键的路径，所以它足以将这些结构放在一个链表上。
*/
struct vxworksFileId {
  struct vxworksFileId *pNext;  /* Next in a list of them all */  //指向链表中的下一个
  int nRef;                     /* Number of references to this one */  //对这一引用的次数
  int nName;                    /* Length of the zCanonicalName[] string */  //zCanonicalName[]字符串的长度
  char *zCanonicalName;         /* Canonical filename */  //规范的文件名
};


/*************** End of Unique File ID Utility Used By VxWorks ****************
******************************************************************************/


/******************************************************************************
*************************** Posix Advisory Locking ****************************
**
** POSIX advisory locks are broken by design.  ANSI STD 1003.1 (1996)
** section 6.5.2.2 lines 483 through 490 specify that when a process
** sets or clears a lock, that operation overrides any prior locks set
** by the same process.  It does not explicitly say so, but this implies
** that it overrides locks set by the same process using a different
** file descriptor.  Consider this test case:
** //POSIX咨询锁是靠设计打破的。ANSI标准1003.1(1996)部分6.5.2.2 483到490行规定，
** //当一个进程设置或者清除一个锁的时候，这个操作将符会覆盖任何之前由相同进程设置的锁。
** //它并没有明确地说明，但是这意味着它将覆盖通过同一进程使用一个不同的文件描述符设置的锁。请考虑此测试用例
**
**       int fd1 = open("./file1", O_RDWR|O_CREAT, 0644);
**       int fd2 = open("./file2", O_RDWR|O_CREAT, 0644);
**
** Suppose ./file1 and ./file2 are really the same file (because
** one is a hard or symbolic link to the other) then if you set
** an exclusive lock on fd1, then try to get an exclusive lock
** on fd2, it works.  I would have expected the second lock to
** fail since there was already a lock on the file due to fd1.
** But not so.  Since both locks came from the same process, the
** second overrides the first, even though they were on different
** file descriptors opened on different file names.
** //假设./file1和./file2实际上是同一个文件（因为一个是硬链接或符号链接到另一个）,
** //然后如果你在fd1设置排它锁，然后试图在fd2获得排它锁，它的工作原理。我语气第二锁定失败，因为由于fd1文件已经锁定。
** //但不是这样的。因为锁来自相同的进程，第二个覆盖第一个，尽管他们在不同的文件名打开的不同的文件描述符上。
**
** This means that we cannot use POSIX locks to synchronize file access
** among competing threads of the same process.  POSIX locks will work fine
** to synchronize access for threads in separate processes, but not
** threads within the same process.
** //这意味着我们不能使用POSIX锁来同步竞争同一进程的线程之间的文件访问。POSIX锁可以同步不同进程的线程访问，
** //但是不能同步同一进程的线程访问
**
** To work around the problem, SQLite has to manage file locks internally
** on its own.  Whenever a new database is opened, we have to find the
** specific inode of the database file (the inode is determined by the
** st_dev and st_ino fields of the stat structure that fstat() fills in)
** and check for locks already existing on that inode.  When locks are
** created or removed, we have to look at our own internal record of the
** locks to see if another thread has previously set a lock on that same
** inode.
** //若想解决这一问题，SQLITE必须在其内部管理文件锁定。每当打开一个新的数据库，我们必须找到特定的数据库文件的i节点
** //（这个i节点由st_dev和st_ino字段的统计结构的fstat()函数填写，并且检查这个锁已经存在在这个i节点上了。）
** //当创建或者删除锁，我们要看看我们自己内部的锁记录，看看是否有另一个线程先在相同的i节点上设置了锁。
**
** (Aside: The use of inode numbers as unique IDs does not work on VxWorks.
** For VxWorks, we have to use the alternative unique ID system based on
** canonical filename and implemented in the previous division.)
** //顺便说句，不能再VxWorks上使用i节点编号作为唯一的ID。对于VxWorks，我们必须使用基于规范的文件名，并且在原先的部分实施。
**
** The sqlite3_file structure for POSIX is no longer just an integer file
** descriptor.  It is now a structure that holds the integer file
** descriptor and a pointer to a structure that describes the internal
** locks on the corresponding inode.  There is one locking structure
** per inode, so if the same inode is opened twice, both unixFile structures
** point to the same locking structure.  The locking structure keeps
** a reference count (so we will know when to delete it) and a "cnt"
** field that tells us its internal lock status.  cnt==0 means the
** file is unlocked.  cnt==-1 means the file has an exclusive lock.
** cnt>0 means there are cnt shared locks on the file.
** //POSIX的sqlite3_file结构不再仅仅是一个整型的文件描述符。它现在是一个包含整型的文件描述符和指向描述内部相应的
** //i节点的锁的结构的指针。每个i节点有一个锁结构，所以如果同意inode打开两次，两个unixFile结构指向同一个锁结构。
** //锁定结构保持引用计数（因此，我们将知道什么时候删除它）并且“cnt”字段告诉我们其内部锁状态。
** //cnt==0意味着文件没有加锁。cnt=-1意味着文件有一个排它锁。cnt>0意味着该文件上有cnt共享锁。
**
** Any attempt to lock or unlock a file first checks the locking
** structure.  The fcntl() system call is only invoked to set a 
** POSIX lock if the internal lock structure transitions between
** a locked and an unlocked state.
** //任何企图要锁定或者解锁文件首先检查锁结构。如果内部锁结构在锁定和解除锁定的状态之间转换,
** //才会调用fcntl()系统调用设置POSIX锁。
**
** But wait:  there are yet more problems with POSIX advisory locks.
** //POSIX咨询锁还是存在一些问题.
**
** If you close a file descriptor that points to a file that has locks,
** all locks on that file that are owned by the current process are
** released.  To work around this problem, each unixInodeInfo object
** maintains a count of the number of pending locks on tha inode.
** When an attempt is made to close an unixFile, if there are
** other unixFile open on the same inode that are holding locks, the call
** to close() the file descriptor is deferred until all of the locks clear.
** The unixInodeInfo structure keeps a list of file descriptors that need to
** be closed and that list is walked (and cleared) when the last lock
** clears.
** //如果你关闭一个指向已锁定的文件的文件描述符，由当前进程对该文件的设置的所有锁都会释放。
** //为了解决这个问题，每个unixInodeInfo对象维护在该i节点上挂起的锁的数目的计数。
** //当试图关闭一个unixFile，如果在该持有锁的i节点上有其他unixFile打开，close()关闭文件描述符的调用将推迟,
** //直到所有的锁都清除。unixInodeInfo结构体保存一个需要关闭的文件描述符列表，并且最有一个锁清除时该列表也将移除（清除）。
**
** Yet another problem:  LinuxThreads do not play well with posix locks.
** //另一个问题，LinuxThreads不能与posix locks很好地工作
**
** Many older versions of linux use the LinuxThreads library which is
** not posix compliant.  Under LinuxThreads, a lock created by thread
** A cannot be modified or overridden by a different thread B.
** Only thread A can modify the lock.  Locking behavior is correct
** if the appliation uses the newer Native Posix Thread Library (NPTL)
** on linux - with NPTL a lock created by thread A can override locks
** in thread B.  But there is no way to know at compile-time which
** threading library is being used.  So there is no way to know at
** compile-time whether or not thread A can override locks on thread B.
** One has to do a run-time check to discover the behavior of the
** current process.
** //许多旧版本的linux使用LinuxThreads库是不兼容posix的。在LinuxThreads下，线程A创建的锁不能由线程B修改或者重写。
** //只有线程A可以修改这个锁。如果应用程序在Linux上使用新的本机Posix线程库（NPTL），锁定行为是正确的。
** //使用NPTL线程A创建的锁可以重写线程B中的锁。但是没有办法知道在编译时使用的是哪一种线程库。
** //所以没有办法知道在编译时线程A是否可以重写线程B上的锁。必须在运行时检查发现当前进程的行为。
**
** SQLite used to support LinuxThreads.  But support for LinuxThreads
** was dropped beginning with version 3.7.0.  SQLite will still work with
** LinuxThreads provided that (1) there is no more than one connection 
** per database file in the same process and (2) database connections
** do not move across threads.
** //SQLTIE过去支持Linux线程。但从3.7.0版本开始就删除了对Linux线程的支持，SQLTIE只规定
** //(1)在同一进程中每个数据库文件没有超过一个以上的连接，并且
** //(2)数据库连接不会跨线程移动的情况下，依然会予以支持
*/

/*
** An instance of the following structure serves as the key used
** to locate a particular unixInodeInfo object.
** //以下结构体的一个实例用于查找一个特定的unixInodeInfo对象的键。
*/
struct unixFileId {
  dev_t dev;                  /* Device number */  //设备号

  /* We are told that some versions of Android contain a bug that
  ** sizes ino_t at only 32-bits instead of 64-bits. (See
  ** https://android-review.googlesource.com/#/c/115351/3/dist/sqlite3.c)
  ** To work around this, always allocate 64-bits for the inode number.  
  ** On small machines that only have 32-bit inodes, this wastes 4 bytes,
  ** but that should not be a big deal. */
  /* WAS:  ino_t ino;   */
  u64 ino;                   /* Inode number */  //i节点数目
};

/*
** An instance of the following structure is allocated for each open
** inode.
** //下面结构体的每一个实例分配给每个打开的i节点。
**
** A single inode can have multiple file descriptors, so each unixFile
** structure contains a pointer to an instance of this object and this
** object keeps a count of the number of unixFile pointing to it.
** //单一的i节点可以由多个文件描述符，因此每个unixFile结构包含一个指针，指向该对象的一个实例，
** //此对象保持unixFile指向它的数目的计数。
**
** Mutex rules:
**
**  (1) Only the pLockMutex mutex must be held in order to read or write
**      any of the locking fields:
**          nShared, nLock, eFileLock, bProcessLock, pUnused
**
**  (2) When nRef>0, then the following fields are unchanging and can
**      be read (but not written) without holding any mutex:
**          fileId, pLockMutex
**
**  (3) With the exceptions above, all the fields may only be read
**      or written while holding the global unixBigLock mutex.
**
** Deadlock prevention:  The global unixBigLock mutex may not
** be acquired while holding the pLockMutex mutex.  If both unixBigLock
** and pLockMutex are needed, then unixBigLock must be acquired first.
*/
struct unixInodeInfo {
  struct unixFileId fileId;       /* The lookup key */  //查找键
  sqlite3_mutex *pLockMutex;      /* Hold this mutex for... */  
  int nShared;                      /* Number of SHARED locks held */  //持有共享锁的数目
  int nLock;                        /* Number of outstanding file locks */  //未完成的文件锁定的数目
  unsigned char eFileLock;          /* One of SHARED_LOCK, RESERVED_LOCK etc. */
  unsigned char bProcessLock;       /* An exclusive process lock is held */  //独占进程锁
  UnixUnusedFd *pUnused;            /* Unused file descriptors to close */  //关闭未使用的文件描述符
  int nRef;                       /* Number of pointers to this structure */  //指向这个结构体的指针数目
  unixShmNode *pShmNode;          /* Shared memory associated with this inode */  //与这个i节点有关的共享内存
  unixInodeInfo *pNext;           /* List of all unixInodeInfo objects */  //所有的unixInodeInfo对象的列表
  unixInodeInfo *pPrev;           /*    .... doubly linked */  //双重链接
#if SQLITE_ENABLE_LOCKING_STYLE
  unsigned long long sharedByte;  /* for AFP simulated shared lock */  //用于模拟APF共享锁
#endif
};

/*
** A lists of all unixInodeInfo objects.
**
** Must hold unixBigLock in order to read or write this variable.
** //unixInodeInfo的所有对象的列表，为了读或者写这个变量，必须有unixBigLock
*/
static unixInodeInfo *inodeList = 0;  /* All unixInodeInfo objects */

/*
**
** This function - unixLogErrorAtLine(), is only ever called via the macro
** unixLogError().
** //这个unixLogErrorAtLine()函数永远只能通过宏定义的unixLogError()函数调用
**
** It is invoked after an error occurs in an OS function and errno has been
** set. It logs a message using sqlite3_log() containing the current value of
** errno and, if possible, the human-readable equivalent from strerror() or
** strerror_r().
** //在操作系统函数出现错误并且错误被设定之后调用它。它使用sqlite3_log()记录一条消息，消息包含当前errno的值，
** //如果可能，任何读的等效来自与strerror()或者strerror_r()
**
** The first argument passed to the macro should be the error code that
** will be returned to SQLite (e.g. SQLITE_IOERR_DELETE, SQLITE_CANTOPEN). 
** The two subsequent arguments should be the name of the OS function that
** failed (e.g. "unlink", "open") and the associated file-system path,
** if any.
** //传递给宏的第一个参数应是将被返回到SQLITE的（例如SQLITE_IOERR_DELETE, SQLITE_CANTOPEN）错误代码。
** //随后的两个参数应该是失败的操作系统函数名称（例如“取消链接”，“打开”）,以及相关的系统文件路径。
*/
#define unixLogError(a,b,c)     unixLogErrorAtLine(a,b,c,__LINE__)
static int unixLogErrorAtLine(
  int errcode,                    /* SQLite error code */  //SQLITE错误代码
  const char *zFunc,              /* Name of OS function that failed */  //失败的操作系统函数名称
  const char *zPath,              /* File path associated with error */  //错误的文件路径关联
  int iLine                       /* Source line number where error occurred */  //发生错误的位置的源代码行号
){
  char *zErr;                     /* Message from strerror() or equivalent */  //strerror()的或与之等效的消息
  int iErrno = errno;             /* Saved syscall error number */  //保存系统调用错误号

  /* If this is not a threadsafe build (SQLITE_THREADSAFE==0), then use
  ** the strerror() function to obtain the human-readable error message
  ** equivalent to errno. Otherwise, use strerror_r().
  ** //如果这不是一个线程安全构建(SQLITE_THREADSAFE == 0)，则使用strerror()函数来获得相当于errno的人可读的错误消息。
  ** //否则，使用strerror_r()。
  */ 
#if SQLITE_THREADSAFE && defined(HAVE_STRERROR_R)
  char aErr[80];
  memset(aErr, 0, sizeof(aErr));
  zErr = aErr;

  /* If STRERROR_R_CHAR_P (set by autoconf scripts) or __USE_GNU is defined,
  ** assume that the system provides the GNU version of strerror_r() that
  ** returns a pointer to a buffer containing the error message. That pointer 
  ** may point to aErr[], or it may point to some static storage somewhere. 
  ** Otherwise, assume that the system provides the POSIX version of 
  ** strerror_r(), which always writes an error message into aErr[].
  ** //如果定义了STRERROR_R_CHAR_P（由autoconf脚本设置）或__USE_GNU,
  ** //假定该系统提供的GNU版本的strerror_r()它返回一个指向包含错误信息的缓冲区，这个指针可以指向aErr[],
  ** //或者指向一些静态存储区域。否则，假设该系统提供的POSIX版本的strerror_r()，总是将错误信息写入到aErr[].
  **
  ** If the code incorrectly assumes that it is the POSIX version that is
  ** available, the error message will often be an empty string. Not a
  ** huge problem. Incorrectly concluding that the GNU version is available 
  ** could lead to a segfault though.
  ** //如果代码错误地假定它是可用的POSIX版本，错误消息通常会是一个空字符串。这不是一个大问题。
  ** //虽然不正确地结束GNU版本可用可能导致分段错误。
  */
#if defined(STRERROR_R_CHAR_P) || defined(__USE_GNU)
  zErr = 
# endif
  strerror_r(iErrno, aErr, sizeof(aErr)-1);

#elif SQLITE_THREADSAFE
  /* This is a threadsafe build, but strerror_r() is not available. */  //这是一个线程安全的构建，但是strerror_r()是不可用的
  zErr = "";
#else
  /* Non-threadsafe build, use strerror(). */  //非线程安全的构建，使用strerror()
  zErr = strerror(iErrno);
#endif

  if( zPath==0 ) zPath = "";
  sqlite3_log(errcode,
      "os_unix.c:%d: (%d) %s(%s) - %s",
      iLine, iErrno, zFunc, zPath, zErr
  );

  return errcode;
}

/*
** Close a file descriptor.
** //关闭一个文件描述符
**
** We assume that close() almost always works, since it is only in a
** very sick application or on a very sick platform that it might fail.
** If it does fail, simply leak the file descriptor, but do log the
** error.
** //因为只在一个非常恶心的应用程序或者在一个非常恶心的平台，
** //我们假设close()几乎总是工作的。如果他失败了，除了记录错误之外，仅仅泄露文件描述符
**
** Note that it is not safe to retry close() after EINTR since the
** file descriptor might have already been reused by another thread.
** So we don't even try to recover from an EINTR.  Just log the error
** and move on.
** //需要注意的是，由于文件描述符可能已经被另一个线程重新使用，在EINTR后重试
** //close()是不安全的。所以，我们甚至不尝试从EINTR恢复，只要记录错误然后继续执行
*/
static void robust_close(unixFile *pFile, int h, int lineno){
  if( osClose(h) ){
    unixLogErrorAtLine(SQLITE_IOERR_CLOSE, "close",
                       pFile ? pFile->zPath : 0, lineno);
  }
}

/*
** Set the pFile->lastErrno.  Do this in a subroutine as that provides
** a convenient place to set a breakpoint.
*/
static void storeLastErrno(unixFile *pFile, int error){
  pFile->lastErrno = error;
}

/*
** Close all file descriptors accumuated in the unixInodeInfo->pUnused list.
** //关闭所有存放在unixInodeInfo->pUnused列表中的文件描述符
*/ 
static void closePendingFds(unixFile *pFile){
  unixInodeInfo *pInode = pFile->pInode;
  UnixUnusedFd *p;
  UnixUnusedFd *pNext;
  assert( unixFileMutexHeld(pFile) );
  for(p=pInode->pUnused; p; p=pNext){
    pNext = p->pNext;
    robust_close(pFile, p->fd, __LINE__);
    sqlite3_free(p);
  }
  pInode->pUnused = 0;
}

/*
** Release a unixInodeInfo structure previously allocated by findInodeInfo().
** //释放之前findInodeInfo()分配一个unixInodeInfo结构
**
** The global mutex must be held when this routine is called, but the mutex
** on the inode being deleted must NOT be held.
** //当这个函数调用时，必须使用unixEnterMutex()函数输入的互斥量
*/
static void releaseInodeInfo(unixFile *pFile){
  unixInodeInfo *pInode = pFile->pInode;
  assert( unixMutexHeld() );
  assert( unixFileMutexNotheld(pFile) );
  if( ALWAYS(pInode) ){
    pInode->nRef--;
    if( pInode->nRef==0 ){
      assert( pInode->pShmNode==0 );
      sqlite3_mutex_enter(pInode->pLockMutex);
      closePendingFds(pFile);
      sqlite3_mutex_leave(pInode->pLockMutex);
      if( pInode->pPrev ){
        assert( pInode->pPrev->pNext==pInode );
        pInode->pPrev->pNext = pInode->pNext;
      }else{
        assert( inodeList==pInode );
        inodeList = pInode->pNext;
      }
      if( pInode->pNext ){
        assert( pInode->pNext->pPrev==pInode );
        pInode->pNext->pPrev = pInode->pPrev;
      }
      sqlite3_mutex_free(pInode->pLockMutex);
      sqlite3_free(pInode);
    }
  }
}

/*
** Given a file descriptor, locate the unixInodeInfo object that
** describes that file descriptor.  Create a new one if necessary.  The
** return value might be uninitialized if an error occurs.
** //给定一个文件描述符，定位unixInodeInfo对象描述文件描述符。在必要时创建一个新的。如果出现错误，返回值可能是未初始化的。
**
** The global mutex must held when calling this routine.
** //当这个函数被调用时，必须使用unixEnterMutex()函数输入的互斥量
**
** Return an appropriate error code.
** //返回相应的错误代码
*/
static int findInodeInfo(
  unixFile *pFile,               /* Unix file with file desc used in the key */  //降序排列的Unix文件使用的键
  unixInodeInfo **ppInode        /* Return the unixInodeInfo object here */  //返回unixInodeInfo对象
){
  int rc;                        /* System call return code */  //系统调用返回代码
  int fd;                        /* The file descriptor for pFile */  //pFile的文件描述符
  struct unixFileId fileId;      /* Lookup key for the unixInodeInfo */  //unixInodeInfo的查找键
  struct stat statbuf;           /* Low-level file information */  //底层文件信息
  unixInodeInfo *pInode = 0;     /* Candidate unixInodeInfo object */  //候选的unixInodeInfo对象

  assert( unixMutexHeld() );

  /* Get low-level information about the file that we can used to
  ** create a unique name for the file.
  ** //获得底层文件信息，我们可以用来为该文件创建一个唯一的名称。
  */
  fd = pFile->h;
  rc = osFstat(fd, &statbuf);
  if( rc!=0 ){
    storeLastErrno(pFile, errno);
#if defined(EOVERFLOW) && defined(SQLITE_DISABLE_LFS)
    if( pFile->lastErrno==EOVERFLOW ) return SQLITE_NOLFS;
#endif
    return SQLITE_IOERR;
  }

  memset(&fileId, 0, sizeof(fileId));
  fileId.dev = statbuf.st_dev;
  fileId.ino = (u64)statbuf.st_ino;
  assert( unixMutexHeld() );
  pInode = inodeList;
  while( pInode && memcmp(&fileId, &pInode->fileId, sizeof(fileId)) ){
    pInode = pInode->pNext;
  }
  if( pInode==0 ){
    pInode = sqlite3_malloc64( sizeof(*pInode) );
    if( pInode==0 ){
      return SQLITE_NOMEM_BKPT;
    }
    memset(pInode, 0, sizeof(*pInode));
    memcpy(&pInode->fileId, &fileId, sizeof(fileId));
    if( sqlite3GlobalConfig.bCoreMutex ){
      pInode->pLockMutex = sqlite3_mutex_alloc(SQLITE_MUTEX_FAST);
      if( pInode->pLockMutex==0 ){
        sqlite3_free(pInode);
        return SQLITE_NOMEM_BKPT;
      }
    }
    pInode->nRef = 1;
    assert( unixMutexHeld() );
    pInode->pNext = inodeList;
    pInode->pPrev = 0;
    if( inodeList ) inodeList->pPrev = pInode;
    inodeList = pInode;
  }else{
    pInode->nRef++;
  }
  *ppInode = pInode;
  return SQLITE_OK;
}

/*
** Return TRUE if pFile has been renamed or unlinked since it was first opened.
*/
static int fileHasMoved(unixFile *pFile){
  struct stat buf;
  return pFile->pInode!=0 &&
      (osStat(pFile->zPath, &buf)!=0 
         || (u64)buf.st_ino!=pFile->pInode->fileId.ino);
}


/*
** Check a unixFile that is a database.  Verify the following:
**
** (1) There is exactly one hard link on the file
** (2) The file is not a symbolic link
** (3) The file has not been renamed or unlinked
**
** Issue sqlite3_log(SQLITE_WARNING,...) messages if anything is not right.
*/
static void verifyDbFile(unixFile *pFile){
  struct stat buf;
  int rc;

  /* These verifications occurs for the main database only */
  if( pFile->ctrlFlags & UNIXFILE_NOLOCK ) return;

  rc = osFstat(pFile->h, &buf);
  if( rc!=0 ){
    sqlite3_log(SQLITE_WARNING, "cannot fstat db file %s", pFile->zPath);
    return;
  }
  if( buf.st_nlink==0 ){
    sqlite3_log(SQLITE_WARNING, "file unlinked while open: %s", pFile->zPath);
    return;
  }
  if( buf.st_nlink>1 ){
    sqlite3_log(SQLITE_WARNING, "multiple links to file: %s", pFile->zPath);
    return;
  }
  if( fileHasMoved(pFile) ){
    sqlite3_log(SQLITE_WARNING, "file renamed while open: %s", pFile->zPath);
    return;
  }
}


/*
** This routine checks if there is a RESERVED lock held on the specified
** file by this or any other process. If such a lock is held, set *pResOut
** to a non-zero value otherwise *pResOut is set to zero.  The return value
** is set to SQLITE_OK unless an I/O error occurs during lock checking.
** //这个例程检查这个进程或其他进程指定的文件是否持有保留锁。如果持有这样的锁，
** //设置*pResOut为非零值，否则设置为0,返回值设置为SQLITE_OK，除非在锁定期间出现I/O错误。
*/
static int unixCheckReservedLock(sqlite3_file *id, int *pResOut){
  int rc = SQLITE_OK;
  int reserved = 0;
  unixFile *pFile = (unixFile*)id;

  SimulateIOError( return SQLITE_IOERR_CHECKRESERVEDLOCK; );

  assert( pFile );
  assert( pFile->eFileLock<=SHARED_LOCK );
  sqlite3_mutex_enter(pFile->pInode->pLockMutex);

  /* Check if a thread in this process holds such a lock */  //检查是否这一进程中的线程持有此类锁
  if( pFile->pInode->eFileLock>SHARED_LOCK ){
    reserved = 1;
  }

  /* Otherwise see if some other process holds it.
  ** //否则，看看是否一些其他进程持有它
  */
#ifndef __DJGPP__
  if( !reserved && !pFile->pInode->bProcessLock ){
    struct flock lock;
    lock.l_whence = SEEK_SET;
    lock.l_start = RESERVED_BYTE;
    lock.l_len = 1;
    lock.l_type = F_WRLCK;
    if( osFcntl(pFile->h, F_GETLK, &lock) ){
      rc = SQLITE_IOERR_CHECKRESERVEDLOCK;
      storeLastErrno(pFile, errno);
    } else if( lock.l_type!=F_UNLCK ){
      reserved = 1;
    }
  }
#endif
  
  sqlite3_mutex_leave(pFile->pInode->pLockMutex);
  OSTRACE(("TEST WR-LOCK %d %d %d (unix)\n", pFile->h, rc, reserved));

  *pResOut = reserved;
  return rc;
}

/* Forward declaration*/  //前向声明
static int unixSleep(sqlite3_vfs*,int);

/*
** Set a posix-advisory-lock.
**
** There are two versions of this routine.  If compiled with
** SQLITE_ENABLE_SETLK_TIMEOUT then the routine has an extra parameter
** which is a pointer to a unixFile.  If the unixFile->iBusyTimeout
** value is set, then it is the number of milliseconds to wait before
** failing the lock.  The iBusyTimeout value is always reset back to
** zero on each call.
**
** If SQLITE_ENABLE_SETLK_TIMEOUT is not defined, then do a non-blocking
** attempt to set the lock.
*/
#ifndef SQLITE_ENABLE_SETLK_TIMEOUT
# define osSetPosixAdvisoryLock(h,x,t) osFcntl(h,F_SETLK,x)
#else
static int osSetPosixAdvisoryLock(
  int h,                /* The file descriptor on which to take the lock */
  struct flock *pLock,  /* The description of the lock */
  unixFile *pFile       /* Structure holding timeout value */
){
  int tm = pFile->iBusyTimeout;
  int rc = osFcntl(h,F_SETLK,pLock);
  while( rc<0 && tm>0 ){
    /* On systems that support some kind of blocking file lock with a timeout,
    ** make appropriate changes here to invoke that blocking file lock.  On
    ** generic posix, however, there is no such API.  So we simply try the
    ** lock once every millisecond until either the timeout expires, or until
    ** the lock is obtained. */
    unixSleep(0,1000);
    rc = osFcntl(h,F_SETLK,pLock);
    tm--;
  }
  return rc;
}
#endif /* SQLITE_ENABLE_SETLK_TIMEOUT */


/*
** Attempt to set a system-lock on the file pFile.  The lock is 
** described by pLock.
** //试图在文件pFile设置一个系统锁，这个锁由pLock描述
**
** If the pFile was opened read/write from unix-excl, then the only lock
** ever obtained is an exclusive lock, and it is obtained exactly once
** the first time any lock is attempted.  All subsequent system locking
** operations become no-ops.  Locking operations still happen internally,
** in order to coordinate access between separate database connections
** within this process, but all of that is handled in memory and the
** operating system does not participate.
** //如果pFile是从unix-excl打开读/写的，那么只有曾经获得的排它锁，这是第一次获得试图得到的任何的锁。
** //所有后续系统锁定操作成为空操作。锁定操作仍然发生在内部，以便协调在这一进程中的单独的数据库连接之间的访问,
** //但这一切都是在内存中处理，操作系统并不参与。
**
** This function is a pass-through to fcntl(F_SETLK) if pFile is using
** any VFS other than "unix-excl" or if pFile is opened on "unix-excl"
** and is read-only.
** //这个函数传递到fcntl（F_SETLK），如果pFile使用了除了“unix-excl”之外的任何VFS，或者pFile在“unix-excl”上时打开的并且是只读的。
**
** Zero is returned if the call completes successfully, or -1 if a call
** to fcntl() fails. In this case, errno is set appropriately (by fcntl()).
** //如果调用成功完成，返回零，或-1，如果调用fcnl()失败，在这种情况下，errno是设置正确的(通过fcntl())。
*/
static int unixFileLock(unixFile *pFile, struct flock *pLock){
  int rc;
  unixInodeInfo *pInode = pFile->pInode;
  assert( pInode!=0 );
  assert( sqlite3_mutex_held(pInode->pLockMutex) );
  if( (pFile->ctrlFlags & (UNIXFILE_EXCL|UNIXFILE_RDONLY))==UNIXFILE_EXCL ){
    if( pInode->bProcessLock==0 ){
      struct flock lock;
      assert( pInode->nLock==0 );
      lock.l_whence = SEEK_SET;
      lock.l_start = SHARED_FIRST;
      lock.l_len = SHARED_SIZE;
      lock.l_type = F_WRLCK;
      rc = osSetPosixAdvisoryLock(pFile->h, &lock, pFile);
      if( rc<0 ) return rc;
      pInode->bProcessLock = 1;
      pInode->nLock++;
    }else{
      rc = 0;
    }
  }else{
    rc = osSetPosixAdvisoryLock(pFile->h, pLock, pFile);
  }
  return rc;
}

/*
** Lock the file with the lock specified by parameter eFileLock - one
** of the following:
** 对文件进行由参数eFileLock指定的下列加锁
**
**     (1) SHARED_LOCK  共享锁
**     (2) RESERVED_LOCK  保留锁
**     (3) PENDING_LOCK  未决锁
**     (4) EXCLUSIVE_LOCK  排它锁
**
** Sometimes when requesting one lock state, additional lock states
** are inserted in between.  The locking might fail on one of the later
** transitions leaving the lock state different from what it started but
** still short of its goal.  The following chart shows the allowed
** transitions and the inserted intermediate states:
** 有时，请求一个锁定状态时，会插入额外的锁定状态。之后的转让锁的状态不同于它开始的状态，
** 锁定可能失败，达不到预期的目标，下面的图表显示了允许转换和插入的中间状态
**
**    UNLOCKED -> SHARED  未加锁 -> 共享
**    SHARED -> RESERVED  共享 -> 保留
**    SHARED -> (PENDING) -> EXCLUSIVE  共享 -> (未决) -> 排他
**    RESERVED -> (PENDING) -> EXCLUSIVE  保留 -> （未决）-> 排他
**    PENDING -> EXCLUSIVE  未决-> 排它
**
** This routine will only increase a lock.  Use the sqlite3OsUnlock()
** routine to lower a locking level.
** 这个程序只会增加锁。使用sqlite3OsUnlock()以降低锁定级别
*/
static int unixLock(sqlite3_file *id, int eFileLock){
  /* The following describes the implementation of the various locks and
  ** lock transitions in terms of the POSIX advisory shared and exclusive
  ** lock primitives (called read-locks and write-locks below, to avoid
  ** confusion with SQLite lock names). The algorithms are complicated
  ** slightly in order to be compatible with Windows95 systems simultaneously
  ** accessing the same database file, in case that is ever required.
  ** 下面描述了各种锁的执行和锁在POSIX咨询共享锁和排它锁之间转换的原语（以下称为读锁和写锁，以避免
  ** 混淆SQLITE的锁名称）算法略显复杂，以便兼容windows95系统同时访问统一数据库，如果有需要的话。
  **
  ** Symbols defined in os.h indentify the 'pending byte' and the 'reserved
  ** byte', each single bytes at well known offsets, and the 'shared byte
  ** range', a range of 510 bytes at a well known offset.
  ** 在os.h中定义的符号识别'pending byte' and the 'reserved byte'，每个字节在众所周知的偏移处，
  ** 'shared byte range'在众所周知的偏移处的510个字节范围内
  **
  ** To obtain a SHARED lock, a read-lock is obtained on the 'pending
  ** byte'.  If this is successful, 'shared byte range' is read-locked
  ** and the lock on the 'pending byte' released.  (Legacy note:  When
  ** SQLite was first developed, Windows95 systems were still very common,
  ** and Widnows95 lacks a shared-lock capability.  So on Windows95, a
  ** single randomly selected by from the 'shared byte range' is locked.
  ** Windows95 is now pretty much extinct, but this work-around for the
  ** lack of shared-locks on Windows95 lives on, for backwards
  ** compatibility.)
  ** 为了获得共享锁，要在'pending byte'获得读锁。如果成功了，从'shared byte range'得到的一个随机字节读
  ** 锁定，并且'pending byte'上的锁移除。
  **
  ** A process may only obtain a RESERVED lock after it has a SHARED lock.
  ** A RESERVED lock is implemented by grabbing a write-lock on the
  ** 'reserved byte'. 
  ** 在有一个共享锁后，一个进程可能只能获得保留的锁。通过获取'resereved byte'上的写锁执行这个保留锁
  **
  ** A process may only obtain a PENDING lock after it has obtained a
  ** SHARED lock. A PENDING lock is implemented by obtaining a write-lock
  ** on the 'pending byte'. This ensures that no new SHARED locks can be
  ** obtained, but existing SHARED locks are allowed to persist. A process
  ** does not have to obtain a RESERVED lock on the way to a PENDING lock.
  ** This property is used by the algorithm for rolling back a journal file
  ** after a crash.
  ** 在已经获得了共享锁之后，一个进程可能只获得一个未决锁。通过获得'pending byte'的写锁执行这个未决锁。
  ** 这确保了不能获得新的共享锁，但现有的共享锁还是允许继续下去。一个进程在获取未决锁的过程中不必获取共享锁。
  ** 该算法使用此属性在崩溃后回滚日志文件。
  **
  ** An EXCLUSIVE lock, obtained after a PENDING lock is held, is
  ** implemented by obtaining a write-lock on the entire 'shared byte
  ** range'. Since all other locks require a read-lock on one of the bytes
  ** within this range, this ensures that no other locks are held on the
  ** database. 
  ** 在持有未决锁后获得的排它锁通过整个'shared byte range'上的写锁实现。
  ** 因为所有其他锁需要在这个范围内的字节上读锁，这可以确保数据库上没有其他锁。
  */
  int rc = SQLITE_OK;
  unixFile *pFile = (unixFile*)id;
  unixInodeInfo *pInode;
  struct flock lock;
  int tErrno = 0;

  assert( pFile );
  OSTRACE(("LOCK    %d %s was %s(%s,%d) pid=%d (unix)\n", pFile->h,
      azFileLock(eFileLock), azFileLock(pFile->eFileLock),
      azFileLock(pFile->pInode->eFileLock), pFile->pInode->nShared,
      osGetpid(0)));

  /* If there is already a lock of this type or more restrictive on the
  ** unixFile, do nothing. Don't use the end_lock: exit path, as
  ** unixEnterMutex() hasn't been called yet.
  ** 如果已经存在此类型的锁或者unixfile上有更强的限制性，什么都不用做。不要使用
  ** end_lock:退出路径，也不调用unixEnterMutex()。
  */
  if( pFile->eFileLock>=eFileLock ){
    OSTRACE(("LOCK    %d %s ok (already held) (unix)\n", pFile->h,
            azFileLock(eFileLock)));
    return SQLITE_OK;
  }

  /* Make sure the locking sequence is correct.  确保锁定序列时正确的
  **  (1) We never move from unlocked to anything higher than shared lock.
  **  (2) SQLite never explicitly requests a pendig lock.
  **  (3) A shared lock is always held when a reserve lock is requested.
  **  (1) 不要从未加锁移动到高于共享锁的状态
  **  (2) SQLITE从未明确要求pending锁
  **  (3) 当请求一个保留锁时，共享锁总是持有的
  */
  assert( pFile->eFileLock!=NO_LOCK || eFileLock==SHARED_LOCK );
  assert( eFileLock!=PENDING_LOCK );
  assert( eFileLock!=RESERVED_LOCK || pFile->eFileLock==SHARED_LOCK );

  /* This mutex is needed because pFile->pInode is shared across threads
  ** 需要这个互斥信号量，因为pFile->pInode在线程之间是共享的
  */
  pInode = pFile->pInode;
  sqlite3_mutex_enter(pInode->pLockMutex);

  /* If some thread using this PID has a lock via a different unixFile*
  ** handle that precludes the requested lock, return BUSY.
  ** 如果使用此PID的线程通过一个不同的unixFile* handle，即排除请求的锁，获得了一个锁，返回BUSY。
  */
  if( (pFile->eFileLock!=pInode->eFileLock && 
          (pInode->eFileLock>=PENDING_LOCK || eFileLock>SHARED_LOCK))
  ){
    rc = SQLITE_BUSY;
    goto end_lock;
  }

  /* If a SHARED lock is requested, and some thread using this PID already
  ** has a SHARED or RESERVED lock, then increment reference counts and
  ** return SQLITE_OK.
  ** 如果请求共享锁时，使用此PID的一些线程已经有一个共享锁或者保留锁，则增加引用计数并返回SQLITE_OK。
  */
  if( eFileLock==SHARED_LOCK && 
      (pInode->eFileLock==SHARED_LOCK || pInode->eFileLock==RESERVED_LOCK) ){
    assert( eFileLock==SHARED_LOCK );
    assert( pFile->eFileLock==0 );
    assert( pInode->nShared>0 );
    pFile->eFileLock = SHARED_LOCK;
    pInode->nShared++;
    pInode->nLock++;
    goto end_lock;
  }


  /* A PENDING lock is needed before acquiring a SHARED lock and before
  ** acquiring an EXCLUSIVE lock.  For the SHARED lock, the PENDING will
  ** be released.
  ** 在获取共享锁和排它锁之前必须有一个未决锁。获得了共享锁，该未决锁将被移除。
  */
  lock.l_len = 1L;
  lock.l_whence = SEEK_SET;
  if( eFileLock==SHARED_LOCK 
      || (eFileLock==EXCLUSIVE_LOCK && pFile->eFileLock<PENDING_LOCK)
  ){
    lock.l_type = (eFileLock==SHARED_LOCK?F_RDLCK:F_WRLCK);
    lock.l_start = PENDING_BYTE;
    if( unixFileLock(pFile, &lock) ){
      tErrno = errno;
      rc = sqliteErrorFromPosixError(tErrno, SQLITE_IOERR_LOCK);
      if( rc!=SQLITE_BUSY ){
        storeLastErrno(pFile, tErrno);
      }
      goto end_lock;
    }
  }


  /* If control gets to this point, then actually go ahead and make
  ** operating system calls for the specified lock.
  ** 如果控制达到这一点，那么实际上继续让操作系统调用指定的锁。
  */
  if( eFileLock==SHARED_LOCK ){
    assert( pInode->nShared==0 );
    assert( pInode->eFileLock==0 );
    assert( rc==SQLITE_OK );

    /* Now get the read-lock */  //获取读锁
    lock.l_start = SHARED_FIRST;
    lock.l_len = SHARED_SIZE;
    if( unixFileLock(pFile, &lock) ){
      tErrno = errno;
      rc = sqliteErrorFromPosixError(tErrno, SQLITE_IOERR_LOCK);
    }

    /* Drop the temporary PENDING lock */  //删除临时的未决锁
    lock.l_start = PENDING_BYTE;
    lock.l_len = 1L;
    lock.l_type = F_UNLCK;
    if( unixFileLock(pFile, &lock) && rc==SQLITE_OK ){
      /* This could happen with a network mount */  //这可能在网络加载时发生
      tErrno = errno;
      rc = SQLITE_IOERR_UNLOCK; 
    }

    if( rc ){
      if( rc!=SQLITE_BUSY ){
        storeLastErrno(pFile, tErrno);
      }
      goto end_lock;
    }else{
      pFile->eFileLock = SHARED_LOCK;
      pInode->nLock++;
      pInode->nShared = 1;
    }
  }else if( eFileLock==EXCLUSIVE_LOCK && pInode->nShared>1 ){
    /* We are trying for an exclusive lock but another thread in this
    ** same process is still holding a shared lock. 
    ** 我们尝试获取排它锁，但是在这一过程中另一个线程一直持有共享锁
    */
    rc = SQLITE_BUSY;
  }else{
    /* The request was for a RESERVED or EXCLUSIVE lock.  It is
    ** assumed that there is a SHARED or greater lock on the file
    ** already.
    ** 请求保留锁或独占锁。假设已经有一个共享锁或一个文件上的更高级的锁
    */
    assert( 0!=pFile->eFileLock );
    lock.l_type = F_WRLCK;

    assert( eFileLock==RESERVED_LOCK || eFileLock==EXCLUSIVE_LOCK );
    if( eFileLock==RESERVED_LOCK ){
      lock.l_start = RESERVED_BYTE;
      lock.l_len = 1L;
    }else{
      lock.l_start = SHARED_FIRST;
      lock.l_len = SHARED_SIZE;
    }

    if( unixFileLock(pFile, &lock) ){
      tErrno = errno;
      rc = sqliteErrorFromPosixError(tErrno, SQLITE_IOERR_LOCK);
      if( rc!=SQLITE_BUSY ){
        storeLastErrno(pFile, tErrno);
      }
    }
  }
  

  if( rc==SQLITE_OK ){
    pFile->eFileLock = eFileLock;
    pInode->eFileLock = eFileLock;
  }else if( eFileLock==EXCLUSIVE_LOCK ){
    pFile->eFileLock = PENDING_LOCK;
    pInode->eFileLock = PENDING_LOCK;
  }

end_lock:
  sqlite3_mutex_leave(pInode->pLockMutex);
  OSTRACE(("LOCK    %d %s %s (unix)\n", pFile->h, azFileLock(eFileLock), 
      rc==SQLITE_OK ? "ok" : "failed"));
  return rc;
}

/*
** Add the file descriptor used by file handle pFile to the corresponding
** pUnused list.
** 添加文件句柄pFile使用的文件描述符到相应的pUnused列表。
*/
static void setPendingFd(unixFile *pFile){
  unixInodeInfo *pInode = pFile->pInode;
  UnixUnusedFd *p = pFile->pPreallocatedUnused;
  assert( unixFileMutexHeld(pFile) );
  p->pNext = pInode->pUnused;
  pInode->pUnused = p;
  pFile->h = -1;
  pFile->pPreallocatedUnused = 0;
}

/*
** Lower the locking level on file descriptor pFile to eFileLock.  eFileLock
** must be either NO_LOCK or SHARED_LOCK.
** 降低文件描述符pFile上的锁定级别为eFileLock。eFileLock必须是NO_LOCK或SHARED_LOCK.
**
** If the locking level of the file descriptor is already at or below
** the requested locking level, this routine is a no-op.
** 如果文件描述符的锁定级别已经达到或低于所请求的锁定级别，此例程是一个空操作。
** 
** If handleNFSUnlock is true, then on downgrading an EXCLUSIVE_LOCK to SHARED
** the byte range is divided into 2 parts and the first part is unlocked then
** set to a read lock, then the other part is simply unlocked.  This works 
** around a bug in BSD NFS lockd (also seen on MacOSX 10.3+) that fails to 
** remove the write lock on a region when a read lock is set.
** 如果handleNFSUnlock为真，则降低一个EXCLUSIVE_LOCK为共享锁，其字节范围分为两部分，
** 一部分是为加锁然后设置为读锁，另一部分则是简单的未加锁。这一错误存在DSB NFS加锁（在MacOSX 10.3+z中也见过）
** 即当设置了一个读锁就无法在该区域删除写锁。
*/
static int posixUnlock(sqlite3_file *id, int eFileLock, int handleNFSUnlock){
  unixFile *pFile = (unixFile*)id;
  unixInodeInfo *pInode;
  struct flock lock;
  int rc = SQLITE_OK;

  assert( pFile );
  OSTRACE(("UNLOCK  %d %d was %d(%d,%d) pid=%d (unix)\n", pFile->h, eFileLock,
      pFile->eFileLock, pFile->pInode->eFileLock, pFile->pInode->nShared,
      osGetpid(0)));

  assert( eFileLock<=SHARED_LOCK );
  if( pFile->eFileLock<=eFileLock ){
    return SQLITE_OK;
  }
  pInode = pFile->pInode;
  sqlite3_mutex_enter(pInode->pLockMutex);
  assert( pInode->nShared!=0 );
  if( pFile->eFileLock>SHARED_LOCK ){
    assert( pInode->eFileLock==pFile->eFileLock );

    /* downgrading to a shared lock on NFS involves clearing the write lock
    ** before establishing the readlock - to avoid a race condition we downgrade
    ** the lock in 2 blocks, so that part of the range will be covered by a 
    ** write lock until the rest is covered by a read lock:
    ** 在NFS上降级到共享锁，包括在建立读锁之前清除写锁——以避免在降低2块锁时有竞争的情况，使一个写锁覆盖部分范围
    ** 直到读锁覆盖剩下的部分：
    **  1:   [WWWWW]
    **  2:   [....W]
    **  3:   [RRRRW]
    **  4:   [RRRR.]
    */
    if( eFileLock==SHARED_LOCK ){
#if !defined(__APPLE__) || !SQLITE_ENABLE_LOCKING_STYLE
      (void)handleNFSUnlock;
      assert( handleNFSUnlock==0 );
#endif
      {
        lock.l_type = F_RDLCK;
        lock.l_whence = SEEK_SET;
        lock.l_start = SHARED_FIRST;
        lock.l_len = SHARED_SIZE;
        if( unixFileLock(pFile, &lock) ){
          /* In theory, the call to unixFileLock() cannot fail because another
          ** process is holding an incompatible lock. If it does, this 
          ** indicates that the other process is not following the locking
          ** protocol. If this happens, return SQLITE_IOERR_RDLOCK. Returning
          ** SQLITE_BUSY would confuse the upper layer (in practice it causes 
          ** an assert to fail). 
          ** 理论上，调用unixFileLock()不能失败，因为另一程序正持有一个不兼容锁。
          ** 如果失败了，这表明其他进程没有遵循加锁协议。如果发生这样的情况，返回SQLITE_IOERR_RDLOCK。
          ** 返回SQLITE_BUSY会让上层混淆（实际上它导致一个失败的生效）。
          */ 
          rc = SQLITE_IOERR_RDLOCK;
          storeLastErrno(pFile, errno);
          goto end_unlock;
        }
      }
    }
    lock.l_type = F_UNLCK;
    lock.l_whence = SEEK_SET;
    lock.l_start = PENDING_BYTE;
    lock.l_len = 2L;  assert( PENDING_BYTE+1==RESERVED_BYTE );
    if( unixFileLock(pFile, &lock)==0 ){
      pInode->eFileLock = SHARED_LOCK;
    }else{
      rc = SQLITE_IOERR_UNLOCK;
      storeLastErrno(pFile, errno);
      goto end_unlock;
    }
  }
  if( eFileLock==NO_LOCK ){
    /* Decrement the shared lock counter.  Release the lock using an
    ** OS call only when all threads in this same process have released
    ** the lock.
    ** 共享锁计数器进行递减。当这一进程中的所有线程释放时，释放使用OS调用的锁。
    */
    pInode->nShared--;
    if( pInode->nShared==0 ){
      lock.l_type = F_UNLCK;
      lock.l_whence = SEEK_SET;
      lock.l_start = lock.l_len = 0L;
      if( unixFileLock(pFile, &lock)==0 ){
        pInode->eFileLock = NO_LOCK;
      }else{
        rc = SQLITE_IOERR_UNLOCK;
        storeLastErrno(pFile, errno);
        pInode->eFileLock = NO_LOCK;
        pFile->eFileLock = NO_LOCK;
      }
    }

    /* Decrement the count of locks against this same file.  When the
    ** count reaches zero, close any other file descriptors whose close
    ** was deferred because of outstanding locks.
    ** 对相同的文件的锁的计数进行递减。当计数到达0时，关闭所有其他由于outstanding锁而推迟关闭的文件描述符
    */
    pInode->nLock--;
    assert( pInode->nLock>=0 );
    if( pInode->nLock==0 ) closePendingFds(pFile);
  }

end_unlock:
  sqlite3_mutex_leave(pInode->pLockMutex);
  if( rc==SQLITE_OK ){
    pFile->eFileLock = eFileLock;
  }
  return rc;
}

/*
** Lower the locking level on file descriptor pFile to eFileLock.  eFileLock
** must be either NO_LOCK or SHARED_LOCK.
** 降低文件描述符pFile上的锁定级别为eFileLock。eFileLock必须是NO_LOCK或SHARED_LOCK。
**
** If the locking level of the file descriptor is already at or below
** the requested locking level, this routine is a no-op.
** 如果文件描述符的锁定级别已经达到或低于所请求的锁定级别，此例程是一个空操作。
*/
static int unixUnlock(sqlite3_file *id, int eFileLock){
#if SQLITE_MAX_MMAP_SIZE>0
  assert( eFileLock==SHARED_LOCK || ((unixFile *)id)->nFetchOut==0 );
#endif
  return posixUnlock(id, eFileLock, 0);
}

#if SQLITE_MAX_MMAP_SIZE>0
static int unixMapfile(unixFile *pFd, i64 nByte);
static void unixUnmapfile(unixFile *pFd);
#endif

/*
** This function performs the parts of the "close file" operation 
** common to all locking schemes. It closes the directory and file
** handles, if they are valid, and sets all fields of the unixFile
** structure to 0.
** 这个函数执行所有文件锁定计划共同的“关闭文件”操作部分。关闭目录和文件句柄，如果他们是
** 有效的，并设置unixFile结构的所有字段为0。
**
** It is *not* necessary to hold the mutex when this routine is called,
** even on VxWorks.  A mutex will be acquired on VxWorks by the
** vxworksReleaseFileId() routine.
** 在调用这个例程时，它并不需要互斥量，即使是在VxWorks。再VxWorks上，互斥量通过vxworksReleaseFileId()例程获得。
*/
static int closeUnixFile(sqlite3_file *id){
  unixFile *pFile = (unixFile*)id;
#if SQLITE_MAX_MMAP_SIZE>0
  unixUnmapfile(pFile);
#endif
  if( pFile->h>=0 ){
    robust_close(pFile, pFile->h, __LINE__);
    pFile->h = -1;
  }

#ifdef SQLITE_UNLINK_AFTER_CLOSE
  if( pFile->ctrlFlags & UNIXFILE_DELETE ){
    osUnlink(pFile->zPath);
    sqlite3_free(*(char**)&pFile->zPath);
    pFile->zPath = 0;
  }
#endif
  OSTRACE(("CLOSE   %-3d\n", pFile->h));
  OpenCounter(-1);
  sqlite3_free(pFile->pPreallocatedUnused);
  memset(pFile, 0, sizeof(unixFile));
  return SQLITE_OK;
}

/*
** Close a file.  关闭一个文件
*/
static int unixClose(sqlite3_file *id){
  int rc = SQLITE_OK;
  unixFile *pFile = (unixFile *)id;
  unixInodeInfo *pInode = pFile->pInode;

  assert( pInode!=0 );
  verifyDbFile(pFile);
  unixUnlock(id, NO_LOCK);
  assert( unixFileMutexNotheld(pFile) );
  unixEnterMutex();

  /* unixFile.pInode is always valid here. Otherwise, a different close
  ** routine (e.g. nolockClose()) would be called instead.
  ** 在这里unixFile.pInode始终是有效的。否则，将被调用一个不同的但相似的程序（如nolockClose()）所代替
  */
  assert( pFile->pInode->nLock>0 || pFile->pInode->bProcessLock==0 );
  sqlite3_mutex_enter(pInode->pLockMutex);
  if( pInode->nLock ){
    /* If there are outstanding locks, do not actually close the file just
    ** yet because that would clear those locks.  Instead, add the file
    ** descriptor to pInode->pUnused list.  It will be automatically closed 
    ** when the last lock is cleared.
    ** 如果有未完成的锁，事实还不能关闭这个文件，因为那样会清除这些锁。
    ** 相反，将该文件描述符添加到pInode->pUnused列表。当清除最后的锁定时，他会自动关闭。
    */
    setPendingFd(pFile);
  }
  sqlite3_mutex_leave(pInode->pLockMutex);
  releaseInodeInfo(pFile);
  assert( pFile->pShm==0 );
  rc = closeUnixFile(id);
  unixLeaveMutex();
  return rc;
}

/************** End of the posix advisory lock implementation *****************
******************************************************************************/

/******************************************************************************
****************************** No-op Locking **********************************
**
** 无操作锁
** Of the various locking implementations available, this is by far the
** simplest:  locking is ignored.  No attempt is made to lock the database
** file for reading or writing.
** 相对于其他一系列锁的实现，这种无操作锁是迄今为止最简单的，它没有试图锁住数据库文件进行读或写。
**
** This locking mode is appropriate for use on read-only databases
** (ex: databases that are burned into CD-ROM, for example.)  It can
** also be used if the application employs some external mechanism to
** prevent simultaneous access of the same database by two or more
** database connections.  But there is a serious risk of database
** corruption if this locking mode is used in situations where multiple
** database connections are accessing the same database file at the same
** time and one or more of those connections are writing.
** 这种模式的锁适用于只读的数据库，例如刻在CD-ROM上的数据库。当一个应用程序使用外部机制去阻止两个
** 或两个以上的数据库连接对同一个数据库同时进行访问时也可以使用它。但是，当这种锁用于大量的数据库连接
** 同时访问一个数据库，并且这些连接当中有一个或者更多的是进行写操作的情况的时候，就会有一个严重的数据
** 库损坏风险。
*/

static int nolockCheckReservedLock(sqlite3_file *NotUsed, int *pResOut){
  UNUSED_PARAMETER(NotUsed);
  *pResOut = 0;
  return SQLITE_OK;
}
static int nolockLock(sqlite3_file *NotUsed, int NotUsed2){
  UNUSED_PARAMETER2(NotUsed, NotUsed2);
  return SQLITE_OK;
}
static int nolockUnlock(sqlite3_file *NotUsed, int NotUsed2){
  UNUSED_PARAMETER2(NotUsed, NotUsed2);
  return SQLITE_OK;
}

/*
** Close the file.
*/
static int nolockClose(sqlite3_file *id) {
  return closeUnixFile(id);
}

/******************* End of the no-op lock implementation *********************
******************************************************************************/

/******************************************************************************
************************* Begin dot-file Locking ******************************
** 点文件锁
** The dotfile locking implementation uses the existence of separate lock
** files (really a directory) to control access to the database.  This works
** on just about every filesystem imaginable.  But there are serious downsides:
**
**    (1)  There is zero concurrency.  A single reader blocks all other
**         connections from reading or writing the database.
**
**    (2)  An application crash or power loss can leave stale lock files
**         sitting around that need to be cleared manually.
**
** Nevertheless, a dotlock is an appropriate locking mode for use if no
** other locking strategy is available.
** 点文件锁的实现是使用存在单独的锁文件，就是一种目录来控制对数据库的访问。这种机制用于任意一种文件
** 系统，但是它又存在严重的缺陷：
** （1）它没有并发性，一个读者就阻碍了其他所有要对数据库进行读写的连接；
** （2）应用程序的崩溃或功率的损耗都会使旧的锁文件无法自动清理，而需要手动清除。
** 不过，点文件锁是一种适用于没有其他锁策略可用的情况的锁机制。
**
** Dotfile locking works by creating a subdirectory in the same directory as
** the database and with the same name but with a ".lock" extension added.
** The existence of a lock directory implies an EXCLUSIVE lock.  All other
** lock types (SHARED, RESERVED, PENDING) are mapped into EXCLUSIVE.
** 点文件锁的工作原理是在同一个目录里面创建次级目录存入数据库，并且有相同的扩展名".lock"。一个锁
** 目录的存在意味着它是排它锁。其他所有类型的锁（共享锁，保留锁，未决锁）都映射到排它锁。
*/

/*
** The file suffix added to the data base filename in order to create the
** lock directory.
** 文件后缀添加到数据库文件里面以创建锁目录
*/
#define DOTLOCK_SUFFIX ".lock"  //预定义锁文件的后缀为.lock

/*
** This routine checks if there is a RESERVED lock held on the specified
** file by this or any other process. If such a lock is held, set *pResOut
** to a non-zero value otherwise *pResOut is set to zero.  The return value
** is set to SQLITE_OK unless an I/O error occurs during lock checking.
** 这个程序会检查指定文件或者是其他任何进程里面是否持有保留锁，如果持有保留锁，将指针*pResOut的值设为
** 非零，否则设为0。返回值设定为SQLITE_OK除非在锁检查期间发生I/O错误。
**
** In dotfile locking, either a lock exists or it does not.  So in this
** variation of CheckReservedLock(), *pResOut is set to true if any lock
** is held on the file and false if the file is unlocked.
** 在点文件锁机制中，锁要么存在要么不存在。所以函数CheckReservedLock()的变化就是如果文件被加锁，*pResOut
** 的值设为true，文件没被加锁，则设为false。
*/
static int dotlockCheckReservedLock(sqlite3_file *id, int *pResOut) {
  int rc = SQLITE_OK;
  int reserved = 0;
  unixFile *pFile = (unixFile*)id;

  SimulateIOError( return SQLITE_IOERR_CHECKRESERVEDLOCK; );
  
  assert( pFile );
  reserved = osAccess((const char*)pFile->lockingContext, 0)==0;
  OSTRACE(("TEST WR-LOCK %d %d %d (dotlock)\n", pFile->h, rc, reserved));
  *pResOut = reserved;
  return rc;
}

/*
** Lock the file with the lock specified by parameter eFileLock - one
** of the following:
**
** 使用参数eFileLock以下值之一指定的锁对文件加锁
**     (1) SHARED_LOCK  //共享锁
**     (2) RESERVED_LOCK  //保留锁
**     (3) PENDING_LOCK  //未决锁
**     (4) EXCLUSIVE_LOCK  //排它锁
**
** Sometimes when requesting one lock state, additional lock states
** are inserted in between.  The locking might fail on one of the later
** transitions leaving the lock state different from what it started but
** still short of its goal.  The following chart shows the allowed
** transitions and the inserted intermediate states:
** 当请求一个锁状态的时候，额外的锁状态会插入其中。锁定失败后，后来的转换会使它不同于开始的时候
** 的锁状态，但是仍能达到目标。以下的记录是允许的转换和插入的状态。
**
**    UNLOCKED -> SHARED  //未加锁->共享锁
**    SHARED -> RESERVED  //共享锁->保留锁
**    SHARED -> (PENDING) -> EXCLUSIVE  //共享锁->未决锁（插入）->排它锁
**    RESERVED -> (PENDING) -> EXCLUSIVE  //保留锁->未决锁（插入）->排它锁
**    PENDING -> EXCLUSIVE  //未决锁->排它锁
**
** This routine will only increase a lock.  Use the sqlite3OsUnlock()
** routine to lower a locking level.
** 这个程序只是增加一个锁。使用sqlite3的OsUnlock()程序降低锁的级别。
**
** With dotfile locking, we really only support state (4): EXCLUSIVE.
** But we track the other locking levels internally.
** 使用点文件锁，实际上只支持第四种状态：排它锁，但是可以在内部追踪其他级别的锁。
*/
static int dotlockLock(sqlite3_file *id, int eFileLock) {
  unixFile *pFile = (unixFile*)id;
  char *zLockFile = (char *)pFile->lockingContext;
  int rc = SQLITE_OK;


  /* If we have any lock, then the lock file already exists.  All we have
  ** to do is adjust our internal record of the lock level.
  ** 如果有任意一种锁，那么锁文件其实已经存在了，我们要做的只是修改内部纪录的锁级别
  */
  if( pFile->eFileLock > NO_LOCK ){
    pFile->eFileLock = eFileLock;
    /* Always update the timestamp on the old file */  //更新旧文件的时间戳
#ifdef HAVE_UTIME
    utime(zLockFile, NULL);
#else
    utimes(zLockFile, NULL);
#endif
    return SQLITE_OK;
  }
  
  /* grab an exclusive lock */  //攫取一个排它锁
  rc = osMkdir(zLockFile, 0777);
  if( rc<0 ){
    /* failed to open/create the lock directory */  //创建锁目录失败
    int tErrno = errno;
    if( EEXIST == tErrno ){
      rc = SQLITE_BUSY;
    } else {
      rc = sqliteErrorFromPosixError(tErrno, SQLITE_IOERR_LOCK);
      if( rc!=SQLITE_BUSY ){
        storeLastErrno(pFile, tErrno);
      }
    }
    return rc;
  } 
  
  /* got it, set the type and return ok */  //获取成功，设置类型并返回ok
  pFile->eFileLock = eFileLock;
  return rc;
}

/*
** Lower the locking level on file descriptor pFile to eFileLock.  eFileLock
** must be either NO_LOCK or SHARED_LOCK.
** 将文件描述符pFile的锁级别降低到eFileLock，eFileLock必须是NO_LOCK or SHARED_LOCK两者之一。
**
** If the locking level of the file descriptor is already at or below
** the requested locking level, this routine is a no-op.
** 如果文件描述符的锁定级别已经在正在请求的锁级别或以下，那么这个程序是一个空操作
**
** When the locking level reaches NO_LOCK, delete the lock file.
** 当锁定级别达到了NO_LOCK时删除锁文件
*/
static int dotlockUnlock(sqlite3_file *id, int eFileLock) {
  unixFile *pFile = (unixFile*)id;
  char *zLockFile = (char *)pFile->lockingContext;
  int rc;

  assert( pFile );
  OSTRACE(("UNLOCK  %d %d was %d pid=%d (dotlock)\n", pFile->h, eFileLock,
           pFile->eFileLock, osGetpid(0)));
  assert( eFileLock<=SHARED_LOCK );
  
  /* no-op if possible */  //如果可能是空操作
  if( pFile->eFileLock==eFileLock ){
    return SQLITE_OK;
  }

  /* To downgrade to shared, simply update our internal notion of the
  ** lock state.  No need to mess with the file on disk.
  ** 降低到共享时，只需简单地更新锁状态的内部概念，而不需要毁坏磁盘文件
  */
  if( eFileLock==SHARED_LOCK ){
    pFile->eFileLock = SHARED_LOCK;
    return SQLITE_OK;
  }
  
  /* To fully unlock the database, delete the lock file */  //完全打开数据库时删除锁文件
  assert( eFileLock==NO_LOCK );
  rc = osRmdir(zLockFile);
  if( rc<0 ){
    int tErrno = errno;
    if( tErrno==ENOENT ){
      rc = SQLITE_OK;
    }else{
      rc = SQLITE_IOERR_UNLOCK;
      storeLastErrno(pFile, tErrno);
    }
    return rc; 
  }
  pFile->eFileLock = NO_LOCK;
  return SQLITE_OK;
}

/*
** Close a file.  Make sure the lock has been released before closing.
** 关闭一个文件，确保锁在关闭之前已经被释放
*/
static int dotlockClose(sqlite3_file *id) {
  unixFile *pFile = (unixFile*)id;
  assert( id!=0 );
  dotlockUnlock(id, NO_LOCK);
  sqlite3_free(pFile->lockingContext);
  return closeUnixFile(id);
}
/****************** End of the dot-file lock implementation *******************
******************************************************************************/

/******************************************************************************
************************** Begin flock Locking ********************************
**
** 聚集锁
** Use the flock() system call to do file locking.  使用系统调用函数flock()执行文件锁定
**
** flock() locking is like dot-file locking in that the various
** fine-grain locking levels supported by SQLite are collapsed into
** a single exclusive lock.  In other words, SHARED, RESERVED, and
** PENDING locks are the same thing as an EXCLUSIVE lock.  SQLite
** still works when you do this, but concurrency is reduced since
** only a single process can be reading the database at a time.
** 聚集锁在sqlite支持各种细粒锁级别紧缩成一个单独的排它锁这一点上和点锁文件是一样的。换句话说，
** 共享锁，保留锁和未决锁对于排它锁来说是一样的。当你这样做时，sqlite仍然工作，但并发性降低，因为
** 同一时间只有一个进程可以读取数据库。
**
** Omit this section if SQLITE_ENABLE_LOCKING_STYLE is turned off
** 如果SQLITE_ENABLE_LOCKING_STYLE被关闭或者VXWORKS被编译，则忽略这部分。
*/
#if SQLITE_ENABLE_LOCKING_STYLE

/*
** Retry flock() calls that fail with EINTR  系统调用中断则重试flock()函数调用
*/
#ifdef EINTR
static int robust_flock(int fd, int op){
  int rc;
  do{ rc = flock(fd,op); }while( rc<0 && errno==EINTR );
  return rc;
}
#else
# define robust_flock(a,b) flock(a,b)
#endif
     

/*
** This routine checks if there is a RESERVED lock held on the specified
** file by this or any other process. If such a lock is held, set *pResOut
** to a non-zero value otherwise *pResOut is set to zero.  The return value
** is set to SQLITE_OK unless an I/O error occurs during lock checking.
** 这个程序会检查指定文件或者是其他任何进程里是否持有保留锁，如果持有保留锁，将指针*pResOut
** 的值设为非零，否则设为0。返回值设为SQLITE_OK除非在锁检查期间发生I/O错误。
*/
static int flockCheckReservedLock(sqlite3_file *id, int *pResOut){
  int rc = SQLITE_OK;
  int reserved = 0;
  unixFile *pFile = (unixFile*)id;
  
  SimulateIOError( return SQLITE_IOERR_CHECKRESERVEDLOCK; );
  
  assert( pFile );
  
  /* Check if a thread in this process holds such a lock */  //检查进程中是否有线程持有保留锁
  if( pFile->eFileLock>SHARED_LOCK ){
    reserved = 1;
  }
  
  /* Otherwise see if some other process holds it. */  //如果没有则查看其他进程
  if( !reserved ){
    /* attempt to get the lock */  //获取锁
    int lrc = robust_flock(pFile->h, LOCK_EX | LOCK_NB);
    if( !lrc ){
      /* got the lock, unlock it */  //获取锁，并解锁
      lrc = robust_flock(pFile->h, LOCK_UN);
      if ( lrc ) {
        int tErrno = errno;
        /* unlock failed with an error */  //解锁发生错误并且失败
        lrc = SQLITE_IOERR_UNLOCK; 
        storeLastErrno(pFile, tErrno);
        rc = lrc;
      }
    } else {
      int tErrno = errno;
      reserved = 1;
      /* someone else might have it reserved */  //其他进程可能保留了锁
      lrc = sqliteErrorFromPosixError(tErrno, SQLITE_IOERR_LOCK); 
      if( IS_LOCK_ERROR(lrc) ){
        storeLastErrno(pFile, tErrno);
        rc = lrc;
      }
    }
  }
  OSTRACE(("TEST WR-LOCK %d %d %d (flock)\n", pFile->h, rc, reserved));

#ifdef SQLITE_IGNORE_FLOCK_LOCK_ERRORS
  if( (rc & 0xff) == SQLITE_IOERR ){
    rc = SQLITE_OK;
    reserved=1;
  }
#endif /* SQLITE_IGNORE_FLOCK_LOCK_ERRORS */
  *pResOut = reserved;
  return rc;
}

/*
** Lock the file with the lock specified by parameter eFileLock - one
** of the following:
**
**     (1) SHARED_LOCK
**     (2) RESERVED_LOCK
**     (3) PENDING_LOCK
**     (4) EXCLUSIVE_LOCK
**
** Sometimes when requesting one lock state, additional lock states
** are inserted in between.  The locking might fail on one of the later
** transitions leaving the lock state different from what it started but
** still short of its goal.  The following chart shows the allowed
** transitions and the inserted intermediate states:
**
**    UNLOCKED -> SHARED
**    SHARED -> RESERVED
**    SHARED -> (PENDING) -> EXCLUSIVE
**    RESERVED -> (PENDING) -> EXCLUSIVE
**    PENDING -> EXCLUSIVE
**
** flock() only really support EXCLUSIVE locks.  We track intermediate
** lock states in the sqlite3_file structure, but all locks SHARED or
** above are really EXCLUSIVE locks and exclude all other processes from
** access the file.
**
** This routine will only increase a lock.  Use the sqlite3OsUnlock()
** routine to lower a locking level.
*/
static int flockLock(sqlite3_file *id, int eFileLock) {
  int rc = SQLITE_OK;
  unixFile *pFile = (unixFile*)id;

  assert( pFile );

  /* if we already have a lock, it is exclusive.  
  ** Just adjust level and punt on outta here. */
  if (pFile->eFileLock > NO_LOCK) {
    pFile->eFileLock = eFileLock;
    return SQLITE_OK;
  }
  
  /* grab an exclusive lock */
  
  if (robust_flock(pFile->h, LOCK_EX | LOCK_NB)) {
    int tErrno = errno;
    /* didn't get, must be busy */
    rc = sqliteErrorFromPosixError(tErrno, SQLITE_IOERR_LOCK);
    if( IS_LOCK_ERROR(rc) ){
      storeLastErrno(pFile, tErrno);
    }
  } else {
    /* got it, set the type and return ok */
    pFile->eFileLock = eFileLock;
  }
  OSTRACE(("LOCK    %d %s %s (flock)\n", pFile->h, azFileLock(eFileLock), 
           rc==SQLITE_OK ? "ok" : "failed"));
#ifdef SQLITE_IGNORE_FLOCK_LOCK_ERRORS
  if( (rc & 0xff) == SQLITE_IOERR ){
    rc = SQLITE_BUSY;
  }
#endif /* SQLITE_IGNORE_FLOCK_LOCK_ERRORS */
  return rc;
}


/*
** Lower the locking level on file descriptor pFile to eFileLock.  eFileLock
** must be either NO_LOCK or SHARED_LOCK.
**
** If the locking level of the file descriptor is already at or below
** the requested locking level, this routine is a no-op.
*/
static int flockUnlock(sqlite3_file *id, int eFileLock) {
  unixFile *pFile = (unixFile*)id;
  
  assert( pFile );
  OSTRACE(("UNLOCK  %d %d was %d pid=%d (flock)\n", pFile->h, eFileLock,
           pFile->eFileLock, osGetpid(0)));
  assert( eFileLock<=SHARED_LOCK );
  
  /* no-op if possible */
  if( pFile->eFileLock==eFileLock ){
    return SQLITE_OK;
  }
  
  /* shared can just be set because we always have an exclusive */
  if (eFileLock==SHARED_LOCK) {
    pFile->eFileLock = eFileLock;
    return SQLITE_OK;
  }
  
  /* no, really, unlock. */
  if( robust_flock(pFile->h, LOCK_UN) ){
#ifdef SQLITE_IGNORE_FLOCK_LOCK_ERRORS
    return SQLITE_OK;
#endif /* SQLITE_IGNORE_FLOCK_LOCK_ERRORS */
    return SQLITE_IOERR_UNLOCK;
  }else{
    pFile->eFileLock = NO_LOCK;
    return SQLITE_OK;
  }
}

/*
** Close a file.
*/
static int flockClose(sqlite3_file *id) {
  assert( id!=0 );
  flockUnlock(id, NO_LOCK);
  return closeUnixFile(id);
}

#endif /* SQLITE_ENABLE_LOCKING_STYLE && !OS_VXWORK */

/******************* End of the flock lock implementation *********************
******************************************************************************/

/******************************************************************************
************************ Begin Named Semaphore Locking ************************
**
** Named semaphore locking is only supported on VxWorks.
**
** Semaphore locking is like dot-lock and flock in that it really only
** supports EXCLUSIVE locking.  Only a single process can read or write
** the database file at a time.  This reduces potential concurrency, but
** makes the lock implementation much easier.
*/

/*
** Named semaphore locking is only available on VxWorks.
**
*************** End of the named semaphore lock implementation ****************
******************************************************************************/


/******************************************************************************
*************************** Begin AFP Locking *********************************
**
** AFP is the Apple Filing Protocol.  AFP is a network filesystem found
** on Apple Macintosh computers - both OS9 and OSX.
**
** Third-party implementations of AFP are available.  But this code here
** only works on OSX.
*/

/*
** The code above is the AFP lock implementation.  The code is specific
** to MacOSX and does not work on other unix platforms.  No alternative
** is available.  If you don't compile for a mac, then the "unix-afp"
** VFS is not available.
**
********************* End of the AFP lock implementation **********************
******************************************************************************/

/******************************************************************************
*************************** Begin NFS Locking ********************************/

/*
** The code above is the NFS lock implementation.  The code is specific
** to MacOSX and does not work on other unix platforms.  No alternative
** is available.  
**
********************* End of the NFS lock implementation **********************
******************************************************************************/

/******************************************************************************
**************** Non-locking sqlite3_file methods *****************************
**
** The next division contains implementations for all methods of the 
** sqlite3_file object other than the locking methods.  The locking
** methods were defined in divisions above (one locking method per
** division).  Those methods that are common to all locking modes
** are gather together into this division.
** 下一个部分包含除锁定方法外所有sqlite3文件对象方法的实现。锁定方法已在以上部分中定义
** （一个部分定义一个锁定方法）。那些对所有锁定模式共同的方法在这个部分聚集在一起
*/

/*
** Seek to the offset passed as the second argument, then read cnt 
** bytes into pBuf. Return the number of bytes actually read.
** 寻求偏移量作为第二个参数传递，然后读取cnt字节到pBuf中。返回实际读取的字节数。
**
** NB:  If you define USE_PREAD or USE_PREAD64, then it might also
** be necessary to define _XOPEN_SOURCE to be 500.  This varies from
** one system to another.  Since SQLite does not define USE_PREAD
** in any form by default, we will not attempt to define _XOPEN_SOURCE.
** See tickets #2741 and #2681.
** 注：如果你定义USE_PREAD或者USE_PREAD64，那么它可能也有必要定义_XOPEN_SOURCE为500。
** 从一个系统到另一个系统这个不同。因为SQLITE在默认情况下不定义USE_PREAD任何形式，我们
** 不会试图定义_XOPEN_SOURCE。看标签#2741到#2681
**
** To avoid stomping the errno value on a failed read the lastErrno value
** is set before returning.
** 为了避免errno值的读取失败，lastErrno值在返回前被设定。
*/
static int seekAndRead(unixFile *id, sqlite3_int64 offset, void *pBuf, int cnt){
  int got;
  int prior = 0;
#if (!defined(USE_PREAD) && !defined(USE_PREAD64))
  i64 newOffset;
#endif
  TIMER_START;
  assert( cnt==(cnt&0x1ffff) );
  assert( id->h>2 );
  do{
#if defined(USE_PREAD)
    got = osPread(id->h, pBuf, cnt, offset);
    SimulateIOError( got = -1 );
#elif defined(USE_PREAD64)
    got = osPread64(id->h, pBuf, cnt, offset);
    SimulateIOError( got = -1 );
#else
    newOffset = lseek(id->h, offset, SEEK_SET);
    SimulateIOError( newOffset = -1 );
    if( newOffset<0 ){
      storeLastErrno((unixFile*)id, errno);
      return -1;
    }
    got = osRead(id->h, pBuf, cnt);
#endif
    if( got==cnt ) break;
    if( got<0 ){
      if( errno==EINTR ){ got = 1; continue; }
      prior = 0;
      storeLastErrno((unixFile*)id,  errno);
      break;
    }else if( got>0 ){
      cnt -= got;
      offset += got;
      prior += got;
      pBuf = (void*)(got + (char*)pBuf);
    }
  }while( got>0 );
  TIMER_END;
  OSTRACE(("READ    %-3d %5d %7lld %llu\n",
            id->h, got+prior, offset-prior, TIMER_ELAPSED));
  return got+prior;
}

/*
** Read data from a file into a buffer.  Return SQLITE_OK if all
** bytes were read successfully and SQLITE_IOERR if anything goes
** wrong.
** 从文件中读取数据到缓冲区。如果所有字节读取成功返回SQLITE_OK,如果出错返回SQLITE_IOERR。
*/
static int unixRead(
  sqlite3_file *id, 
  void *pBuf, 
  int amt,
  sqlite3_int64 offset
){
  unixFile *pFile = (unixFile *)id;
  int got;
  assert( id );
  assert( offset>=0 );
  assert( amt>0 );

  /* If this is a database file (not a journal, super-journal or temp
  ** file), the bytes in the locking range should never be read or written. 
  ** 如果这是一个数据库文件（不是一个日志，主日志或者临时文件），锁定范围的字节不能读或写。
  */
#if 0
  assert( pFile->pPreallocatedUnused==0
       || offset>=PENDING_BYTE+512
       || offset+amt<=PENDING_BYTE 
  );
#endif

#if SQLITE_MAX_MMAP_SIZE>0
  /* Deal with as much of this read request as possible by transfering
  ** data from the memory mapping using memcpy().  */
  if( offset<pFile->mmapSize ){
    if( offset+amt <= pFile->mmapSize ){
      memcpy(pBuf, &((u8 *)(pFile->pMapRegion))[offset], amt);
      return SQLITE_OK;
    }else{
      int nCopy = pFile->mmapSize - offset;
      memcpy(pBuf, &((u8 *)(pFile->pMapRegion))[offset], nCopy);
      pBuf = &((u8 *)pBuf)[nCopy];
      amt -= nCopy;
      offset += nCopy;
    }
  }
#endif

  got = seekAndRead(pFile, offset, pBuf, amt);
  if( got==amt ){
    return SQLITE_OK;
  }else if( got<0 ){
    /* pFile->lastErrno has been set by seekAndRead().
    ** Usually we return SQLITE_IOERR_READ here, though for some
    ** kinds of errors we return SQLITE_IOERR_CORRUPTFS.  The
    ** SQLITE_IOERR_CORRUPTFS will be converted into SQLITE_CORRUPT
    ** prior to returning to the application by the sqlite3ApiExit()
    ** routine.
    */
    switch( pFile->lastErrno ){
      case ERANGE:
      case EIO:
#ifdef ENXIO
      case ENXIO:
#endif
#ifdef EDEVERR
      case EDEVERR:
#endif
        return SQLITE_IOERR_CORRUPTFS;
    }
    return SQLITE_IOERR_READ;
  }else{
    storeLastErrno(pFile, 0);   /* not a system error */
    /* Unread parts of the buffer must be zero-filled */
    memset(&((char*)pBuf)[got], 0, amt-got);
    return SQLITE_IOERR_SHORT_READ;
  }
}

/*
** Attempt to seek the file-descriptor passed as the first argument to
** absolute offset iOff, then attempt to write nBuf bytes of data from
** pBuf to it. If an error occurs, return -1 and set *piErrno. Otherwise, 
** return the actual number of bytes written (which may be less than
** nBuf).
*/
static int seekAndWriteFd(
  int fd,                         /* File descriptor to write to */
  i64 iOff,                       /* File offset to begin writing at */
  const void *pBuf,               /* Copy data from this buffer to the file */
  int nBuf,                       /* Size of buffer pBuf in bytes */
  int *piErrno                    /* OUT: Error number if error occurs */
){
  int rc = 0;                     /* Value returned by system call */

  assert( nBuf==(nBuf&0x1ffff) );
  assert( fd>2 );
  assert( piErrno!=0 );
  nBuf &= 0x1ffff;
  TIMER_START;

#if defined(USE_PREAD)
  do{ rc = (int)osPwrite(fd, pBuf, nBuf, iOff); }while( rc<0 && errno==EINTR );
#elif defined(USE_PREAD64)
  do{ rc = (int)osPwrite64(fd, pBuf, nBuf, iOff);}while( rc<0 && errno==EINTR);
#else
  do{
    i64 iSeek = lseek(fd, iOff, SEEK_SET);
    SimulateIOError( iSeek = -1 );
    if( iSeek<0 ){
      rc = -1;
      break;
    }
    rc = osWrite(fd, pBuf, nBuf);
  }while( rc<0 && errno==EINTR );
#endif

  TIMER_END;
  OSTRACE(("WRITE   %-3d %5d %7lld %llu\n", fd, rc, iOff, TIMER_ELAPSED));

  if( rc<0 ) *piErrno = errno;
  return rc;
}


/*
** Seek to the offset in id->offset then read cnt bytes into pBuf.
** Return the number of bytes actually read.  Update the offset.
**
** 用id->offset寻找偏移，然后读取cnt字节到pBuf中。返回实际读取的字节数，更新偏移。
** To avoid stomping the errno value on a failed write the lastErrno value
** is set before returning.
** 为了避免errno的值写入失败，lastErrno的值在返回前被设定。
*/
static int seekAndWrite(unixFile *id, i64 offset, const void *pBuf, int cnt){
  return seekAndWriteFd(id->h, offset, pBuf, cnt, &id->lastErrno);
}


/*
** Write data from a buffer into a file.  Return SQLITE_OK on success
** or some other error code on failure.
** 从缓冲区写入数据到一个文件中。成功返回SQLITE_OK或者失败返回一些其他错误代码。
*/
static int unixWrite(
  sqlite3_file *id, 
  const void *pBuf, 
  int amt,
  sqlite3_int64 offset 
){
  unixFile *pFile = (unixFile*)id;
  int wrote = 0;
  assert( id );
  assert( amt>0 );

  /* If this is a database file (not a journal, super-journal or temp
  ** file), the bytes in the locking range should never be read or written. 
  ** 如果这是一个数据库文件（不是一个日志，主文件或者临时文件），锁定范围的字节不能读或写
  */
#if 0
  assert( pFile->pPreallocatedUnused==0
       || offset>=PENDING_BYTE+512
       || offset+amt<=PENDING_BYTE 
  );
#endif

#if defined(SQLITE_MMAP_READWRITE) && SQLITE_MAX_MMAP_SIZE>0
  /* Deal with as much of this write request as possible by transfering
  ** data from the memory mapping using memcpy().  */
  if( offset<pFile->mmapSize ){
    if( offset+amt <= pFile->mmapSize ){
      memcpy(&((u8 *)(pFile->pMapRegion))[offset], pBuf, amt);
      return SQLITE_OK;
    }else{
      int nCopy = pFile->mmapSize - offset;
      memcpy(&((u8 *)(pFile->pMapRegion))[offset], pBuf, nCopy);
      pBuf = &((u8 *)pBuf)[nCopy];
      amt -= nCopy;
      offset += nCopy;
    }
  }
#endif
 
  while( (wrote = seekAndWrite(pFile, offset, pBuf, amt))<amt && wrote>0 ){
    amt -= wrote;
    offset += wrote;
    pBuf = &((char*)pBuf)[wrote];
  }
  SimulateIOError(( wrote=(-1), amt=1 ));
  SimulateDiskfullError(( wrote=0, amt=1 ));

  if( amt>wrote ){
    if( wrote<0 && pFile->lastErrno!=ENOSPC ){
      /* lastErrno set by seekAndWrite */
      return SQLITE_IOERR_WRITE;
    }else{
      storeLastErrno(pFile, 0); /* not a system error */
      return SQLITE_FULL;
    }
  }

  return SQLITE_OK;
}

#ifdef SQLITE_TEST
/*
** Count the number of fullsyncs and normal syncs.  This is used to test
** that syncs and fullsyncs are occurring at the right times.
** 计算fullsyncs和正常syncs的数量，这用来测试syncs和fullsyncs在正确的时间发生。
*/
int sqlite3_sync_count = 0;
int sqlite3_fullsync_count = 0;
#endif

/*
** We do not trust systems to provide a working fdatasync().  Some do.
** Others do no.  To be safe, we will stick with the (slightly slower)
** fsync(). If you know that your system does support fdatasync() correctly,
** then simply compile with -Dfdatasync=fdatasync or -DHAVE_FDATASYNC
** 我们不相信系统提供一个有效的fdatasync()。有些时这样的，其他的不是。为了安全起见，我们
** 将坚持（稍慢）fsync()。如果你知道你的系统确实正确的支持fdatasync()，那么用-Dfdatasync=fdatasync
** 简单地编译
*/
#if !defined(fdatasync) && !HAVE_FDATASYNC
# define fdatasync fsync
#endif

/*
** Define HAVE_FULLFSYNC to 0 or 1 depending on whether or not
** the F_FULLFSYNC macro is defined.  F_FULLFSYNC is currently
** only available on Mac OS X.  But that could change.
** 定义HAVE_FULLFSYNC为0或者1取决于F_FULLFSYNC宏是否被定义。F_FULLFSYNC
** 目前仅在Mac OS X上可用。但可能会改变。
*/
#ifdef F_FULLFSYNC
# define HAVE_FULLFSYNC 1
#else
# define HAVE_FULLFSYNC 0
#endif


/*
** The fsync() system call does not work as advertised on many
** unix systems.  The following procedure is an attempt to make
** it work better.
** 在很多unix系统上，fsync()系统调用不像广告一样起了作用。下面的程序是为了让
** 它工作的更好。
**
** The SQLITE_NO_SYNC macro disables all fsync()s.  This is useful
** for testing when we want to run through the test suite quickly.
** You are strongly advised *not* to deploy with SQLITE_NO_SYNC
** enabled, however, since with SQLITE_NO_SYNC enabled, an OS crash
** or power failure will likely corrupt the database file.
** SQLITE_NO_SYNC宏禁用所有fsync()。当我们想快速地运行测试套件的时候，这对测试
** 是有用的。强烈的建议不再SQLITE_NO_SYNC启用下进行配置，因为启用了SQLITE_NO_SYNC
** 后，一个操作系统崩溃或者电源故障可能会毁坏数据库文件
**
** SQLite sets the dataOnly flag if the size of the file is unchanged.
** The idea behind dataOnly is that it should only write the file content
** to disk, not the inode.  We only set dataOnly if the file size is 
** unchanged since the file size is part of the inode.  However, 
** Ted Ts'o tells us that fdatasync() will also write the inode if the
** file size has changed.  The only real difference between fdatasync()
** and fsync(), Ted tells us, is that fdatasync() will not flush the
** inode if the mtime or owner or other inode attributes have changed.
** We only care about the file size, not the other file attributes, so
** as far as SQLite is concerned, an fdatasync() is always adequate.
** So, we always use fdatasync() if it is available, regardless of
** the value of the dataOnly flag.
** 如果文件的大小不变，SQLITE设置dataOnly标志。
** dataOnly背后的想法是它应该只写文件内容到磁盘，而不是索引节点。由于文件大小是索引节
** 的一部分，如果文件大小没变，我们只设置dataOnly。但是，Ted Ts'o告诉我们，fdatasync()也将
** 写索引结点，如果文件大小已经改变。fdatasync()和fsync()之间唯一真正的区别，Ted告诉我们了，
** 是fdatasync()不会刷新索引节点，如果mtime或者所有者或者其他索引节点属性已经改变了。
** 我们只关心文件大小，不是其他文件属性，所以对sqlite而言，一个fdatasync()总是足够的。
** 所以我们总是只用fdatasync()，如果它是可用的，不管dataOnly标志的值。
*/
static int full_fsync(int fd, int fullSync, int dataOnly){
  int rc;

  /* The following "ifdef/elif/else/" block has the same structure as
  ** the one below. It is replicated here solely to avoid cluttering 
  ** up the real code with the UNUSED_PARAMETER() macros.
  ** 下面的，"ifdef/elif/else/"块如下面一个块一样具有相同的结构。它单独地复制在这里
  ** 避免弄乱真正的带UNUSED_PARAMETER()宏的代码
  */
#ifdef SQLITE_NO_SYNC
  UNUSED_PARAMETER(fd);
  UNUSED_PARAMETER(fullSync);
  UNUSED_PARAMETER(dataOnly);
#elif HAVE_FULLFSYNC
  UNUSED_PARAMETER(dataOnly);
#else
  UNUSED_PARAMETER(fullSync);
  UNUSED_PARAMETER(dataOnly);
#endif

  /* Record the number of times that we do a normal fsync() and 
  ** FULLSYNC.  This is used during testing to verify that this procedure
  ** gets called with the correct arguments.
  ** 记录我们做一个正常的fsync()和FULLSUNC的次数。这是在测试期间使用来检验这个
  ** 过程以正确的参数被调用了。
  */
#ifdef SQLITE_TEST
  if( fullSync ) sqlite3_fullsync_count++;
  sqlite3_sync_count++;
#endif

  /* If we compiled with the SQLITE_NO_SYNC flag, then syncing is a
  ** no-op.  But go ahead and call fstat() to validate the file
  ** descriptor as we need a method to provoke a failure during
  ** coverate testing.
  ** 如果我们用SQLITE_NO_SYNC标志编译，那么同步是一个空操作。
  */
#ifdef SQLITE_NO_SYNC
  {
    struct stat buf;
    rc = osFstat(fd, &buf);
  }
#elif HAVE_FULLFSYNC
  if( fullSync ){
    rc = osFcntl(fd, F_FULLFSYNC, 0);
  }else{
    rc = 1;
  }
  /* If the FULLFSYNC failed, fall back to attempting an fsync().
  ** It shouldn't be possible for fullfsync to fail on the local 
  ** file system (on OSX), so failure indicates that FULLFSYNC
  ** isn't supported for this file system. So, attempt an fsync 
  ** and (for now) ignore the overhead of a superfluous fcntl call.  
  ** It'd be better to detect fullfsync support once and avoid 
  ** the fcntl call every time sync is called.
  ** 如果FULLFSYNC失败，回退尝试一个fsync()。
  ** fullfsync在本地文件系统（在OSX上）失败，是不该可能的，所以失败表明
  ** FULLFSYNC不支持这个文件系统。所以，尝试一个fsync，（现在）忽略多余的
  ** fcntl调用开销。最好检测fullfsync支持一次，避免每次fcntl调用sync被调用。
  */
  if( rc ) rc = fsync(fd);
#else 
  rc = fdatasync(fd);

#endif /* ifdef SQLITE_NO_SYNC elif HAVE_FULLFSYNC */

  return rc;
}

/*
** Open a file descriptor to the directory containing file zFilename.
** If successful, *pFd is set to the opened file descriptor and
** SQLITE_OK is returned. If an error occurs, either SQLITE_NOMEM
** or SQLITE_CANTOPEN is returned and *pFd is set to an undefined
** value.
** 打开一个文件描述符到包含文件zFilename的目录。
** 如果成功，*pFd设置为打开的文件描述符，SQLITE_OK返回。如果出现错误，SQLITE_NOMEM
** 或者SQLITE_CANTOPEN返回，*pFd设置一个未定义的值。
**
** The directory file descriptor is used for only one thing - to
** fsync() a directory to make sure file creation and deletion events
** are flushed to disk.  Such fsyncs are not needed on newer
** journaling filesystems, but are required on older filesystems.
** 目录文件描述符用于只有一件事，fsync()一个目录，确保文件创建和删除事件被刷新到磁盘
** 这样的fsyncs在更新的日志文件系统不需要，但在旧文件系统需要。
**
** This routine can be overridden using the xSetSysCall interface.
** The ability to override this routine was added in support of the
** chromium sandbox.  Opening a directory is a security risk (we are
** told) so making it overrideable allows the chromium sandbox to
** replace this routine with a harmless no-op.  To make this routine
** a no-op, replace it with a stub that returns SQLITE_OK but leaves
** *pFd set to a negative number.
** 这个例程使用xSetSysCall接口可以被覆盖。
** 覆盖这个例程的能力被增加来支持chromiun浏览器沙箱。打开一个目录是一个安全风险（我们被告知）
** 所以使他可覆盖允许chromium沙箱用一种无害的空操作来代替这个例程，代之以一个存根，返回
** SQLITE_OK但是留下*pFd设置为负数。
**
** If SQLITE_OK is returned, the caller is responsible for closing
** the file descriptor *pFd using close().
** 如果SQLITE_OK返回，调用者负责关闭文件描述符*pFd，使用close()。
*/
static int openDirectory(const char *zFilename, int *pFd){
  int ii;
  int fd = -1;
  char zDirname[MAX_PATHNAME+1];

  sqlite3_snprintf(MAX_PATHNAME, zDirname, "%s", zFilename);
  for(ii=(int)strlen(zDirname); ii>0 && zDirname[ii]!='/'; ii--);
  if( ii>0 ){
    zDirname[ii] = '\0';
  }else{
    if( zDirname[0]!='/' ) zDirname[0] = '.';
    zDirname[1] = 0;
  }
  fd = robust_open(zDirname, O_RDONLY|O_BINARY, 0);
  if( fd>=0 ){
    OSTRACE(("OPENDIR %-3d %s\n", fd, zDirname));
  }
  *pFd = fd;
  if( fd>=0 ) return SQLITE_OK;
  return unixLogError(SQLITE_CANTOPEN_BKPT, "openDirectory", zDirname);
}

/*
** Make sure all writes to a particular file are committed to disk.
** 确保所有的写入到一个特定文件提交到磁盘
**
** If dataOnly==0 then both the file itself and its metadata (file
** size, access time, etc) are synced.  If dataOnly!=0 then only the
** file data is synced.
** 如果dataOnly==0,那么文件本身及其元数据（文件大小、访问时间等）是同步的。
** 如果dataOnly != 0，那么只有文件数据是同步的。
**
** Under Unix, also make sure that the directory entry for the file
** has been created by fsync-ing the directory that contains the file.
** If we do not do this and we encounter a power failure, the directory
** entry for the journal might not exist after we reboot.  The next
** SQLite to access the file will not know that the journal exists (because
** the directory entry for the journal was never created) and the transaction
** will not roll back - possibly leading to database corruption.
** 在Unix中，也确保文件的目录条目已经通过同步包含文件的目录创建。
** 如果我们不这么做，我们遇到停电，日志目录条目在我们重启后可能不存在。下一个SQLITE访问
** 文件将不知道日志存在（因为日志目录条目从未创建），事务不将回滚，可能导致数据库毁坏。
*/
static int unixSync(sqlite3_file *id, int flags){
  int rc;
  unixFile *pFile = (unixFile*)id;

  int isDataOnly = (flags&SQLITE_SYNC_DATAONLY);
  int isFullsync = (flags&0x0F)==SQLITE_SYNC_FULL;

  /* Check that one of SQLITE_SYNC_NORMAL or FULL was passed */  //检查SQLITE_SYNC_NORMAL或者FULL的一个通过。
  assert((flags&0x0F)==SQLITE_SYNC_NORMAL
      || (flags&0x0F)==SQLITE_SYNC_FULL
  );

  /* Unix cannot, but some systems may return SQLITE_FULL from here. This
  ** line is to test that doing so does not cause any problems.
  ** Unix不能，但是一些系统可能从这里返回SQLITE_FULL。这行用于测试那样做，
  ** 所以这里不会引起任何问题。
  */
  SimulateDiskfullError( return SQLITE_FULL );

  assert( pFile );
  OSTRACE(("SYNC    %-3d\n", pFile->h));
  rc = full_fsync(pFile->h, isFullsync, isDataOnly);
  SimulateIOError( rc=1 );
  if( rc ){
    storeLastErrno(pFile, errno);
    return unixLogError(SQLITE_IOERR_FSYNC, "full_fsync", pFile->zPath);
  }

  /* Also fsync the directory containing the file if the DIRSYNC flag
  ** is set.  This is a one-time occurrence.  Many systems (examples: AIX)
  ** are unable to fsync a directory, so ignore errors on the fsync.
  ** 也同步包含这个文件的目录，如果DIRSYNC标志被设定。这是一次性的发生。很多系统
  ** （例如：AIX）不能同步一个目录，所以忽略同步错误。
  */
  if( pFile->ctrlFlags & UNIXFILE_DIRSYNC ){
    int dirfd;
    OSTRACE(("DIRSYNC %s (have_fullfsync=%d fullsync=%d)\n", pFile->zPath,
            HAVE_FULLFSYNC, isFullsync));
    rc = osOpenDirectory(pFile->zPath, &dirfd);
    if( rc==SQLITE_OK ){
      full_fsync(dirfd, 0, 0);
      robust_close(pFile, dirfd, __LINE__);
    }else{
      assert( rc==SQLITE_CANTOPEN );
      rc = SQLITE_OK;
    }
    pFile->ctrlFlags &= ~UNIXFILE_DIRSYNC;
  }
  return rc;
}

/*
** Truncate an open file to a specified size
** 截断一个打开的文件到指定的大小。
*/
static int unixTruncate(sqlite3_file *id, i64 nByte){
  unixFile *pFile = (unixFile *)id;
  int rc;
  assert( pFile );
  SimulateIOError( return SQLITE_IOERR_TRUNCATE );

  /* If the user has configured a chunk-size for this file, truncate the
  ** file so that it consists of an integer number of chunks (i.e. the
  ** actual file size after the operation may be larger than the requested
  ** size).
  ** 如果用户已经配置了这个文件的块的大小，截断文件以便于包含整数的块（即操作之后的实际文件
  ** 大小可能已经大于请求的大小）
  */
  if( pFile->szChunk>0 ){
    nByte = ((nByte + pFile->szChunk - 1)/pFile->szChunk) * pFile->szChunk;
  }

  rc = robust_ftruncate(pFile->h, nByte);
  if( rc ){
    storeLastErrno(pFile, errno);
    return unixLogError(SQLITE_IOERR_TRUNCATE, "ftruncate", pFile->zPath);
  }else{

#if SQLITE_MAX_MMAP_SIZE>0
    /* If the file was just truncated to a size smaller than the currently
    ** mapped region, reduce the effective mapping size as well. SQLite will
    ** use read() and write() to access data beyond this point from now on.  
    */
    if( nByte<pFile->mmapSize ){
      pFile->mmapSize = nByte;
    }
#endif

    return SQLITE_OK;
  }
}

/*
** Determine the current size of a file in bytes
** 确定当前文件的大小，字节为单位
*/
static int unixFileSize(sqlite3_file *id, i64 *pSize){
  int rc;
  struct stat buf;
  assert( id );
  rc = osFstat(((unixFile*)id)->h, &buf);
  SimulateIOError( rc=1 );
  if( rc!=0 ){
    storeLastErrno((unixFile*)id, errno);
    return SQLITE_IOERR_FSTAT;
  }
  *pSize = buf.st_size;

  /* When opening a zero-size database, the findInodeInfo() procedure
  ** writes a single byte into that file in order to work around a bug
  ** in the OS-X msdos filesystem.  In order to avoid problems with upper
  ** layers, we need to report this file size as zero even though it is
  ** really 1.   Ticket #3260.
  ** 当打开一个大小为0的数据库，findInodeInfo()程序将一个字节写入该文件为了解决一个
  ** 在OS-X msdos文件系统上的错误。为了避免上面的问题，我们需要报告这个文件大小为0，
  ** 即使它真是1。 标签#3260
  */
  if( *pSize==1 ) *pSize = 0;


  return SQLITE_OK;
}

/* 
** This function is called to handle the SQLITE_FCNTL_SIZE_HINT 
** file-control operation.  Enlarge the database to nBytes in size
** (rounded up to the next chunk-size).  If the database is already
** nBytes or larger, this routine is a no-op.
** 调用这个函数来处理SQLITE_FCNTL_SIZE_HIN文件控制操作。扩大数据库到nBytes大小
** （算到下一个块大小）。如果数据库已经nBytes或者更大，这个例程是一个空操作。
*/
static int fcntlSizeHint(unixFile *pFile, i64 nByte){
  if( pFile->szChunk>0 ){
    i64 nSize;                    /* Required file size ，请求文件大小*/
    struct stat buf;              /* Used to hold return values of fstat() ，用来保存fstat()返回值*/
   
    if( osFstat(pFile->h, &buf) ){
      return SQLITE_IOERR_FSTAT;
    }

    nSize = ((nByte+pFile->szChunk-1) / pFile->szChunk) * pFile->szChunk;
    if( nSize>(i64)buf.st_size ){

#if defined(HAVE_POSIX_FALLOCATE) && HAVE_POSIX_FALLOCATE
      /* The code below is handling the return value of osFallocate() 
      ** correctly. posix_fallocate() is defined to "returns zero on success, 
      ** or an error number on  failure". See the manpage for details. */
      int err;
      do{
        err = osFallocate(pFile->h, buf.st_size, nSize-buf.st_size);
      }while( err==EINTR );
      if( err && err!=EINVAL ) return SQLITE_IOERR_WRITE;
#else
      /* If the OS does not have posix_fallocate(), fake it. Write a 
      ** single byte to the last byte in each block that falls entirely
      ** within the extended region. Then, if required, a single byte
      ** at offset (nSize-1), to set the size of the file correctly.
      ** This is a similar technique to that used by glibc on systems
      ** that do not have a real fallocate() call.
      */
      int nBlk = buf.st_blksize;  /* File-system block size */
      int nWrite = 0;             /* Number of bytes written by seekAndWrite */
      i64 iWrite;                 /* Next offset to write to */

      iWrite = (buf.st_size/nBlk)*nBlk + nBlk - 1;
      assert( iWrite>=buf.st_size );
      assert( ((iWrite+1)%nBlk)==0 );
      for(/*no-op*/; iWrite<nSize+nBlk-1; iWrite+=nBlk ){
        if( iWrite>=nSize ) iWrite = nSize - 1;
        nWrite = seekAndWrite(pFile, iWrite, "", 1);
        if( nWrite!=1 ) return SQLITE_IOERR_WRITE;
      }
#endif
    }
  }

#if SQLITE_MAX_MMAP_SIZE>0
  if( pFile->mmapSizeMax>0 && nByte>pFile->mmapSize ){
    int rc;
    if( pFile->szChunk<=0 ){
      if( robust_ftruncate(pFile->h, nByte) ){
        storeLastErrno(pFile, errno);
        return unixLogError(SQLITE_IOERR_TRUNCATE, "ftruncate", pFile->zPath);
      }
    }

    rc = unixMapfile(pFile, nByte);
    return rc;
  }
#endif

  return SQLITE_OK;
}

/*
** If *pArg is initially negative then this is a query.  Set *pArg to
** 1 or 0 depending on whether or not bit mask of pFile->ctrlFlags is set.
** 如果*pArg初始是负的，那么这是一个查询。设置*pArg为1或者0取决于是否pFile->ctrlFlags
** 位屏蔽被设置。
**
** If *pArg is 0 or 1, then clear or set the mask bit of pFile->ctrlFlags.
** 如果*pArg为0或者1，那么清除或者设置pFile->ctrlFlags屏蔽位。
*/
static void unixModeBit(unixFile *pFile, unsigned char mask, int *pArg){
  if( *pArg<0 ){
    *pArg = (pFile->ctrlFlags & mask)!=0;
  }else if( (*pArg)==0 ){
    pFile->ctrlFlags &= ~mask;
  }else{
    pFile->ctrlFlags |= mask;
  }
}

/* Forward declaration */
static int unixGetTempname(int nBuf, char *zBuf);

/*
** Information and control of an open file handle.
** 有关一个打开文件句柄的信息和控制
*/
static int unixFileControl(sqlite3_file *id, int op, void *pArg){
  unixFile *pFile = (unixFile*)id;
  switch( op ){
#if defined(__linux__) && defined(SQLITE_ENABLE_BATCH_ATOMIC_WRITE)
    case SQLITE_FCNTL_BEGIN_ATOMIC_WRITE: {
      int rc = osIoctl(pFile->h, F2FS_IOC_START_ATOMIC_WRITE);
      return rc ? SQLITE_IOERR_BEGIN_ATOMIC : SQLITE_OK;
    }
    case SQLITE_FCNTL_COMMIT_ATOMIC_WRITE: {
      int rc = osIoctl(pFile->h, F2FS_IOC_COMMIT_ATOMIC_WRITE);
      return rc ? SQLITE_IOERR_COMMIT_ATOMIC : SQLITE_OK;
    }
    case SQLITE_FCNTL_ROLLBACK_ATOMIC_WRITE: {
      int rc = osIoctl(pFile->h, F2FS_IOC_ABORT_VOLATILE_WRITE);
      return rc ? SQLITE_IOERR_ROLLBACK_ATOMIC : SQLITE_OK;
    }
#endif /* __linux__ && SQLITE_ENABLE_BATCH_ATOMIC_WRITE */

    case SQLITE_FCNTL_LOCKSTATE: {
      *(int*)pArg = pFile->eFileLock;
      return SQLITE_OK;
    }
    case SQLITE_FCNTL_LAST_ERRNO: {
      *(int*)pArg = pFile->lastErrno;
      return SQLITE_OK;
    }
    case SQLITE_FCNTL_CHUNK_SIZE: {
      pFile->szChunk = *(int *)pArg;
      return SQLITE_OK;
    }
    case SQLITE_FCNTL_SIZE_HINT: {
      int rc;
      SimulateIOErrorBenign(1);
      rc = fcntlSizeHint(pFile, *(i64 *)pArg);
      SimulateIOErrorBenign(0);
      return rc;
    }
    case SQLITE_FCNTL_PERSIST_WAL: {
      unixModeBit(pFile, UNIXFILE_PERSIST_WAL, (int*)pArg);
      return SQLITE_OK;
    }
    case SQLITE_FCNTL_POWERSAFE_OVERWRITE: {
      unixModeBit(pFile, UNIXFILE_PSOW, (int*)pArg);
      return SQLITE_OK;
    }
    case SQLITE_FCNTL_VFSNAME: {
      *(char**)pArg = sqlite3_mprintf("%s", pFile->pVfs->zName);
      return SQLITE_OK;
    }
    case SQLITE_FCNTL_TEMPFILENAME: {
      char *zTFile = sqlite3_malloc64( pFile->pVfs->mxPathname );
      if( zTFile ){
        unixGetTempname(pFile->pVfs->mxPathname, zTFile);
        *(char**)pArg = zTFile;
      }
      return SQLITE_OK;
    }
    case SQLITE_FCNTL_HAS_MOVED: {
      *(int*)pArg = fileHasMoved(pFile);
      return SQLITE_OK;
    }
#ifdef SQLITE_ENABLE_SETLK_TIMEOUT
    case SQLITE_FCNTL_LOCK_TIMEOUT: {
      int iOld = pFile->iBusyTimeout;
      pFile->iBusyTimeout = *(int*)pArg;
      *(int*)pArg = iOld;
      return SQLITE_OK;
    }
#endif
#if SQLITE_MAX_MMAP_SIZE>0
    case SQLITE_FCNTL_MMAP_SIZE: {
      i64 newLimit = *(i64*)pArg;
      int rc = SQLITE_OK;
      if( newLimit>sqlite3GlobalConfig.mxMmap ){
        newLimit = sqlite3GlobalConfig.mxMmap;
      }

      /* The value of newLimit may be eventually cast to (size_t) and passed
      ** to mmap(). Restrict its value to 2GB if (size_t) is not at least a
      ** 64-bit type. */
      if( newLimit>0 && sizeof(size_t)<8 ){
        newLimit = (newLimit & 0x7FFFFFFF);
      }

      *(i64*)pArg = pFile->mmapSizeMax;
      if( newLimit>=0 && newLimit!=pFile->mmapSizeMax && pFile->nFetchOut==0 ){
        pFile->mmapSizeMax = newLimit;
        if( pFile->mmapSize>0 ){
          unixUnmapfile(pFile);
          rc = unixMapfile(pFile, -1);
        }
      }
      return rc;
    }
#endif

  }
  return SQLITE_NOTFOUND;
}

/*
** If pFd->sectorSize is non-zero when this function is called, it is a
** no-op. Otherwise, the values of pFd->sectorSize and 
** pFd->deviceCharacteristics are set according to the file-system 
** characteristics. 
**
** There are two versions of this function. One for QNX and one for all
** other systems.
*/
#ifndef __QNXNTO__
static void setDeviceCharacteristics(unixFile *pFd){
  assert( pFd->deviceCharacteristics==0 || pFd->sectorSize!=0 );
  if( pFd->sectorSize==0 ){
#if defined(__linux__) && defined(SQLITE_ENABLE_BATCH_ATOMIC_WRITE)
    int res;
    u32 f = 0;

    /* Check for support for F2FS atomic batch writes. */
    res = osIoctl(pFd->h, F2FS_IOC_GET_FEATURES, &f);
    if( res==0 && (f & F2FS_FEATURE_ATOMIC_WRITE) ){
      pFd->deviceCharacteristics = SQLITE_IOCAP_BATCH_ATOMIC;
    }
#endif /* __linux__ && SQLITE_ENABLE_BATCH_ATOMIC_WRITE */

    /* Set the POWERSAFE_OVERWRITE flag if requested. */
    if( pFd->ctrlFlags & UNIXFILE_PSOW ){
      pFd->deviceCharacteristics |= SQLITE_IOCAP_POWERSAFE_OVERWRITE;
    }

    pFd->sectorSize = SQLITE_DEFAULT_SECTOR_SIZE;
  }
}
#else
#include <sys/dcmd_blk.h>
#include <sys/statvfs.h>
static void setDeviceCharacteristics(unixFile *pFile){
  if( pFile->sectorSize == 0 ){
    struct statvfs fsInfo;
       
    /* Set defaults for non-supported filesystems */
    pFile->sectorSize = SQLITE_DEFAULT_SECTOR_SIZE;
    pFile->deviceCharacteristics = 0;
    if( fstatvfs(pFile->h, &fsInfo) == -1 ) {
      return;
    }

    if( !strcmp(fsInfo.f_basetype, "tmp") ) {
      pFile->sectorSize = fsInfo.f_bsize;
      pFile->deviceCharacteristics =
        SQLITE_IOCAP_ATOMIC4K |       /* All ram filesystem writes are atomic */
        SQLITE_IOCAP_SAFE_APPEND |    /* growing the file does not occur until
                                      ** the write succeeds */
        SQLITE_IOCAP_SEQUENTIAL |     /* The ram filesystem has no write behind
                                      ** so it is ordered */
        0;
    }else if( strstr(fsInfo.f_basetype, "etfs") ){
      pFile->sectorSize = fsInfo.f_bsize;
      pFile->deviceCharacteristics =
        /* etfs cluster size writes are atomic */
        (pFile->sectorSize / 512 * SQLITE_IOCAP_ATOMIC512) |
        SQLITE_IOCAP_SAFE_APPEND |    /* growing the file does not occur until
                                      ** the write succeeds */
        SQLITE_IOCAP_SEQUENTIAL |     /* The ram filesystem has no write behind
                                      ** so it is ordered */
        0;
    }else if( !strcmp(fsInfo.f_basetype, "qnx6") ){
      pFile->sectorSize = fsInfo.f_bsize;
      pFile->deviceCharacteristics =
        SQLITE_IOCAP_ATOMIC |         /* All filesystem writes are atomic */
        SQLITE_IOCAP_SAFE_APPEND |    /* growing the file does not occur until
                                      ** the write succeeds */
        SQLITE_IOCAP_SEQUENTIAL |     /* The ram filesystem has no write behind
                                      ** so it is ordered */
        0;
    }else if( !strcmp(fsInfo.f_basetype, "qnx4") ){
      pFile->sectorSize = fsInfo.f_bsize;
      pFile->deviceCharacteristics =
        /* full bitset of atomics from max sector size and smaller */
        ((pFile->sectorSize / 512 * SQLITE_IOCAP_ATOMIC512) << 1) - 2 |
        SQLITE_IOCAP_SEQUENTIAL |     /* The ram filesystem has no write behind
                                      ** so it is ordered */
        0;
    }else if( strstr(fsInfo.f_basetype, "dos") ){
      pFile->sectorSize = fsInfo.f_bsize;
      pFile->deviceCharacteristics =
        /* full bitset of atomics from max sector size and smaller */
        ((pFile->sectorSize / 512 * SQLITE_IOCAP_ATOMIC512) << 1) - 2 |
        SQLITE_IOCAP_SEQUENTIAL |     /* The ram filesystem has no write behind
                                      ** so it is ordered */
        0;
    }else{
      pFile->deviceCharacteristics =
        SQLITE_IOCAP_ATOMIC512 |      /* blocks are atomic */
        SQLITE_IOCAP_SAFE_APPEND |    /* growing the file does not occur until
                                      ** the write succeeds */
        0;
    }
  }
  /* Last chance verification.  If the sector size isn't a multiple of 512
  ** then it isn't valid.*/
  if( pFile->sectorSize % 512 != 0 ){
    pFile->deviceCharacteristics = 0;
    pFile->sectorSize = SQLITE_DEFAULT_SECTOR_SIZE;
  }
}
#endif

/*
** Return the sector size in bytes of the underlying block device for
** the specified file. This is almost always 512 bytes, but may be
** larger for some devices.
** 以指定文件的底层块设备来返回扇区大小。这几乎总是512字节，但可能对于一些设备来说更大。
**
** SQLite code assumes this function cannot fail. It also assumes that
** if two files are created in the same file-system directory (i.e.
** a database and its journal file) that the sector size will be the
** same for both.
** SQLITE代码假设这个函数不会失败。它还假设如果两个文件在相同的文件系统目录中创建（即一个
** 数据库和它的日志文件）两个扇区大小将一样。
*/
static int unixSectorSize(sqlite3_file *id){
  unixFile *pFd = (unixFile*)id;
  setDeviceCharacteristics(pFd);
  return pFd->sectorSize;
}

/*
** Return the device characteristics for the file.
** 返回文件的设备特征。
**
** This VFS is set up to return SQLITE_IOCAP_POWERSAFE_OVERWRITE by default.
** However, that choice is controversial since technically the underlying
** file system does not always provide powersafe overwrites.  (In other
** words, after a power-loss event, parts of the file that were never
** written might end up being altered.)  However, non-PSOW behavior is very,
** very rare.  And asserting PSOW makes a large reduction in the amount
** of required I/O for journaling, since a lot of padding is eliminated.
**  Hence, while POWERSAFE_OVERWRITE is on by default, there is a file-control
** available to turn it off and URI query parameter available to turn it off.
** 这个VFS设置来默认返回SQLITE_IOCAP_POWERSAFE_OVERWRITE.
** 但是，这个选择是矛盾的，因为技术上底层文件系统不会总是提供电源安全覆盖。
** （换句话说，发生停电事件后，部分从来没被写入的文件可能最终被修改。）但是，non-PSOW行为
** 是非常罕见的。并且声明PSOW使日志的I/O请求数量大量减少，因为很多填充被消除。因此，虽然
** POWERSAFE_OVERWRITE在默认情况下，但有个文件控制可用来关掉它且URI查询参数可用来关掉它。
*/
static int unixDeviceCharacteristics(sqlite3_file *id){
  unixFile *pFd = (unixFile*)id;
  setDeviceCharacteristics(pFd);
  return pFd->deviceCharacteristics;
}

#if !defined(SQLITE_OMIT_WAL) || SQLITE_MAX_MMAP_SIZE>0

/*
** Return the system page size.
**
** This function should not be called directly by other code in this file. 
** Instead, it should be called via macro osGetpagesize().
*/
static int unixGetpagesize(void){
#else
  return (int)sysconf(_SC_PAGESIZE);
#endif
}

#endif /* !defined(SQLITE_OMIT_WAL) || SQLITE_MAX_MMAP_SIZE>0 */

#ifndef SQLITE_OMIT_WAL

/*
** Object used to represent an shared memory buffer.  
** 对象用来代表一个共享内存缓冲区
**
** When multiple threads all reference the same wal-index, each thread
** has its own unixShm object, but they all point to a single instance
** of this unixShmNode object.  In other words, each wal-index is opened
** only once per process.
** 当多个线程都引用相同的wal-index索引，每个线程有它自己的unixShm对象，但是它们
** 都指向这个unixShmNode对象的一个实例。换句话说，每个wal-index每个过程只打开一次。
**
** Each unixShmNode object is connected to a single unixInodeInfo object.
** We could coalesce this object into unixInodeInfo, but that would mean
** every open file that does not use shared memory (in other words, most
** open files) would have to carry around this extra information.  So
** the unixInodeInfo object contains a pointer to this unixShmNode object
** and the unixShmNode object is created only when needed.
** 每个unixShmNode对象关联到一个unixInodeInfo对象。我们可以合并这个对象到unixInodeInfo,
** 但这意味着每一个打开的文件不使用共享内存（换句话说，大多数打开的文件）必须携带这额外的信息
** 所以这个unixInodeInfo对象包含这个unixShmNode对象的一个指针，且unixShmNode对象仅当需要的时候创建
**
** unixMutexHeld() must be true when creating or destroying
** this object or while reading or writing the following fields:
** unixMutexHeld()必须为真，当创建或者销毁这个对象时，或当读或写以下字段：
**
**      nRef
**
** The following fields are read-only after the object is created:
** 以下字段在对象创建之后是只读的。
** 
**      hShm
**      zFilename
**
** Either unixShmNode.pShmMutex must be held or unixShmNode.nRef==0 and
** unixMutexHeld() is true when reading or writing any other field
** in this structure.
** 要么unixShmNode.mutex互斥必须被持有或unixShmNode.nRef为0，并且当读或写在这个结构体
** 的任何其他字段时unixMutexHeld()为真
*/
struct unixShmNode {
  unixInodeInfo *pInode;     /* unixInodeInfo that owns this SHM node */ //unixInodeInfo拥有这个SHM节点
  sqlite3_mutex *pShmMutex;  /* Mutex to access this object */ //互斥访问这个对象
  char *zFilename;           /* Name of the mmapped file */
  int hShm;                  /* Open file descriptor */  //打开文件描述符
  int szRegion;              /* Size of shared-memory regions */ //共享内存区域的大小
  u16 nRegion;               /* Size of array apRegion */ //数组apRegion大小
  u8 isReadonly;             /* True if read-only */
  u8 isUnlocked;             /* True if no DMS lock held */
  char **apRegion;           /* Array of mapped shared-memory regions */ //映射共享内存区域的数组
  int nRef;                  /* Number of unixShm objects pointing to this */ //许多unixShm对象指向这一点
  unixShm *pFirst;           /* All unixShm objects pointing to this */
  int aLock[SQLITE_SHM_NLOCK];  /* # shared locks on slot, -1==excl lock */
};

/*
** Structure used internally by this VFS to record the state of an
** open shared memory connection.
** 这个VFS使用内部的结构体来记录一个打开的共享内存连接的状态
**
** The following fields are initialized when this object is created and
** are read-only thereafter:
** 以下字段被初始化是当这个对象创建时，且之后是只读的。
**
**    unixShm.pShmNode
**    unixShm.id
**
** All other fields are read/write.  The unixShm.pShmNode->pShmMutex must
** be held while accessing any read/write fields.
** 所以其他字段是读/写。unixShm.pFile->mutex必须被持有，当访问任何读/写字段。
*/
struct unixShm {
  unixShmNode *pShmNode;     /* The underlying unixShmNode object */
                             //底层unixShmNode对象
  unixShm *pNext;            /* Next unixShm with the same unixShmNode */
                             //下一个一样的unixShmNode的unixShm
  u8 hasMutex;               /* True if holding the unixShmNode->pShmMutex */
                             //如果持有unixShmNode互斥则为真
  u8 id;                     /* Id of this connection within its unixShmNode */
                             //unixShmNode内连接的Id
  u16 sharedMask;            /* Mask of shared locks held */
                             //持有共享锁掩码
  u16 exclMask;              /* Mask of exclusive locks held */
                             //持有排它锁掩码
};

/*
** Constants used for locking
*/
#define UNIX_SHM_BASE   ((22+SQLITE_SHM_NLOCK)*4)         /* first lock byte */
#define UNIX_SHM_DMS    (UNIX_SHM_BASE+SQLITE_SHM_NLOCK)  /* deadman switch */

/*
** Apply posix advisory locks for all bytes from ofst through ofst+n-1.
**
** Locks block if the mask is exactly UNIX_SHM_C and are non-blocking
** otherwise.
*/
static int unixShmSystemLock(
  unixFile *pFile,       /* Open connection to the WAL file */
  int lockType,          /* F_UNLCK, F_RDLCK, or F_WRLCK */
  int ofst,              /* First byte of the locking range */
  int n                  /* Number of bytes to lock */
){
  unixShmNode *pShmNode; /* Apply locks to this open shared-memory segment */
  struct flock f;        /* The posix advisory locking structure */
  int rc = SQLITE_OK;    /* Result code form fcntl() */

  /* Access to the unixShmNode object is serialized by the caller */
  pShmNode = pFile->pInode->pShmNode;
  assert( pShmNode->nRef==0 || sqlite3_mutex_held(pShmNode->pShmMutex) );
  assert( pShmNode->nRef>0 || unixMutexHeld() );

  /* Shared locks never span more than one byte */
  assert( n==1 || lockType!=F_RDLCK );

  /* Locks are within range */
  assert( n>=1 && n<=SQLITE_SHM_NLOCK );

  if( pShmNode->hShm>=0 ){
    int res;
    /* Initialize the locking parameters */
    f.l_type = lockType;
    f.l_whence = SEEK_SET;
    f.l_start = ofst;
    f.l_len = n;
    res = osSetPosixAdvisoryLock(pShmNode->hShm, &f, pFile);
    if( res==-1 ){
#ifdef SQLITE_ENABLE_SETLK_TIMEOUT
      rc = (pFile->iBusyTimeout ? SQLITE_BUSY_TIMEOUT : SQLITE_BUSY);
#else
      rc = SQLITE_BUSY;
#endif
    }
  }

  return rc;        
}

/*
** Return the minimum number of 32KB shm regions that should be mapped at
** a time, assuming that each mapping must be an integer multiple of the
** current system page-size.
**
** Usually, this is 1. The exception seems to be systems that are configured
** to use 64KB pages - in this case each mapping must cover at least two
** shm regions.
*/
static int unixShmRegionPerMap(void){
  int shmsz = 32*1024;            /* SHM region size */
  int pgsz = osGetpagesize();   /* System page size */
  assert( ((pgsz-1)&pgsz)==0 );   /* Page size must be a power of 2 */
  if( pgsz<shmsz ) return 1;
  return pgsz/shmsz;
}

/*
** Purge the unixShmNodeList list of all entries with unixShmNode.nRef==0.
**
** This is not a VFS shared-memory method; it is a utility function called
** by VFS shared-memory methods.
*/
static void unixShmPurge(unixFile *pFd){
  unixShmNode *p = pFd->pInode->pShmNode;
  assert( unixMutexHeld() );
  if( p && ALWAYS(p->nRef==0) ){
    int nShmPerMap = unixShmRegionPerMap();
    int i;
    assert( p->pInode==pFd->pInode );
    sqlite3_mutex_free(p->pShmMutex);
    for(i=0; i<p->nRegion; i+=nShmPerMap){
      if( p->hShm>=0 ){
        osMunmap(p->apRegion[i], p->szRegion);
      }else{
        sqlite3_free(p->apRegion[i]);
      }
    }
    sqlite3_free(p->apRegion);
    if( p->hShm>=0 ){
      robust_close(pFd, p->hShm, __LINE__);
      p->hShm = -1;
    }
    p->pInode->pShmNode = 0;
    sqlite3_free(p);
  }
}

/*
** The DMS lock has not yet been taken on shm file pShmNode. Attempt to
** take it now. Return SQLITE_OK if successful, or an SQLite error
** code otherwise.
**
** If the DMS cannot be locked because this is a readonly_shm=1 
** connection and no other process already holds a lock, return
** SQLITE_READONLY_CANTINIT and set pShmNode->isUnlocked=1.
*/
static int unixLockSharedMemory(unixFile *pDbFd, unixShmNode *pShmNode){
  struct flock lock;
  int rc = SQLITE_OK;

  /* Use F_GETLK to determine the locks other processes are holding
  ** on the DMS byte. If it indicates that another process is holding
  ** a SHARED lock, then this process may also take a SHARED lock
  ** and proceed with opening the *-shm file. 
  **
  ** Or, if no other process is holding any lock, then this process
  ** is the first to open it. In this case take an EXCLUSIVE lock on the
  ** DMS byte and truncate the *-shm file to zero bytes in size. Then
  ** downgrade to a SHARED lock on the DMS byte.
  **
  ** If another process is holding an EXCLUSIVE lock on the DMS byte,
  ** return SQLITE_BUSY to the caller (it will try again). An earlier
  ** version of this code attempted the SHARED lock at this point. But
  ** this introduced a subtle race condition: if the process holding
  ** EXCLUSIVE failed just before truncating the *-shm file, then this
  ** process might open and use the *-shm file without truncating it.
  ** And if the *-shm file has been corrupted by a power failure or
  ** system crash, the database itself may also become corrupt.  */
  lock.l_whence = SEEK_SET;
  lock.l_start = UNIX_SHM_DMS;
  lock.l_len = 1;
  lock.l_type = F_WRLCK;
  if( osFcntl(pShmNode->hShm, F_GETLK, &lock)!=0 ) {
    rc = SQLITE_IOERR_LOCK;
  }else if( lock.l_type==F_UNLCK ){
    if( pShmNode->isReadonly ){
      pShmNode->isUnlocked = 1;
      rc = SQLITE_READONLY_CANTINIT;
    }else{
      rc = unixShmSystemLock(pDbFd, F_WRLCK, UNIX_SHM_DMS, 1);
      /* The first connection to attach must truncate the -shm file.  We
      ** truncate to 3 bytes (an arbitrary small number, less than the
      ** -shm header size) rather than 0 as a system debugging aid, to
      ** help detect if a -shm file truncation is legitimate or is the work
      ** or a rogue process. */
      if( rc==SQLITE_OK && robust_ftruncate(pShmNode->hShm, 3) ){
        rc = unixLogError(SQLITE_IOERR_SHMOPEN,"ftruncate",pShmNode->zFilename);
      }
    }
  }else if( lock.l_type==F_WRLCK ){
    rc = SQLITE_BUSY;
  }

  if( rc==SQLITE_OK ){
    assert( lock.l_type==F_UNLCK || lock.l_type==F_RDLCK );
    rc = unixShmSystemLock(pDbFd, F_RDLCK, UNIX_SHM_DMS, 1);
  }
  return rc;
}

/*
** Open a shared-memory area associated with open database file pDbFd.  
** This particular implementation uses mmapped files.
**
** The file used to implement shared-memory is in the same directory
** as the open database file and has the same name as the open database
** file with the "-shm" suffix added.  For example, if the database file
** is "/home/user1/config.db" then the file that is created and mmapped
** for shared memory will be called "/home/user1/config.db-shm".  
**
** Another approach to is to use files in /dev/shm or /dev/tmp or an
** some other tmpfs mount. But if a file in a different directory
** from the database file is used, then differing access permissions
** or a chroot() might cause two different processes on the same
** database to end up using different files for shared memory - 
** meaning that their memory would not really be shared - resulting
** in database corruption.  Nevertheless, this tmpfs file usage
** can be enabled at compile-time using -DSQLITE_SHM_DIRECTORY="/dev/shm"
** or the equivalent.  The use of the SQLITE_SHM_DIRECTORY compile-time
** option results in an incompatible build of SQLite;  builds of SQLite
** that with differing SQLITE_SHM_DIRECTORY settings attempt to use the
** same database file at the same time, database corruption will likely
** result. The SQLITE_SHM_DIRECTORY compile-time option is considered
** "unsupported" and may go away in a future SQLite release.
**
** When opening a new shared-memory file, if no other instances of that
** file are currently open, in this process or in other processes, then
** the file must be truncated to zero length or have its header cleared.
**
** If the original database file (pDbFd) is using the "unix-excl" VFS
** that means that an exclusive lock is held on the database file and
** that no other processes are able to read or write the database.  In
** that case, we do not really need shared memory.  No shared memory
** file is created.  The shared memory will be simulated with heap memory.
*/
static int unixOpenSharedMemory(unixFile *pDbFd){
  struct unixShm *p = 0;          /* The connection to be opened */
  struct unixShmNode *pShmNode;   /* The underlying mmapped file */
  int rc = SQLITE_OK;             /* Result code */
  unixInodeInfo *pInode;          /* The inode of fd */
  char *zShm;             /* Name of the file used for SHM */
  int nShmFilename;               /* Size of the SHM filename in bytes */

  /* Allocate space for the new unixShm object. */
  p = sqlite3_malloc64( sizeof(*p) );
  if( p==0 ) return SQLITE_NOMEM_BKPT;
  memset(p, 0, sizeof(*p));
  assert( pDbFd->pShm==0 );

  /* Check to see if a unixShmNode object already exists. Reuse an existing
  ** one if present. Create a new one if necessary.
  */
  assert( unixFileMutexNotheld(pDbFd) );
  unixEnterMutex();
  pInode = pDbFd->pInode;
  pShmNode = pInode->pShmNode;
  if( pShmNode==0 ){
    struct stat sStat;                 /* fstat() info for database file */
#ifndef SQLITE_SHM_DIRECTORY
    const char *zBasePath = pDbFd->zPath;
#endif

    /* Call fstat() to figure out the permissions on the database file. If
    ** a new *-shm file is created, an attempt will be made to create it
    ** with the same permissions.
    */
    if( osFstat(pDbFd->h, &sStat) ){
      rc = SQLITE_IOERR_FSTAT;
      goto shm_open_err;
    }

#ifdef SQLITE_SHM_DIRECTORY
    nShmFilename = sizeof(SQLITE_SHM_DIRECTORY) + 31;
#else
    nShmFilename = 6 + (int)strlen(zBasePath);
#endif
    pShmNode = sqlite3_malloc64( sizeof(*pShmNode) + nShmFilename );
    if( pShmNode==0 ){
      rc = SQLITE_NOMEM_BKPT;
      goto shm_open_err;
    }
    memset(pShmNode, 0, sizeof(*pShmNode)+nShmFilename);
    zShm = pShmNode->zFilename = (char*)&pShmNode[1];
#ifdef SQLITE_SHM_DIRECTORY
    sqlite3_snprintf(nShmFilename, zShm, 
                     SQLITE_SHM_DIRECTORY "/sqlite-shm-%x-%x",
                     (u32)sStat.st_ino, (u32)sStat.st_dev);
#else
    sqlite3_snprintf(nShmFilename, zShm, "%s-shm", zBasePath);
    sqlite3FileSuffix3(pDbFd->zPath, zShm);
#endif
    pShmNode->hShm = -1;
    pDbFd->pInode->pShmNode = pShmNode;
    pShmNode->pInode = pDbFd->pInode;
    if( sqlite3GlobalConfig.bCoreMutex ){
      pShmNode->pShmMutex = sqlite3_mutex_alloc(SQLITE_MUTEX_FAST);
      if( pShmNode->pShmMutex==0 ){
        rc = SQLITE_NOMEM_BKPT;
        goto shm_open_err;
      }
    }

    if( pInode->bProcessLock==0 ){
      if( 0==sqlite3_uri_boolean(pDbFd->zPath, "readonly_shm", 0) ){
        pShmNode->hShm = robust_open(zShm, O_RDWR|O_CREAT|O_NOFOLLOW,
                                     (sStat.st_mode&0777));
      }
      if( pShmNode->hShm<0 ){
        pShmNode->hShm = robust_open(zShm, O_RDONLY|O_NOFOLLOW,
                                     (sStat.st_mode&0777));
        if( pShmNode->hShm<0 ){
          rc = unixLogError(SQLITE_CANTOPEN_BKPT, "open", zShm);
          goto shm_open_err;
        }
        pShmNode->isReadonly = 1;
      }

      /* If this process is running as root, make sure that the SHM file
      ** is owned by the same user that owns the original database.  Otherwise,
      ** the original owner will not be able to connect.
      */
      robustFchown(pShmNode->hShm, sStat.st_uid, sStat.st_gid);

      rc = unixLockSharedMemory(pDbFd, pShmNode);
      if( rc!=SQLITE_OK && rc!=SQLITE_READONLY_CANTINIT ) goto shm_open_err;
    }
  }

  /* Make the new connection a child of the unixShmNode */
  p->pShmNode = pShmNode;
  pShmNode->nRef++;
  pDbFd->pShm = p;
  unixLeaveMutex();

  /* The reference count on pShmNode has already been incremented under
  ** the cover of the unixEnterMutex() mutex and the pointer from the
  ** new (struct unixShm) object to the pShmNode has been set. All that is
  ** left to do is to link the new object into the linked list starting
  ** at pShmNode->pFirst. This must be done while holding the
  ** pShmNode->pShmMutex.
  */
  sqlite3_mutex_enter(pShmNode->pShmMutex);
  p->pNext = pShmNode->pFirst;
  pShmNode->pFirst = p;
  sqlite3_mutex_leave(pShmNode->pShmMutex);
  return rc;

  /* Jump here on any error */
shm_open_err:
  unixShmPurge(pDbFd);       /* This call frees pShmNode if required */
  sqlite3_free(p);
  unixLeaveMutex();
  return rc;
}

/*
** This function is called to obtain a pointer to region iRegion of the 
** shared-memory associated with the database file fd. Shared-memory regions 
** are numbered starting from zero. Each shared-memory region is szRegion 
** bytes in size.
**
** If an error occurs, an error code is returned and *pp is set to NULL.
**
** Otherwise, if the bExtend parameter is 0 and the requested shared-memory
** region has not been allocated (by any client, including one running in a
** separate process), then *pp is set to NULL and SQLITE_OK returned. If 
** bExtend is non-zero and the requested shared-memory region has not yet 
** been allocated, it is allocated by this function.
**
** If the shared-memory region has already been allocated or is allocated by
** this call as described above, then it is mapped into this processes 
** address space (if it is not already), *pp is set to point to the mapped 
** memory and SQLITE_OK returned.
*/
static int unixShmMap(
  sqlite3_file *fd,               /* Handle open on database file */
  int iRegion,                    /* Region to retrieve */
  int szRegion,                   /* Size of regions */
  int bExtend,                    /* True to extend file if necessary */
  void volatile **pp              /* OUT: Mapped memory */
){
  unixFile *pDbFd = (unixFile*)fd;
  unixShm *p;
  unixShmNode *pShmNode;
  int rc = SQLITE_OK;
  int nShmPerMap = unixShmRegionPerMap();
  int nReqRegion;

  /* If the shared-memory file has not yet been opened, open it now. */
  if( pDbFd->pShm==0 ){
    rc = unixOpenSharedMemory(pDbFd);
    if( rc!=SQLITE_OK ) return rc;
  }

  p = pDbFd->pShm;
  pShmNode = p->pShmNode;
  sqlite3_mutex_enter(pShmNode->pShmMutex);
  if( pShmNode->isUnlocked ){
    rc = unixLockSharedMemory(pDbFd, pShmNode);
    if( rc!=SQLITE_OK ) goto shmpage_out;
    pShmNode->isUnlocked = 0;
  }
  assert( szRegion==pShmNode->szRegion || pShmNode->nRegion==0 );
  assert( pShmNode->pInode==pDbFd->pInode );
  assert( pShmNode->hShm>=0 || pDbFd->pInode->bProcessLock==1 );
  assert( pShmNode->hShm<0 || pDbFd->pInode->bProcessLock==0 );

  /* Minimum number of regions required to be mapped. */
  nReqRegion = ((iRegion+nShmPerMap) / nShmPerMap) * nShmPerMap;

  if( pShmNode->nRegion<nReqRegion ){
    char **apNew;                      /* New apRegion[] array */
    int nByte = nReqRegion*szRegion;   /* Minimum required file size */
    struct stat sStat;                 /* Used by fstat() */

    pShmNode->szRegion = szRegion;

    if( pShmNode->hShm>=0 ){
      /* The requested region is not mapped into this processes address space.
      ** Check to see if it has been allocated (i.e. if the wal-index file is
      ** large enough to contain the requested region).
      */
      if( osFstat(pShmNode->hShm, &sStat) ){
        rc = SQLITE_IOERR_SHMSIZE;
        goto shmpage_out;
      }
  
      if( sStat.st_size<nByte ){
        /* The requested memory region does not exist. If bExtend is set to
        ** false, exit early. *pp will be set to NULL and SQLITE_OK returned.
        */
        if( !bExtend ){
          goto shmpage_out;
        }

        /* Alternatively, if bExtend is true, extend the file. Do this by
        ** writing a single byte to the end of each (OS) page being
        ** allocated or extended. Technically, we need only write to the
        ** last page in order to extend the file. But writing to all new
        ** pages forces the OS to allocate them immediately, which reduces
        ** the chances of SIGBUS while accessing the mapped region later on.
        */
        else{
          static const int pgsz = 4096;
          int iPg;

          /* Write to the last byte of each newly allocated or extended page */
          assert( (nByte % pgsz)==0 );
          for(iPg=(sStat.st_size/pgsz); iPg<(nByte/pgsz); iPg++){
            int x = 0;
            if( seekAndWriteFd(pShmNode->hShm, iPg*pgsz + pgsz-1,"",1,&x)!=1 ){
              const char *zFile = pShmNode->zFilename;
              rc = unixLogError(SQLITE_IOERR_SHMSIZE, "write", zFile);
              goto shmpage_out;
            }
          }
        }
      }
    }

    /* Map the requested memory region into this processes address space. */
    apNew = (char **)sqlite3_realloc(
        pShmNode->apRegion, nReqRegion*sizeof(char *)
    );
    if( !apNew ){
      rc = SQLITE_IOERR_NOMEM_BKPT;
      goto shmpage_out;
    }
    pShmNode->apRegion = apNew;
    while( pShmNode->nRegion<nReqRegion ){
      int nMap = szRegion*nShmPerMap;
      int i;
      void *pMem;
      if( pShmNode->hShm>=0 ){
        pMem = osMmap(0, nMap,
            pShmNode->isReadonly ? PROT_READ : PROT_READ|PROT_WRITE, 
            MAP_SHARED, pShmNode->hShm, szRegion*(i64)pShmNode->nRegion
        );
        if( pMem==MAP_FAILED ){
          rc = unixLogError(SQLITE_IOERR_SHMMAP, "mmap", pShmNode->zFilename);
          goto shmpage_out;
        }
      }else{
        pMem = sqlite3_malloc64(nMap);
        if( pMem==0 ){
          rc = SQLITE_NOMEM_BKPT;
          goto shmpage_out;
        }
        memset(pMem, 0, nMap);
      }

      for(i=0; i<nShmPerMap; i++){
        pShmNode->apRegion[pShmNode->nRegion+i] = &((char*)pMem)[szRegion*i];
      }
      pShmNode->nRegion += nShmPerMap;
    }
  }

shmpage_out:
  if( pShmNode->nRegion>iRegion ){
    *pp = pShmNode->apRegion[iRegion];
  }else{
    *pp = 0;
  }
  if( pShmNode->isReadonly && rc==SQLITE_OK ) rc = SQLITE_READONLY;
  sqlite3_mutex_leave(pShmNode->pShmMutex);
  return rc;
}


/*
** Change the lock state for a shared-memory segment.
**
** Note that the relationship between SHAREd and EXCLUSIVE locks is a little
** different here than in posix.  In xShmLock(), one can go from unlocked
** to shared and back or from unlocked to exclusive and back.  But one may
** not go from shared to exclusive or from exclusive to shared.
*/
static int unixShmLock(
  sqlite3_file *fd,          /* Database file holding the shared memory */
  int ofst,                  /* First lock to acquire or release */
  int n,                     /* Number of locks to acquire or release */
  int flags                  /* What to do with the lock */
){
  unixFile *pDbFd = (unixFile*)fd;      /* Connection holding shared memory */
  unixShm *p = pDbFd->pShm;             /* The shared memory being locked */
  unixShmNode *pShmNode = p->pShmNode;  /* The underlying file iNode */
  int rc = SQLITE_OK;                   /* Result code */
  u16 mask;                             /* Mask of locks to take or release */
  int *aLock = pShmNode->aLock;

  assert( pShmNode==pDbFd->pInode->pShmNode );
  assert( pShmNode->pInode==pDbFd->pInode );
  assert( ofst>=0 && ofst+n<=SQLITE_SHM_NLOCK );
  assert( n>=1 );
  assert( flags==(SQLITE_SHM_LOCK | SQLITE_SHM_SHARED)
       || flags==(SQLITE_SHM_LOCK | SQLITE_SHM_EXCLUSIVE)
       || flags==(SQLITE_SHM_UNLOCK | SQLITE_SHM_SHARED)
       || flags==(SQLITE_SHM_UNLOCK | SQLITE_SHM_EXCLUSIVE) );
  assert( n==1 || (flags & SQLITE_SHM_EXCLUSIVE)!=0 );
  assert( pShmNode->hShm>=0 || pDbFd->pInode->bProcessLock==1 );
  assert( pShmNode->hShm<0 || pDbFd->pInode->bProcessLock==0 );

  /* Check that, if this to be a blocking lock, no locks that occur later
  ** in the following list than the lock being obtained are already held:
  **
  **   1. Checkpointer lock (ofst==1).
  **   2. Write lock (ofst==0).
  **   3. Read locks (ofst>=3 && ofst<SQLITE_SHM_NLOCK).
  **
  ** In other words, if this is a blocking lock, none of the locks that
  ** occur later in the above list than the lock being obtained may be
  ** held.  
  **
  ** It is not permitted to block on the RECOVER lock.
  */
#ifdef SQLITE_ENABLE_SETLK_TIMEOUT
  assert( (flags & SQLITE_SHM_UNLOCK) || pDbFd->iBusyTimeout==0 || (
         (ofst!=2)                                   /* not RECOVER */
      && (ofst!=1 || (p->exclMask|p->sharedMask)==0)
      && (ofst!=0 || (p->exclMask|p->sharedMask)<3)
      && (ofst<3  || (p->exclMask|p->sharedMask)<(1<<ofst))
  ));
#endif

  mask = (1<<(ofst+n)) - (1<<ofst);
  assert( n>1 || mask==(1<<ofst) );
  sqlite3_mutex_enter(pShmNode->pShmMutex);
  assert( assertLockingArrayOk(pShmNode) );
  if( flags & SQLITE_SHM_UNLOCK ){
    if( (p->exclMask|p->sharedMask) & mask ){
      int ii;
      int bUnlock = 1;

      for(ii=ofst; ii<ofst+n; ii++){
        if( aLock[ii]>((p->sharedMask & (1<<ii)) ? 1 : 0) ){
          bUnlock = 0;
        }
      }

      if( bUnlock ){
        rc = unixShmSystemLock(pDbFd, F_UNLCK, ofst+UNIX_SHM_BASE, n);
        if( rc==SQLITE_OK ){
          memset(&aLock[ofst], 0, sizeof(int)*n);
        }
      }else if( ALWAYS(p->sharedMask & (1<<ofst)) ){
        assert( n==1 && aLock[ofst]>1 );
        aLock[ofst]--;
      }

      /* Undo the local locks */
      if( rc==SQLITE_OK ){
        p->exclMask &= ~mask;
        p->sharedMask &= ~mask;
      } 
    }
  }else if( flags & SQLITE_SHM_SHARED ){
    assert( n==1 );
    assert( (p->exclMask & (1<<ofst))==0 );
    if( (p->sharedMask & mask)==0 ){
      if( aLock[ofst]<0 ){
        rc = SQLITE_BUSY;
      }else if( aLock[ofst]==0 ){
        rc = unixShmSystemLock(pDbFd, F_RDLCK, ofst+UNIX_SHM_BASE, n);
      }

      /* Get the local shared locks */
      if( rc==SQLITE_OK ){
        p->sharedMask |= mask;
        aLock[ofst]++;
      }
    }
  }else{
    /* Make sure no sibling connections hold locks that will block this
    ** lock.  If any do, return SQLITE_BUSY right away.  */
    int ii;
    for(ii=ofst; ii<ofst+n; ii++){
      assert( (p->sharedMask & mask)==0 );
      if( ALWAYS((p->exclMask & (1<<ii))==0) && aLock[ii] ){
        rc = SQLITE_BUSY;
        break;
      }
    }

    /* Get the exclusive locks at the system level. Then if successful
    ** also update the in-memory values. */
    if( rc==SQLITE_OK ){
      rc = unixShmSystemLock(pDbFd, F_WRLCK, ofst+UNIX_SHM_BASE, n);
      if( rc==SQLITE_OK ){
        assert( (p->sharedMask & mask)==0 );
        p->exclMask |= mask;
        for(ii=ofst; ii<ofst+n; ii++){
          aLock[ii] = -1;
        }
      }
    }
  }
  assert( assertLockingArrayOk(pShmNode) );
  sqlite3_mutex_leave(pShmNode->pShmMutex);
  OSTRACE(("SHM-LOCK shmid-%d, pid-%d got %03x,%03x\n",
           p->id, osGetpid(0), p->sharedMask, p->exclMask));
  return rc;
}

/*
** Implement a memory barrier or memory fence on shared memory.  
**
** All loads and stores begun before the barrier must complete before
** any load or store begun after the barrier.
*/
static void unixShmBarrier(
  sqlite3_file *fd                /* Database file holding the shared memory */
){
  UNUSED_PARAMETER(fd);
  sqlite3MemoryBarrier();         /* compiler-defined memory barrier */
  assert( fd->pMethods->xLock==nolockLock 
       || unixFileMutexNotheld((unixFile*)fd) 
  );
  unixEnterMutex();               /* Also mutex, for redundancy */
  unixLeaveMutex();
}

/*
** Close a connection to shared-memory.  Delete the underlying 
** storage if deleteFlag is true.
**
** If there is no shared memory associated with the connection then this
** routine is a harmless no-op.
*/
static int unixShmUnmap(
  sqlite3_file *fd,               /* The underlying database file */
  int deleteFlag                  /* Delete shared-memory if true */
){
  unixShm *p;                     /* The connection to be closed */
  unixShmNode *pShmNode;          /* The underlying shared-memory file */
  unixShm **pp;                   /* For looping over sibling connections */
  unixFile *pDbFd;                /* The underlying database file */

  pDbFd = (unixFile*)fd;
  p = pDbFd->pShm;
  if( p==0 ) return SQLITE_OK;
  pShmNode = p->pShmNode;

  assert( pShmNode==pDbFd->pInode->pShmNode );
  assert( pShmNode->pInode==pDbFd->pInode );

  /* Remove connection p from the set of connections associated
  ** with pShmNode */
  sqlite3_mutex_enter(pShmNode->pShmMutex);
  for(pp=&pShmNode->pFirst; (*pp)!=p; pp = &(*pp)->pNext){}
  *pp = p->pNext;

  /* Free the connection p */
  sqlite3_free(p);
  pDbFd->pShm = 0;
  sqlite3_mutex_leave(pShmNode->pShmMutex);

  /* If pShmNode->nRef has reached 0, then close the underlying
  ** shared-memory file, too */
  assert( unixFileMutexNotheld(pDbFd) );
  unixEnterMutex();
  assert( pShmNode->nRef>0 );
  pShmNode->nRef--;
  if( pShmNode->nRef==0 ){
    if( deleteFlag && pShmNode->hShm>=0 ){
      osUnlink(pShmNode->zFilename);
    }
    unixShmPurge(pDbFd);
  }
  unixLeaveMutex();

  return SQLITE_OK;
}


#else
# define unixShmMap     0
# define unixShmLock    0
# define unixShmBarrier 0
# define unixShmUnmap   0
#endif /* #ifndef SQLITE_OMIT_WAL */

#if SQLITE_MAX_MMAP_SIZE>0
/*
** If it is currently memory mapped, unmap file pFd.
*/
static void unixUnmapfile(unixFile *pFd){
  assert( pFd->nFetchOut==0 );
  if( pFd->pMapRegion ){
    osMunmap(pFd->pMapRegion, pFd->mmapSizeActual);
    pFd->pMapRegion = 0;
    pFd->mmapSize = 0;
    pFd->mmapSizeActual = 0;
  }
}

/*
** Attempt to set the size of the memory mapping maintained by file 
** descriptor pFd to nNew bytes. Any existing mapping is discarded.
**
** If successful, this function sets the following variables:
**
**       unixFile.pMapRegion
**       unixFile.mmapSize
**       unixFile.mmapSizeActual
**
** If unsuccessful, an error message is logged via sqlite3_log() and
** the three variables above are zeroed. In this case SQLite should
** continue accessing the database using the xRead() and xWrite()
** methods.
*/
static void unixRemapfile(
  unixFile *pFd,                  /* File descriptor object */
  i64 nNew                        /* Required mapping size */
){
  const char *zErr = "mmap";
  int h = pFd->h;                      /* File descriptor open on db file */
  u8 *pOrig = (u8 *)pFd->pMapRegion;   /* Pointer to current file mapping */
  i64 nOrig = pFd->mmapSizeActual;     /* Size of pOrig region in bytes */
  u8 *pNew = 0;                        /* Location of new mapping */
  int flags = PROT_READ;               /* Flags to pass to mmap() */

  assert( pFd->nFetchOut==0 );
  assert( nNew>pFd->mmapSize );
  assert( nNew<=pFd->mmapSizeMax );
  assert( nNew>0 );
  assert( pFd->mmapSizeActual>=pFd->mmapSize );
  assert( MAP_FAILED!=0 );

#ifdef SQLITE_MMAP_READWRITE
  if( (pFd->ctrlFlags & UNIXFILE_RDONLY)==0 ) flags |= PROT_WRITE;
#endif

  if( pOrig ){
#if HAVE_MREMAP
    i64 nReuse = pFd->mmapSize;
#else
    const int szSyspage = osGetpagesize();
    i64 nReuse = (pFd->mmapSize & ~(szSyspage-1));
#endif
    u8 *pReq = &pOrig[nReuse];

    /* Unmap any pages of the existing mapping that cannot be reused. */
    if( nReuse!=nOrig ){
      osMunmap(pReq, nOrig-nReuse);
    }

#if HAVE_MREMAP
    pNew = osMremap(pOrig, nReuse, nNew, MREMAP_MAYMOVE);
    zErr = "mremap";
#else
    pNew = osMmap(pReq, nNew-nReuse, flags, MAP_SHARED, h, nReuse);
    if( pNew!=MAP_FAILED ){
      if( pNew!=pReq ){
        osMunmap(pNew, nNew - nReuse);
        pNew = 0;
      }else{
        pNew = pOrig;
      }
    }
#endif

    /* The attempt to extend the existing mapping failed. Free it. */
    if( pNew==MAP_FAILED || pNew==0 ){
      osMunmap(pOrig, nReuse);
    }
  }

  /* If pNew is still NULL, try to create an entirely new mapping. */
  if( pNew==0 ){
    pNew = osMmap(0, nNew, flags, MAP_SHARED, h, 0);
  }

  if( pNew==MAP_FAILED ){
    pNew = 0;
    nNew = 0;
    unixLogError(SQLITE_OK, zErr, pFd->zPath);

    /* If the mmap() above failed, assume that all subsequent mmap() calls
    ** will probably fail too. Fall back to using xRead/xWrite exclusively
    ** in this case.  */
    pFd->mmapSizeMax = 0;
  }
  pFd->pMapRegion = (void *)pNew;
  pFd->mmapSize = pFd->mmapSizeActual = nNew;
}

/*
** Memory map or remap the file opened by file-descriptor pFd (if the file
** is already mapped, the existing mapping is replaced by the new). Or, if 
** there already exists a mapping for this file, and there are still 
** outstanding xFetch() references to it, this function is a no-op.
**
** If parameter nByte is non-negative, then it is the requested size of 
** the mapping to create. Otherwise, if nByte is less than zero, then the 
** requested size is the size of the file on disk. The actual size of the
** created mapping is either the requested size or the value configured 
** using SQLITE_FCNTL_MMAP_LIMIT, whichever is smaller.
**
** SQLITE_OK is returned if no error occurs (even if the mapping is not
** recreated as a result of outstanding references) or an SQLite error
** code otherwise.
*/
static int unixMapfile(unixFile *pFd, i64 nMap){
  assert( nMap>=0 || pFd->nFetchOut==0 );
  assert( nMap>0 || (pFd->mmapSize==0 && pFd->pMapRegion==0) );
  if( pFd->nFetchOut>0 ) return SQLITE_OK;

  if( nMap<0 ){
    struct stat statbuf;          /* Low-level file information */
    if( osFstat(pFd->h, &statbuf) ){
      return SQLITE_IOERR_FSTAT;
    }
    nMap = statbuf.st_size;
  }
  if( nMap>pFd->mmapSizeMax ){
    nMap = pFd->mmapSizeMax;
  }

  assert( nMap>0 || (pFd->mmapSize==0 && pFd->pMapRegion==0) );
  if( nMap!=pFd->mmapSize ){
    unixRemapfile(pFd, nMap);
  }

  return SQLITE_OK;
}
#endif /* SQLITE_MAX_MMAP_SIZE>0 */

/*
** If possible, return a pointer to a mapping of file fd starting at offset
** iOff. The mapping must be valid for at least nAmt bytes.
**
** If such a pointer can be obtained, store it in *pp and return SQLITE_OK.
** Or, if one cannot but no error occurs, set *pp to 0 and return SQLITE_OK.
** Finally, if an error does occur, return an SQLite error code. The final
** value of *pp is undefined in this case.
**
** If this function does return a pointer, the caller must eventually 
** release the reference by calling unixUnfetch().
*/
static int unixFetch(sqlite3_file *fd, i64 iOff, int nAmt, void **pp){
#if SQLITE_MAX_MMAP_SIZE>0
  unixFile *pFd = (unixFile *)fd;   /* The underlying database file */
#endif
  *pp = 0;

#if SQLITE_MAX_MMAP_SIZE>0
  if( pFd->mmapSizeMax>0 ){
    if( pFd->pMapRegion==0 ){
      int rc = unixMapfile(pFd, -1);
      if( rc!=SQLITE_OK ) return rc;
    }
    if( pFd->mmapSize >= iOff+nAmt ){
      *pp = &((u8 *)pFd->pMapRegion)[iOff];
      pFd->nFetchOut++;
    }
  }
#endif
  return SQLITE_OK;
}

/*
** If the third argument is non-NULL, then this function releases a 
** reference obtained by an earlier call to unixFetch(). The second
** argument passed to this function must be the same as the corresponding
** argument that was passed to the unixFetch() invocation. 
**
** Or, if the third argument is NULL, then this function is being called 
** to inform the VFS layer that, according to POSIX, any existing mapping 
** may now be invalid and should be unmapped.
*/
static int unixUnfetch(sqlite3_file *fd, i64 iOff, void *p){
#if SQLITE_MAX_MMAP_SIZE>0
  unixFile *pFd = (unixFile *)fd;   /* The underlying database file */
  UNUSED_PARAMETER(iOff);

  /* If p==0 (unmap the entire file) then there must be no outstanding 
  ** xFetch references. Or, if p!=0 (meaning it is an xFetch reference),
  ** then there must be at least one outstanding.  */
  assert( (p==0)==(pFd->nFetchOut==0) );

  /* If p!=0, it must match the iOff value. */
  assert( p==0 || p==&((u8 *)pFd->pMapRegion)[iOff] );

  if( p ){
    pFd->nFetchOut--;
  }else{
    unixUnmapfile(pFd);
  }

  assert( pFd->nFetchOut>=0 );
#else
  UNUSED_PARAMETER(fd);
  UNUSED_PARAMETER(p);
  UNUSED_PARAMETER(iOff);
#endif
  return SQLITE_OK;
}

/*
** Here ends the implementation of all sqlite3_file methods.
**
********************** End sqlite3_file Methods *******************************
******************************************************************************/

/*
** This division contains definitions of sqlite3_io_methods objects that
** implement various file locking strategies.  It also contains definitions
** of "finder" functions.  A finder-function is used to locate the appropriate
** sqlite3_io_methods object for a particular database file.  The pAppData
** field of the sqlite3_vfs VFS objects are initialized to be pointers to
** the correct finder-function for that VFS.
**
** Most finder functions return a pointer to a fixed sqlite3_io_methods
** object.  The only interesting finder-function is autolockIoFinder, which
** looks at the filesystem type and tries to guess the best locking
** strategy from that.
**
** For finder-function F, two objects are created:
**
**    (1) The real finder-function named "FImpt()".
**
**    (2) A constant pointer to this function named just "F".
**
**
** A pointer to the F pointer is used as the pAppData value for VFS
** objects.  We have to do this instead of letting pAppData point
** directly at the finder-function since C90 rules prevent a void*
** from be cast into a function pointer.
**
**
** Each instance of this macro generates two objects:
**
**   *  A constant sqlite3_io_methods object call METHOD that has locking
**      methods CLOSE, LOCK, UNLOCK, CKRESLOCK.
**
**   *  An I/O method finder function called FINDER that returns a pointer
**      to the METHOD object in the previous bullet.
*/
#define IOMETHODS(FINDER,METHOD,VERSION,CLOSE,LOCK,UNLOCK,CKLOCK,SHMMAP)     \
static const sqlite3_io_methods METHOD = {                                   \
   VERSION,                    /* iVersion */                                \
   CLOSE,                      /* xClose */                                  \
   unixRead,                   /* xRead */                                   \
   unixWrite,                  /* xWrite */                                  \
   unixTruncate,               /* xTruncate */                               \
   unixSync,                   /* xSync */                                   \
   unixFileSize,               /* xFileSize */                               \
   LOCK,                       /* xLock */                                   \
   UNLOCK,                     /* xUnlock */                                 \
   CKLOCK,                     /* xCheckReservedLock */                      \
   unixFileControl,            /* xFileControl */                            \
   unixSectorSize,             /* xSectorSize */                             \
   unixDeviceCharacteristics,  /* xDeviceCapabilities */                     \
   SHMMAP,                     /* xShmMap */                                 \
   unixShmLock,                /* xShmLock */                                \
   unixShmBarrier,             /* xShmBarrier */                             \
   unixShmUnmap,               /* xShmUnmap */                               \
   unixFetch,                  /* xFetch */                                  \
   unixUnfetch,                /* xUnfetch */                                \
};                                                                           \
static const sqlite3_io_methods *FINDER##Impl(const char *z, unixFile *p){   \
  UNUSED_PARAMETER(z); UNUSED_PARAMETER(p);                                  \
  return &METHOD;                                                            \
}                                                                            \
static const sqlite3_io_methods *(*const FINDER)(const char*,unixFile *p)    \
    = FINDER##Impl;

/*
** Here are all of the sqlite3_io_methods objects for each of the
** locking strategies.  Functions that return pointers to these methods
** are also created.
*/
IOMETHODS(
  posixIoFinder,            /* Finder function name */
  posixIoMethods,           /* sqlite3_io_methods object name */
  3,                        /* shared memory and mmap are enabled */
  unixClose,                /* xClose method */
  unixLock,                 /* xLock method */
  unixUnlock,               /* xUnlock method */
  unixCheckReservedLock,    /* xCheckReservedLock method */
  unixShmMap                /* xShmMap method */
)
IOMETHODS(
  nolockIoFinder,           /* Finder function name */
  nolockIoMethods,          /* sqlite3_io_methods object name */
  3,                        /* shared memory and mmap are enabled */
  nolockClose,              /* xClose method */
  nolockLock,               /* xLock method */
  nolockUnlock,             /* xUnlock method */
  nolockCheckReservedLock,  /* xCheckReservedLock method */
  0                         /* xShmMap method */
)
IOMETHODS(
  dotlockIoFinder,          /* Finder function name */
  dotlockIoMethods,         /* sqlite3_io_methods object name */
  1,                        /* shared memory is disabled */
  dotlockClose,             /* xClose method */
  dotlockLock,              /* xLock method */
  dotlockUnlock,            /* xUnlock method */
  dotlockCheckReservedLock, /* xCheckReservedLock method */
  0                         /* xShmMap method */
)

#if SQLITE_ENABLE_LOCKING_STYLE
IOMETHODS(
  flockIoFinder,            /* Finder function name */
  flockIoMethods,           /* sqlite3_io_methods object name */
  1,                        /* shared memory is disabled */
  flockClose,               /* xClose method */
  flockLock,                /* xLock method */
  flockUnlock,              /* xUnlock method */
  flockCheckReservedLock,   /* xCheckReservedLock method */
  0                         /* xShmMap method */
)
#endif


/*
** An abstract type for a pointer to an IO method finder function:
*/
typedef const sqlite3_io_methods *(*finder_type)(const char*,unixFile*);


/****************************************************************************
**************************** sqlite3_vfs methods ****************************
**
** This division contains the implementation of methods on the
** sqlite3_vfs object.
*/

/*
** Initialize the contents of the unixFile structure pointed to by pId.
*/
static int fillInUnixFile(
  sqlite3_vfs *pVfs,      /* Pointer to vfs object */
  int h,                  /* Open file descriptor of file being opened */
  sqlite3_file *pId,      /* Write to the unixFile structure here */
  const char *zFilename,  /* Name of the file being opened */
  int ctrlFlags           /* Zero or more UNIXFILE_* values */
){
  const sqlite3_io_methods *pLockingStyle;
  unixFile *pNew = (unixFile *)pId;
  int rc = SQLITE_OK;

  assert( pNew->pInode==NULL );

  /* No locking occurs in temporary files */
  assert( zFilename!=0 || (ctrlFlags & UNIXFILE_NOLOCK)!=0 );

  OSTRACE(("OPEN    %-3d %s\n", h, zFilename));
  pNew->h = h;
  pNew->pVfs = pVfs;
  pNew->zPath = zFilename;
  pNew->ctrlFlags = (u8)ctrlFlags;
#if SQLITE_MAX_MMAP_SIZE>0
  pNew->mmapSizeMax = sqlite3GlobalConfig.szMmap;
#endif
  if( sqlite3_uri_boolean(((ctrlFlags & UNIXFILE_URI) ? zFilename : 0),
                           "psow", SQLITE_POWERSAFE_OVERWRITE) ){
    pNew->ctrlFlags |= UNIXFILE_PSOW;
  }
  if( strcmp(pVfs->zName,"unix-excl")==0 ){
    pNew->ctrlFlags |= UNIXFILE_EXCL;
  }


  if( ctrlFlags & UNIXFILE_NOLOCK ){
    pLockingStyle = &nolockIoMethods;
  }else{
    pLockingStyle = (**(finder_type*)pVfs->pAppData)(zFilename, pNew);
#if SQLITE_ENABLE_LOCKING_STYLE
    /* Cache zFilename in the locking context (AFP and dotlock override) for
    ** proxyLock activation is possible (remote proxy is based on db name)
    ** zFilename remains valid until file is closed, to support */
    pNew->lockingContext = (void*)zFilename;
#endif
  }

  if( pLockingStyle == &posixIoMethods
  ){
    unixEnterMutex();
    rc = findInodeInfo(pNew, &pNew->pInode);
    if( rc!=SQLITE_OK ){
      /* If an error occurred in findInodeInfo(), close the file descriptor
      ** immediately, before releasing the mutex. findInodeInfo() may fail
      ** in two scenarios:
      **
      **   (a) A call to fstat() failed.
      **   (b) A malloc failed.
      **
      ** Scenario (b) may only occur if the process is holding no other
      ** file descriptors open on the same file. If there were other file
      ** descriptors on this file, then no malloc would be required by
      ** findInodeInfo(). If this is the case, it is quite safe to close
      ** handle h - as it is guaranteed that no posix locks will be released
      ** by doing so.
      **
      ** If scenario (a) caused the error then things are not so safe. The
      ** implicit assumption here is that if fstat() fails, things are in
      ** such bad shape that dropping a lock or two doesn't matter much.
      */
      robust_close(pNew, h, __LINE__);
      h = -1;
    }
    unixLeaveMutex();
  }


  else if( pLockingStyle == &dotlockIoMethods ){
    /* Dotfile locking uses the file path so it needs to be included in
    ** the dotlockLockingContext 
    */
    char *zLockFile;
    int nFilename;
    assert( zFilename!=0 );
    nFilename = (int)strlen(zFilename) + 6;
    zLockFile = (char *)sqlite3_malloc64(nFilename);
    if( zLockFile==0 ){
      rc = SQLITE_NOMEM_BKPT;
    }else{
      sqlite3_snprintf(nFilename, zLockFile, "%s" DOTLOCK_SUFFIX, zFilename);
    }
    pNew->lockingContext = zLockFile;
  }
  
  storeLastErrno(pNew, 0);

  if( rc!=SQLITE_OK ){
    if( h>=0 ) robust_close(pNew, h, __LINE__);
  }else{
    pId->pMethods = pLockingStyle;
    OpenCounter(+1);
    verifyDbFile(pNew);
  }
  return rc;
}

/*
** Return the name of a directory in which to put temporary files.
** If no suitable temporary file directory can be found, return NULL.
*/
static const char *unixTempFileDir(void){
  static const char *azDirs[] = {
     0,
     0,
     "/var/tmp",
     "/usr/tmp",
     "/tmp",
     "."
  };
  unsigned int i = 0;
  struct stat buf;
  const char *zDir = sqlite3_temp_directory;

  if( !azDirs[0] ) azDirs[0] = getenv("SQLITE_TMPDIR");
  if( !azDirs[1] ) azDirs[1] = getenv("TMPDIR");
  while(1){
    if( zDir!=0
     && osStat(zDir, &buf)==0
     && S_ISDIR(buf.st_mode)
     && osAccess(zDir, 03)==0
    ){
      return zDir;
    }
    if( i>=sizeof(azDirs)/sizeof(azDirs[0]) ) break;
    zDir = azDirs[i++];
  }
  return 0;
}

/*
** Create a temporary file name in zBuf.  zBuf must be allocated
** by the calling process and must be big enough to hold at least
** pVfs->mxPathname bytes.
*/
static int unixGetTempname(int nBuf, char *zBuf){
  const char *zDir;
  int iLimit = 0;

  /* It's odd to simulate an io-error here, but really this is just
  ** using the io-error infrastructure to test that SQLite handles this
  ** function failing. 
  */
  zBuf[0] = 0;
  SimulateIOError( return SQLITE_IOERR );

  zDir = unixTempFileDir();
  if( zDir==0 ) return SQLITE_IOERR_GETTEMPPATH;
  do{
    u64 r;
    sqlite3_randomness(sizeof(r), &r);
    assert( nBuf>2 );
    zBuf[nBuf-2] = 0;
    sqlite3_snprintf(nBuf, zBuf, "%s/"SQLITE_TEMP_FILE_PREFIX"%llx%c",
                     zDir, r, 0);
    if( zBuf[nBuf-2]!=0 || (iLimit++)>10 ) return SQLITE_ERROR;
  }while( osAccess(zBuf,0)==0 );
  return SQLITE_OK;
}


/*
** Search for an unused file descriptor that was opened on the database 
** file (not a journal or super-journal file) identified by pathname
** zPath with SQLITE_OPEN_XXX flags matching those passed as the second
** argument to this function.
**
** Such a file descriptor may exist if a database connection was closed
** but the associated file descriptor could not be closed because some
** other file descriptor open on the same file is holding a file-lock.
** Refer to comments in the unixClose() function and the lengthy comment
** describing "Posix Advisory Locking" at the start of this file for 
** further details. Also, ticket #4018.
**
** If a suitable file descriptor is found, then it is returned. If no
** such file descriptor is located, -1 is returned.
*/
static UnixUnusedFd *findReusableFd(const char *zPath, int flags){
  UnixUnusedFd *pUnused = 0;

  /* Do not search for an unused file descriptor on vxworks. Not because
  ** vxworks would not benefit from the change (it might, we're not sure),
  ** but because no way to test it is currently available. It is better 
  ** not to risk breaking vxworks support for the sake of such an obscure 
  ** feature.  */
#if !OS_VXWORKS
  struct stat sStat;                   /* Results of stat() call */

  unixEnterMutex();

  /* A stat() call may fail for various reasons. If this happens, it is
  ** almost certain that an open() call on the same path will also fail.
  ** For this reason, if an error occurs in the stat() call here, it is
  ** ignored and -1 is returned. The caller will try to open a new file
  ** descriptor on the same path, fail, and return an error to SQLite.
  **
  ** Even if a subsequent open() call does succeed, the consequences of
  ** not searching for a reusable file descriptor are not dire.  */
  if( inodeList!=0 && 0==osStat(zPath, &sStat) ){
    unixInodeInfo *pInode;

    pInode = inodeList;
    while( pInode && (pInode->fileId.dev!=sStat.st_dev
                     || pInode->fileId.ino!=(u64)sStat.st_ino) ){
       pInode = pInode->pNext;
    }
    if( pInode ){
      UnixUnusedFd **pp;
      assert( sqlite3_mutex_notheld(pInode->pLockMutex) );
      sqlite3_mutex_enter(pInode->pLockMutex);
      flags &= (SQLITE_OPEN_READONLY|SQLITE_OPEN_READWRITE);
      for(pp=&pInode->pUnused; *pp && (*pp)->flags!=flags; pp=&((*pp)->pNext));
      pUnused = *pp;
      if( pUnused ){
        *pp = pUnused->pNext;
      }
      sqlite3_mutex_leave(pInode->pLockMutex);
    }
  }
  unixLeaveMutex();
#endif    /* if !OS_VXWORKS */
  return pUnused;
}

/*
** Find the mode, uid and gid of file zFile. 
*/
static int getFileMode(
  const char *zFile,              /* File name */
  mode_t *pMode,                  /* OUT: Permissions of zFile */
  uid_t *pUid,                    /* OUT: uid of zFile. */
  gid_t *pGid                     /* OUT: gid of zFile. */
){
  struct stat sStat;              /* Output of stat() on database file */
  int rc = SQLITE_OK;
  if( 0==osStat(zFile, &sStat) ){
    *pMode = sStat.st_mode & 0777;
    *pUid = sStat.st_uid;
    *pGid = sStat.st_gid;
  }else{
    rc = SQLITE_IOERR_FSTAT;
  }
  return rc;
}

/*
** This function is called by unixOpen() to determine the unix permissions
** to create new files with. If no error occurs, then SQLITE_OK is returned
** and a value suitable for passing as the third argument to open(2) is
** written to *pMode. If an IO error occurs, an SQLite error code is 
** returned and the value of *pMode is not modified.
**
** In most cases, this routine sets *pMode to 0, which will become
** an indication to robust_open() to create the file using
** SQLITE_DEFAULT_FILE_PERMISSIONS adjusted by the umask.
** But if the file being opened is a WAL or regular journal file, then 
** this function queries the file-system for the permissions on the 
** corresponding database file and sets *pMode to this value. Whenever 
** possible, WAL and journal files are created using the same permissions 
** as the associated database file.
**
** If the SQLITE_ENABLE_8_3_NAMES option is enabled, then the
** original filename is unavailable.  But 8_3_NAMES is only used for
** FAT filesystems and permissions do not matter there, so just use
** the default permissions.  In 8_3_NAMES mode, leave *pMode set to zero.
*/
static int findCreateFileMode(
  const char *zPath,              /* Path of file (possibly) being created */
  int flags,                      /* Flags passed as 4th argument to xOpen() */
  mode_t *pMode,                  /* OUT: Permissions to open file with */
  uid_t *pUid,                    /* OUT: uid to set on the file */
  gid_t *pGid                     /* OUT: gid to set on the file */
){
  int rc = SQLITE_OK;             /* Return Code */
  *pMode = 0;
  *pUid = 0;
  *pGid = 0;
  if( flags & (SQLITE_OPEN_WAL|SQLITE_OPEN_MAIN_JOURNAL) ){
    char zDb[MAX_PATHNAME+1];     /* Database file path */
    int nDb;                      /* Number of valid bytes in zDb */

    /* zPath is a path to a WAL or journal file. The following block derives
    ** the path to the associated database file from zPath. This block handles
    ** the following naming conventions:
    **
    **   "<path to db>-journal"
    **   "<path to db>-wal"
    **   "<path to db>-journalNN"
    **   "<path to db>-walNN"
    **
    ** where NN is a decimal number. The NN naming schemes are 
    ** used by the test_multiplex.c module.
    */
    nDb = sqlite3Strlen30(zPath) - 1; 
    while( zPath[nDb]!='-' ){
      /* In normal operation, the journal file name will always contain
      ** a '-' character.  However in 8+3 filename mode, or if a corrupt
      ** rollback journal specifies a super-journal with a goofy name, then
      ** the '-' might be missing. */
      if( nDb==0 || zPath[nDb]=='.' ) return SQLITE_OK;
      nDb--;
    }
    memcpy(zDb, zPath, nDb);
    zDb[nDb] = '\0';

    rc = getFileMode(zDb, pMode, pUid, pGid);
  }else if( flags & SQLITE_OPEN_DELETEONCLOSE ){
    *pMode = 0600;
  }else if( flags & SQLITE_OPEN_URI ){
    /* If this is a main database file and the file was opened using a URI
    ** filename, check for the "modeof" parameter. If present, interpret
    ** its value as a filename and try to copy the mode, uid and gid from
    ** that file.  */
    const char *z = sqlite3_uri_parameter(zPath, "modeof");
    if( z ){
      rc = getFileMode(z, pMode, pUid, pGid);
    }
  }
  return rc;
}

/*
** Open the file zPath.
** 
** Previously, the SQLite OS layer used three functions in place of this
** one:
**
**     sqlite3OsOpenReadWrite();
**     sqlite3OsOpenReadOnly();
**     sqlite3OsOpenExclusive();
**
** These calls correspond to the following combinations of flags:
**
**     ReadWrite() ->     (READWRITE | CREATE)
**     ReadOnly()  ->     (READONLY) 
**     OpenExclusive() -> (READWRITE | CREATE | EXCLUSIVE)
**
** The old OpenExclusive() accepted a boolean argument - "delFlag". If
** true, the file was configured to be automatically deleted when the
** file handle closed. To achieve the same effect using this new 
** interface, add the DELETEONCLOSE flag to those specified above for 
** OpenExclusive().
*/
static int unixOpen(
  sqlite3_vfs *pVfs,           /* The VFS for which this is the xOpen method */
  const char *zPath,           /* Pathname of file to be opened */
  sqlite3_file *pFile,         /* The file descriptor to be filled in */
  int flags,                   /* Input flags to control the opening */
  int *pOutFlags               /* Output flags returned to SQLite core */
){
  unixFile *p = (unixFile *)pFile;
  int fd = -1;                   /* File descriptor returned by open() */
  int openFlags = 0;             /* Flags to pass to open() */
  int eType = flags&0x0FFF00;  /* Type of file to open */
  int noLock;                    /* True to omit locking primitives */
  int rc = SQLITE_OK;            /* Function Return Code */
  int ctrlFlags = 0;             /* UNIXFILE_* flags */

  int isExclusive  = (flags & SQLITE_OPEN_EXCLUSIVE);
  int isDelete     = (flags & SQLITE_OPEN_DELETEONCLOSE);
  int isCreate     = (flags & SQLITE_OPEN_CREATE);
  int isReadonly   = (flags & SQLITE_OPEN_READONLY);
  int isReadWrite  = (flags & SQLITE_OPEN_READWRITE);
#if SQLITE_ENABLE_LOCKING_STYLE
  int isAutoProxy  = (flags & SQLITE_OPEN_AUTOPROXY);
#endif

  /* If creating a super- or main-file journal, this function will open
  ** a file-descriptor on the directory too. The first time unixSync()
  ** is called the directory file descriptor will be fsync()ed and close()d.
  */
  int isNewJrnl = (isCreate && (
        eType==SQLITE_OPEN_SUPER_JOURNAL 
     || eType==SQLITE_OPEN_MAIN_JOURNAL 
     || eType==SQLITE_OPEN_WAL
  ));

  /* If argument zPath is a NULL pointer, this function is required to open
  ** a temporary file. Use this buffer to store the file name in.
  */
  char zTmpname[MAX_PATHNAME+2];
  const char *zName = zPath;

  /* Check the following statements are true: 
  **
  **   (a) Exactly one of the READWRITE and READONLY flags must be set, and 
  **   (b) if CREATE is set, then READWRITE must also be set, and
  **   (c) if EXCLUSIVE is set, then CREATE must also be set.
  **   (d) if DELETEONCLOSE is set, then CREATE must also be set.
  */
  assert((isReadonly==0 || isReadWrite==0) && (isReadWrite || isReadonly));
  assert(isCreate==0 || isReadWrite);
  assert(isExclusive==0 || isCreate);
  assert(isDelete==0 || isCreate);

  /* The main DB, main journal, WAL file and super-journal are never 
  ** automatically deleted. Nor are they ever temporary files.  */
  assert( (!isDelete && zName) || eType!=SQLITE_OPEN_MAIN_DB );
  assert( (!isDelete && zName) || eType!=SQLITE_OPEN_MAIN_JOURNAL );
  assert( (!isDelete && zName) || eType!=SQLITE_OPEN_SUPER_JOURNAL );
  assert( (!isDelete && zName) || eType!=SQLITE_OPEN_WAL );

  /* Assert that the upper layer has set one of the "file-type" flags. */
  assert( eType==SQLITE_OPEN_MAIN_DB      || eType==SQLITE_OPEN_TEMP_DB 
       || eType==SQLITE_OPEN_MAIN_JOURNAL || eType==SQLITE_OPEN_TEMP_JOURNAL 
       || eType==SQLITE_OPEN_SUBJOURNAL   || eType==SQLITE_OPEN_SUPER_JOURNAL 
       || eType==SQLITE_OPEN_TRANSIENT_DB || eType==SQLITE_OPEN_WAL
  );

  /* Detect a pid change and reset the PRNG.  There is a race condition
  ** here such that two or more threads all trying to open databases at
  ** the same instant might all reset the PRNG.  But multiple resets
  ** are harmless.
  */
  if( randomnessPid!=osGetpid(0) ){
    randomnessPid = osGetpid(0);
    sqlite3_randomness(0,0);
  }
  memset(p, 0, sizeof(unixFile));

  if( eType==SQLITE_OPEN_MAIN_DB ){
    UnixUnusedFd *pUnused;
    pUnused = findReusableFd(zName, flags);
    if( pUnused ){
      fd = pUnused->fd;
    }else{
      pUnused = sqlite3_malloc64(sizeof(*pUnused));
      if( !pUnused ){
        return SQLITE_NOMEM_BKPT;
      }
    }
    p->pPreallocatedUnused = pUnused;

    /* Database filenames are double-zero terminated if they are not
    ** URIs with parameters.  Hence, they can always be passed into
    ** sqlite3_uri_parameter(). */
    assert( (flags & SQLITE_OPEN_URI) || zName[strlen(zName)+1]==0 );

  }else if( !zName ){
    /* If zName is NULL, the upper layer is requesting a temp file. */
    assert(isDelete && !isNewJrnl);
    rc = unixGetTempname(pVfs->mxPathname, zTmpname);
    if( rc!=SQLITE_OK ){
      return rc;
    }
    zName = zTmpname;

    /* Generated temporary filenames are always double-zero terminated
    ** for use by sqlite3_uri_parameter(). */
    assert( zName[strlen(zName)+1]==0 );
  }

  /* Determine the value of the flags parameter passed to POSIX function
  ** open(). These must be calculated even if open() is not called, as
  ** they may be stored as part of the file handle and used by the 
  ** 'conch file' locking functions later on.  */
  if( isReadonly )  openFlags |= O_RDONLY;
  if( isReadWrite ) openFlags |= O_RDWR;
  if( isCreate )    openFlags |= O_CREAT;
  if( isExclusive ) openFlags |= (O_EXCL|O_NOFOLLOW);
  openFlags |= (O_LARGEFILE|O_BINARY|O_NOFOLLOW);

  if( fd<0 ){
    mode_t openMode;              /* Permissions to create file with */
    uid_t uid;                    /* Userid for the file */
    gid_t gid;                    /* Groupid for the file */
    rc = findCreateFileMode(zName, flags, &openMode, &uid, &gid);
    if( rc!=SQLITE_OK ){
      assert( !p->pPreallocatedUnused );
      assert( eType==SQLITE_OPEN_WAL || eType==SQLITE_OPEN_MAIN_JOURNAL );
      return rc;
    }
    fd = robust_open(zName, openFlags, openMode);
    OSTRACE(("OPENX   %-3d %s 0%o\n", fd, zName, openFlags));
    assert( !isExclusive || (openFlags & O_CREAT)!=0 );
    if( fd<0 ){
      if( isNewJrnl && errno==EACCES && osAccess(zName, F_OK) ){
        /* If unable to create a journal because the directory is not
        ** writable, change the error code to indicate that. */
        rc = SQLITE_READONLY_DIRECTORY;
      }else if( errno!=EISDIR && isReadWrite ){
        /* Failed to open the file for read/write access. Try read-only. */
        flags &= ~(SQLITE_OPEN_READWRITE|SQLITE_OPEN_CREATE);
        openFlags &= ~(O_RDWR|O_CREAT);
        flags |= SQLITE_OPEN_READONLY;
        openFlags |= O_RDONLY;
        isReadonly = 1;
        fd = robust_open(zName, openFlags, openMode);
      }
    }
    if( fd<0 ){
      int rc2 = unixLogError(SQLITE_CANTOPEN_BKPT, "open", zName);
      if( rc==SQLITE_OK ) rc = rc2;
      goto open_finished;
    }

    /* The owner of the rollback journal or WAL file should always be the
    ** same as the owner of the database file.  Try to ensure that this is
    ** the case.  The chown() system call will be a no-op if the current
    ** process lacks root privileges, be we should at least try.  Without
    ** this step, if a root process opens a database file, it can leave
    ** behinds a journal/WAL that is owned by root and hence make the
    ** database inaccessible to unprivileged processes.
    **
    ** If openMode==0, then that means uid and gid are not set correctly
    ** (probably because SQLite is configured to use 8+3 filename mode) and
    ** in that case we do not want to attempt the chown().
    */
    if( openMode && (flags & (SQLITE_OPEN_WAL|SQLITE_OPEN_MAIN_JOURNAL))!=0 ){
      robustFchown(fd, uid, gid);
    }
  }
  assert( fd>=0 );
  if( pOutFlags ){
    *pOutFlags = flags;
  }

  if( p->pPreallocatedUnused ){
    p->pPreallocatedUnused->fd = fd;
    p->pPreallocatedUnused->flags = 
                          flags & (SQLITE_OPEN_READONLY|SQLITE_OPEN_READWRITE);
  }

  if( isDelete ){
    osUnlink(zName);
  }
#if SQLITE_ENABLE_LOCKING_STYLE
  else{
    p->openFlags = openFlags;
  }
#endif

  /* Set up appropriate ctrlFlags */
  if( isDelete )                ctrlFlags |= UNIXFILE_DELETE;
  if( isReadonly )              ctrlFlags |= UNIXFILE_RDONLY;
  noLock = eType!=SQLITE_OPEN_MAIN_DB;
  if( noLock )                  ctrlFlags |= UNIXFILE_NOLOCK;
  if( isNewJrnl )               ctrlFlags |= UNIXFILE_DIRSYNC;
  if( flags & SQLITE_OPEN_URI ) ctrlFlags |= UNIXFILE_URI;

#if SQLITE_ENABLE_LOCKING_STYLE
#if SQLITE_PREFER_PROXY_LOCKING
  isAutoProxy = 1;
#endif
  if( isAutoProxy && (zPath!=NULL) && (!noLock) && pVfs->xOpen ){
    char *envforce = getenv("SQLITE_FORCE_PROXY_LOCKING");
    int useProxy = 0;

    /* SQLITE_FORCE_PROXY_LOCKING==1 means force always use proxy, 0 means 
    ** never use proxy, NULL means use proxy for non-local files only.  */
    if( envforce!=NULL ){
      useProxy = atoi(envforce)>0;
    }else{
      useProxy = !(fsInfo.f_flags&MNT_LOCAL);
    }
    if( useProxy ){
      rc = fillInUnixFile(pVfs, fd, pFile, zPath, ctrlFlags);
      if( rc==SQLITE_OK ){
        rc = proxyTransformUnixFile((unixFile*)pFile, ":auto:");
        if( rc!=SQLITE_OK ){
          /* Use unixClose to clean up the resources added in fillInUnixFile 
          ** and clear all the structure's references.  Specifically, 
          ** pFile->pMethods will be NULL so sqlite3OsClose will be a no-op 
          */
          unixClose(pFile);
          return rc;
        }
      }
      goto open_finished;
    }
  }
#endif
  
  assert( zPath==0 || zPath[0]=='/' 
      || eType==SQLITE_OPEN_SUPER_JOURNAL || eType==SQLITE_OPEN_MAIN_JOURNAL 
  );
  rc = fillInUnixFile(pVfs, fd, pFile, zPath, ctrlFlags);

open_finished:
  if( rc!=SQLITE_OK ){
    sqlite3_free(p->pPreallocatedUnused);
  }
  return rc;
}


/*
** Delete the file at zPath. If the dirSync argument is true, fsync()
** the directory after deleting the file.
*/
static int unixDelete(
  sqlite3_vfs *NotUsed,     /* VFS containing this as the xDelete method */
  const char *zPath,        /* Name of file to be deleted */
  int dirSync               /* If true, fsync() directory after deleting file */
){
  int rc = SQLITE_OK;
  UNUSED_PARAMETER(NotUsed);
  SimulateIOError(return SQLITE_IOERR_DELETE);
  if( osUnlink(zPath)==(-1) ){
    if( errno==ENOENT
    ){
      rc = SQLITE_IOERR_DELETE_NOENT;
    }else{
      rc = unixLogError(SQLITE_IOERR_DELETE, "unlink", zPath);
    }
    return rc;
  }
#ifndef SQLITE_DISABLE_DIRSYNC
  if( (dirSync & 1)!=0 ){
    int fd;
    rc = osOpenDirectory(zPath, &fd);
    if( rc==SQLITE_OK ){
      if( full_fsync(fd,0,0) ){
        rc = unixLogError(SQLITE_IOERR_DIR_FSYNC, "fsync", zPath);
      }
      robust_close(0, fd, __LINE__);
    }else{
      assert( rc==SQLITE_CANTOPEN );
      rc = SQLITE_OK;
    }
  }
#endif
  return rc;
}

/*
** Test the existence of or access permissions of file zPath. The
** test performed depends on the value of flags:
**
**     SQLITE_ACCESS_EXISTS: Return 1 if the file exists
**     SQLITE_ACCESS_READWRITE: Return 1 if the file is read and writable.
**     SQLITE_ACCESS_READONLY: Return 1 if the file is readable.
**
** Otherwise return 0.
*/
static int unixAccess(
  sqlite3_vfs *NotUsed,   /* The VFS containing this xAccess method */
  const char *zPath,      /* Path of the file to examine */
  int flags,              /* What do we want to learn about the zPath file? */
  int *pResOut            /* Write result boolean here */
){
  UNUSED_PARAMETER(NotUsed);
  SimulateIOError( return SQLITE_IOERR_ACCESS; );
  assert( pResOut!=0 );

  /* The spec says there are three possible values for flags.  But only
  ** two of them are actually used */
  assert( flags==SQLITE_ACCESS_EXISTS || flags==SQLITE_ACCESS_READWRITE );

  if( flags==SQLITE_ACCESS_EXISTS ){
    struct stat buf;
    *pResOut = 0==osStat(zPath, &buf) &&
                (!S_ISREG(buf.st_mode) || buf.st_size>0);
  }else{
    *pResOut = osAccess(zPath, W_OK|R_OK)==0;
  }
  return SQLITE_OK;
}

/*
** If the last component of the pathname in z[0]..z[j-1] is something
** other than ".." then back it out and return true.  If the last
** component is empty or if it is ".." then return false.
*/
static int unixBackupDir(const char *z, int *pJ){
  int j = *pJ;
  int i;
  if( j<=0 ) return 0;
  for(i=j-1; ALWAYS(i>0) && z[i-1]!='/'; i--){}
  if( z[i]=='.' && i==j-2 && z[i+1]=='.' ) return 0;
  *pJ = i-1;
  return 1;
}

/*
** Convert a relative pathname into a full pathname.  Also
** simplify the pathname as follows:
**
**    Remove all instances of /./
**    Remove all isntances of /X/../ for any X
*/
static int mkFullPathname(
  const char *zPath,              /* Input path */
  char *zOut,                     /* Output buffer */
  int nOut                        /* Allocated size of buffer zOut */
){
  int nPath = sqlite3Strlen30(zPath);
  int iOff = 0;
  int i, j;
  if( zPath[0]!='/' ){
    if( osGetcwd(zOut, nOut-2)==0 ){
      return unixLogError(SQLITE_CANTOPEN_BKPT, "getcwd", zPath);
    }
    iOff = sqlite3Strlen30(zOut);
    zOut[iOff++] = '/';
  }
  if( (iOff+nPath+1)>nOut ){
    /* SQLite assumes that xFullPathname() nul-terminates the output buffer
    ** even if it returns an error.  */
    zOut[iOff] = '\0';
    return SQLITE_CANTOPEN_BKPT;
  }
  sqlite3_snprintf(nOut-iOff, &zOut[iOff], "%s", zPath);

  /* Remove duplicate '/' characters.  Except, two // at the beginning
  ** of a pathname is allowed since this is important on windows. */
  for(i=j=1; zOut[i]; i++){
    zOut[j++] = zOut[i];
    while( zOut[i]=='/' && zOut[i+1]=='/' ) i++;
  }
  zOut[j] = 0;

  assert( zOut[0]=='/' );
  for(i=j=0; zOut[i]; i++){
    if( zOut[i]=='/' ){
      /* Skip over internal "/." directory components */
      if( zOut[i+1]=='.' && zOut[i+2]=='/' ){
        i += 1;
        continue;
      }

      /* If this is a "/.." directory component then back out the
      ** previous term of the directory if it is something other than "..".
      */
      if( zOut[i+1]=='.'
       && zOut[i+2]=='.'
       && zOut[i+3]=='/'
       && unixBackupDir(zOut, &j)
      ){
        i += 2;
        continue;
      }
    }
    if( ALWAYS(j>=0) ) zOut[j] = zOut[i];
    j++;
  }
  if( NEVER(j==0) ) zOut[j++] = '/';
  zOut[j] = 0;
  return SQLITE_OK;
}

/*
** Turn a relative pathname into a full pathname. The relative path
** is stored as a nul-terminated string in the buffer pointed to by
** zPath. 
**
** zOut points to a buffer of at least sqlite3_vfs.mxPathname bytes 
** (in this case, MAX_PATHNAME bytes). The full-path is written to
** this buffer before returning.
*/
static int unixFullPathname(
  sqlite3_vfs *pVfs,            /* Pointer to vfs object */
  const char *zPath,            /* Possibly relative input path */
  int nOut,                     /* Size of output buffer in bytes */
  char *zOut                    /* Output buffer */
){
#if !defined(HAVE_READLINK) || !defined(HAVE_LSTAT)
  return mkFullPathname(zPath, zOut, nOut);
#else
  int rc = SQLITE_OK;
  int nByte;
  int nLink = 0;                /* Number of symbolic links followed so far */
  const char *zIn = zPath;      /* Input path for each iteration of loop */
  char *zDel = 0;

  assert( pVfs->mxPathname==MAX_PATHNAME );
  UNUSED_PARAMETER(pVfs);

  /* It's odd to simulate an io-error here, but really this is just
  ** using the io-error infrastructure to test that SQLite handles this
  ** function failing. This function could fail if, for example, the
  ** current working directory has been unlinked.
  */
  SimulateIOError( return SQLITE_ERROR );

  do {

    /* Call stat() on path zIn. Set bLink to true if the path is a symbolic
    ** link, or false otherwise.  */
    int bLink = 0;
    struct stat buf;
    if( osLstat(zIn, &buf)!=0 ){
      if( errno!=ENOENT ){
        rc = unixLogError(SQLITE_CANTOPEN_BKPT, "lstat", zIn);
      }
    }else{
      bLink = S_ISLNK(buf.st_mode);
    }

    if( bLink ){
      nLink++;
      if( zDel==0 ){
        zDel = sqlite3_malloc(nOut);
        if( zDel==0 ) rc = SQLITE_NOMEM_BKPT;
      }else if( nLink>=SQLITE_MAX_SYMLINKS ){
        rc = SQLITE_CANTOPEN_BKPT;
      }

      if( rc==SQLITE_OK ){
        nByte = osReadlink(zIn, zDel, nOut-1);
        if( nByte<0 ){
          rc = unixLogError(SQLITE_CANTOPEN_BKPT, "readlink", zIn);
        }else{
          if( zDel[0]!='/' ){
            int n;
            for(n = sqlite3Strlen30(zIn); n>0 && zIn[n-1]!='/'; n--);
            if( nByte+n+1>nOut ){
              rc = SQLITE_CANTOPEN_BKPT;
            }else{
              memmove(&zDel[n], zDel, nByte+1);
              memcpy(zDel, zIn, n);
              nByte += n;
            }
          }
          zDel[nByte] = '\0';
        }
      }

      zIn = zDel;
    }

    assert( rc!=SQLITE_OK || zIn!=zOut || zIn[0]=='/' );
    if( rc==SQLITE_OK && zIn!=zOut ){
      rc = mkFullPathname(zIn, zOut, nOut);
    }
    if( bLink==0 ) break;
    zIn = zOut;
  }while( rc==SQLITE_OK );

  sqlite3_free(zDel);
  if( rc==SQLITE_OK && nLink ) rc = SQLITE_OK_SYMLINK;
  return rc;
#endif   /* HAVE_READLINK && HAVE_LSTAT */
}


#ifndef SQLITE_OMIT_LOAD_EXTENSION
/*
** Interfaces for opening a shared library, finding entry points
** within the shared library, and closing the shared library.
*/
#include <dlfcn.h>
static void *unixDlOpen(sqlite3_vfs *NotUsed, const char *zFilename){
  UNUSED_PARAMETER(NotUsed);
  return dlopen(zFilename, RTLD_NOW | RTLD_GLOBAL);
}

/*
** SQLite calls this function immediately after a call to unixDlSym() or
** unixDlOpen() fails (returns a null pointer). If a more detailed error
** message is available, it is written to zBufOut. If no error message
** is available, zBufOut is left unmodified and SQLite uses a default
** error message.
*/
static void unixDlError(sqlite3_vfs *NotUsed, int nBuf, char *zBufOut){
  const char *zErr;
  UNUSED_PARAMETER(NotUsed);
  unixEnterMutex();
  zErr = dlerror();
  if( zErr ){
    sqlite3_snprintf(nBuf, zBufOut, "%s", zErr);
  }
  unixLeaveMutex();
}
static void (*unixDlSym(sqlite3_vfs *NotUsed, void *p, const char*zSym))(void){
  /* 
  ** GCC with -pedantic-errors says that C90 does not allow a void* to be
  ** cast into a pointer to a function.  And yet the library dlsym() routine
  ** returns a void* which is really a pointer to a function.  So how do we
  ** use dlsym() with -pedantic-errors?
  **
  ** Variable x below is defined to be a pointer to a function taking
  ** parameters void* and const char* and returning a pointer to a function.
  ** We initialize x by assigning it a pointer to the dlsym() function.
  ** (That assignment requires a cast.)  Then we call the function that
  ** x points to.  
  **
  ** This work-around is unlikely to work correctly on any system where
  ** you really cannot cast a function pointer into void*.  But then, on the
  ** other hand, dlsym() will not work on such a system either, so we have
  ** not really lost anything.
  */
  void (*(*x)(void*,const char*))(void);
  UNUSED_PARAMETER(NotUsed);
  x = (void(*(*)(void*,const char*))(void))dlsym;
  return (*x)(p, zSym);
}
static void unixDlClose(sqlite3_vfs *NotUsed, void *pHandle){
  UNUSED_PARAMETER(NotUsed);
  dlclose(pHandle);
}
#else /* if SQLITE_OMIT_LOAD_EXTENSION is defined: */
  #define unixDlOpen  0
  #define unixDlError 0
  #define unixDlSym   0
  #define unixDlClose 0
#endif

/*
** Write nBuf bytes of random data to the supplied buffer zBuf.
*/
static int unixRandomness(sqlite3_vfs *NotUsed, int nBuf, char *zBuf){
  UNUSED_PARAMETER(NotUsed);
  assert((size_t)nBuf>=(sizeof(time_t)+sizeof(int)));

  /* We have to initialize zBuf to prevent valgrind from reporting
  ** errors.  The reports issued by valgrind are incorrect - we would
  ** prefer that the randomness be increased by making use of the
  ** uninitialized space in zBuf - but valgrind errors tend to worry
  ** some users.  Rather than argue, it seems easier just to initialize
  ** the whole array and silence valgrind, even if that means less randomness
  ** in the random seed.
  **
  ** When testing, initializing zBuf[] to zero is all we do.  That means
  ** that we always use the same random number sequence.  This makes the
  ** tests repeatable.
  */
  memset(zBuf, 0, nBuf);
  randomnessPid = osGetpid(0);  
#if !defined(SQLITE_TEST) && !defined(SQLITE_OMIT_RANDOMNESS)
  {
    int fd, got;
    fd = robust_open("/dev/urandom", O_RDONLY, 0);
    if( fd<0 ){
      time_t t;
      time(&t);
      memcpy(zBuf, &t, sizeof(t));
      memcpy(&zBuf[sizeof(t)], &randomnessPid, sizeof(randomnessPid));
      assert( sizeof(t)+sizeof(randomnessPid)<=(size_t)nBuf );
      nBuf = sizeof(t) + sizeof(randomnessPid);
    }else{
      do{ got = osRead(fd, zBuf, nBuf); }while( got<0 && errno==EINTR );
      robust_close(0, fd, __LINE__);
    }
  }
#endif
  return nBuf;
}


/*
** Sleep for a little while.  Return the amount of time slept.
** The argument is the number of microseconds we want to sleep.
** The return value is the number of microseconds of sleep actually
** requested from the underlying operating system, a number which
** might be greater than or equal to the argument, but not less
** than the argument.
*/
static int unixSleep(sqlite3_vfs *NotUsed, int microseconds){
  int seconds = (microseconds+999999)/1000000;
  sleep(seconds);
  UNUSED_PARAMETER(NotUsed);
  return seconds*1000000;
}

/*
** The following variable, if set to a non-zero value, is interpreted as
** the number of seconds since 1970 and is used to set the result of
** sqlite3OsCurrentTime() during testing.
*/
#ifdef SQLITE_TEST
int sqlite3_current_time = 0;  /* Fake system time in seconds since 1970. */
#endif

/*
** Find the current time (in Universal Coordinated Time).  Write into *piNow
** the current time and date as a Julian Day number times 86_400_000.  In
** other words, write into *piNow the number of milliseconds since the Julian
** epoch of noon in Greenwich on November 24, 4714 B.C according to the
** proleptic Gregorian calendar.
**
** On success, return SQLITE_OK.  Return SQLITE_ERROR if the time and date 
** cannot be found.
*/
static int unixCurrentTimeInt64(sqlite3_vfs *NotUsed, sqlite3_int64 *piNow){
  static const sqlite3_int64 unixEpoch = 24405875*(sqlite3_int64)8640000;
  int rc = SQLITE_OK;
#if defined(NO_GETTOD)
  time_t t;
  time(&t);
  *piNow = ((sqlite3_int64)t)*1000 + unixEpoch;
#else
  struct timeval sNow;
  (void)gettimeofday(&sNow, 0);  /* Cannot fail given valid arguments */
  *piNow = unixEpoch + 1000*(sqlite3_int64)sNow.tv_sec + sNow.tv_usec/1000;
#endif

#ifdef SQLITE_TEST
  if( sqlite3_current_time ){
    *piNow = 1000*(sqlite3_int64)sqlite3_current_time + unixEpoch;
  }
#endif
  UNUSED_PARAMETER(NotUsed);
  return rc;
}

#ifndef SQLITE_OMIT_DEPRECATED
/*
** Find the current time (in Universal Coordinated Time).  Write the
** current time and date as a Julian Day number into *prNow and
** return 0.  Return 1 if the time and date cannot be found.
*/
static int unixCurrentTime(sqlite3_vfs *NotUsed, double *prNow){
  sqlite3_int64 i = 0;
  int rc;
  UNUSED_PARAMETER(NotUsed);
  rc = unixCurrentTimeInt64(0, &i);
  *prNow = i/86400000.0;
  return rc;
}
#else
# define unixCurrentTime 0
#endif

/*
** The xGetLastError() method is designed to return a better
** low-level error message when operating-system problems come up
** during SQLite operation.  Only the integer return code is currently
** used.
*/
static int unixGetLastError(sqlite3_vfs *NotUsed, int NotUsed2, char *NotUsed3){
  UNUSED_PARAMETER(NotUsed);
  UNUSED_PARAMETER(NotUsed2);
  UNUSED_PARAMETER(NotUsed3);
  return errno;
}


/*
************************ End of sqlite3_vfs methods ***************************
******************************************************************************/

/******************************************************************************
************************** Begin Proxy Locking ********************************
**
** Proxy locking is a "uber-locking-method" in this sense:  It uses the
** other locking methods on secondary lock files.  Proxy locking is a
** meta-layer over top of the primitive locking implemented above.  For
** this reason, the division that implements of proxy locking is deferred
** until late in the file (here) after all of the other I/O methods have
** been defined - so that the primitive locking methods are available
** as services to help with the implementation of proxy locking.
**
****
**
** The default locking schemes in SQLite use byte-range locks on the
** database file to coordinate safe, concurrent access by multiple readers
** and writers [http://sqlite.org/lockingv3.html].  The five file locking
** states (UNLOCKED, PENDING, SHARED, RESERVED, EXCLUSIVE) are implemented
** as POSIX read & write locks over fixed set of locations (via fsctl),
** on AFP and SMB only exclusive byte-range locks are available via fsctl
** with _IOWR('z', 23, struct ByteRangeLockPB2) to track the same 5 states.
** To simulate a F_RDLCK on the shared range, on AFP a randomly selected
** address in the shared range is taken for a SHARED lock, the entire
** shared range is taken for an EXCLUSIVE lock):
**
**      PENDING_BYTE        0x40000000
**      RESERVED_BYTE       0x40000001
**      SHARED_RANGE        0x40000002 -> 0x40000200
**
** This works well on the local file system, but shows a nearly 100x
** slowdown in read performance on AFP because the AFP client disables
** the read cache when byte-range locks are present.  Enabling the read
** cache exposes a cache coherency problem that is present on all OS X
** supported network file systems.  NFS and AFP both observe the
** close-to-open semantics for ensuring cache coherency
** [http://nfs.sourceforge.net/#faq_a8], which does not effectively
** address the requirements for concurrent database access by multiple
** readers and writers
** [http://www.nabble.com/SQLite-on-NFS-cache-coherency-td15655701.html].
**
** To address the performance and cache coherency issues, proxy file locking
** changes the way database access is controlled by limiting access to a
** single host at a time and moving file locks off of the database file
** and onto a proxy file on the local file system.  
**
**
** Using proxy locks
** -----------------
**
** C APIs
**
**  sqlite3_file_control(db, dbname, SQLITE_FCNTL_SET_LOCKPROXYFILE,
**                       <proxy_path> | ":auto:");
**  sqlite3_file_control(db, dbname, SQLITE_FCNTL_GET_LOCKPROXYFILE,
**                       &<proxy_path>);
**
**
** SQL pragmas
**
**  PRAGMA [database.]lock_proxy_file=<proxy_path> | :auto:
**  PRAGMA [database.]lock_proxy_file
**
** Specifying ":auto:" means that if there is a conch file with a matching
** host ID in it, the proxy path in the conch file will be used, otherwise
** a proxy path based on the user's temp dir
** (via confstr(_CS_DARWIN_USER_TEMP_DIR,...)) will be used and the
** actual proxy file name is generated from the name and path of the
** database file.  For example:
**
**       For database path "/Users/me/foo.db" 
**       The lock path will be "<tmpdir>/sqliteplocks/_Users_me_foo.db:auto:")
**
** Once a lock proxy is configured for a database connection, it can not
** be removed, however it may be switched to a different proxy path via
** the above APIs (assuming the conch file is not being held by another
** connection or process). 
**
**
** How proxy locking works
** -----------------------
**
** Proxy file locking relies primarily on two new supporting files: 
**
**   *  conch file to limit access to the database file to a single host
**      at a time
**
**   *  proxy file to act as a proxy for the advisory locks normally
**      taken on the database
**
** The conch file - to use a proxy file, sqlite must first "hold the conch"
** by taking an sqlite-style shared lock on the conch file, reading the
** contents and comparing the host's unique host ID (see below) and lock
** proxy path against the values stored in the conch.  The conch file is
** stored in the same directory as the database file and the file name
** is patterned after the database file name as ".<databasename>-conch".
** If the conch file does not exist, or its contents do not match the
** host ID and/or proxy path, then the lock is escalated to an exclusive
** lock and the conch file contents is updated with the host ID and proxy
** path and the lock is downgraded to a shared lock again.  If the conch
** is held by another process (with a shared lock), the exclusive lock
** will fail and SQLITE_BUSY is returned.
**
** The proxy file - a single-byte file used for all advisory file locks
** normally taken on the database file.   This allows for safe sharing
** of the database file for multiple readers and writers on the same
** host (the conch ensures that they all use the same local lock file).
**
** Requesting the lock proxy does not immediately take the conch, it is
** only taken when the first request to lock database file is made.  
** This matches the semantics of the traditional locking behavior, where
** opening a connection to a database file does not take a lock on it.
** The shared lock and an open file descriptor are maintained until 
** the connection to the database is closed. 
**
** The proxy file and the lock file are never deleted so they only need
** to be created the first time they are used.
**
** Configuration options
** ---------------------
**
**  SQLITE_PREFER_PROXY_LOCKING
**
**       Database files accessed on non-local file systems are
**       automatically configured for proxy locking, lock files are
**       named automatically using the same logic as
**       PRAGMA lock_proxy_file=":auto:"
**    
**  SQLITE_PROXY_DEBUG
**
**       Enables the logging of error messages during host id file
**       retrieval and creation
**
**  LOCKPROXYDIR
**
**       Overrides the default directory used for lock proxy files that
**       are named automatically via the ":auto:" setting
**
**  SQLITE_DEFAULT_PROXYDIR_PERMISSIONS
**
**       Permissions to use when creating a directory for storing the
**       lock proxy files, only used when LOCKPROXYDIR is not set.
**    
**    
** As mentioned above, when compiled with SQLITE_PREFER_PROXY_LOCKING,
** setting the environment variable SQLITE_FORCE_PROXY_LOCKING to 1 will
** force proxy locking to be used for every database file opened, and 0
** will force automatic proxy locking to be disabled for all database
** files (explicitly calling the SQLITE_FCNTL_SET_LOCKPROXYFILE pragma or
** sqlite_file_control API is not affected by SQLITE_FORCE_PROXY_LOCKING).
*/

/*
** Proxy locking is only available on MacOSX 
*/

/*
** The proxy locking style is intended for use with AFP filesystems.
** And since AFP is only supported on MacOSX, the proxy locking is also
** restricted to MacOSX.
** 
**
******************* End of the proxy lock implementation **********************
******************************************************************************/

/*
** Initialize the operating system interface.
**
** This routine registers all VFS implementations for unix-like operating
** systems.  This routine, and the sqlite3_os_end() routine that follows,
** should be the only routines in this file that are visible from other
** files.
**
** This routine is called once during SQLite initialization and by a
** single thread.  The memory allocation and mutex subsystems have not
** necessarily been initialized when this routine is called, and so they
** should not be used.
*/
int sqlite3_os_init(void){ 
  /* 
  ** The following macro defines an initializer for an sqlite3_vfs object.
  ** The name of the VFS is NAME.  The pAppData is a pointer to a pointer
  ** to the "finder" function.  (pAppData is a pointer to a pointer because
  ** silly C90 rules prohibit a void* from being cast to a function pointer
  ** and so we have to go through the intermediate pointer to avoid problems
  ** when compiling with -pedantic-errors on GCC.)
  **
  ** The FINDER parameter to this macro is the name of the pointer to the
  ** finder-function.  The finder-function returns a pointer to the
  ** sqlite_io_methods object that implements the desired locking
  ** behaviors.  See the division above that contains the IOMETHODS
  ** macro for addition information on finder-functions.
  **
  ** Most finders simply return a pointer to a fixed sqlite3_io_methods
  ** object.  But the "autolockIoFinder" available on MacOSX does a little
  ** more than that; it looks at the filesystem type that hosts the 
  ** database file and tries to choose an locking method appropriate for
  ** that filesystem time.
  */
  #define UNIXVFS(VFSNAME, FINDER) {                        \
    3,                    /* iVersion */                    \
    sizeof(unixFile),     /* szOsFile */                    \
    MAX_PATHNAME,         /* mxPathname */                  \
    0,                    /* pNext */                       \
    VFSNAME,              /* zName */                       \
    (void*)&FINDER,       /* pAppData */                    \
    unixOpen,             /* xOpen */                       \
    unixDelete,           /* xDelete */                     \
    unixAccess,           /* xAccess */                     \
    unixFullPathname,     /* xFullPathname */               \
    unixDlOpen,           /* xDlOpen */                     \
    unixDlError,          /* xDlError */                    \
    unixDlSym,            /* xDlSym */                      \
    unixDlClose,          /* xDlClose */                    \
    unixRandomness,       /* xRandomness */                 \
    unixSleep,            /* xSleep */                      \
    unixCurrentTime,      /* xCurrentTime */                \
    unixGetLastError,     /* xGetLastError */               \
    unixCurrentTimeInt64, /* xCurrentTimeInt64 */           \
    unixSetSystemCall,    /* xSetSystemCall */              \
    unixGetSystemCall,    /* xGetSystemCall */              \
    unixNextSystemCall,   /* xNextSystemCall */             \
  }

  /*
  ** All default VFSes for unix are contained in the following array.
  **
  ** Note that the sqlite3_vfs.pNext field of the VFS object is modified
  ** by the SQLite core when the VFS is registered.  So the following
  ** array cannot be const.
  */
  static sqlite3_vfs aVfs[] = {
    UNIXVFS("unix",          posixIoFinder ),
    UNIXVFS("unix-none",     nolockIoFinder ),
    UNIXVFS("unix-dotfile",  dotlockIoFinder ),
    UNIXVFS("unix-excl",     posixIoFinder ),
#if SQLITE_ENABLE_LOCKING_STYLE || OS_VXWORKS
    UNIXVFS("unix-posix",    posixIoFinder ),
#endif
#if SQLITE_ENABLE_LOCKING_STYLE
    UNIXVFS("unix-flock",    flockIoFinder ),
#endif
  };
  unsigned int i;          /* Loop counter */

  /* Double-check that the aSyscall[] array has been constructed
  ** correctly.  See ticket [bb3a86e890c8e96ab] */
  assert( ArraySize(aSyscall)==29 );

  /* Register all VFSes defined in the aVfs[] array */
  for(i=0; i<(sizeof(aVfs)/sizeof(sqlite3_vfs)); i++){
    sqlite3_vfs_register(&aVfs[i], i==0);
  }
  unixBigLock = sqlite3MutexAlloc(SQLITE_MUTEX_STATIC_VFS1);
  return SQLITE_OK; 
}

/*
** Shutdown the operating system interface.
**
** Some operating systems might need to do some cleanup in this routine,
** to release dynamically allocated objects.  But not on unix.
** This routine is a no-op for unix.
*/
int sqlite3_os_end(void){ 
  unixBigLock = 0;
  return SQLITE_OK; 
}
 
#endif /* SQLITE_OS_UNIX */
