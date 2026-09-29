# agent_tool — webview 桌面客户端
# 编译 thin_agent 的 discovery.cpp（不链 agent 核心库，零依赖）
# Windows: 组播发现暂未适配（Phase 4），仅构建 webview 壳

cmake_minimum_required(VERSION 3.16)
project(agent_tool LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ── thin_agent discovery 源文件（从后端项目复用）─────────────────
set(THIN_AGENT_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../thin_agent"
    CACHE PATH "Path to thin_agent repository root")

if(NOT WIN32)
    set(DISCOVERY_SOURCES
        ${THIN_AGENT_ROOT}/src/api/discovery.cpp
    )
else()
    # Windows: discovery.cpp 尚未适配 Winsock2（Phase 4 待办）
    # 桌面壳仍可正常构建，discoverBackends 返回空数组
    set(DISCOVERY_SOURCES)
endif()

# ── webview 头文件路径 ────────────────────────────────────────────
set(WEBVIEW_INCLUDE "${CMAKE_CURRENT_SOURCE_DIR}/third_party/webview/core/include")

# ── 嵌入 ws_agent.html（纯 CMake，零外部依赖，跨平台）─────────
set(HTML_SRC "${CMAKE_CURRENT_SOURCE_DIR}/web/ws_agent.html")
if(EXISTS "${HTML_SRC}")
    set(HTML_HEADER "${CMAKE_CURRENT_BINARY_DIR}/ws_agent_html.h")

    # 读取原始字节的 hex 表示（配置期完成，无需 xxd）
    file(READ "${HTML_SRC}" html_hex HEX)
    file(SIZE "${HTML_SRC}" html_size)
    string(REGEX REPLACE "\n" "" html_hex "${html_hex}")

    # 每两个 hex 字符一组 → "0xNN," C 数组
    string(REGEX MATCHALL ".." html_pairs "${html_hex}")
    set(html_array "")
    foreach(pair ${html_pairs})
        string(APPEND html_array "0x${pair},")
    endforeach()

    file(WRITE "${HTML_HEADER}"
        "unsigned char ws_agent_html[] = { ${html_array} };\n"
        "unsigned int ws_agent_html_len = ${html_size};\n"
    )

    set(EMBED_HTML_GEN "${HTML_HEADER}")

    # HTML 变化时自动触发 reconfigure
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${HTML_SRC}")
else()
    message(WARNING "ws_agent.html not found at ${HTML_SRC}; will load from file at runtime")
endif()

# ── 生成 Windows 资源文件（嵌入图标，解决 rc.exe 路径/编码问题）─
if(WIN32)
    # Use absolute path so rc.exe finds the .ico regardless of working directory
    set(AGENT_TOOL_ICON_PATH "${CMAKE_CURRENT_SOURCE_DIR}/assets/agent_tool.ico")
    configure_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/src/agent_tool.rc.in"
        "${CMAKE_CURRENT_BINARY_DIR}/agent_tool.rc"
        @ONLY
    )
endif()

# ── 可执行文件 ────────────────────────────────────────────────────
if(WIN32)
    add_executable(agent_tool WIN32
        src/agent_tool.cpp
        src/DebugLogger.cpp
        "${CMAKE_CURRENT_BINARY_DIR}/agent_tool.rc"
        ${DISCOVERY_SOURCES}
        ${EMBED_HTML_GEN}
    )
    # 用 main() 替代 WinMain() 作为入口点
    target_link_options(agent_tool PRIVATE "/ENTRY:mainCRTStartup")
else()
    add_executable(agent_tool
        src/agent_tool.cpp
        src/DebugLogger.cpp
        ${DISCOVERY_SOURCES}
        ${EMBED_HTML_GEN}
    )
endif()

target_include_directories(agent_tool PRIVATE
    ${WEBVIEW_INCLUDE}
    ${CMAKE_CURRENT_BINARY_DIR}   # for ws_agent_html.h
)

if(NOT WIN32)
    target_include_directories(agent_tool PRIVATE
        ${THIN_AGENT_ROOT}/include   # discovery.h, nlohmann/json
    )
endif()

target_compile_definitions(agent_tool PRIVATE
    THIN_DESKTOP_EMBEDDED_HTML=1
)

# ── 平台特定链接 ──────────────────────────────────────────────────
if(WIN32)
    # ── 自动下载 WebView2 SDK（NuGet，首次 ~15MB）───────────────
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

    target_include_directories(agent_tool PRIVATE
        "${mswebview2_SOURCE_DIR}/build/native/include"
    )
    target_link_libraries(agent_tool PRIVATE
        "${mswebview2_SOURCE_DIR}/build/native/x64/WebView2LoaderStatic.lib"
    )
    target_compile_definitions(agent_tool PRIVATE UNICODE _UNICODE)
    target_compile_options(agent_tool PRIVATE "/utf-8")
    target_link_libraries(agent_tool PRIVATE ws2_32)

elseif(APPLE)
    # macOS: WKWebView (系统内置)
    find_library(WEBKIT_FWK WebKit)
    target_link_libraries(agent_tool PRIVATE ${WEBKIT_FWK})

else()
    # Linux: WebKitGTK
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(WEBKIT REQUIRED webkit2gtk-4.1)
    target_link_libraries(agent_tool PRIVATE ${WEBKIT_LIBRARIES})
    target_include_directories(agent_tool PRIVATE ${WEBKIT_INCLUDE_DIRS})
    target_compile_options(agent_tool PRIVATE ${WEBKIT_CFLAGS_OTHER})
endif()

# ── 通用链接 ──────────────────────────────────────────────────────
if(UNIX)
    target_link_libraries(agent_tool PRIVATE pthread)
endif()
