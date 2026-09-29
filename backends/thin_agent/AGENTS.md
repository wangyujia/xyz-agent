# thin_agent Project Context

## Build
- CMake + GCC 13, build with `cmake --build build -j$(nproc)`
- Tests: `cd build && ctest --timeout 150 -j4`（全量 **138 项**，实测 ~200s）。**真 MCP 链路（npx）自 v0.54.9 起移到 e2e 层且默认 SKIP**——它单次实测可达 470s+（原占全量 74%），需要时显式开：`TA_MCP_REAL=1 ctest -R e2e_mcp_real_chain --output-on-failure`。`--timeout 5` 会掐死慢测试——v0.53.15 实测 28 项误报红
- **ctest 前必须全 target 重链（v0.53.82 实锤）**：只编 `--target thin_agent`/`thin_agent_core` 时测试可执行不重链，ctest 会跑**旧代码**——本轮实测 build/ 下 106 个测试二进制**全部**早于 libthin_agent_core.a（test_agent_service_unit 落后 13 个版本），导致 v0.53.77 引入的 ping/action/task_* 自死锁（P0，服务永久冻结）静默通过"ctest 全绿"上线。判据：`ls -la --time-style=+%m-%d_%H:%M build/test_* | awk '$6 < core_mtime'`，或直接 `cmake --build build -j` 再 ctest。旧 .so/旧测试二进制骗绿与骗红同理（v0.53.72 同族）
- Debug build: `cmake -B build -DCMAKE_BUILD_TYPE=Debug`
- 双 target 陷阱：`thin_agent_core`（静态 .a）与 `thin_agent_core_shared`（.so）——改核心代码后两者都要重编，否则插件/外部测试链到旧实现（v0.53.10 实锤滞后 4 小时）；主程序编译用 `cmake --build . --target thin_agent`，插件用各自 `skill_*` target
- **三段同步铁律（v0.53.47 实锤行为面）**：改 core 后必须 ①重编 core 双 target ②重编全部 skill_* 插件 ③`cp build/libskill_*.so ~/.thin_agent/plugins/<mode>/`。缺任何一步，旧 .so 里内联/静态拷贝的旧逻辑会在静态单例上运行——表现为"core 修了但行为没变"（CronScheduler UPDATE 不生效实锤：旧 .so 的 ticker 拷贝绕过新逻辑，next_run_ts 永不推进）。排障时先 `ls -la` 对比 build/ 与 plugins/ 的 .so 时间戳

## Code Style
- C++17, Google style, 100-char line limit
- Commit messages in Chinese

## Architecture
- AgentService: main orchestrator, FC loop, system_prompt builder
- AgentLoop: sub-agent execution engine
- Plugin system: .so plugins loaded via dlopen, SkillRegistry dispatch
- Tool execution: code_dev.so, git.so, workflow.so

## Key Rules
- Never modify third_party/ files
- All new features must have corresponding ctest
- Version.h and CHANGELOG.md must be updated together
