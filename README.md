# xyz-agent

单仓聚合工程：**后端（thin_agent + knowledge_base）+ 前端（agent_tool + zing_agent）**。

## 目录结构

```
xyz-agent/
├── backends/
│   ├── thin_agent/          # Agent 后端（WS 服务 / CLI / 插件）；C++17 + CMake
│   └── knowledge_base/      # 知识库（Markdown Wiki + SQLite FTS5 + HTTP API + MCP）
└── frontends/
    ├── agent_tool/          # 前端工具（webview；web/ws_agent.html 为思考流副本）
    └── zing_agent/          # 前端（webview；web/chat.html）
```

## 版本锚点

| 组件 | 版本 / 指针 | 说明 |
|---|---|---|
| backends/thin_agent | `v0.54.34`（`include/thin_agent/Version.h`） | 与源仓 `github.com/wangyujia/thin_agent` 提交 `d547582` 一致 |
| frontends | 源仓提交 `70641a7` | 与 `thin_agent_frontends` 一致 |
| backends/knowledge_base | 无版本号（源侧非 git 仓） | 以 markdown 正文为准 |

---

## 从零开始（换一台机器请按 0 → 4 顺序做）

### 0. 系统依赖

| 依赖 | 要求 | 说明 |
|---|---|---|
| CMake | **≥ 3.16** | `cmake_minimum_required(VERSION 3.16)` |
| C++ 编译器 | **C++17**（GCC 13 实测） | |
| OpenSSL 开发包 | 默认需要 | 仅当 `-DTHIN_AGENT_BUILD_IM_GATEWAY=ON`（**默认 ON**，飞书 wss 长连接）时 `find_package(OpenSSL REQUIRED)`；不需要可 `-DTHIN_AGENT_BUILD_IM_GATEWAY=OFF` |
| python3 | 测试/e2e 用 | e2e 另需 `pytest`、`websocket-client`（仓内暂无 requirements 文件，见 TODO.md） |
| mingw-w64（**可选**） | `x86_64-w64-mingw32-g++` | 仅用于 Windows 交叉编译判官 `static_win32_cross_compile`；**缺则打印 `SKIP:` 并标记 Skipped**，不算失败 |
| fdbus / leaptic 头文件（**可选**） | 自备路径 | 仅设备版（`build.sh cam`）需要，见步骤 2 |

> ⚠ 若源码放在 **Windows 盘（`/mnt/*`，DrvFs/9p）**：没有执行位（`*.sh` 请用 `bash xxx.sh`）、大小写不敏感、I/O 慢、**符号链接会退化为 0 字节**。**建议把构建产物放到 Linux 侧 ext4 做 out-of-tree 构建**。

### 1. 先补齐"不入库的依赖"（**必须在构建之前**）

本仓只提交源码/文档/脚本/测试/配置/小资源。**克隆后 `third_party/onnxruntime/lib/` 里没有任何 `.so`（该目录在忽略列表内，克隆后可能不存在），直接构建会在链接阶段失败。**

```bash
# 最小必需：ONNX Runtime 预编译库（x86_64）
# 完整步骤（含 URL、sha256 校验、DrvFs 上必须物化符号链接的坑）见 THIRD_PARTY.md §2.2
```

按需补齐（详见 **[THIRD_PARTY.md](THIRD_PARTY.md)**）：

| 需要什么 | 何时补 |
|---|---|
| ONNX Runtime `libonnxruntime.so`（x86_64） | **链接 `thin_agent` 必需 ⇒ 构建前必做**（§2.2） |
| ONNX Runtime aarch64 | 仅 `build.sh cam`（会自动下载，§2.3） |
| 意图模型 `models/intent/onnx/model.onnx` | 需要 ONNX 意图推理时（也可用本仓脚本再生成，§2.4） |
| GGUF 权重 ×3 | 需要本地/离线推理时（云模式不需要，§2.5） |
| llama.cpp 源码树 | 仅 `-DTHIN_AGENT_WITH_LLAMA_CPP=ON`（**默认 OFF**，§2.1） |
| `dist/aarch64`、KB 索引、凭据 | 见 §2.6 / §2.7 / §3 |

### 2. 构建

两个入口任选（都推荐把产物放 ext4）：

```bash
# ① 工程自带统一入口
cd backends/thin_agent
bash build.sh                 # 默认 pc（本机 x86，FakeDevice）
bash build.sh pc --clean      # 清理重建
bash build.sh cam             # 设备 aarch64（默认 FDBus ON）
bash build.sh cam --no-fdbus  # 设备版但不链 FDBus
bash build.sh cam --fdbus-root /path/to/fdbus --leaptic-include /path/to/leaptic_app/include

# ② 或直接 CMake（out-of-tree，产物落 Linux 侧）
cmake -S backends/thin_agent -B ~/build_xyz -DCMAKE_BUILD_TYPE=Release
cmake --build ~/build_xyz -j$(nproc)
```

> `bash build.sh cam` 需要**自备** fdbus 源码根目录与 leaptic_app 头文件目录（用上面两个参数指定），本仓不包含它们。

常用 CMake 开关：`THIN_AGENT_WITH_FDBUS`(OFF) / `THIN_AGENT_BUILD_TESTS`(ON) / `THIN_AGENT_BUILD_IM_GATEWAY`(ON) / `THIN_AGENT_EMBEDDED_CAM`(OFF) / `THIN_AGENT_WITH_LLAMA_CPP`(OFF)。

### 3. 自测

```bash
cd ~/build_xyz && ctest --timeout 150 -j4
```

> **铁律**：跑 ctest 前必须**全 target 重链**（`cmake --build <dir>` 不带 `--target`）。只构建单个 target 会让 ctest 跑旧二进制"骗绿"。
> 判官：`static_win32_cross_compile`（无 mingw 时 SKIP）、`static_test_registration`。

