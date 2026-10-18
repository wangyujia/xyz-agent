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

## Cursor Cloud
- 编译器必须是 GCC 13。镜像里 `/usr/bin/c++` 可能指向 clang++，链接会报找不到 `libstdc++`。`update-alternatives` 把 `cc`/`c++` 指到 `gcc`/`g++`。
- Go 测试模块声明 `go 1.26`，apt 的 Go 1.22 不够。用 `/usr/local/go`（1.27.2），`/usr/local/bin/go` 已优先于 `/usr/bin/go`。`test/source_go/run.sh` 默认 `GOTOOLCHAIN=local`。
- ONNX Runtime 1.18.1 的 `libonnxruntime.so` 不入库。放到 `third_party/onnxruntime/lib/`，并且必须是真实文件，不能是会在某些挂载上变成 0 字节的符号链接。校验和见仓库根目录 `THIRD_PARTY.md`。
- 知识库 HTTP 服务用 `/opt/xyz-dev/venv`，端口 8900，检索表是 `kb_fts`（`server.core.searcher.reindex_all`）。`scripts/reindex.py` 写的是另一张 `docs` 表。
- 测试硬编码了这些路径：`/root/code/thin_agent`、`/root/.thin_agent/config`、`/root/.hermes/node/bin/node`、`/usr/share/nodejs/ws`。云端环境用符号链接和 `node-ws` 满足它们。云端 e2e 需要的 `deepseek.env` / API key 不在仓库里；没有 key 时 `e2e_stream_cmdid` 会 SKIP。
- 不要和 `xyz-base` 的 `build/linux/host_start.sh` 同时占 `/tmp/xyz_mesh`。平台冒烟是 `build/linux/smoke_platform_gw.sh`。Go 跨语言用例要先有 `test/source/interop/xyz_ipc_peer`（`test/source/interop/build.sh`）。
