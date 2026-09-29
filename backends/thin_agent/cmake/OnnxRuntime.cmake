# 解析 ONNX Runtime 库路径（宿主 / 交叉按目标架构选择，避免误链）

if(NOT CMAKE_HOST_SYSTEM_PROCESSOR STREQUAL CMAKE_SYSTEM_PROCESSOR
   OR CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64")
  if(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64")
    set(_onnx_default_root "${CMAKE_CURRENT_SOURCE_DIR}/third_party/onnxruntime-aarch64")
    set(_onnx_search_paths "${_onnx_default_root}/lib")
    set(_onnx_use_sysroot_bypass TRUE)
  else()
    message(FATAL_ERROR "Unsupported cross-compile target: ${CMAKE_SYSTEM_PROCESSOR}")
  endif()
else()
  set(_onnx_default_root "${CMAKE_CURRENT_SOURCE_DIR}/third_party/onnxruntime")
  set(_onnx_search_paths /usr/local/lib "${_onnx_default_root}/lib")
  set(_onnx_use_sysroot_bypass FALSE)
endif()

set(THIN_AGENT_ONNXRUNTIME_ROOT "${_onnx_default_root}" CACHE PATH "ONNX Runtime root (include/ + lib/)")

if(_onnx_use_sysroot_bypass)
  find_library(THIN_AGENT_ONNXRUNTIME_LIB
    NAMES onnxruntime libonnxruntime
    PATHS ${_onnx_search_paths}
    NO_DEFAULT_PATH
    NO_CMAKE_FIND_ROOT_PATH
  )
else()
  find_library(THIN_AGENT_ONNXRUNTIME_LIB
    NAMES onnxruntime libonnxruntime
    PATHS ${_onnx_search_paths}
    NO_DEFAULT_PATH
  )
endif()

if(NOT THIN_AGENT_ONNXRUNTIME_LIB)
  message(FATAL_ERROR
    "onnxruntime not found under ${_onnx_search_paths}. "
    "aarch64: run bash build.sh cam (auto-fetches third_party/onnxruntime-aarch64 if missing); "
    "x86: install to /usr/local/lib or third_party/onnxruntime/lib")
endif()

if(EXISTS "${THIN_AGENT_ONNXRUNTIME_ROOT}/include")
  set(THIN_AGENT_ONNXRUNTIME_INCLUDE "${THIN_AGENT_ONNXRUNTIME_ROOT}/include")
elseif(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/onnxruntime/include")
  set(THIN_AGENT_ONNXRUNTIME_INCLUDE "${CMAKE_CURRENT_SOURCE_DIR}/third_party/onnxruntime/include")
else()
  message(FATAL_ERROR "onnxruntime headers not found under ${THIN_AGENT_ONNXRUNTIME_ROOT}/include")
endif()

message(STATUS "ONNX Runtime: lib=${THIN_AGENT_ONNXRUNTIME_LIB}")
message(STATUS "ONNX Runtime: include=${THIN_AGENT_ONNXRUNTIME_INCLUDE}")
