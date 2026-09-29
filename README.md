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

## 提交范围（本仓的规则）

**入库**：源码、文档、脚本、测试、配置、小资源（如 `models/intent/*.json|onnx|txt`、`tokenizer.json`）、两个前端的 `web/` 页面。

**不入库**（见 `.gitignore`）：

- 构建产物：`build*/`、`*/build-linux/`、`Testing/`、`CMakeFiles/`
- Python 环境与缓存：`.venv/`、`__pycache__/`、`.pytest_cache/`
- 大件 / 外部获取物：`models/gguf/`（1.6G 权重）、`models/intent/onnx/model.onnx`、`third_party/onnxruntime*/lib/`（预编译 .so）、`dist/`
- 运行期产物：`logs/`、`*.log`、`.server.pid`、`.auto_progress_state.json`、`checkpoints/`
- **凭据**：`.env*`、`access_tokens.yaml`（需单独投放，`chmod 600`）
- 知识库派生索引：`kb.db`、`.search.db`（正文在 markdown）

> 说明：`backends/thin_agent/tests/` 在源仓的 `.gitignore` 里被忽略（源仓靠 `git add -f` 纳入）；本仓同样已强制纳入。

## 构建（推荐：源码在上面这份目录，构建产物放 Linux 侧 ext4）

> `/mnt/e` 是 Windows 盘（9p/DrvFs）：**没有 POSIX 权限位/执行位、大小写不敏感、I/O 慢**，不适合直接在里面构建；`*.sh` 请用 `bash xxx.sh` 调用。

```bash
# out-of-tree 构建（源码读本仓，产物落 Linux 侧）
cmake -S backends/thin_agent -B ~/build_xyz -DCMAKE_BUILD_TYPE=Release
cmake --build ~/build_xyz -j$(nproc)

# 测试（ctest 前必须全 target 重链，否则会跑旧二进制骗绿）
cd ~/build_xyz && ctest --timeout 150 -j4
```

## 运行时 home（`THIN_AGENT_HOME`）

后端运行需要 home（`plugins/`、`config/`、`checkpoints/`、`logs/`、`kb/`）。建议：

```bash
export THIN_AGENT_HOME=/path/to/xyz-agent/backends/thin_agent_home
```

**三段同步铁律**（改 core 后必须）：①重编 core 双 target ②重编全部 `skill_*` 插件 ③`cp ~/build_xyz/libskill_*.so $THIN_AGENT_HOME/plugins/<mode>/`。
缺任一步，旧 `.so` 内联的旧逻辑会绕过新代码（表现为"改了没生效"）。

## 与源仓的关系（重要）

- 本仓由 `/root/code/thin_agent`、`/root/code/thin_agent_frontends`、`/root/code/knowledge_base` **拷贝**而来；
  **子目录的 `.git` 已移除**，本目录是**唯一** git 仓。
- 请**只在一侧修改**。若继续在源仓开发，需单向同步到本仓（`rsync` 排除本 `.gitignore` 所列内容），避免"双真源"漂移。
- `ws_agent.html` 有两份：`backends/thin_agent/ws_agent.html`（**工程版真源**）与 `frontends/agent_tool/web/ws_agent.html`（思考流副本）——改真源后必须同步副本。

## 已知差异（拷贝时产生，已处理）

1. `third_party/onnxruntime/lib/libonnxruntime.so` 与 `third_party/onnxruntime-aarch64/lib/libonnxruntime.so`：源侧是**符号链接**，Windows 文件系统上会退化为 0 字节文件 ⇒ 已**物化为真文件**（与源侧目标逐字节一致）。**若重新拷贝，务必重做这一步**，否则链接会失败。
2. 子目录 `.git`、`frontends/.gitmodules` 已按需移除（历史仍在各自源仓/远端）。
