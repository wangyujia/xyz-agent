
# P2/P3 单测：ServiceManager 全链（probe/start/stop/pidfile/watch）+
# RemoteBackend 纯逻辑（命令构造/源码回归锚）。仅 Unix（Windows 分支
# 由 BUILD-WINDOWS 侧真机验证）。
if(NOT WIN32)
add_executable(service_manager_test
    service_manager_test.cpp
    ${CMAKE_SOURCE_DIR}/src/ServiceManager.cpp
)
target_include_directories(service_manager_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_include_directories(service_manager_test PRIVATE ${THIN_AGENT_ROOT}/include)
target_link_libraries(service_manager_test PRIVATE pthread)
add_test(NAME service_manager_test COMMAND service_manager_test)

add_executable(config_store_test
    config_store_test.cpp
    ${CMAKE_SOURCE_DIR}/src/ConfigStore.cpp
)
target_include_directories(config_store_test PRIVATE ${CMAKE_SOURCE_DIR}/src ${THIN_AGENT_ROOT}/include)
add_test(NAME config_store_test COMMAND config_store_test)

# chat.html JS 纯逻辑测试（node 提取法——无 node 则跳过）
find_program(NODE_BIN node)
if(NODE_BIN)
    add_test(NAME chat_logic_test COMMAND ${NODE_BIN} ${CMAKE_SOURCE_DIR}/tests/chat_logic_test.js)
    add_test(NAME queue_test COMMAND ${NODE_BIN} ${CMAKE_SOURCE_DIR}/tests/queue_test.js)
    add_test(NAME theme_test COMMAND ${NODE_BIN} ${CMAKE_SOURCE_DIR}/tests/theme_test.js)
    add_test(NAME render_test COMMAND ${NODE_BIN} ${CMAKE_SOURCE_DIR}/tests/render_test.js)
endif()

add_executable(remote_logic_test
    remote_logic_test.cpp
    ${CMAKE_SOURCE_DIR}/src/RemoteBackend.cpp
)
target_compile_definitions(remote_logic_test PRIVATE SRC_DIR="${CMAKE_SOURCE_DIR}")
target_include_directories(remote_logic_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
add_test(NAME remote_logic_test COMMAND remote_logic_test)
endif()
