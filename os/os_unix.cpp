/*************************************************
Copyright (C) 2020-2030 PENDLE. All Rights Reserved
File name : os.cpp
Author : pendle
Version : V1.0
Date : 20210126
Description : 底层os部分的unix版本的相关实现的.cpp文件
Others:
History:
*************************************************/

#include "../sqlite3.h"

typedef struct unix_file unix_file;
struct unix_file 
{
    sqlite3_file base;  //base data, must be first

    int fd;  //文件描述符
    char* filename;  //文件名
    unix_file* next;  //指向下一个unix文件
}

unix_file* head = NULL;  //指向第一个被打开的VFS文件


/**
* @DESCRIPTION : 检查判断是否文件被打开的接口
* @PARAM[IN] : filename, 文件名
* @RETURN : 若文件已经被打开，返回指向被打开文件的指针，否则，返回NULL
*/
unix_file* unixAlreadyOpened(const char* filename)
{
    unix_file* p = head;
    for(; p && (strcmp(p->filename, filename)); )
    {
        p = p->next;
    }

    return p;
}


/**
* @DESCRIPTION : 打开一个unix文件的接口
* @PARAM[IN] : file, 指向这个文件的指针
* @PARAM[IN] : filename, 文件名
* @flags[IN] : flags, 文件打开模式，是否所有者可读之类的
* @RETURN : 打开成功，返回SQLITE_OK；否则，返回SQLITE_ERROR
*/
int unixOpenFile(unix_file* file, const char* filename, int flags)
{
    //参数校验
    if((NULL == file) || (NULL == filename))
    {
        return SQLITE_ERROR;
    }

    int fd = open(filename, flags, SQLITE_DEFAULT_FILE_CREATE_MODE);

    if(fd < 0)
    {
        return SQLITE_ERROR;
    }

    file->fd = fd;
    int n = strlen(filename);
    file->filename = (char*)malloc(n + 1);
    memcpy(file->filename, filename, n);
    file->filename[n] = 0;

    return SQLITE_OK; 
}


/**
* @DESCRIPTION : 关闭一个unix文件的接口
* @RETURN : 操作成功，返回SQLITE_OK；否则，返回SQLITE_ERROR
*/
int unxiClose(sqlite3_file* file)
{
    //参数校验
    if (NULL == file)
    {
        return SQLITE_ERROR;
    }

    unix_file* dp = (unix_file*)file;
    unix_file** p = &head;

    for (; (NULL != *p) && (dp != *p);)
    {
        p = &((*p)->next);
    }

    if (NULL == *p)
    {
        return SQLITE_ERROR;
    }

    *p = dp->next;
    int ret = close(dp->fd);
    free(dp->filename);
    return ret;
}


/**
* @DESCRIPTION : 向一个unix文件中写数据的接口
* @PARAM[IN] : file, 指向这个文件的指针
* @PARAM[OUT] : buf, 读取到的文件中的内容
* @PARAM[IN] : buf_sz, 读取的大小
* @PARAM[IN] : offset, 开始进行读取的位置
* @RETURN : 操作成功，返回SQLITE_OK；否则，返回SQLITE_ERROR
*/
int unixRead(sqlite3_file* file, void* buf, int buf_sz, int offset)
{
    //参数校验
    if ((NULL == file) || (NULL == buf))
    {
        return SQLITE_ERROR;
    }

    unix_file* p = (unix_file*)file;
    int off = lseek(p->fd, offset, SEEK_SET);  //移动的距离文件开头offset个字节的位置

    if (off != offset)
    {
        return SQLITE_ERROR;
    }

    return write(p->fd, buf, buf_sz);
}


/**
* @DESCRIPTION : 向一个unix文件中写数据的接口
* @PARAM[IN] : file, 指向这个文件的指针
* @PARAM[IN] : content, 写入的文本信息
* @PARAM[IN] : sz, 写入的大小
* @PARAM[IN] : offset, 开始写入的位置
* @RETURN : 操作成功，返回SQLITE_OK；否则，返回SQLITE_ERROR
*/
int unixWrite(sqlite3_file* file, const void* content, int sz, int offset)
{
    //参数校验
    if ((NULL == file) || (NULL == content))
    {
        return SQLITE_ERROR;
    }

    unix_file* p = (unix_file*)file;
    int off = lseek(p->fd, offset, SEEK_SET);
    if (off != offset)
    {
        return SQLITE_ERROR;
    }

    return write(p->fd, content, sz);
}


/**
* @DESCRIPTION : 改变一个unix文件大小的接口
* @PARAM[IN] : file, 指向这个文件的指针
* @PARAM[IN] : sz, 最终期望改变成的大小
* @RETURN : 操作成功，返回SQLITE_OK；否则，返回SQLITE_ERROR
*/
int unixTruncate(sqlite3_file* file, int sz)
{
    //参数校验
    if (NULL == file)
    {
        return SQLITE_ERROR;
    }

    unix_file* p = (unix_file*)file;
    return ftruncate(p->fd, sz);
}


