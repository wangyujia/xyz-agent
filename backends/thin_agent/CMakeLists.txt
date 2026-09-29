# thin_agent CMake 构建说明
#
# 目标产物：
#   thin_agent_core   — 静态库（Agent / 策略 / 任务 / LLM / 设备抽象）
#   thin_agent        — Agent Core WebSocket 服务（ws_agent_main + mongoose）
#   thin_agent_gw     — IM 网关（飞书/微信长连接，im_gateway_main）
#   thin_agent_demo   — CLI 冒烟（配置加载 + FakeDevice 动作链）
#
# 选项：
#   THIN_AGENT_WITH_FDBUS   — 真机 FDBus 适配（默认 OFF，x86 demo 用 FakeDeviceControl）
#   THIN_AGENT_BUILD_TESTS  — 单元/演示测试（默认 ON，ctest 含 golden 评估）
#
# 依赖：sqlite3、libcurl、onnxruntime（见 third_party/onnxruntime）

cmake_minimum_required(VERSION 3.16)
project(thin_agent LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(THIN_AGENT_WITH_FDBUS "Enable real FDBus adapter (device runtime only)" OFF)
option(THIN_AGENT_BUILD_TESTS "Build unit+demo tests" ON)
option(THIN_AGENT_BUILD_IM_GATEWAY "Build thin_agent_gw IM gateway (needs OpenSSL)" ON)
option(THIN_AGENT_EMBEDDED_CAM "Use embedded camera runtime default paths" OFF)
option(THIN_AGENT_WITH_LLAMA_CPP "Enable local GGUF model inference via llama.cpp" OFF)

# ── v0.54.6: 机械判官——**局部变量同名遮蔽**编译期判红 ──────────────────────────
# 起因（真实回归，P1）：v0.54.3 在 `CronScheduler::trigger_now` 函数内重复声明
# `nlohmann::json task;`（默认构造是 **null**，把锁内被填充的那份遮蔽）⇒ 放锁后的
# `execute_task(task)` 收到 null ⇒ 宿主 callback 首次 `.value()` 抛
# `json type_error.306: cannot use value() with null`，"手动立即触发"自该版起**必然失败**；
# 旧断言只数 callback 次数，对这种"传进去的内容是空的"零覆盖 ⇒ 静默通过。
# 同名遮蔽是同族隐患（改了内层忘了外层 / 取错对象），交给**编译器**当判官比人工 review 可靠：
#   GCC/Clang: -Wshadow=local（只在局部/形参层面报警，避免成员名遮蔽的噪音）+ 判红
#   MSVC:      C4456/C4457/C4458（同名遮蔽）→ /we 升为错误
# 注：third_party 头文件（onnxruntime）自身含同名遮蔽，策略禁改 → 其 include 置为 SYSTEM
# （见下方 target_include_directories(... SYSTEM ...)），门禁只约束**本仓源码**。
if(MSVC)
  add_compile_options(/we4456 /we4457 /we4458)
else()
  add_compile_options(-Wshadow=local -Werror=shadow)
endif()

include(cmake/OnnxRuntime.cmake)
# --- 第三方：Mongoose（仅 thin_agent WS 可执行文件链接）---
add_library(mongoose_vendor STATIC
  third_party/mongoose/mongoose.c
)
target_include_directories(mongoose_vendor PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/third_party/mongoose
)
set_target_properties(mongoose_vendor PROPERTIES POSITION_INDEPENDENT_CODE ON)

# Mongoose + OpenSSL（thin_agent_gw 出站 wss:// 飞书长连接，仅 BUILD_IM_GATEWAY 时编译）
if(THIN_AGENT_BUILD_IM_GATEWAY)
  find_package(OpenSSL REQUIRED)
  add_library(mongoose_gw_vendor STATIC
    third_party/mongoose/mongoose.c
  )
  target_include_directories(mongoose_gw_vendor PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/mongoose
  )
  target_compile_definitions(mongoose_gw_vendor PUBLIC MG_TLS=MG_TLS_OPENSSL)
  target_link_libraries(mongoose_gw_vendor PUBLIC OpenSSL::SSL OpenSSL::Crypto)
  set_target_properties(mongoose_gw_vendor PROPERTIES POSITION_INDEPENDENT_CODE ON)
endif()

# --- 核心库：按层组织 ---
#   api/      C ABI 公共接口（agent_api.cpp）
#   core/     AgentService, ChatPolicy, IntentScorer, TaskEngine, ExternalInfoClient, IntentOnnx
#   fdbus/    FakeDeviceControl（始终编译）；FdbusDeviceControl（WITH_FDBUS 时）
#   llm/      DemoConfigCompat, CloudLlmClient, ProviderFactory, ModelConfig
#   agent/    ToolSchema, ToolRegistry, AgentLoop, EmbeddingProvider, VectorStore, MemoryManager
#
# 两个 target:
#   thin_agent_core        — STATIC 库（现有测试 + 可执行文件链接）
#   thin_agent_core_shared — SHARED 库（C ABI，仅导出 THIN_API, visibility=hidden）
add_library(thin_agent_core
  src/api/discovery.cpp
  src/core/ActionExecutor.cpp
  src/core/AgentService.cpp
  src/core/AgentServiceChat.cpp
  src/core/AgentServiceUtil.cpp
  src/core/AgentServiceWs.cpp
  src/core/AgentServiceTools.cpp
  src/core/WorkflowManager.cpp
  src/core/ChatPolicy.cpp
  src/core/IntentScorer.cpp
  src/core/DialogSlotRecall.cpp
  src/core/ExternalInfoClient.cpp
  src/core/ExternalIntentHandlers.cpp
  src/core/TaskEngine.cpp
  src/core/IntentOnnx.cpp
  src/core/MediaPlan.cpp
  src/core/SkillRegistry.cpp
  src/core/FuzzyPatcher.cpp
  src/core/FactStore.cpp
  src/core/SessionStore.cpp
  src/core/FcRunStore.cpp
  src/core/HookSystem.cpp
  src/core/PatrolProbe.cpp
  src/core/RequestDispatch.cpp
  src/core/SyntaxChecker.cpp
  src/core/PseudoTerminal.cpp
  src/core/BackgroundProcessManager.cpp
  src/core/SecurityRedactor.cpp
  src/core/CommandValidator.cpp
  src/core/SandboxExecutor.cpp
  src/core/PathValidator.cpp
  src/fdbus/FakeDeviceControl.cpp
  src/llm/ModelConfig.cpp
  src/llm/ProviderFactory.cpp
  src/llm/DemoConfigCompat.cpp
  src/llm/CloudLlmClient.cpp
  src/llm/CurlHttpClient.cpp
  src/agent/ToolRegistry.cpp
  src/agent/Tools.cpp
  src/agent/AgentLoop.cpp
  src/agent/EmbeddingProvider.cpp
  src/agent/VectorStore.cpp
  src/agent/MemoryManager.cpp
  src/agent/McpClient.cpp
  src/agent/McpServer.cpp
  src/agent/StdioTransport.cpp
  src/agent/HttpTransport.cpp
  src/agent/AgentDirectory.cpp
  src/agent/AgentTracer.cpp
  src/agent/ConversationSummarizer.cpp
  src/agent/CheckpointManager.cpp
  src/agent/FilesystemCheckpoint.cpp
  src/agent/ErrorCorrectionStore.cpp
  src/agent/GoalManager.cpp
  src/agent/SkillManager.cpp
  src/agent/AgentRoleManager.cpp
  src/agent/TokenBudget.cpp
  src/agent/ConvergenceTracker.cpp
  src/core/CronScheduler.cpp
  src/core/AgentOrchestrator.cpp
  src/core/ProactiveMonitor.cpp
  src/core/SubAgentBus.cpp
  src/core/Blackboard.cpp
  src/core/CredentialPool.cpp
  src/core/WebhookClient.cpp
  src/core/ResponseCache.cpp
  src/core/V4APatcher.cpp
  src/core/KanbanBoard.cpp
  src/core/KbSearcher.cpp
  src/plugin/PluginContext.cpp
  src/plugin/PluginLoader.cpp
  src/local/ModelPool.cpp
  src/local/TemplateModel.cpp
  src/local/GgufModel.cpp
  src/local/OnnxChatModel.cpp
  src/local/HybridRouter.cpp
)

if(THIN_AGENT_WITH_FDBUS)
  target_sources(thin_agent_core PRIVATE
    src/fdbus/FdbusDeviceControl.cpp
  )
  target_compile_definitions(thin_agent_core PUBLIC THIN_AGENT_WITH_FDBUS=1)
else()
  target_compile_definitions(thin_agent_core PUBLIC THIN_AGENT_WITH_FDBUS=0)
endif()

target_include_directories(thin_agent_core
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/mongoose
)
# v0.54.6: onnxruntime 头属 third_party（策略禁改）——以 **SYSTEM** 引入，其自身的同名遮蔽
# 告警随之被抑制，-Wshadow 门禁只管本仓源码（含 nlohmann 之外的第三方头）。
target_include_directories(thin_agent_core SYSTEM PUBLIC
  ${THIN_AGENT_ONNXRUNTIME_INCLUDE}
)

target_link_libraries(thin_agent_core PUBLIC sqlite3 curl "${THIN_AGENT_ONNXRUNTIME_LIB}" dl)
target_compile_definitions(thin_agent_core PUBLIC
  THIN_AGENT_EMBEDDED_CAM=$<BOOL:${THIN_AGENT_EMBEDDED_CAM}>
)
# v0.54.16: **静态库也必须声明"正在编译该 API 本体"**。`src/api/agent_api.cpp` 同时被
# `thin_agent_core`（静态）与 `thin_agent_core_shared`（动态）编译；只有 SHARED 侧定义了
# `THIN_AGENT_BUILDING_DLL` ⇒ 静态侧 `THIN_API` 展开成 `__declspec(dllimport)`，而该 TU 里是
# **函数定义** —— MSVC/MinGW 都硬报 "definition is marked dllimport"（交叉编译实测）。
# 该宏在非 Windows 下不被使用（THIN_API 走 visibility 分支），故无条件添加无副作用。
target_compile_definitions(thin_agent_core PRIVATE
  THIN_AGENT_BUILDING_DLL
)

# C ABI 动态库（SHARED，仅导出 THIN_API 符号）
# 链接 thin_agent_core 静态库 + api 层，visibility=hidden
add_library(thin_agent_core_shared SHARED
  src/api/agent_api.cpp
  src/api/discovery.cpp
)
target_link_libraries(thin_agent_core_shared PRIVATE thin_agent_core
  sqlite3 curl "${THIN_AGENT_ONNXRUNTIME_LIB}" mongoose_vendor)
target_include_directories(thin_agent_core_shared PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/include
  ${CMAKE_CURRENT_SOURCE_DIR}/third_party/mongoose
)
# v0.54.6: 同 thin_agent_core——third_party 头以 SYSTEM 引入（见上）
target_include_directories(thin_agent_core_shared SYSTEM PRIVATE
  ${THIN_AGENT_ONNXRUNTIME_INCLUDE}
)
target_compile_definitions(thin_agent_core_shared PRIVATE
  THIN_AGENT_BUILDING_DLL
  THIN_AGENT_EMBEDDED_CAM=$<BOOL:${THIN_AGENT_EMBEDDED_CAM}>
  THIN_AGENT_WITH_FDBUS=0
)
set_target_properties(thin_agent_core_shared PROPERTIES
  CXX_VISIBILITY_PRESET hidden
  VISIBILITY_INLINES_HIDDEN ON
  OUTPUT_NAME thin_agent_core
)

# thin_agent_core 静态库需 -fPIC 以便链接进 shared 库
set_target_properties(thin_agent_core PROPERTIES POSITION_INDEPENDENT_CODE ON)

if(THIN_AGENT_WITH_FDBUS)
  include(cmake/FdbusIntegration.cmake)
endif()

# --- 测试（ctest）---
if(THIN_AGENT_BUILD_TESTS)
  include(CTest)
  enable_testing()

  add_executable(test_action_executor_unit
    tests/unit/test_action_executor.cpp
  )
  target_link_libraries(test_action_executor_unit PRIVATE thin_agent_core)
  add_test(NAME unit_action_executor COMMAND test_action_executor_unit)

  # CredentialPool — 加载/轮转/降级/统计
  add_executable(test_credential_pool_unit
    tests/unit/test_credential_pool.cpp
  )
  target_link_libraries(test_credential_pool_unit PRIVATE thin_agent_core)
  add_test(NAME unit_credential_pool COMMAND test_credential_pool_unit)

  # WebhookClient — 配置/脱敏/HMAC 签名投递（本地回环服务器）
  add_executable(test_webhook_client_unit
    tests/unit/test_webhook_client.cpp
  )
  target_link_libraries(test_webhook_client_unit PRIVATE thin_agent_core)
  add_test(NAME unit_webhook_client COMMAND test_webhook_client_unit)

  # MCP 传输层 — HttpTransport（回环回显）+ StdioTransport（fork cat 回显）
  add_executable(test_agent_transport_unit
    tests/unit/test_agent_transport.cpp
  )
  target_link_libraries(test_agent_transport_unit PRIVATE thin_agent_core)
  add_test(NAME unit_agent_transport COMMAND test_agent_transport_unit)

  # HITL 审批锁范围 — 全局锁不被审批长操作持有（跨 session 不阻塞）
  add_executable(test_hitl_lock_scope_unit
    tests/unit/test_hitl_lock_scope.cpp
  )
  target_link_libraries(test_hitl_lock_scope_unit PRIVATE thin_agent_core)
  add_test(NAME unit_hitl_lock_scope COMMAND test_hitl_lock_scope_unit)

  # 会话项目上下文 TLS 范围 — ro/rw 全分支生效 + 无跨会话残留 + RAII
  add_executable(test_project_tls_scope_unit
    tests/unit/test_project_tls_scope.cpp
  )
  target_link_libraries(test_project_tls_scope_unit PRIVATE thin_agent_core)
  add_test(NAME unit_project_tls_scope COMMAND test_project_tls_scope_unit)

  # UDP 组播发现 API — 参数校验、生命周期、默认值
  add_executable(test_discovery_unit
    tests/unit/test_discovery.cpp
  )
  target_link_libraries(test_discovery_unit PRIVATE thin_agent_core)
  add_test(NAME unit_discovery COMMAND test_discovery_unit)

  add_executable(test_tools_fallback_unit
    tests/unit/test_tools_fallback.cpp
  )
  target_link_libraries(test_tools_fallback_unit PRIVATE thin_agent_core)
  add_test(NAME unit_tools_fallback COMMAND test_tools_fallback_unit)

  add_executable(test_web_search_config_unit
    tests/unit/test_web_search_config.cpp
  )
  target_include_directories(test_web_search_config_unit PRIVATE
    \${CMAKE_CURRENT_SOURCE_DIR}/include
  )
  target_link_libraries(test_web_search_config_unit PRIVATE thin_agent_core)
  add_test(NAME unit_web_search_config COMMAND test_web_search_config_unit)

  add_executable(test_session_persist_unit
    tests/unit/test_session_persist.cpp
  )
  target_link_libraries(test_session_persist_unit PRIVATE thin_agent_core)
  add_test(NAME unit_session_persist COMMAND test_session_persist_unit)

  add_executable(test_sandbox_path_rewrite_unit
    tests/unit/test_sandbox_path_rewrite.cpp
  )
  target_link_libraries(test_sandbox_path_rewrite_unit PRIVATE thin_agent_core)
  add_test(NAME unit_sandbox_path_rewrite COMMAND test_sandbox_path_rewrite_unit)

  add_executable(test_slash_command_unit
    tests/unit/test_slash_command.cpp
  )
  target_link_libraries(test_slash_command_unit PRIVATE thin_agent_core)
  add_test(NAME unit_slash_command COMMAND test_slash_command_unit)

  add_executable(test_skill_loader_unit
    tests/unit/test_skill_loader.cpp
  )
  target_include_directories(test_skill_loader_unit PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
  )
  target_link_libraries(test_skill_loader_unit PRIVATE thin_agent_core)
  add_test(NAME unit_skill_loader COMMAND test_skill_loader_unit)

  add_executable(test_provider_config_unit
    tests/unit/test_provider_config.cpp
  )
  target_link_libraries(test_provider_config_unit PRIVATE thin_agent_core)
  add_test(NAME unit_provider_config COMMAND test_provider_config_unit)

  # Provider format conversion (Anthropic ↔ OpenAI roundtrip)
  add_executable(test_provider_format_unit
    tests/unit/test_provider_format.cpp)
  target_link_libraries(test_provider_format_unit PRIVATE thin_agent_core)
  add_test(NAME unit_provider_format COMMAND test_provider_format_unit)

  # Agent service
  add_executable(test_demo_config_loader_unit
    tests/unit/test_demo_config_loader.cpp
  )
  target_link_libraries(test_demo_config_loader_unit PRIVATE thin_agent_core)
  add_test(NAME unit_demo_config_loader COMMAND test_demo_config_loader_unit)

  # DemoConfigCompat::load_demo_profile_compat — 扩展字段（预算/外部查询/复杂意图）
  add_executable(test_demo_config_compat_unit
    tests/unit/test_demo_config_compat.cpp
  )
  target_link_libraries(test_demo_config_compat_unit PRIVATE thin_agent_core)
  add_test(NAME unit_demo_config_compat COMMAND test_demo_config_compat_unit)

  # AgentService 路由、槽位、端云策略、ChatPolicy 配置化
  add_executable(test_agent_service_unit
    tests/unit/test_agent_service.cpp
  )
  # v0.53.3: whole-archive——测试进程 dlopen 插件时需要核心符号全量导出
  #（插件依赖 KanbanBoard::push_batch 等无测试引用的成员，普通链接被裁）
  target_link_libraries(test_agent_service_unit PRIVATE
    -Wl,--whole-archive thin_agent_core -Wl,--no-whole-archive)
  target_link_options(test_agent_service_unit PRIVATE -rdynamic)
  add_test(NAME unit_agent_service COMMAND test_agent_service_unit)
  set_tests_properties(unit_agent_service PROPERTIES TIMEOUT 300)  # v0.27.2: WSL 环境下需更长时间（云 LLM 超时等待）

  add_executable(test_media_plan_unit
    tests/unit/test_media_plan.cpp
  )
  target_link_libraries(test_media_plan_unit PRIVATE thin_agent_core)
  add_test(NAME unit_media_plan COMMAND test_media_plan_unit)

  add_executable(test_intent_scorer_unit
    tests/unit/test_intent_scorer.cpp
  )
  target_link_libraries(test_intent_scorer_unit PRIVATE thin_agent_core)
  add_test(NAME unit_intent_scorer COMMAND test_intent_scorer_unit)

  add_executable(test_intent_onnx_unit
    tests/unit/test_intent_onnx.cpp
  )
  target_link_libraries(test_intent_onnx_unit PRIVATE thin_agent_core)
  add_test(NAME unit_intent_onnx COMMAND test_intent_onnx_unit)

  # ONNX holdout regression (models/intent/golden_eval.jsonl); needs .venv + numpy/onnxruntime
  add_test(
    NAME unit_intent_golden
    COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/scripts/intent/run_eval_golden_ctest.sh
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
  )

  add_executable(test_task_engine_unit
    tests/unit/test_task_engine.cpp
  )
  target_link_libraries(test_task_engine_unit PRIVATE thin_agent_core)
  add_test(NAME unit_task_engine COMMAND test_task_engine_unit)

  # Agent Core 组件（ToolRegistry / EmbeddingProvider / VectorStore / MemoryManager）
  add_executable(test_agent_core_unit
    tests/unit/test_agent_core.cpp
  )
  target_link_libraries(test_agent_core_unit PRIVATE thin_agent_core)
  add_test(NAME unit_agent_core COMMAND test_agent_core_unit)

  # FuzzyPatcher 模糊匹配引擎
  add_executable(test_fuzzy_patcher_unit
    tests/unit/test_fuzzy_patcher.cpp
  )
  target_link_libraries(test_fuzzy_patcher_unit PRIVATE thin_agent_core)
  add_test(NAME unit_fuzzy_patcher COMMAND test_fuzzy_patcher_unit)

  # V4APatcher 多文件 patch (v0.45.0)
  add_executable(test_v4a_patcher_unit
    tests/unit/test_v4a_patcher.cpp
  )
  target_link_libraries(test_v4a_patcher_unit PRIVATE thin_agent_core)
  add_test(NAME unit_v4a_patcher COMMAND test_v4a_patcher_unit)

  # UTF-8 安全截断工具
  add_executable(test_utf8_truncate_unit
    tests/unit/test_utf8_truncate.cpp
  )
  target_link_libraries(test_utf8_truncate_unit PRIVATE thin_agent_core)
  add_test(NAME unit_utf8_truncate COMMAND test_utf8_truncate_unit)

  # FactStore 持久记忆存储
  add_executable(test_fact_store_unit
    tests/unit/test_fact_store.cpp)
  target_link_libraries(test_fact_store_unit PRIVATE thin_agent_core)
  add_test(NAME unit_fact_store COMMAND test_fact_store_unit)

  # SessionStore 会话搜索
  add_executable(test_session_store_unit
    tests/unit/test_session_store.cpp)
  target_link_libraries(test_session_store_unit PRIVATE thin_agent_core)
  add_test(NAME unit_session_store COMMAND test_session_store_unit)

  # PatrolProbe 系统巡检
  add_executable(test_patrol_probe_unit
    tests/unit/test_patrol_probe.cpp)
  target_link_libraries(test_patrol_probe_unit PRIVATE thin_agent_core)
  add_test(NAME unit_patrol_probe COMMAND test_patrol_probe_unit)

  # Python code execution (A1 — pure computation, subprocess isolation)
  add_executable(test_code_exec_unit
    tests/unit/test_code_exec.cpp)
  target_link_libraries(test_code_exec_unit PRIVATE thin_agent_core)
  add_test(NAME unit_code_exec COMMAND test_code_exec_unit)

  # code_dev plugin (code_read_file / code_patch / code_search / code_write_file)
  add_executable(test_code_dev_unit
    tests/unit/test_code_dev.cpp)
  target_link_libraries(test_code_dev_unit PRIVATE thin_agent_core)
  add_test(NAME unit_code_dev COMMAND test_code_dev_unit)

  # SyntaxChecker (JSON/YAML/Python/JS/C++ syntax validation)
  add_executable(test_syntax_check_unit
    tests/unit/test_syntax_check.cpp)
  target_link_libraries(test_syntax_check_unit PRIVATE thin_agent_core)
  add_test(NAME unit_syntax_check COMMAND test_syntax_check_unit)

  # PTY + Background Process tests (pty_exec, BackgroundProcessManager)
  add_executable(test_pty_background_unit
    tests/unit/test_pty_background.cpp)
  target_link_libraries(test_pty_background_unit PRIVATE thin_agent_core)
  add_test(NAME unit_pty_background COMMAND test_pty_background_unit)

  # Security Redaction + Command Validator tests
  add_executable(test_security_unit
    tests/unit/test_security.cpp)
  target_link_libraries(test_security_unit PRIVATE thin_agent_core)
  add_test(NAME unit_security COMMAND test_security_unit)

  # PathValidator (v0.48.0: 路径安全校验 — 黑名单/白名单/逃逸/会话隔离)
  add_executable(test_path_validator_unit
    tests/unit/test_path_validator.cpp)
  target_link_libraries(test_path_validator_unit PRIVATE thin_agent_core)
  add_test(NAME unit_path_validator COMMAND test_path_validator_unit)

  # HITL (v0.49.0: FC循环危险工具审批 + 统一风险分级)
  add_executable(test_hitl_fc_unit
    tests/unit/test_hitl_fc.cpp)
  target_link_libraries(test_hitl_fc_unit PRIVATE thin_agent_core)
  add_test(NAME unit_hitl_fc COMMAND test_hitl_fc_unit)

  # MCP Server tests
  add_executable(test_stdio_transport_args_unit
    tests/unit/test_stdio_transport_args.cpp
  )
  target_link_libraries(test_stdio_transport_args_unit PRIVATE thin_agent_core)
  add_test(NAME unit_stdio_transport_args COMMAND test_stdio_transport_args_unit)
  # v0.54.9 (R93「测试预算」): 真链路（npx MCP server）已移出 unit 层 → e2e_mcp_real_chain（见下）。
  # 本测试现在只剩**确定性**组（python3 -c 假 server，实测 ~1s），TIMEOUT 从 560 收紧到 60：
  # 原来 560 是给"npx 冷启动 210s"留的窗，而那个窗口已随真链路一起搬走。
  set_tests_properties(unit_stdio_transport_args PROPERTIES TIMEOUT 60)

  # v0.54.9 (R93「测试预算」): 真 MCP 链路 = **e2e 层 + 显式门控**。它是真网络链路（同一目标实测
  # 76.97~474.37s，占全量 637s 的 74%），住 unit 层会把全量预算与确定性一起拖坏。默认 SKIP，
  # 需要时：TA_MCP_REAL=1 ctest -R e2e_mcp_real_chain --output-on-failure
  add_executable(e2e_mcp_real_chain
    tests/e2e/test_mcp_real_chain.cpp
  )
  target_link_libraries(e2e_mcp_real_chain PRIVATE thin_agent_core)
  add_test(NAME e2e_mcp_real_chain COMMAND e2e_mcp_real_chain)
  set_tests_properties(e2e_mcp_real_chain PROPERTIES
    TIMEOUT 900
    SKIP_REGULAR_EXPRESSION "SKIP:"
  )

  # v0.54.11 (R93「测试登记债」): `test_spawn_nesting_limit.py` 从"未登记 ⇒ 静默腐烂"变**进 CI**。
  # 它是 pytest 型（用 tests/e2e/conftest.py 的 fixture），故用 `-m pytest` 跑；本机无 pytest 时
  # 不注册（与 node 型测试的 find_program 约定一致）。它此前断言腐烂（顶层 turns_exhausted vs
  # 白名单不符）+ 护栏拒绝无日志，两者已在 v0.54.11 一并修好。
  execute_process(COMMAND python3 -c "import pytest"
                  RESULT_VARIABLE _PYTEST_PROBE_RC OUTPUT_QUIET ERROR_QUIET)
  if(_PYTEST_PROBE_RC EQUAL 0)
    add_test(NAME e2e_spawn_nesting_limit
      COMMAND python3 -m pytest -q ${CMAKE_SOURCE_DIR}/tests/e2e/test_spawn_nesting_limit.py)
    set_tests_properties(e2e_spawn_nesting_limit PROPERTIES
      TIMEOUT 300
      RUN_SERIAL TRUE)   # 独占 mock 服务端口 8792

    # v0.54.12 (R93「测试登记债」): `test_plugin_split.py` 进 CI——覆盖 9 条插件链路
    # （kanban/collab/collab_msg/meta/monitor/state/cron/patrol/cam_media），此前**零覆盖**。
    # 进 CI 前已加 home 隔离（避免与其它测试抢真实 ~/.thin_agent）。
    add_test(NAME e2e_plugin_split
      COMMAND python3 -m pytest -q ${CMAKE_SOURCE_DIR}/tests/e2e/test_plugin_split.py)
    set_tests_properties(e2e_plugin_split PROPERTIES
      TIMEOUT 300
      RUN_SERIAL TRUE)   # 独占服务端口 18793

    # v0.54.13 (R93「测试登记债」): `test_smoke_and_safety.py` 登记进 CI——但它是**真模型 e2e**
    # （chat 经真云链路，实测 7 passed / 120.37s），故与 e2e_mcp_real_chain 同一纪律：
    # **显式门控**，默认 SKIP，需要时 `TA_SMOKE_REAL=1 ctest -R e2e_smoke_real --output-on-failure`。
    # 全仓唯一覆盖：白名单编译器不弹审批 / rm -rf 被 HITL 拦截且拒绝后不执行 / 沙箱路径映射 /
    # 多语言编程闭环 / cron 无人值守拒绝。
    add_test(NAME e2e_smoke_real
      COMMAND python3 -m pytest -q ${CMAKE_SOURCE_DIR}/tests/e2e/test_smoke_and_safety.py)
    set_tests_properties(e2e_smoke_real PROPERTIES
      TIMEOUT 900
      RUN_SERIAL TRUE)   # 独占服务端口 8765

    # v0.54.14 (R93「测试登记债」收官): 两项"隔离即红"用例**自足化后**进 CI。
    # 实测澄清：R92 记的"依赖真实 home 既有状态"实为**路径硬编码的症状**——空 home 下服务会自建
    # 全部前置表（fc_runs/fc_approvals/gw_chat_sessions/goals，schema 与真实 home 一致）、30 条
    # 插件照常加载、复杂任务路由正常。两文件现已把 home 参数化为 THIN_AGENT_HOME（import 时清空
    # 重建）并派生断言路径，实测：decompose 3 passed / checkpoint+hooks 5 passed（均在隔离 home）。
    add_test(NAME e2e_decompose_route
      COMMAND python3 -m pytest -q ${CMAKE_SOURCE_DIR}/tests/e2e/test_decompose_route.py)
    set_tests_properties(e2e_decompose_route PROPERTIES
      TIMEOUT 300
      RUN_SERIAL TRUE)   # 独占服务端口 8791
    add_test(NAME e2e_checkpoint_resume_hooks
      COMMAND python3 -m pytest -q ${CMAKE_SOURCE_DIR}/tests/e2e/test_checkpoint_resume_hooks.py)
    set_tests_properties(e2e_checkpoint_resume_hooks PROPERTIES
      TIMEOUT 300
      RUN_SERIAL TRUE)   # 独占服务端口 8793（含崩溃重启，端口须独占）
  else()
    message(STATUS "pytest 未安装——跳过注册 e2e_spawn_nesting_limit / e2e_plugin_split / e2e_smoke_real / e2e_decompose_route / e2e_checkpoint_resume_hooks")
  endif()

  # v0.53.57: FC 批量工具并发上限
  add_executable(test_fc_tool_concurrency_unit
    tests/unit/test_fc_tool_concurrency.cpp)
  target_link_libraries(test_fc_tool_concurrency_unit PRIVATE thin_agent_core)
  target_include_directories(test_fc_tool_concurrency_unit PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include)
  add_test(NAME unit_fc_tool_concurrency COMMAND test_fc_tool_concurrency_unit)

  # v0.53.53: FTS 特字符查询
  add_executable(test_session_fts_special_unit
    tests/unit/test_session_fts_special.cpp)
  target_link_libraries(test_session_fts_special_unit PRIVATE thin_agent_core)
  target_include_directories(test_session_fts_special_unit PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include)
  add_test(NAME unit_session_fts_special COMMAND test_session_fts_special_unit)

  # v0.53.50: 插件静态指针悬垂防护系统性回归
  add_executable(test_plugin_static_ptrs_unit
    tests/unit/test_plugin_static_ptrs.cpp)
  target_link_libraries(test_plugin_static_ptrs_unit PRIVATE thin_agent_core_shared)
  target_include_directories(test_plugin_static_ptrs_unit PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include)
  add_test(NAME unit_plugin_static_ptrs COMMAND test_plugin_static_ptrs_unit)

  # v0.53.48: AgentLoop 续跑
  add_executable(test_agentloop_resume_unit
    tests/unit/test_agentloop_resume.cpp)
  target_link_libraries(test_agentloop_resume_unit PRIVATE thin_agent_core)
  target_include_directories(test_agentloop_resume_unit PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include)
  add_test(NAME unit_agentloop_resume COMMAND test_agentloop_resume_unit)

  # v0.53.47: PluginContext 悬垂保护
  add_executable(test_plugin_ctx_lifetime_unit
    tests/unit/test_plugin_ctx_lifetime.cpp)
  target_link_libraries(test_plugin_ctx_lifetime_unit PRIVATE thin_agent_core_shared)
  target_include_directories(test_plugin_ctx_lifetime_unit PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include)
  add_test(NAME unit_plugin_ctx_lifetime COMMAND test_plugin_ctx_lifetime_unit)

  # v0.53.46: cron 两段式锁回归
  add_executable(test_cron_lock_release_unit
    tests/unit/test_cron_lock_release.cpp)
  target_link_libraries(test_cron_lock_release_unit PRIVATE thin_agent_core)

  # v0.53.75: 畸形 schedule 防御(stoi 异常直穿 ticker)
  add_executable(test_cron_bad_schedule_unit
    tests/unit/test_cron_bad_schedule.cpp)
  target_link_libraries(test_cron_bad_schedule_unit PRIVATE thin_agent_core)
  add_test(NAME unit_cron_bad_schedule COMMAND test_cron_bad_schedule_unit)
  set_tests_properties(unit_cron_bad_schedule PROPERTIES TIMEOUT 30)
  add_test(NAME unit_cron_lock_release COMMAND test_cron_lock_release_unit)

  # v0.54.3 (R90): trigger_now 两段式锁（慢 callback 不阻塞 add/二次触发不重复执行）
  add_executable(test_cron_trigger_lock_unit tests/unit/test_cron_trigger_lock.cpp)
  target_link_libraries(test_cron_trigger_lock_unit PRIVATE thin_agent_core)
  target_include_directories(test_cron_trigger_lock_unit PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/unit
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/nlohmann)
  add_test(NAME unit_cron_trigger_lock COMMAND test_cron_trigger_lock_unit)
  set_tests_properties(unit_cron_trigger_lock PROPERTIES TIMEOUT 60)

  # v0.53.44: agent_api C ABI 生命周期+队列化
  add_executable(test_agent_api_lifecycle_unit
    tests/unit/test_agent_api_lifecycle.cpp)
  target_link_libraries(test_agent_api_lifecycle_unit PRIVATE
    thin_agent_core_shared)
  target_include_directories(test_agent_api_lifecycle_unit PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/nlohmann)
  add_test(NAME unit_agent_api_lifecycle COMMAND test_agent_api_lifecycle_unit)
  # v0.53.97: C ABI 停机契约（不排空队列/快速返回/排队请求收停机通告）+ 契约面
  add_executable(test_agent_api_shutdown_unit tests/unit/test_agent_api_shutdown.cpp)
  target_link_libraries(test_agent_api_shutdown_unit PRIVATE thin_agent_core_shared)
  target_include_directories(test_agent_api_shutdown_unit PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/unit
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/nlohmann)
  add_test(NAME unit_agent_api_shutdown COMMAND test_agent_api_shutdown_unit)
  # v0.53.98: 组播发现面 C ABI 稳健性/契约（NULL 不崩 / timeout=0 不挂死 / 去重 / 停机有界）
  add_executable(test_discovery_robustness_unit tests/unit/test_discovery_robustness.cpp)
  target_link_libraries(test_discovery_robustness_unit PRIVATE thin_agent_core_shared)
  target_include_directories(test_discovery_robustness_unit PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/unit
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/nlohmann)
  add_test(NAME unit_discovery_robustness COMMAND test_discovery_robustness_unit)
  set_tests_properties(unit_discovery_robustness PROPERTIES TIMEOUT 60)

  # v0.53.40: execute_code 多语言(类驱动白名单)
  add_executable(test_execute_code_languages_unit
    tests/unit/test_execute_code_languages.cpp)
  target_link_libraries(test_execute_code_languages_unit PRIVATE
    -Wl,--whole-archive thin_agent_core -Wl,--no-whole-archive)
  target_link_options(test_execute_code_languages_unit PRIVATE -rdynamic)
  target_include_directories(test_execute_code_languages_unit PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/nlohmann)
  add_test(NAME unit_execute_code_languages COMMAND test_execute_code_languages_unit)

  add_executable(test_mcp_server_unit
    tests/unit/test_mcp_server.cpp
    src/agent/McpServer.cpp)
  target_include_directories(test_mcp_server_unit PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/nlohmann)
  target_link_libraries(test_mcp_server_unit PRIVATE thin_agent_core)
  add_test(NAME unit_mcp_server COMMAND test_mcp_server_unit)

  # Sandbox Executor tests (Linux namespace isolation)
  add_executable(test_sandbox_unit
    tests/unit/test_sandbox_executor.cpp)
  target_include_directories(test_sandbox_unit PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/nlohmann)
  target_link_libraries(test_sandbox_unit PRIVATE thin_agent_core)
  add_test(NAME unit_sandbox COMMAND test_sandbox_unit)

  # Discord adapter tests (platform integration)
  add_executable(test_discord_unit
    tests/unit/test_discord_unit.cpp
    src/gateway/DiscordAdapter.cpp)
  target_include_directories(test_discord_unit PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/nlohmann
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/mongoose
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/curl/include)
  target_link_libraries(test_discord_unit PRIVATE
    thin_agent_core mongoose_gw_vendor curl)
  add_test(NAME unit_discord COMMAND test_discord_unit)

  # FilesystemCheckpoint (save / rollback / diff / dedup / exclude / prune)
  add_executable(test_filesystem_checkpoint_unit
    tests/unit/test_filesystem_checkpoint.cpp)
  target_link_libraries(test_filesystem_checkpoint_unit PRIVATE thin_agent_core)
  add_test(NAME unit_filesystem_checkpoint COMMAND test_filesystem_checkpoint_unit)

  # git_ops plugin (git_status / diff / log / add / commit / show / checkout / branch)
  add_executable(test_git_ops_unit
    tests/unit/test_git_ops.cpp)
  target_link_libraries(test_git_ops_unit PRIVATE thin_agent_core)
  add_test(NAME unit_git_ops COMMAND test_git_ops_unit
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

  # workflow plugin (workflow_list / view / create / set / delete / run)
  add_executable(test_workflow_unit
    tests/unit/test_workflow.cpp)
  target_link_libraries(test_workflow_unit PRIVATE thin_agent_core)
  add_test(NAME unit_workflow COMMAND test_workflow_unit)

  # kb plugin (kb_dir / kb_db_path / load_kb_index / save_kb_index helpers)
  add_executable(test_kb_unit
    tests/unit/test_kb.cpp)
  target_link_libraries(test_kb_unit PRIVATE thin_agent_core sqlite3)
  add_test(NAME unit_kb COMMAND test_kb_unit)

  # Local models (ModelPool / TemplateModel / HybridRouter / AgentTracer)
  add_executable(test_local_models_unit
    tests/unit/test_local_models.cpp
  )
  target_link_libraries(test_local_models_unit PRIVATE thin_agent_core)
  add_test(NAME unit_local_models COMMAND test_local_models_unit)

  # 自主性基础设施：TokenBudget + ConvergenceTracker + safety + LLM retry
  add_executable(test_autonomy_unit
    tests/unit/test_autonomy.cpp
  )
  target_link_libraries(test_autonomy_unit PRIVATE thin_agent_core)
  add_test(NAME unit_autonomy COMMAND test_autonomy_unit)

  # v0.38.0: 语义记忆自动注入测试
  add_executable(test_memory_injection_unit
    tests/unit/test_memory_injection.cpp
  )
  target_link_libraries(test_memory_injection_unit PRIVATE thin_agent_core)
  add_test(NAME unit_memory_injection COMMAND test_memory_injection_unit)

  # v0.38.1: 技能自进化测试
  add_executable(test_skill_evolution_unit
    tests/unit/test_skill_evolution.cpp
  )
  target_link_libraries(test_skill_evolution_unit PRIVATE thin_agent_core)
  add_test(NAME unit_skill_evolution COMMAND test_skill_evolution_unit)

  # v0.38.2: 主FC路径注入链测试（memory + skill + goal + correction 全闭环）
  add_executable(test_prompt_injection_unit
    tests/unit/test_prompt_injection.cpp
  )
  target_link_libraries(test_prompt_injection_unit PRIVATE thin_agent_core)
  add_test(NAME unit_prompt_injection COMMAND test_prompt_injection_unit)

  # v0.39.0: Vision 多模态测试
  add_executable(test_vision_unit
    tests/unit/test_vision.cpp
  )
  target_link_libraries(test_vision_unit PRIVATE thin_agent_core)
  add_test(NAME unit_vision COMMAND test_vision_unit)

  # v0.39.1: AgentLoop 子Agent路径测试（skill注入 + vision端到端）
  add_executable(test_agent_loop_unit
    tests/unit/test_agent_loop.cpp
  )
  target_link_libraries(test_agent_loop_unit PRIVATE thin_agent_core)
  add_test(NAME unit_agent_loop COMMAND test_agent_loop_unit)

  # v0.53.82: 同互斥量嵌套获取静态扫描——补锁前置三问第一问的机械化判官
  # （v0.53.77 "五处补锁无嵌套核查" 造成 ping/action/task_* 自死锁实锤）
  find_program(PYTHON3_BIN_LOCKSCAN python3)
  if(PYTHON3_BIN_LOCKSCAN)
    add_test(NAME static_nested_locks
      COMMAND ${PYTHON3_BIN_LOCKSCAN}
              ${CMAKE_SOURCE_DIR}/scripts/check_nested_locks.py
              ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
    set_tests_properties(static_nested_locks PROPERTIES TIMEOUT 60)
  endif()

  # v0.53.82: 即时请求"必须返回"兜底（看门狗 30s 强杀）+ 判据表契约
  add_executable(test_instant_requests_unit
    tests/unit/test_instant_requests.cpp
  )
  target_link_libraries(test_instant_requests_unit PRIVATE thin_agent_core)
  add_test(NAME unit_instant_requests COMMAND test_instant_requests_unit)
  set_tests_properties(unit_instant_requests PROPERTIES TIMEOUT 120)

  # v0.53.82: WS 事件循环活性 e2e——长任务期间 ping 必须及时响应
  # （长任务白名单漏网的"可观测化"判据：mock LLM 延迟放大冻结时长）
  find_program(PYTHON3_BIN_E2E_FREEZE python3)
  if(PYTHON3_BIN_E2E_FREEZE)
    add_test(NAME e2e_event_loop_freeze
      COMMAND ${PYTHON3_BIN_E2E_FREEZE} ${CMAKE_SOURCE_DIR}/tests/e2e/test_event_loop_freeze.py)
    set_tests_properties(e2e_event_loop_freeze PROPERTIES
      SKIP_REGULAR_EXPRESSION "SKIP:"
      TIMEOUT 240 RUN_SERIAL TRUE)  # 独占 mock 1234 口
  endif()

  # v0.53.83: 网关发送"失败可观测"统一判据（R67 同款待修收口）
  add_executable(test_http_observability_unit
    tests/unit/test_http_observability.cpp
  )
  target_include_directories(test_http_observability_unit PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/curl/include)
  target_link_libraries(test_http_observability_unit PRIVATE curl)
  add_test(NAME unit_http_observability COMMAND test_http_observability_unit)
  set_tests_properties(unit_http_observability PROPERTIES TIMEOUT 60)

  # v0.53.86: 网关发送队列有界策略（无界队列家族收口，配 unit_gateway_send_queue）
  add_executable(test_gateway_send_queue_unit
    tests/unit/test_gateway_send_queue.cpp
  )
  target_include_directories(test_gateway_send_queue_unit PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include)
  add_test(NAME unit_gateway_send_queue COMMAND test_gateway_send_queue_unit)

  # v0.53.99 (R86): 重连退避策略（此前 gw2 每 3s 无退避重连）
  add_executable(test_reconnect_backoff_unit tests/unit/test_reconnect_backoff.cpp)
  target_include_directories(test_reconnect_backoff_unit PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include)
  add_test(NAME unit_reconnect_backoff COMMAND test_reconnect_backoff_unit)
  set_tests_properties(unit_reconnect_backoff PROPERTIES TIMEOUT 30)

  # v0.53.99 (R86): 网关重连退避 e2e（起真 gw2 指向死端口，读轮转日志断言档位递增）
  add_test(NAME e2e_gateway_reconnect_backoff
    COMMAND python3 ${CMAKE_SOURCE_DIR}/tests/e2e/test_gateway_reconnect_backoff.py)
  set_tests_properties(e2e_gateway_reconnect_backoff PROPERTIES TIMEOUT 120 RUN_SERIAL TRUE)

  # v0.54.1 (R88): 同一判据跑 v1（孪生漏网复查——R86 只修了 v2）
  add_test(NAME e2e_gateway_reconnect_backoff_v1
    COMMAND python3 ${CMAKE_SOURCE_DIR}/tests/e2e/test_gateway_reconnect_backoff.py)
  set_tests_properties(e2e_gateway_reconnect_backoff_v1 PROPERTIES
    TIMEOUT 120 RUN_SERIAL TRUE ENVIRONMENT "GW_BIN=build/thin_agent_gw")

  # v0.54.1 (R88): 单请求内扇出上限（agent_debate: roles×rounds 客户端可控无上限）
  add_executable(test_fanout_limits_unit tests/unit/test_fanout_limits.cpp)
  target_include_directories(test_fanout_limits_unit PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
  add_test(NAME unit_fanout_limits COMMAND test_fanout_limits_unit)

  # v0.54.2 (R89): 机械判官——单请求内重活/扇出无上限家族普查（新增请求体驱动循环必须登记上限）
  add_test(NAME static_unbounded_fanout
    COMMAND python3 ${CMAKE_SOURCE_DIR}/scripts/check_unbounded_fanout.py --root ${CMAKE_SOURCE_DIR})
  set_tests_properties(static_unbounded_fanout PROPERTIES TIMEOUT 60)

  # v0.54.3 (R90): 机械判官——持锁作用域内含长耗时调用（锁跨重活）普查
  add_test(NAME static_lock_across_heavy
    COMMAND python3 ${CMAKE_SOURCE_DIR}/scripts/check_lock_across_heavy.py --root ${CMAKE_SOURCE_DIR})
  set_tests_properties(static_lock_across_heavy PROPERTIES TIMEOUT 60)

  # v0.54.7 (R93): 机械判官——CronScheduler 测试必须密闭（固定路径 DB 须在 start() 前清除；
  # 否则库里"已到期 enabled=1"的残留会被 ticker 补跑，断言随库状态漂 = 假抖动）
  add_test(NAME static_cron_test_hermetic
    COMMAND python3 ${CMAKE_SOURCE_DIR}/scripts/check_cron_test_hermetic.py --root ${CMAKE_SOURCE_DIR})
  set_tests_properties(static_cron_test_hermetic PROPERTIES TIMEOUT 60)

  # v0.54.10 (R93 地雷清理): 机械判官——**不得宽匹配杀进程**（禁止 pkill/killall/fuser -k；
  # 把 pgrep 结果拿去 kill 的文件，其模式必须带 -- 级辨识参数）。起因是两处真实地雷：
  # demo_smoke_all.sh 的 `pgrep -f "build/thin_agent" | kill`（会误杀同机所有 thin_agent 实例）与
  # test_plugin_split.py 的 `pgrep -f "port 18793" | SIGKILL`（宽匹配）。
  add_test(NAME static_no_broad_kill
    COMMAND python3 ${CMAKE_SOURCE_DIR}/scripts/check_no_broad_kill.py --root ${CMAKE_SOURCE_DIR})
  set_tests_properties(static_no_broad_kill PROPERTIES TIMEOUT 60)

  # v0.54.5 (R92): 机械判官——测试文件是否被 CI 引用（棘轮：存量冻结、新增未登记判红）
  add_test(NAME static_test_registration
    COMMAND python3 ${CMAKE_SOURCE_DIR}/scripts/check_test_registration.py --root ${CMAKE_SOURCE_DIR})
  set_tests_properties(static_test_registration PROPERTIES TIMEOUT 60)

  # v0.54.16: **Windows 段真编译**门禁（MinGW-w64 交叉编译，棘轮式）。`_WIN32` 分支在 Linux CI 里
  # 从不被编译器解析 = "从未编译的代码"（历史上靠肉眼预扫）。判据：含 `_WIN32` 的文件**动态发现**
  # 且必须真编译通过；其余失败冻结在分类 BACKLOG（环境类/真缺陷类）；出现新失败即红。
  # 本机无交叉编译器时脚本打印 SKIP: ⇒ ctest 标 Skipped（不谎绿）。
  add_test(NAME static_win32_cross_compile
    COMMAND python3 ${CMAKE_SOURCE_DIR}/scripts/check_win32_cross_compile.py --root ${CMAKE_SOURCE_DIR})
  set_tests_properties(static_win32_cross_compile PROPERTIES
    TIMEOUT 300
    SKIP_REGULAR_EXPRESSION "SKIP:")
  set_tests_properties(unit_fanout_limits PROPERTIES TIMEOUT 30)
  set_tests_properties(unit_gateway_send_queue PROPERTIES TIMEOUT 30)

  # v0.53.88: 前端断线收尾源码面守卫（在途思考占位/流式态必须收尾）
  find_program(NODE_BIN_FRONTEND node)
  if(NODE_BIN_FRONTEND)
    add_test(NAME unit_frontend_inflight_cleanup
      COMMAND ${NODE_BIN_FRONTEND} ${CMAKE_SOURCE_DIR}/tests/unit/test_frontend_inflight_cleanup.js)
    set_tests_properties(unit_frontend_inflight_cleanup PROPERTIES TIMEOUT 30)
  endif()

  # v0.53.89: TaskEngine 协作式取消（记账式取消修复：结果后写保护/退避可打断）
  add_executable(test_task_cancel_unit tests/unit/test_task_cancel.cpp)
  target_include_directories(test_task_cancel_unit PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include ${CMAKE_CURRENT_SOURCE_DIR}/tests/unit)
  target_link_libraries(test_task_cancel_unit PRIVATE thin_agent_core)  # pthread 由 core 传递
  add_test(NAME unit_task_cancel COMMAND test_task_cancel_unit)
  set_tests_properties(unit_task_cancel PROPERTIES TIMEOUT 60)

  # v0.53.90: 技能/插件调用异常隔离（dispatch_cpp 单点收口）
  add_executable(test_skill_dispatch_exception_unit tests/unit/test_skill_dispatch_exception.cpp)
  target_include_directories(test_skill_dispatch_exception_unit PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include ${CMAKE_CURRENT_SOURCE_DIR}/tests/unit)
  target_link_libraries(test_skill_dispatch_exception_unit PRIVATE thin_agent_core)
  add_test(NAME unit_skill_dispatch_exception COMMAND test_skill_dispatch_exception_unit)
  set_tests_properties(unit_skill_dispatch_exception PROPERTIES TIMEOUT 30)

  # v0.53.91: 僵尸任务启动收尾（崩溃/重启后的非终态行）
  add_executable(test_task_reap_unit tests/unit/test_task_reap.cpp)
  target_include_directories(test_task_reap_unit PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include ${CMAKE_CURRENT_SOURCE_DIR}/tests/unit)
  target_link_libraries(test_task_reap_unit PRIVATE thin_agent_core sqlite3)
  add_test(NAME unit_task_reap COMMAND test_task_reap_unit)
  set_tests_properties(unit_task_reap PROPERTIES TIMEOUT 30)

  # v0.53.92: MCP stdio 协议一致性 e2e（stdout 只许合法 JSON/通知不应答/字符串 id 透传）
  if(NODE_BIN_FRONTEND)
    # v0.53.96: 测试间固定资源冲突判官（端口/硬编码 db 路径——-j4 并行下互相踩）
    find_program(PYTHON3_BIN_RESCOL python3)
    if(PYTHON3_BIN_RESCOL)
      add_test(NAME static_test_resource_collision
        COMMAND ${PYTHON3_BIN_RESCOL} ${CMAKE_SOURCE_DIR}/scripts/check_test_resource_collisions.py)
      set_tests_properties(static_test_resource_collision PROPERTIES TIMEOUT 60)
    endif()
    # v0.53.94: CLI 一次性模式终局契约（回复即退/退出码 0/2/3/4/结构化错误）
    add_test(NAME e2e_cli_outcome
      COMMAND ${NODE_BIN_FRONTEND} ${CMAKE_SOURCE_DIR}/tests/e2e/test_cli_outcome.js)
    set_tests_properties(e2e_cli_outcome PROPERTIES TIMEOUT 120 RUN_SERIAL TRUE
      ENVIRONMENT "REPO=${CMAKE_SOURCE_DIR}")
    # v0.53.93: 停机收尾 e2e（排队任务收停机通告 / 在飞结果不丢 / 停机不被队列拖住）
    add_test(NAME e2e_shutdown_notice
      COMMAND ${NODE_BIN_FRONTEND} ${CMAKE_SOURCE_DIR}/tests/e2e/test_shutdown_notice.js)
    set_tests_properties(e2e_shutdown_notice PROPERTIES TIMEOUT 120 RUN_SERIAL TRUE
      ENVIRONMENT "REPO=${CMAKE_SOURCE_DIR};WS_PORT=18799")
    add_test(NAME e2e_mcp_stdio_conformance
      COMMAND ${NODE_BIN_FRONTEND} ${CMAKE_SOURCE_DIR}/tests/e2e/test_mcp_stdio_conformance.js)
    set_tests_properties(e2e_mcp_stdio_conformance PROPERTIES TIMEOUT 60
      ENVIRONMENT "MCP_BIN=${CMAKE_BINARY_DIR}/thin_agent_mcp_server;MCP_CONFIG=${CMAKE_SOURCE_DIR}/config/chat_policy.json")
  endif()

  # v0.53.84: WS API 全类型面扫描（80 个 handler）——"让服务动起来"的判据
  # （R72 用它 30 秒内复现 get_project/set_project_mode 持锁 finish 自死锁）
  find_program(PYTHON3_BIN_SWEEP python3)
  if(PYTHON3_BIN_SWEEP)
    add_test(NAME e2e_ws_api_surface_sweep
      COMMAND ${PYTHON3_BIN_SWEEP} ${CMAKE_SOURCE_DIR}/tests/e2e/ws_api_surface_sweep.py)
    set_tests_properties(e2e_ws_api_surface_sweep PROPERTIES
      SKIP_REGULAR_EXPRESSION "SKIP:"
      TIMEOUT 600 RUN_SERIAL TRUE)  # 独占 mock 1234 口
  endif()

  # v0.54.7 (R93): WS 关闭归因契约 e2e——"谁关的、关时在跑什么"必须可从服务端日志自证
  # （对端 CLOSE 帧经 mongoose 会 echo+置 is_draining ⇒ 与"服务端自己关"签名相同，须靠本埋点分辨）
  find_program(PYTHON3_BIN_WSATTR python3)
  if(PYTHON3_BIN_WSATTR)
    add_test(NAME e2e_ws_close_attribution
      COMMAND ${PYTHON3_BIN_WSATTR} ${CMAKE_SOURCE_DIR}/tests/e2e/test_ws_close_attribution.py)
    set_tests_properties(e2e_ws_close_attribution PROPERTIES
      SKIP_REGULAR_EXPRESSION "SKIP:"
      TIMEOUT 90)
  endif()

  # v0.54.30: idle_ms 归因诚实性 e2e —— 静默 65s 后关闭，[ws-close] 的 idle_ms 必须 >=60000
  # （旧实现被 30s 心跳节拍刷新 ⇒ 上限 ≈30000，诊断字段撒谎；实测修前 65s 静默被报成 10011ms）
  find_program(PYTHON3_BIN_WSIDLE python3)
  if(PYTHON3_BIN_WSIDLE)
    add_test(NAME e2e_ws_idle_attribution
      COMMAND ${PYTHON3_BIN_WSIDLE} ${CMAKE_SOURCE_DIR}/tests/e2e/test_ws_idle_attribution.py)
    set_tests_properties(e2e_ws_idle_attribution PROPERTIES
      SKIP_REGULAR_EXPRESSION "SKIP:"
      TIMEOUT 150)
  endif()

  # v0.54.21: 存活机制真链路 e2e —— 30s 心跳节拍 + 静默活连接不被误踢（实测 ~40s，含 35s 静默期）
  find_program(PYTHON3_BIN_WSHB python3)
  if(PYTHON3_BIN_WSHB)
    # v0.54.24: 日志行原子性（并发读者不得看到半行；真服务 + 真并发读）
  add_test(NAME e2e_log_line_atomicity
    COMMAND ${CMAKE_COMMAND} -E env python3
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/e2e/test_log_line_atomicity.py)
  set_tests_properties(e2e_log_line_atomicity PROPERTIES TIMEOUT 120)
  add_test(NAME e2e_ws_heartbeat_liveness
      COMMAND ${PYTHON3_BIN_WSHB} ${CMAKE_SOURCE_DIR}/tests/e2e/test_ws_heartbeat_liveness.py)
    set_tests_properties(e2e_ws_heartbeat_liveness PROPERTIES
      SKIP_REGULAR_EXPRESSION "SKIP:"
      TIMEOUT 150)
  endif()

  # v0.53.30: chat_chunk cmd_id 契约 + FC 流式回放 e2e（node；无 key 自动 SKIP:）
  find_program(NODE_BIN_E2E node)
  if(NODE_BIN_E2E)
    add_test(NAME e2e_stream_cmdid
      COMMAND ${NODE_BIN_E2E} ${CMAKE_SOURCE_DIR}/tests/e2e/test_stream_cmdid.js)
    set_tests_properties(e2e_stream_cmdid PROPERTIES
      SKIP_REGULAR_EXPRESSION "SKIP:"
      TIMEOUT 300)  # v0.53.71: 150→300——GLM 慢日流式全回复实测>150s,3/5 过=环境慢非回归(OO1 id 化若破坏链路应稳定死)
  endif()

  # v0.53.35: /metrics Prometheus 导出 e2e（格式律+鉴权矩阵）
  find_program(PYTHON3_BIN_METRICS python3)
  if(PYTHON3_BIN_METRICS)
    add_test(NAME e2e_metrics
      COMMAND ${PYTHON3_BIN_METRICS} ${CMAKE_SOURCE_DIR}/tests/e2e/metrics_e2e.py)
    set_tests_properties(e2e_metrics PROPERTIES TIMEOUT 90)
  endif()

  # v0.53.34: lmstudio provider 端到端（本机 mock 1234 → provider → ws 回复）
  find_program(PYTHON3_BIN_E2E python3)
  if(PYTHON3_BIN_E2E)
    add_test(NAME e2e_lmstudio
      COMMAND ${PYTHON3_BIN_E2E} ${CMAKE_SOURCE_DIR}/tests/e2e/lmstudio_e2e.py)
    set_tests_properties(e2e_lmstudio PROPERTIES TIMEOUT 150 RUN_SERIAL TRUE)  # 独占 1234 mock 口
  endif()

  # v0.40.3: execute_code 功能验证（通过 shell 间接测试）
  add_test(NAME unit_execute_code
    COMMAND python3 -c "import subprocess,json; r=subprocess.run(['python3','-c','print(42)'],capture_output=True,text=True,timeout=3); assert '42' in r.stdout, 'smoke'; print('PASS: execute_code smoke')"
  )

  if(THIN_AGENT_WITH_FDBUS)
    add_executable(test_fdbus_stub_unit
      tests/unit/test_fdbus_stub.cpp
    )
    target_link_libraries(test_fdbus_stub_unit PRIVATE thin_agent_core)
    add_test(NAME unit_fdbus_stub COMMAND test_fdbus_stub_unit)
  endif()

  add_executable(test_demo_capture_flow
    tests/demo/test_demo_capture_flow.cpp
  )
  target_link_libraries(test_demo_capture_flow PRIVATE thin_agent_core)
  add_test(NAME demo_capture_flow COMMAND test_demo_capture_flow)

  # v0.47.0: 多 worker 并发队列安全测试
  add_executable(test_chat_workers_concurrency_unit
    tests/unit/test_chat_workers_concurrency.cpp
  )
  target_link_libraries(test_chat_workers_concurrency_unit PRIVATE
    thin_agent_core pthread)
  add_test(NAME unit_chat_workers_concurrency
    COMMAND test_chat_workers_concurrency_unit)

  # v0.47.1: token 用量解析测试
  add_executable(test_usage_tracking_unit
    tests/unit/test_usage_tracking.cpp
  )
  target_link_libraries(test_usage_tracking_unit PRIVATE thin_agent_core)
  add_test(NAME unit_usage_tracking
    COMMAND test_usage_tracking_unit)

  # v0.54.31: FilesystemCheckpoint 并发 save + manifest 原子性（跨进程读者不得读到半截 manifest）
  add_executable(test_fs_checkpoint_concurrency_unit
    tests/unit/test_fs_checkpoint_concurrency.cpp
  )
  target_link_libraries(test_fs_checkpoint_concurrency_unit PRIVATE thin_agent_core)
  add_test(NAME unit_fs_checkpoint_concurrency
    COMMAND $<TARGET_FILE:test_fs_checkpoint_concurrency_unit>)

  # v0.54.27: RotatingLogBuf 行原子性不变量（改动过两次的组件，此前无单测）
  add_executable(test_rotating_log_atomicity_unit
    tests/unit/test_rotating_log_atomicity.cpp
  )
  target_link_libraries(test_rotating_log_atomicity_unit PRIVATE thin_agent_core)
  add_test(NAME unit_rotating_log_atomicity
    COMMAND $<TARGET_FILE:test_rotating_log_atomicity_unit>)

  # v0.54.22: 后台进程 stdin 真链路（写入字节必须真的到达子进程；失败必须如实返回 false）
  add_executable(test_bg_stdin_unit
    tests/unit/test_bg_stdin.cpp
  )
  target_link_libraries(test_bg_stdin_unit PRIVATE thin_agent_core)
  add_test(NAME unit_bg_stdin
    COMMAND $<TARGET_FILE:test_bg_stdin_unit>)

  # v0.54.21: 原 unit_ws_heartbeat 已删除 —— 它是**自欺单测**：在自己文件里复刻
  # kDeadConnectionMs/kHeartbeatIntervalMs 常量并用 mock 数据**重实现**算法，从不调用产品代码，
  # 因此产品里那段"180s 收割"不可达（死防线）时它照样绿。其声称覆盖的活性语义改由真实链路验证：
  #   · e2e_ws_heartbeat_liveness（30s 心跳节拍 + 静默活连接不被误踢）
  #   · e2e_ws_close_attribution（FIN/RST 关闭归因签名表）

  # v0.47.3: 流式中断标志逻辑测试
  add_executable(test_chat_abort_unit
    tests/unit/test_chat_abort.cpp
  )
  add_test(NAME unit_chat_abort
    COMMAND test_chat_abort_unit)

  # v0.47.5: LLM 响应缓存 LRU+TTL 测试
  add_executable(test_response_cache_unit
    tests/unit/test_response_cache.cpp
  )
  target_link_libraries(test_response_cache_unit PRIVATE thin_agent_core)
  add_test(NAME unit_response_cache
    COMMAND test_response_cache_unit)

  # v0.50.7: LogEvent 结构化日志测试
  add_executable(test_log_event_unit
    tests/unit/test_log_event.cpp
  )
  target_link_libraries(test_log_event_unit PRIVATE thin_agent_core)
  add_test(NAME unit_log_event
    COMMAND test_log_event_unit)

  # v0.50.8: /stats 运维汇总统计测试
  add_executable(test_ops_stats_unit
    tests/unit/test_ops_stats.cpp
  )
  target_link_libraries(test_ops_stats_unit PRIVATE thin_agent_core)
  add_test(NAME unit_ops_stats
    COMMAND test_ops_stats_unit)

  # v0.51.2: fast 通道 HITL 审批回归（mock LLM server 驱动真实 FC 路径）
  add_executable(test_hitl_fast_path_unit
    tests/unit/test_hitl_fast_path.cpp
  )
  target_link_libraries(test_hitl_fast_path_unit PRIVATE thin_agent_core)
  add_test(NAME unit_hitl_fast_path
    COMMAND test_hitl_fast_path_unit)

  # v0.51.3: cron 硬校验 + 孤儿审批清理回归
  add_executable(test_cron_guard_unit
    tests/unit/test_cron_guard.cpp
  )
  target_link_libraries(test_cron_guard_unit PRIVATE thin_agent_core)
  add_test(NAME unit_cron_guard
    COMMAND test_cron_guard_unit)

  # v0.51.4: 空回复守卫（401 错误体不得伪装成功）
  add_executable(test_empty_reply_guard_unit
    tests/unit/test_empty_reply_guard.cpp
  )
  target_link_libraries(test_empty_reply_guard_unit PRIVATE thin_agent_core)
  add_test(NAME unit_empty_reply_guard
    COMMAND test_empty_reply_guard_unit)

  # v0.53.76: 流式 usage 尾帧(choices 缺失)断言崩防御
  add_executable(test_sse_usage_tail_unit
    tests/unit/test_sse_usage_tail.cpp)
  target_link_libraries(test_sse_usage_tail_unit PRIVATE thin_agent_core)
  add_test(NAME unit_sse_usage_tail COMMAND test_sse_usage_tail_unit)

  # v0.53.81: 记忆裁剪零拷贝快路径等价性
  add_executable(test_memory_trim_unit tests/unit/test_memory_trim.cpp)
  target_link_libraries(test_memory_trim_unit PRIVATE thin_agent_core)
  add_test(NAME unit_memory_trim COMMAND test_memory_trim_unit)

  # v0.52.0: shell_exec 参数级风险分级
  add_executable(test_shell_risk_grading_unit
    tests/unit/test_shell_risk_grading.cpp
  )
  target_link_libraries(test_shell_risk_grading_unit PRIVATE thin_agent_core)
  add_test(NAME unit_shell_risk_grading
    COMMAND test_shell_risk_grading_unit)

  # v0.52.1: cron 无人值守 HITL 语义
  add_executable(test_cron_unattended_hitl_unit
    tests/unit/test_cron_unattended_hitl.cpp
  )
  target_link_libraries(test_cron_unattended_hitl_unit PRIVATE thin_agent_core)
  add_test(NAME unit_cron_unattended_hitl
    COMMAND test_cron_unattended_hitl_unit)

  # v0.52.1: 工具参数宽容取值
  add_executable(test_json_coerce_unit
    tests/unit/test_json_coerce.cpp
  )
  target_link_libraries(test_json_coerce_unit PRIVATE thin_agent_core)
  add_test(NAME unit_json_coerce
    COMMAND test_json_coerce_unit)

  # v0.52.2: 方案 C — 批准后续跑 FC 循环
  add_executable(test_fc_resume_after_approval_unit
    tests/unit/test_fc_resume_after_approval.cpp
  )
  target_link_libraries(test_fc_resume_after_approval_unit PRIVATE thin_agent_core)
  add_test(NAME unit_fc_resume_after_approval
    COMMAND test_fc_resume_after_approval_unit)

  # v0.52.3: 任务树 + complex_task 意图
  add_executable(test_goal_tree_complex_task_unit
    tests/unit/test_goal_tree_complex_task.cpp
  )
  target_link_libraries(test_goal_tree_complex_task_unit PRIVATE thin_agent_core)
  add_test(NAME unit_goal_tree_complex_task
    COMMAND test_goal_tree_complex_task_unit)

  # v0.52.3: LLM 输出 JSON 提取
  add_executable(test_json_extract_unit
    tests/unit/test_json_extract.cpp
  )
  target_link_libraries(test_json_extract_unit PRIVATE thin_agent_core)
  add_test(NAME unit_json_extract
    COMMAND test_json_extract_unit)

  # v0.52.3: 分解→任务树 mock 链路
  add_executable(test_decompose_goal_tree_unit
    tests/unit/test_decompose_goal_tree.cpp
  )
  target_link_libraries(test_decompose_goal_tree_unit PRIVATE thin_agent_core)
  add_test(NAME unit_decompose_goal_tree
    COMMAND test_decompose_goal_tree_unit)

  # v0.52.4: 分解断点续跑
  add_executable(test_decompose_resume_unit
    tests/unit/test_decompose_resume.cpp
  )
  target_link_libraries(test_decompose_resume_unit PRIVATE thin_agent_core)
  add_test(NAME unit_decompose_resume
    COMMAND test_decompose_resume_unit)

  # v0.52.4 L3: 验证 gate
  add_executable(test_l3_gate_unit
    tests/unit/test_l3_gate.cpp
  )
  target_link_libraries(test_l3_gate_unit PRIVATE thin_agent_core)
  add_test(NAME unit_l3_gate
    COMMAND test_l3_gate_unit)

  # v0.52.29: Hook System
  add_executable(test_agent_loop_hook_unit
    tests/unit/test_agent_loop_hook.cpp
  )
  target_link_libraries(test_agent_loop_hook_unit PRIVATE thin_agent_core)
  add_test(NAME unit_agent_loop_hook COMMAND test_agent_loop_hook_unit)

  add_executable(test_hook_system_unit tests/unit/test_hook_system.cpp)
  target_include_directories(test_hook_system_unit PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include)
  target_link_libraries(test_hook_system_unit PRIVATE thin_agent_core)
  add_test(NAME unit_hook_system COMMAND test_hook_system_unit)

  # v0.53.0: 插件 v2 机制（init2+PluginContext 注入）
  add_executable(test_plugin_v2_unit tests/unit/test_plugin_v2.cpp)
  target_include_directories(test_plugin_v2_unit PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include)
  target_link_libraries(test_plugin_v2_unit PRIVATE thin_agent_core ${CMAKE_DL_LIBS})
  target_link_options(test_plugin_v2_unit PRIVATE -rdynamic)  # v2 插件符号解析（同 thin_agent 主程序）
  add_test(NAME unit_plugin_v2 COMMAND test_plugin_v2_unit)

  # v0.52.28: 网关会话映射持久化
  add_executable(test_gateway_session_map_unit tests/unit/test_gateway_session_map.cpp)
  target_include_directories(test_gateway_session_map_unit PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include)
  target_link_libraries(test_gateway_session_map_unit PRIVATE sqlite3)
  add_test(NAME unit_gateway_session_map COMMAND test_gateway_session_map_unit)

  # v0.52.26: FC 运行日志（断点续跑）
  add_executable(test_fc_run_store_unit tests/unit/test_fc_run_store.cpp)
  target_include_directories(test_fc_run_store_unit PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include)
  target_link_libraries(test_fc_run_store_unit PRIVATE thin_agent_core)
  add_test(NAME unit_fc_run_store COMMAND test_fc_run_store_unit)

  # v0.52.25: TokenBudget 窗口动态化
  add_executable(test_context_window_unit tests/unit/test_context_window.cpp)
  target_link_libraries(test_context_window_unit PRIVATE ${CORE_LIBS})
  add_test(NAME unit_context_window COMMAND test_context_window_unit)

  # v0.52.24: 注册表双向同步
  add_executable(test_registry_bidirectional_unit
    tests/unit/test_registry_bidirectional.cpp)
  target_link_libraries(test_registry_bidirectional_unit PRIVATE thin_agent_core)
  add_test(NAME unit_registry_bidirectional COMMAND test_registry_bidirectional_unit)

  # v0.52.23: 协作式取消
  add_executable(test_tool_cancel_cooperative_unit
    tests/unit/test_tool_cancel_cooperative.cpp)
  target_link_libraries(test_tool_cancel_cooperative_unit PRIVATE thin_agent_core)
  add_test(NAME unit_tool_cancel_cooperative COMMAND test_tool_cancel_cooperative_unit)

  # v0.52.22: 主动限流
  add_executable(test_llm_rate_limiter_unit
    tests/unit/test_llm_rate_limiter.cpp)
  target_link_libraries(test_llm_rate_limiter_unit PRIVATE thin_agent_core)
  add_test(NAME unit_llm_rate_limiter COMMAND test_llm_rate_limiter_unit)

  # v0.53.36: 限流并发公平性(cv 排队 vs 旧惊群;4s 预算防慢机误报)
  add_executable(test_rate_limiter_concurrency_unit
    tests/unit/test_rate_limiter_concurrency.cpp)
  target_link_libraries(test_rate_limiter_concurrency_unit PRIVATE thin_agent_core)
  add_test(NAME unit_rate_limiter_concurrency COMMAND test_rate_limiter_concurrency_unit)
  set_tests_properties(unit_rate_limiter_concurrency PROPERTIES RUN_SERIAL ON)

  # v0.52.20b: 嵌套崩溃复现
  add_executable(test_spawn_nest_crash_unit
    tests/unit/test_spawn_nest_crash.cpp)
  target_link_libraries(test_spawn_nest_crash_unit PRIVATE thin_agent_core)
  add_test(NAME unit_spawn_nest_crash COMMAND test_spawn_nest_crash_unit)

  # v0.52.20: spawn 嵌套深度护栏
  add_executable(test_spawn_depth_guard_unit
    tests/unit/test_spawn_depth_guard.cpp)
  target_link_libraries(test_spawn_depth_guard_unit PRIVATE thin_agent_core)
  add_test(NAME unit_spawn_depth_guard COMMAND test_spawn_depth_guard_unit)

  # v0.52.19: 限流语义识别
  add_executable(test_rate_limit_semantic_unit
    tests/unit/test_rate_limit_semantic.cpp)
  target_link_libraries(test_rate_limit_semantic_unit PRIVATE thin_agent_core)
  add_test(NAME unit_rate_limit_semantic COMMAND test_rate_limit_semantic_unit)

  # v0.52.19: 动态桥接
  add_executable(test_dynamic_bridge_unit
    tests/unit/test_dynamic_bridge.cpp)
  target_link_libraries(test_dynamic_bridge_unit PRIVATE thin_agent_core)
  add_test(NAME unit_dynamic_bridge COMMAND test_dynamic_bridge_unit)

  # v0.52.17: GoalManager 并发压测
  add_executable(test_goal_sqlite_concurrency_unit
    tests/unit/test_goal_sqlite_concurrency.cpp)
  target_link_libraries(test_goal_sqlite_concurrency_unit PRIVATE thin_agent_core)
  add_test(NAME unit_goal_sqlite_concurrency COMMAND test_goal_sqlite_concurrency_unit)

  # v0.52.16: 内置工具注册表化+参数 coerce
  add_executable(test_builtin_tool_registry_unit
    tests/unit/test_builtin_tool_registry.cpp)
  target_link_libraries(test_builtin_tool_registry_unit PRIVATE thin_agent_core)
  add_test(NAME unit_builtin_tool_registry COMMAND test_builtin_tool_registry_unit)

  # v0.52.15: WS 发送刷新活动时间
  add_executable(test_ws_send_refresh_unit
    tests/unit/test_ws_send_refreshes_activity.cpp)
  target_link_libraries(test_ws_send_refresh_unit PRIVATE thin_agent_core)
  add_test(NAME unit_ws_send_refresh COMMAND test_ws_send_refresh_unit)

  # v0.52.14: Skill→Tool 桥接
  add_executable(test_skill_tool_bridge_unit
    tests/unit/test_skill_tool_bridge.cpp)
  target_link_libraries(test_skill_tool_bridge_unit PRIVATE thin_agent_core)
  add_test(NAME unit_skill_tool_bridge COMMAND test_skill_tool_bridge_unit)

  # v0.52.13: 残缺响应守卫
  add_executable(test_llm_malformed_guard_unit
    tests/unit/test_llm_malformed_guard.cpp)
  target_link_libraries(test_llm_malformed_guard_unit PRIVATE thin_agent_core)
  add_test(NAME unit_llm_malformed_guard COMMAND test_llm_malformed_guard_unit)

  # v0.52.12: 假绿终结
  add_executable(test_agent_degraded_unit
    tests/unit/test_agent_degraded_no_false_green.cpp)
  target_link_libraries(test_agent_degraded_unit PRIVATE thin_agent_core)
  add_test(NAME unit_agent_degraded COMMAND test_agent_degraded_unit)

  # v0.52.11: 工具超时语义
  add_executable(test_tool_registry_timeout_unit
    tests/unit/test_tool_registry_timeout.cpp)
  target_link_libraries(test_tool_registry_timeout_unit PRIVATE thin_agent_core)
  add_test(NAME unit_tool_registry_timeout COMMAND test_tool_registry_timeout_unit)

  # v0.52.10: code_exec 进程组收割
  add_executable(test_code_exec_pgroup_unit
    tests/unit/test_code_exec_pgroup.cpp)
  target_link_libraries(test_code_exec_pgroup_unit PRIVATE thin_agent_core)
  add_test(NAME unit_code_exec_pgroup COMMAND test_code_exec_pgroup_unit)

  # v0.52.6: LLM 熔断+jitter
  add_executable(test_llm_circuit_breaker_unit
    tests/unit/test_llm_circuit_breaker.cpp
  )
  target_link_libraries(test_llm_circuit_breaker_unit PRIVATE thin_agent_core)
  add_test(NAME unit_llm_circuit_breaker
    COMMAND test_llm_circuit_breaker_unit)


  # v0.52.7: 子系统审计修复（注入/分级/workflow 拦截）
  add_executable(test_plugin_audit_fixes_unit
    tests/unit/test_plugin_audit_fixes.cpp
  )
  target_link_libraries(test_plugin_audit_fixes_unit PRIVATE thin_agent_core)
  add_test(NAME unit_plugin_audit_fixes
    COMMAND test_plugin_audit_fixes_unit)
endif()

# --- 可执行文件 ---
# CLI：./thin_agent_demo [profile]
add_executable(thin_agent_demo
  src/demo/main.cpp
)
target_link_libraries(thin_agent_demo PRIVATE thin_agent_core)

# WS：./thin_agent --config config/demo.model.yaml --profile zai_main_demo
# 默认 ws://127.0.0.1:8765/ws
add_executable(thin_agent
  src/demo/ws_agent_main.cpp
)
target_link_libraries(thin_agent PRIVATE mongoose_vendor)
# --whole-archive: 确保静态库所有符号都进入动态符号表，供 dlopen 插件解析
target_link_libraries(thin_agent PRIVATE
  -Wl,--whole-archive thin_agent_core -Wl,--no-whole-archive)
# 导出符号供 dlopen 插件解析 (如 SkillRegistry::register_cpp_handler)
target_link_options(thin_agent PRIVATE -rdynamic)

# IM Gateway：出站长连接，连接本地 thin_agent WS（x86 网关部署；嵌入式设备通常不编）
if(THIN_AGENT_BUILD_IM_GATEWAY)
  # 独立可执行文件（向后兼容）
  add_executable(thin_agent_gw
    src/demo/im_gateway_main.cpp
  )
  target_include_directories(thin_agent_gw PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
  )
  target_link_libraries(thin_agent_gw PRIVATE mongoose_gw_vendor curl)

  # ── Gateway v2（多平台适配器架构）──
  add_executable(thin_agent_gw2
    src/demo/im_gateway_v2.cpp
    src/gateway/FeishuAdapter.cpp
    src/gateway/WechatAdapter.cpp
    src/gateway/TelegramAdapter.cpp
    src/gateway/DiscordAdapter.cpp
  )
  target_include_directories(thin_agent_gw2 PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
  )
  target_link_libraries(thin_agent_gw2 PRIVATE mongoose_gw_vendor curl)

  # ── MCP Server ──
  add_executable(thin_agent_mcp_server
    src/mcp/mcp_server_main.cpp
    src/agent/McpServer.cpp)
  target_include_directories(thin_agent_mcp_server PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/nlohmann)
  target_link_libraries(thin_agent_mcp_server PRIVATE thin_agent_core)

  # SHARED 库版本（THIN_GATEWAY_AS_LIB 跳过 main()）
  add_library(thin_agent_gateway_shared SHARED
    src/demo/im_gateway_main.cpp
  )
  target_compile_definitions(thin_agent_gateway_shared PRIVATE
    THIN_GATEWAY_AS_LIB
    THIN_AGENT_BUILDING_DLL
  )
  target_include_directories(thin_agent_gateway_shared PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
  )
  target_link_libraries(thin_agent_gateway_shared PRIVATE
    mongoose_gw_vendor curl
  )
  set_target_properties(thin_agent_gateway_shared PROPERTIES
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
    OUTPUT_NAME thin_agent_gateway
  )
endif()

# thin_agentd：守护进程（dlopen libthin_agent_core.so + 组播发现 + 信号处理）
add_executable(thin_agentd
  src/daemon/thin_agentd_main.cpp
)
target_include_directories(thin_agentd PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_link_libraries(thin_agentd PRIVATE dl)

# thin_agent_cli：前端 CLI（WS 客户端 + 组播发现 + REPL）
# 不依赖 thin_agent_core，仅用 mongoose + nlohmann/json
add_executable(thin_agent_cli
  src/cli/thin_agent_cli.cpp
  third_party/mongoose/mongoose.c
)
target_include_directories(thin_agent_cli PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/include
  ${CMAKE_CURRENT_SOURCE_DIR}/third_party/mongoose
)

# -*- 插件安装目录 -*-
# run_agent.sh 始终以 HOME=/root 启动 agent，因此插件必须安装到 /root/.thin_agent/plugins/
# 如果非 Hermes 环境直接开发，可将此行改为：set(PLUGIN_DEV_DIR "$ENV{HOME}/.thin_agent/plugins/dev")
# v0.54.34: 默认落到 build/plugins/dev（可写）；可用 -DPLUGIN_DEV_DIR= 覆盖到 ~/.thin_agent
if(NOT DEFINED PLUGIN_DEV_DIR)
  set(PLUGIN_DEV_DIR "${CMAKE_BINARY_DIR}/plugins/dev")
endif()

# --- 插件：编译为独立 .so，运行时由 PluginLoader dlopen 加载 ---
# skill_code_dev：编程技能插件（code_read_file / code_patch / code_search）
# 仅依赖头文件，不链接 thin_agent_core（符号在运行时从主二进制解析）
add_library(skill_code_dev SHARED
  src/plugin/skills/code_dev.cpp
)
target_include_directories(skill_code_dev PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_compile_definitions(skill_code_dev PRIVATE THIN_AGENT_EMBEDDED_CAM=0 THIN_AGENT_WITH_FDBUS=0)
install(TARGETS skill_code_dev
  LIBRARY DESTINATION ${CMAKE_INSTALL_PREFIX}/plugins/dev
)
add_custom_command(TARGET skill_code_dev POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E make_directory "${PLUGIN_DEV_DIR}"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "$<TARGET_FILE:skill_code_dev>" "${PLUGIN_DEV_DIR}/"
  COMMENT "Copying skill_code_dev.so to ${PLUGIN_DEV_DIR}/"
)

# v0.53.0: 核心瘦身插件（cam_media/collab/kanban/monitor/state/cron）
# 从 AgentService 迁出的场景 handler；类随迁或经 PluginContext 注入。
foreach(_plug cam_media collab kanban monitor state cron meta patrol collab_msg)
  add_library(skill_${_plug} SHARED src/plugin/skills/${_plug}.cpp)
  target_include_directories(skill_${_plug} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
  target_compile_definitions(skill_${_plug} PRIVATE THIN_AGENT_EMBEDDED_CAM=0 THIN_AGENT_WITH_FDBUS=0)
  install(TARGETS skill_${_plug} LIBRARY DESTINATION ${CMAKE_INSTALL_PREFIX}/plugins/common)
  add_custom_command(TARGET skill_${_plug} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/plugins/common"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:skill_${_plug}>" "${CMAKE_BINARY_DIR}/plugins/common/"
    COMMENT "Copying skill_${_plug}.so")
endforeach()

# skill_workflow：工作流管理插件（workflow_list / view / create / set / delete）
add_library(skill_workflow SHARED
  src/plugin/skills/workflow.cpp
)
target_include_directories(skill_workflow PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_compile_definitions(skill_workflow PRIVATE THIN_AGENT_EMBEDDED_CAM=0 THIN_AGENT_WITH_FDBUS=0)
install(TARGETS skill_workflow
  LIBRARY DESTINATION ${CMAKE_INSTALL_PREFIX}/plugins/dev
)
add_custom_command(TARGET skill_workflow POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E make_directory "${PLUGIN_DEV_DIR}"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "$<TARGET_FILE:skill_workflow>" "${PLUGIN_DEV_DIR}/"
  COMMENT "Copying skill_workflow.so to ${PLUGIN_DEV_DIR}/"
)

# skill_git：Git 操作插件（git_status / git_diff / git_log）
add_library(skill_git SHARED
  src/plugin/skills/git_ops.cpp
)
target_include_directories(skill_git PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_compile_definitions(skill_git PRIVATE THIN_AGENT_EMBEDDED_CAM=0 THIN_AGENT_WITH_FDBUS=0)
install(TARGETS skill_git
  LIBRARY DESTINATION ${CMAKE_INSTALL_PREFIX}/plugins/dev
)
add_custom_command(TARGET skill_git POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E make_directory "${PLUGIN_DEV_DIR}"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "$<TARGET_FILE:skill_git>" "${PLUGIN_DEV_DIR}/"
  COMMENT "Copying skill_git.so to ${PLUGIN_DEV_DIR}/"
)

# skill_kb：知识库管理插件（kb_index / kb_status / kb_add）
# 需要 sqlite3 读写 codebase.db；WAL 模式与核心 KbSearcher 并发
add_library(skill_kb SHARED
  src/plugin/skills/kb.cpp
)
target_include_directories(skill_kb PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_link_libraries(skill_kb PRIVATE sqlite3)
target_compile_definitions(skill_kb PRIVATE THIN_AGENT_EMBEDDED_CAM=0 THIN_AGENT_WITH_FDBUS=0)
install(TARGETS skill_kb
  LIBRARY DESTINATION ${CMAKE_INSTALL_PREFIX}/plugins/dev
)
add_custom_command(TARGET skill_kb POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E make_directory "${PLUGIN_DEV_DIR}"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "$<TARGET_FILE:skill_kb>" "${PLUGIN_DEV_DIR}/"
  COMMENT "Copying skill_kb.so to ${PLUGIN_DEV_DIR}/"
)

# skill_loader：SKILL.md 适配器（Anthropic Agent Skills 规范，渐进披露）
add_library(skill_skill_loader SHARED
  src/plugin/skills/skill_loader.cpp
)
target_include_directories(skill_skill_loader PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_compile_definitions(skill_skill_loader PRIVATE THIN_AGENT_EMBEDDED_CAM=0 THIN_AGENT_WITH_FDBUS=0)
install(TARGETS skill_skill_loader
  LIBRARY DESTINATION ${CMAKE_INSTALL_PREFIX}/plugins/dev
)
add_custom_command(TARGET skill_skill_loader POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E make_directory "${PLUGIN_DEV_DIR}"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "$<TARGET_FILE:skill_skill_loader>" "${PLUGIN_DEV_DIR}/"
  COMMENT "Copying skill_skill_loader.so to ${PLUGIN_DEV_DIR}/"
)

# skill_code_exec：Python 代码执行插件（A1 纯计算模式，子进程隔离）
add_library(skill_code_exec SHARED
  src/plugin/skills/code_exec.cpp
)
target_include_directories(skill_code_exec PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_compile_definitions(skill_code_exec PRIVATE THIN_AGENT_EMBEDDED_CAM=0 THIN_AGENT_WITH_FDBUS=0)
install(TARGETS skill_code_exec
  LIBRARY DESTINATION ${CMAKE_INSTALL_PREFIX}/plugins/dev
)
add_custom_command(TARGET skill_code_exec POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E make_directory "${PLUGIN_DEV_DIR}"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "$<TARGET_FILE:skill_code_exec>" "${PLUGIN_DEV_DIR}/"
  COMMENT "Copying skill_code_exec.so to ${PLUGIN_DEV_DIR}/"
)

# --- 可选：llama.cpp 本地模型推理 ---
if(THIN_AGENT_WITH_LLAMA_CPP)
  # 用户需自行编译 llama.cpp 并设置 LLAMA_CPP_DIR
  if(DEFINED ENV{LLAMA_CPP_DIR})
    set(LLAMA_CPP_DIR "$ENV{LLAMA_CPP_DIR}")
  endif()
  if(NOT LLAMA_CPP_DIR)
    set(LLAMA_CPP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/llama.cpp")
  endif()

  add_library(llama_vendor STATIC IMPORTED)
  set_target_properties(llama_vendor PROPERTIES
    IMPORTED_LOCATION "${LLAMA_CPP_DIR}/build/src/libllama.a")

  add_library(ggml_vendor STATIC IMPORTED)
  set_target_properties(ggml_vendor PROPERTIES
    IMPORTED_LOCATION "${LLAMA_CPP_DIR}/build/ggml/src/libggml.a")
  add_library(ggml_base_vendor STATIC IMPORTED)
  set_target_properties(ggml_base_vendor PROPERTIES
    IMPORTED_LOCATION "${LLAMA_CPP_DIR}/build/ggml/src/libggml-base.a")
  add_library(ggml_cpu_vendor STATIC IMPORTED)
  set_target_properties(ggml_cpu_vendor PROPERTIES
    IMPORTED_LOCATION "${LLAMA_CPP_DIR}/build/ggml/src/libggml-cpu.a")

  target_include_directories(thin_agent_core PUBLIC
    "${LLAMA_CPP_DIR}/include"
    "${LLAMA_CPP_DIR}/ggml/include"
    "${LLAMA_CPP_DIR}/build/ggml/include")
  target_link_libraries(thin_agent_core PUBLIC
    llama_vendor ggml_vendor ggml_base_vendor ggml_cpu_vendor)
  target_compile_definitions(thin_agent_core PUBLIC THIN_AGENT_WITH_LLAMA_CPP)
  message(STATUS "llama.cpp: ${LLAMA_CPP_DIR}")
endif()
