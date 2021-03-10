sqlite源码介绍：
代码有四种模式，分别在不同的状态下使用。
SQLITE_MUTEX_OMIT : 不适用互斥体的代码，初始化时，静态互斥体结构体不被覆盖。
SQLITE_MUTEX_NOOP : 单线程应用时的代码，没有提供互斥排它锁，初始化时，静态互斥体结构体被覆盖
SQLITE_MUTEX_PTHREADS : UNIX多线程多线程调用的代码
SQLITE_MUTEX_W32 : WIN32下多线程调用的代码 
