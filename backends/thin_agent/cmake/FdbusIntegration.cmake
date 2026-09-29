# Device-side FDBus integration gate（与 x86 demo 路径隔离）
#
# 启用：
#   cmake -DTHIN_AGENT_WITH_FDBUS=ON \
#         -DFDBUS_ROOT=<fdbus 头文件/库路径> \
#         -DLEAPTIC_INCLUDE=<leaptic_app/include>
#
# 要求：
#   FDBUS_ROOT/include/fdbus/fdbus_clib.h
#   FDBUS_ROOT/lib/libfdbus-clib.so*
#   LEAPTIC_INCLUDE/cabin_cmd_interface.h

if(NOT THIN_AGENT_WITH_FDBUS)
  return()
endif()

if(NOT DEFINED FDBUS_ROOT OR FDBUS_ROOT STREQUAL "")
  message(FATAL_ERROR "THIN_AGENT_WITH_FDBUS=ON requires -DFDBUS_ROOT=<path>")
endif()

if(NOT DEFINED LEAPTIC_INCLUDE OR LEAPTIC_INCLUDE STREQUAL "")
  message(FATAL_ERROR "THIN_AGENT_WITH_FDBUS=ON requires -DLEAPTIC_INCLUDE=<leaptic_app/include>")
endif()

set(FDBUS_INCLUDE_DIR "${FDBUS_ROOT}/include")
set(FDBUS_LIB_DIR "${FDBUS_ROOT}/lib")

if(NOT EXISTS "${FDBUS_INCLUDE_DIR}/fdbus/fdbus_clib.h")
  message(FATAL_ERROR "Missing header: ${FDBUS_INCLUDE_DIR}/fdbus/fdbus_clib.h")
endif()

if(NOT EXISTS "${LEAPTIC_INCLUDE}/cabin_cmd_interface.h")
  message(FATAL_ERROR "Missing header: ${LEAPTIC_INCLUDE}/cabin_cmd_interface.h")
endif()

find_library(FDBUS_CLIB
  NAMES fdbus-clib fdbus_clib
  PATHS "${FDBUS_LIB_DIR}"
  NO_DEFAULT_PATH
)

find_library(FDBUS_CORE
  NAMES fdbus
  PATHS "${FDBUS_LIB_DIR}"
  NO_DEFAULT_PATH
)

if(NOT FDBUS_CLIB)
  message(FATAL_ERROR "Missing fdbus-clib library under ${FDBUS_LIB_DIR}")
endif()

target_include_directories(thin_agent_core PRIVATE
  "${FDBUS_INCLUDE_DIR}"
  "${LEAPTIC_INCLUDE}"
)
target_link_libraries(thin_agent_core PRIVATE "${FDBUS_CLIB}")
if(FDBUS_CORE)
  target_link_libraries(thin_agent_core PRIVATE "${FDBUS_CORE}")
endif()

message(STATUS "FDBus enabled: include=${FDBUS_INCLUDE_DIR}, leaptic=${LEAPTIC_INCLUDE}, lib=${FDBUS_CLIB}")
