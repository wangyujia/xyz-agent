# CMake toolchain: aarch64-oe-linux（QTI / Yocto SDK）
#
# 用法:
#   source /opt/toolchain/environment-setup-aarch64-oe-linux
#   cmake -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-aarch64-oe.cmake ...

set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

set(CMAKE_SYSROOT $ENV{SDKTARGETSYSROOT})

set(CMAKE_C_COMPILER aarch64-oe-linux-gcc)
set(CMAKE_CXX_COMPILER aarch64-oe-linux-g++)
set(CMAKE_RC_COMPILER aarch64-oe-linux-windres)

set(CMAKE_FIND_ROOT_PATH ${CMAKE_SYSROOT})
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

set(CMAKE_C_FLAGS_INIT "-Wa,--noexecstack -Wno-error=maybe-uninitialized -Wno-error=unused-result" CACHE STRING "Initial C flags")
set(CMAKE_CXX_FLAGS_INIT "-Wa,--noexecstack -Wno-error=maybe-uninitialized -Wno-error=unused-result" CACHE STRING "Initial CXX flags")
set(CMAKE_EXE_LINKER_FLAGS_INIT "-Wl,-O1 -Wl,--hash-style=gnu -Wl,--as-needed -Wl,-z,relro,-z,now,-z,noexecstack" CACHE STRING "Initial linker flags")

set(ENV{PKG_CONFIG_SYSROOT_DIR} ${CMAKE_SYSROOT})
set(ENV{PKG_CONFIG_PATH} ${CMAKE_SYSROOT}/usr/lib/pkgconfig:${CMAKE_SYSROOT}/usr/share/pkgconfig)
