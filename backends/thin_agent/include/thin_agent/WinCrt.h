#pragma once

// MSVC 的 POSIX 名字（popen/ssize_t）默认不在全局命名空间里。
// 用 /FI 提前塞进每个本仓翻译单元，调用点不用再分平台。
#ifdef _MSC_VER
#include <stdio.h>
#include <time.h>
#include <BaseTsd.h>
#ifndef popen
#define popen _popen
#endif
#ifndef pclose
#define pclose _pclose
#endif
#ifndef gmtime_r
#define gmtime_r(timer, result) (gmtime_s((result), (timer)) == 0 ? (result) : nullptr)
#endif
#ifndef timegm
#define timegm _mkgmtime
#endif
#if !defined(__ssize_t_defined)
typedef SSIZE_T ssize_t;
#define __ssize_t_defined
#endif
#endif
