/*************************************************
Copyright (C) 2020-2030 PENDLE. All Rights Reserved
File name : os.cpp
Author : pendle
Version : V1.0
Date : 20210126
Description : 底层os部分进行单元测试的.cpp文件
Others:
History:
*************************************************/

#include "os.h"
#include "../sqlite3.h"


void osTest()
{
    sqlite3_vfs* os = osGetVFSHandle("unix");
    sqlite3_file* file = osGetFileHandle(os);
    printf("%d\n", osOpen(os, "data", file, O_RDWR | O_CREAT));
    printf("%d\n", osWrite(file, "123", 3, 0));
    printf("%d\n", osWrite(file, "abcd", 4, 1));
    printf("%d\n", osTruncate(file, 2));

    char buf[20];
    int ret = osRead(file, buf, 20, 0);
    printf("%d\n", ret);
    buf[ret] = 0;
    printf("%s\n", buf);
}

int main(int argc, char** argv)
{
    osTest();
    return 0;
}