/**
* @DESCRIPTION : unix下刷新缓存区的接口
* @PARAM[IN] : file, 指向这个文件的指针
* @RETURN : 操作成功，返回SQLITE_OK；否则，返回SQLITE_ERROR
*/
int unixSync(sqlite3_file* file)
{
    //参数校验
    if (NULL == file)
    {
        return SQLITE_ERROR;
    }

    unix_file* p = (unix_file*)file;
    return syncfs(p->fd);
}


/**
* @DESCRIPTION : unix下获取文件大小的接口
* @PARAM[IN] : file, 指向这个文件的指针
* @PARAM[OUT] : ret, 返回的获取到的文件的大小
* @RETURN : 操作成功，返回SQLITE_OK；否则，返回SQLITE_ERROR
*/
int unixFileSize(sqlite3_file* file, int* ret)
{
    //参数校验
    if ((NULL == file) || (NULL == ret))
    {
        return SQLITE_ERROR;
    }

    unix_file* p = (unix_file*)file;
    struct stat st;
    if (fstat(p->fd, &st) < 0)
    {
        return SQLITE_ERROR;
    }

    *ret = st.st_size;
    return SQLITE_ERROR;
}


int unixOpen(sqlite3_vfs* vfs, const char* filename, sqlite3_file* ret, int flags)
{
    //参数校验
    if ((NULL == vfs) || (NULL == filename) || (NULL == ret))
    {
        return SQLITE_ERROR;
    }

    const static sqlite3_io_method UNIX_METHODS = {
        unxiClose,
        unixRead,
        unixWrite,
        unixTruncate,
        unixSync,
        unixFileSize
    };

    unix_file* dp = unixAlreadyOpened(filename);
    if (NULL == dp)
    {
        return SQLITE_ERROR;
    }

    unix_file* p = (unix_file*)ret;
    p->base.p_methods = &UNIX_METHODS;

    if (unixOpenFile(p, filename, flags))
    {
        return SQLITE_ERROR;
    }

    p->next = head;
    head = p;
    return SQLITE_OK;
}


int unixDelete(sqlite3_vfs* vfs, const char* filename)
{
    //参数校验
    if ((NULL == vfs) || (NULL == filename))
    {
        return SQLITE_ERROR;
    }

    unix_file* dp = unixAlreadyOpened(filename);
    if (NULL != dp)
    {
        unxiClose((sqlite3_file*)dp);
    }

    //删除文件或断开文件的链接
    return unlink(filename);
}


/**
* @DESCRIPTION : 确认用户是否有对此文件访问权的接口
* @PARAM[IN] : vfs, 指向这个文件的指针
* @PARAM[IN] : filename, 返回的获取到的文件的大小
* @PARAM[IN] : flag, 欲检查的访问权限
* @PARAM[OUT] : ret, 返回的是否有访问权限的内容 
* @RETURN : 操作成功，返回SQLITE_OK；否则，返回SQLITE_ERROR
*/
int unixAccess(sqlite3_vfs* vfs, const char* filename, int flag, int* ret)
{
    //参数校验
    if ((NULL == vfs) || (NULL == filename) || (NULL == ret))
    {
        return SQLITE_ERROR;
    }

    *ret = access(filename, flag);
    return SQLITE_OK;
}


/**
* @DESCRIPTION : 休眠操作相关的接口
* @PARAM[IN] : vfs, 指向这个文件的指针
* @PARAM[IN] : micro_secs, 要休眠的时间
* @RETURN : 操作成功，返回SQLITE_OK；否则，返回SQLITE_ERROR
*/
int unixSleep(sqlite3_vfs* vfs, int micro_secs)
{
    static const int T = 1000000;
    sleep(micro_secs / T);
    usleep(micro_secs % T);
    return SQLITE_OK;
}


/**
* @DESCRIPTION : 获取当前时间的接口
* @PARAM[IN] : vfs, 指向这个文件的指针
* @PARAM[OUT] : ret, 返回的当前时间
* @RETURN : 操作成功，返回SQLITE_OK；否则，返回SQLITE_ERROR
*/
int unixCurrentTime(sqlite3_vfs* vfs, int* ret)
{
    time(ret);
    return SQLITE_OK;
}


sqlite3_vfs* unixGetOS()
{
    static struct sqlite3_vfs UNIX_OS = {
        sizeof(unix_file),
        200,
        "unix",
        0,

        unixOpen,
        unixDelete,
        unixAccess,
        unixSleep,
        unixCurrentTime
    };

    return &UNIX_OS;
}