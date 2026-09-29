# zing_agent — 正式客户端桌面端（P1：聊天客户端）
# 与 agent_tool 同构：webview 宿主 + chat.html 嵌入。
# 复用 agent_tool 的 webview 子模块（../agent_tool/third_party/webview）。

cmake_minimum_required(VERSION 3.16)
project(zing_agent LANGUAGES C CXX)
include(CTest)  # 顶层 enable_testing（子目录 add_test 生效前提）

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ── thin_agent discovery 源文件（从后端项目复用）─────────────────
set(THIN_AGENT_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../thin_agent"
    CACHE PATH "Path to thin_agent repository root")

if(NOT WIN32)
    set(DISCOVERY_SOURCES ${THIN_AGENT_ROOT}/src/api/discovery.cpp)
else()
    set(DISCOVERY_SOURCES)
endif()

# ── webview 头文件路径（共享 agent_tool 的子模块）─────────────────
set(WEBVIEW_INCLUDE "${CMAKE_CURRENT_SOURCE_DIR}/../agent_tool/third_party/webview/core/include")

# ── 嵌入 chat.html（纯 CMake hex 嵌入，同 agent_tool 模式）─────────
set(HTML_SRC "${CMAKE_CURRENT_SOURCE_DIR}/web/chat.html")
set(EMBED_HTML_GEN "")
if(EXISTS "${HTML_SRC}")
    set(HTML_HEADER "${CMAKE_CURRENT_BINARY_DIR}/chat_html.h")
    file(READ "${HTML_SRC}" html_hex HEX)
    file(SIZE "${HTML_SRC}" html_size)
    string(REGEX REPLACE "\n" "" html_hex "${html_hex}")
    string(REGEX MATCHALL ".." html_pairs "${html_hex}")
    set(html_array "")
    foreach(pair ${html_pairs})
        string(APPEND html_array "0x${pair},")
    endforeach()
    file(WRITE "${HTML_HEADER}"
        "unsigned char chat_html[] = { ${html_array} };\n"
        "unsigned int chat_html_len = ${html_size};\n"
    )
    set(EMBED_HTML_GEN "${HTML_HEADER}")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${HTML_SRC}")
else()
    message(WARNING "chat.html not found at ${HTML_SRC}; will load from file at runtime")
endif()

# ── 可执行文件 ────────────────────────────────────────────────────
if(WIN32)
    # v18: WIN32 子系统——GUI 程序不挂黑色控制台窗（同 agent_tool）
    add_executable(zing_agent WIN32
        src/zing_agent.cpp
        src/DebugLogger.cpp
        src/ServiceManager.cpp
        src/RemoteBackend.cpp
        src/ConfigStore.cpp
        ${DISCOVERY_SOURCES}
        ${EMBED_HTML_GEN}
    )
else()
    add_executable(zing_agent
        src/zing_agent.cpp
        src/DebugLogger.cpp
        src/ServiceManager.cpp
        src/RemoteBackend.cpp
        src/ConfigStore.cpp
        ${DISCOVERY_SOURCES}
        ${EMBED_HTML_GEN}
    )
endif()

# P3: 远程后端单测（本机 sshd 自环 2222——无 sshd 环境返回 77 跳过；仅 Unix）
if(NOT WIN32)
add_executable(remote_test
    tests/remote_test.cpp
    src/RemoteBackend.cpp
)
target_include_directories(remote_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
add_test(NAME remote_backend_test COMMAND remote_test)
set_tests_properties(remote_backend_test PROPERTIES SKIP_REGULAR_EXPRESSION "SKIP:")  # 无 sshd=跳过
endif()

target_include_directories(zing_agent PRIVATE
    ${WEBVIEW_INCLUDE}
    ${CMAKE_CURRENT_BINARY_DIR}   # for chat_html.h
)

if(NOT WIN32)
    target_include_directories(zing_agent PRIVATE
        ${THIN_AGENT_ROOT}/include   # discovery.h, nlohmann/json
    )
else()
    # Windows: nlohmann/json 从本仓 third_party 取（与 thin_agent 仓同源
    # 单头 vendored）——Windows 构建零 thin_agent 仓库依赖
    target_include_directories(zing_agent PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../third_party)
endif()

target_compile_definitions(zing_agent PRIVATE
    ZING_EMBEDDED_HTML=1
)

# P2/P3 单测（tests/ 子目录——service_manager 全链 + remote 纯逻辑）
if(NOT WIN32)
    add_subdirectory(tests)
endif()

# ── 平台特定链接（同 agent_tool）──────────────────────────────────
if(WIN32)
    include(FetchContent)
    cmake_policy(SET CMP0135 NEW)
    if(POLICY CMP0169)
      cmake_policy(SET CMP0169 OLD)
    endif()
    set(WEBVIEW2_VERSION "1.0.1150.38")
    FetchContent_Declare(mswebview2
        URL "https://www.nuget.org/api/v2/package/Microsoft.Web.WebView2/${WEBVIEW2_VERSION}"
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    )
    FetchContent_GetProperties(mswebview2)
    if(NOT mswebview2_POPULATED)
        FetchContent_Populate(mswebview2)
    endif()
    target_include_directories(zing_agent PRIVATE
        "${mswebview2_SOURCE_DIR}/build/native/include"
    )
    target_link_libraries(zing_agent PRIVATE
        "${mswebview2_SOURCE_DIR}/build/native/x64/WebView2LoaderStatic.lib"
    )
    target_compile_definitions(zing_agent PRIVATE UNICODE _UNICODE)
    target_compile_options(zing_agent PRIVATE "/utf-8")
    target_link_libraries(zing_agent PRIVATE ws2_32)

elseif(APPLE)
    find_library(WEBKIT_FWK WebKit)
    target_link_libraries(zing_agent PRIVATE ${WEBKIT_FWK})

else()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(WEBKIT REQUIRED webkit2gtk-4.1)
    target_link_libraries(zing_agent PRIVATE ${WEBKIT_LIBRARIES})
    target_include_directories(zing_agent PRIVATE ${WEBKIT_INCLUDE_DIRS})
    target_compile_options(zing_agent PRIVATE ${WEBKIT_CFLAGS_OTHER})
endif()

if(UNIX)
    target_link_libraries(zing_agent PRIVATE pthread)
endif()