### 4. 运行

```bash
cd /path/to/xyz-agent                                             # 下面用相对路径，先切到仓根
export THIN_AGENT_HOME=/path/to/xyz-agent/backends/thin_agent_home   # 见下文「运行时 home」
~/build_xyz/thin_agent --config backends/thin_agent/config/demo.model.yaml --profile offline_demo
```

其余入口（CLI、端云协同、冒烟）见 **`backends/thin_agent/README.md`** 的「快速上手（15 分钟）」。

### 5. 各子工程文档（导航）

| 子工程 | 文档 | 用途 |
|---|---|---|
| thin_agent | `backends/thin_agent/README.md` | 快速上手、构建、端侧/端云运行、冒烟 |
| thin_agent | `backends/thin_agent/scripts/README.md` | 脚本与校验清单 |
| knowledge_base | `backends/knowledge_base/README.md` | 架构、HTTP API / MCP 接入、token 写接口、本地开发 |
| knowledge_base | `backends/knowledge_base/DEPLOY.md`、`ARCHITECTURE.md` | 部署与架构 |
| zing_agent | `frontends/zing_agent/README.md`、`BUILD-WINDOWS.md` | 前端构建（Windows 指南含依赖/产物/连接 WSL 服务/环境变量） |
| agent_tool | `frontends/agent_tool/README.md` | 前端工具 |

> **Linux 侧前端构建依赖目前未在仓内文档化**（现有指南只覆盖 Windows）——已记入 TODO.md。

---

## 提交范围（本仓的规则）

**入库**：源码、文档、脚本、测试、配置、小资源（如 `models/intent/*.json|onnx|txt`、`tokenizer.json`）、两个前端的 `web/` 页面。

**不入库**（见 `.gitignore`）：

- 构建产物：`build*/`、`*/build-linux/`、`Testing/`、`CMakeFiles/`
- Python 环境与缓存：`.venv/`、`__pycache__/`、`.pytest_cache/`
- 大件 / 外部获取物：`models/gguf/`（1.6G 权重）、`models/intent/onnx/model.onnx`、`third_party/onnxruntime*/lib/`（预编译 .so）、`third_party/llama.cpp/`（220M）、`dist/`
- 运行期产物：`logs/`、`*.log`、`.server.pid`、`.auto_progress_state.json`、`checkpoints/`
- **凭据**：`.env*`、`access_tokens.yaml`（需单独投放，`chmod 600`）
- 知识库派生索引：`kb.db`、`.search.db`（正文在 markdown）

> 说明：`backends/thin_agent/tests/` 在源仓的 `.gitignore` 里被忽略（源仓靠 `git add -f` 纳入）；本仓同样已强制纳入。

**不入库依赖的完整说明（路径/体积/何时需要/恢复步骤/sha256 校验）见 [THIRD_PARTY.md](THIRD_PARTY.md)。**

## 运行时 home（`THIN_AGENT_HOME`）

后端运行需要 home（`plugins/`、`config/`、`checkpoints/`、`logs/`、`kb/`）。建议放 `backends/thin_agent_home/`：

```bash
export THIN_AGENT_HOME=/path/to/xyz-agent/backends/thin_agent_home
```

- **未设置时**会回落到默认 home（用户目录下的 `.thin_agent`），可能与其它实例混用 ⇒ 建议显式设置。
- `plugins/` **需要由构建产物填充**（不是自动生成）：见下面的同步铁律。
- `config/`、`checkpoints/`、`logs/`、`kb/` 运行期自行创建。
- 凭据（如云通道 `deepseek.env`）单独投放并 `chmod 600`。

**三段同步铁律**（改 core 后必须）：①重编 core 双 target ②重编全部 `skill_*` 插件 ③`cp ~/build_xyz/libskill_*.so $THIN_AGENT_HOME/plugins/<mode>/`。
缺任一步，旧 `.so` 内联的旧逻辑会绕过新代码（表现为"改了没生效"）。

## 与源仓的关系（重要）

- 本仓由 `/root/code/thin_agent`、`/root/code/thin_agent_frontends`、`/root/code/knowledge_base` **拷贝**而来；
  **子目录的 `.git` 已移除**，本目录是**唯一** git 仓。
- 请**只在一侧修改**。若继续在源仓开发，需单向同步到本仓（`rsync` 排除本 `.gitignore` 所列内容），避免"双真源"漂移。
- `ws_agent.html` 有两份：`backends/thin_agent/ws_agent.html`（**工程版真源**）与 `frontends/agent_tool/web/ws_agent.html`（思考流副本）——改真源后必须同步副本。

## 已知差异（拷贝/聚合时产生，已处理或有说明）

1. `third_party/onnxruntime/lib/libonnxruntime.so` 与 `third_party/onnxruntime-aarch64/lib/libonnxruntime.so`：源侧是**符号链接**，Windows 文件系统上会退化为 0 字节文件 ⇒ 已**物化为真文件**（与源侧目标逐字节一致）。**若重新拷贝，务必重做这一步**，否则链接会失败。
2. 子目录 `.git`、`frontends/.gitmodules` 已移除（历史仍在各自源仓/远端）。
3. **本仓不再有子模块**：`frontends/agent_tool/third_party/webview` 已是**普通文件**（原先来自上游 submodule）⇒
   ① `frontends/zing_agent/BUILD-WINDOWS.md` 里"必须初始化 submodule"这一步**在本仓不再需要**（该文档待同步，见 TODO.md）；
   ② 因此也不要用 `git submodule update` 去"修复"它。

## 待办

见 **[TODO.md](TODO.md)**（下一步要做的事、已知缺口、验收标准）。
