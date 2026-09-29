# thin_agent

> 轻量嵌入式智能体（C++17），支持离线自治与端云协同。一次编写，多端运行。

[![Build](https://img.shields.io/badge/build-cmake-blue)](CMakeLists.txt)
[![Tests](https://img.shields.io/badge/tests-26%2F26-brightgreen)](.)
[![Version](https://img.shields.io/badge/version-v0.31.0-blue)](include/thin_agent/Version.h)
[![License](https://img.shields.io/badge/license-MIT-green)](.)

---

## 快速上手（15 分钟）

```bash
# 1. 构建
cd ~/code/thin_agent
cmake --build build -j$(nproc)
ctest --test-dir build --output-on-failure
# 期望: 100% tests passed

# 2. 纯端侧
~/.thin_agent/run_agent.sh --local --port 18765
python3 tests/demo/e2e_ws_v02.py --port 18765   # 期望: E2E_OK

# 3. 端云协同（需 GLM_API_KEY）
~/.thin_agent/run_agent.sh --main --port 18766
python3 tests/demo/e2e_cloud_chat.py --port 18766 --expect-mode cloud --expect-substr "[端云协同-本地裁决]"
# 期望: E2E_CLOUD_OK

# 3b. 开发模式（shell_exec 黑名单）
~/.thin_agent/run_agent.sh --dev --port 18767

# 4. 一键冒烟
bash scripts/demo_smoke_all.sh --quick
# 期望: ALL_PASS ✓
```

浏览器打开 `ws_agent.html`，连接 `ws://127.0.0.1:<port>/ws` 即可交互。

---

## 启动参数

| 参数 | 脚本 | 二进制 | 说明 |
|------|:----:|:------:|------|
| `--fast` / `--flash` | ✅ | — | 快通道模型（`zai_fast_demo`），意图分类 + 短回复 |
| `--main` / `--pro` | ✅ | — | 主力模型（`zai_main_demo`），深度推理 + Native Function Calling |
| `--local` | ✅ | — | 纯离线（`offline_local_demo`），规则引擎 + 本地模型 |
| `--cloud-only` | ✅ | — | 强制走云，失败不降级（与 `--fast`/`--main` 组合） |
| `--dev` | ✅ | — | 开发模式，`shell_exec` 黑名单 + 自动加载 `~/.thin_agent/plugins/dev/*.so` |
| `--host <H>` | ✅ | ✅ | 绑定地址（pc 默认 127.0.0.1，cam 默认 0.0.0.0） |
| `--port <P>` | ✅ | ✅ | 监听端口（默认 8765） |
| `--config <path>` | — | ✅ | 模型配置文件路径 |
| `--profile <name>` | — | ✅ | 配置文件中的 profile 名称 |
| `--help` / `-h` | ✅ | ✅ | 打印帮助信息 |
| `--auto` | ✅ | — | 路由策略：本地优先级联，未命中再上云（默认云侧用 fast，可与 `--main` 组合） |

> `--flash` / `--pro` 保留向下兼容，推荐使用 `--fast` / `--main`。

## 演示模式

| 模式 | 命令 | 说明 |
|---|---|---|
| 纯端侧 | `run_agent.sh --local` | 离线运行，规则引擎 + 本地模型，不调云 |
| 端云协同 Fast | `run_agent.sh --fast` | 快通道 glm-4.5-flash，意图分类 + 短回复 |
| 端云协同 Main | `run_agent.sh --main` | 主力 glm-5.2，Native Function Calling + streaming |
| 纯云不回退 | `run_agent.sh --main --cloud-only` | 强制走云，失败不降级 |
| 开发模式 | `run_agent.sh --dev [--fast\|--main]` | shell_exec 黑名单 + code_dev 插件 + developer 角色 |

### 插件系统（v0.23）

插件按目录组织，不同模式加载不同插件：

```
~/.thin_agent/plugins/
├── common/    ← 始终加载（通用插件，目前空）
└── dev/       ← --dev 模式加载
    └── libskill_code_dev.so   ← 编程技能插件
```

**开发模式插件 (`code_dev`)** 提供 3 个结构化 C++ handler：

| Handler | 功能 | 对比 shell 版 |
|---------|------|:---:|
| `code_read_file(path, offset, limit)` | 带行号、分页读取 | vs `cat` 无行号 |
| `code_patch(path, old, new)` | 精准字符串替换 | vs `sed` 转义地狱 |
| `code_search(pattern, dir, glob, limit)` | 结构化搜索 + 文件过滤 | vs `grep -rn` 无过滤 |

**角色系统** 内置 9 个角色。主 agent（名为 `default`）按模式自动切换：

| 模式 | default 角色 | 说明 |
|------|:----------:|------|
| 普通模式 | `worker` | 通用工作助手 |
| `--dev` | `developer` | 代码开发智能体 |
| `--embedded`（规划中） | `device` | 嵌入式设备智能体 |
| `--server`（规划中） | `server` | 服务器运维智能体 |

子 agent（`spawn_agent` 使用）角色：

| 角色 | 类型 | 工具 |
|------|------|------|
| `viewer` | 代码审查 | read_file, search_code |
| `tester` | 测试分析 | read_file, search_code, terminal |
| `debugger` | 崩溃分析 | read_file, search_code, terminal |
| `researcher` | 信息调研 | web_search, read_file |
| `summarizer` | 多 agent 汇总 | 无（纯推理） |

**添加新插件** 只需 3 步：
1. 写 `src/plugin/skills/xxx.cpp`，导出 `thin_agent_plugin_init()`
2. `CMakeLists.txt` 加 `add_library(skill_xxx SHARED ...)`
3. `chat_policy.json` `plugins.modes` 加目录映射

### 模式内部调用策略（v0.16）

thin_agent 采用五层决策漏斗，路由随模式变化：

```
第 0 层（0ms）：本地意图秒回 — 关键词命中本地 handler → 模板回复
第 1 层（0ms）：离线降级      — 无网络/缺 API key → 规则引擎兜底
第 2 层（0-300ms）：auto 先遣 — HybridRouter 级联（Template → Qwen → Gemma）
第 3 层（2-5s）：Fast 快通道   — 简单文件操作（list/read/write/search）→ Fast FC
第 4 层（5-8s）：Main 深度推理 — Native Function Calling + streaming

输入翻译（v0.16）：非简体中文/非英文用户输入自动翻译成英文后再做关键词匹配，
翻译优先走本地 Qwen（已预加载），不可用则 Fast API 兜底。
```

```
--local（纯离线）
  ├─ classify_local_intent → 关键词规则匹配
  ├─ HybridRouter（Template → Qwen 0.5B → Gemma 1B）级联
  └─ 全部失败 → 模板兜底

--main / --fast（云端）
  ├─ classify_local_intent → 关键词规则匹配
  ├─ Fast 快通道（简单文件操作）→ Fast FC 执行
  ├─ Tier 2 云 Fast/Main 意图分类
  │    └─ profile/status/event/general_query → 本地 handler 秒回
  └─ Tier 3 云 Main/Fast 深度推理（Native Function Calling + streaming）
```

### 云端调用链路细节

```
                     用户输入
                        ↓
              classify_local_intent()
              意图：general / weather / task / ...
                        ↓
    ┌───────────────────┼───────────────────┐
    ↓ (general)         ↓ (weather/news)    ↓ (task/action)
  Tier 2 Fast 分类     local handler       local handler
  "正在理解意图..."       直接执行            直接提交
    ↓
  命中本地 handler?
    ├─ profile/status → 模板秒回
    └─ general_query → Fast 短回复
    └─ 未命中 ↓
  Tier 3 Main 深度推理（Native Function Calling）
  "深度推理中..."
    ├─ tools schema 注入（从 SkillRegistry 动态生成）
    ├─ streaming + tool_choice: auto
    ├─ LLM 返回 tool_call → 本地立即执行
    │    ├─ read_file / write_file / list_dir / search_code\n    │    ├─ code_read_file / code_patch / code_search（--dev 插件）
    │    └─ 结果回喂 messages → LLM 继续
    └─ LLM 文本回复（最终答案）
         └─ 失败 → HybridRouter 级联 → 模板兜底
```

### 同题对比

| 输入 | --local | --main | --cloud-only |
|---|---|---|---|
| "你是谁" | local_profile | local_profile | local_profile |
| "状态" | local_status | cloud-strategy → local_status | cloud-strategy → local_status |
| "天气" → "上海" | external_weather(mock) | cloud-strategy | cloud-strategy |
| "帮我拍照" | local_task_inline | cloud-strategy → task | cloud-strategy → task |
| "看看当前目录" | local_llm (Qwen) | cloud → pipeline | cloud → pipeline |

---

## IM 网关（飞书）

```bash
# 终端 A：Agent Core
~/.thin_agent/run_agent.sh --main

# 终端 B：IM 网关（需 FEISHU_APP_ID/SECRET）
~/.thin_agent/run_gateway.sh
```

---

## 故障排查

### 端口被占用
```bash
ss -tlnp | grep 8765
~/.thin_agent/run_agent.sh --local --port 18770   # 换端口
```

### 云模式缺密钥

`run_agent.sh` 启动时会自动 `source ~/.thin_agent/zai.env` 加载 `GLM_API_KEY`。
**不要直接调二进制**，否则 key 不会被加载 → 云调用失败 → 回退离线模式，表现
为回复「我当前处于离线模式」。

```bash
# ✅ 正确：通过脚本启动（自动加载 zai.env）
~/.thin_agent/run_agent.sh --main

# ❌ 错误：直接调二进制会缺 GLM_API_KEY
./build/thin_agent --config ~/.thin_agent/demo.model.yaml --profile zai_main_demo

# 检查 key 是否已配置
cat ~/.thin_agent/zai.env   # 应含 GLM_API_KEY=sk-...
```

### 云调用超时/失败
```bash
curl -s -o /dev/null -w "%{http_code}" https://open.bigmodel.cn/api/coding/paas/v4/models
# 期望: 200。若失败，检查 VPN/代理
```

### 构建失败
```bash
rm -rf build && mkdir build && cd build && cmake .. && cmake --build . -j$(nproc)
```

### 单测失败
```bash
ctest --test-dir build --output-on-failure -V   # 看具体断言
```

### ws_agent.html 连不上
```bash
pgrep -a thin_agent           # 确认进程在跑
ss -tlnp | grep <port>         # 确认端口监听
curl -i http://127.0.0.1:<port>/ws   # 期望 101
```

### 清理残留进程
```bash
pkill -x thin_agent
pkill -x thin_agent_gw
```

---

## 验收清单（发版前）

### 构建与单测
- [ ] `cmake --build build -j$(nproc)` 成功
- [ ] `ctest --test-dir build --output-on-failure` 13/13 全绿

### 核心对话回归（`--local` 或 `--main`）

| 输入 | 期望 route | 期望 intent |
|---|---|---|
| `你是谁` | local_profile | profile |
| `详细能力介绍` | local_profile | profile (detailed) |
| `你是什么模型` | local_status | status |
| `查天气` | local_external_clarify | weather |
| `今天上海天气` | local_external_weather | weather |
| `看新闻` | local_external_clarify | news |
| `最近AI新闻` | local_external_news | news |
| `帮我拍一张照并告诉我结果` | local_task_inline | task_capture |
| `拍照` | local_action | action_capture_photo |
| `好的` | local_clarify | clarify (short_ack) |
| `消息能力` | local_clarify | clarify |

### 多轮对话
- [ ] `上海天气` → `深圳的呢`：第二次查深圳
- [ ] `查天气` → `上海`：槽位填充后执行
- [ ] `查天气` → `看新闻`：切换为 news 澄清
- [ ] 天气执行后 → `需要`：weather_advice

### 各模式验证
- [ ] `--local`：mode=offline，e2e_ws_v02.py → E2E_OK
- [ ] `--fast`：快通道模型，意图分类 + 短回复
- [ ] `--main`：mode=cloud，e2e_cloud_chat.py → E2E_CLOUD_OK
- [ ] `--auto`：本地优先级联路由，云端用 fast
- [ ] `--auto --main`：本地优先级联路由，云端用 main
- [ ] `--main --cloud-only`：云失败不回退 → cloud-error
- [ ] `--dev`：开发模式，shell_exec 黑名单 + 全功能

### 一键冒烟
- [ ] `bash scripts/demo_smoke_all.sh` → ALL_PASS ✓

### 前端
- [ ] ws_agent.html 连接正常，决策轨迹可读，证据行渲染正常

---

## 文档索引

| 文档 | 说明 |
|---|---|
| [ARCHITECTURE.md](docs/ARCHITECTURE.md) | 跨平台架构 + 嵌入式部署附录 |
| [PRD.md](docs/PRD.md) | 产品需求 + v0.8 功能规格附录 |
| [PROTOCOL.md](docs/PROTOCOL.md) | WS 协议与消息定义 |
| [PROJECT_STRUCTURE.md](docs/PROJECT_STRUCTURE.md) | 目录结构与模块边界 |
| [DB_SCHEMA.md](docs/DB_SCHEMA.md) | SQLite 表结构 |
| [STATE_MACHINE.md](docs/STATE_MACHINE.md) | 任务状态机 |
| [TESTING_STRATEGY.md](docs/TESTING_STRATEGY.md) | 测试策略与回归清单 |
| [COMMERCIAL-GAP-ASSESSMENT.md](docs/COMMERCIAL-GAP-ASSESSMENT.md) | 商用落地差距评估（R21 对照检视） |
| [DEPLOY-TLS.md](docs/DEPLOY-TLS.md) | TLS 终结部署指南（前置 Caddy/nginx，B 方案） |
| [CHAT_POLICY_CONFIG.md](docs/CHAT_POLICY_CONFIG.md) | chat_policy.json 配置指南 |
| [CHANGELOG.md](CHANGELOG.md) | 版本变更记录 |
