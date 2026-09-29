# TODO（下一步要做的事）

> 用途：把"聚合为 xyz-agent 单仓"之后**尚未完成**的工作固定下来，含背景、影响文件、建议做法、验收标准，便于换机器/换会话直接接手。
> 维护约定：完成一项就把该项移到文末「已完成」，并保留一句证据（提交号/实测命令）。

---

## P0（立即待办）

### T0. 推送未同步的提交

**现状**：远端 `github.com/wangyujia/xyz-agent` 的默认分支是 **`master`**（**不是 `main`**），tip = `8bf1525`；
本地 `HEAD` = `4756e9c`（README「从零开始」+ TODO.md）**尚未推送**。

```bash
# WSL 内不通 github，用 Windows 侧 git（宿主可通）：
powershell.exe -NoProfile -Command 'git -C "E:\code\xyz-agent" push origin master'
# 之后核对：git -C /mnt/e/code/xyz-agent log --oneline origin/master -1   # 应等于本地 HEAD
```

**约定**：本仓远端分支是 `master`；每次文档/代码改动后顺手推送，并在 TODO 记录提交号。

---

## P1（影响可用性，建议先做）

### T1. 消除"影子依赖"：仓库里写死的旧路径

**问题**：这些路径写死在**源仓 `/root/code/thin_agent`**，搬到别的位置后会**静默指回旧树**（测的是旧代码/加载旧模型），属于"骗绿/骗运行"隐患。

| 位置 | 现状 | 后果 |
|---|---|---|
| `backends/thin_agent/src/core/AgentServiceUtil.cpp:862` | 候选列表**最后一项**写死 `/root/code/thin_agent/models/intent/intent_stub.onnx` | 相对候选找不到时**静默加载旧树模型** |
| `backends/thin_agent/src/core/IntentOnnx.cpp:40` | 同上，`roots` 里写死 `/root/code/thin_agent/models/intent/` | 同上 |
| `tests/e2e/conftest.py:27`、`test_gateway_reconnect_backoff.py:15`、`test_checkpoint_resume_hooks.py:29` | `os.environ.get("THIN_AGENT_REPO", "/root/code/thin_agent")` | 未设 env 时 e2e **测的是旧树**（已可 env 覆盖，属"半安全"） |
| `tests/e2e/lmstudio_e2e.py:43` | 硬编码 `cwd=` | 同上 |
| `scripts/run_agent.sh:30`、`run_gateway.sh:25` | 候选仅 `$HOME/code/thin_agent` 与 `/root/code/thin_agent` | 在新位置**两个候选都匹配不上 ⇒ 脚本找不到工程** |
| `scripts/test.sh`（6 处） | 提示词文本 + 一处 `git checkout -- <写死路径>` 清理 | 搬迁后清理命令失效 |

**建议做法（通用方案，别再写死一个新路径）**：以 `THIN_AGENT_REPO` 环境变量 / **脚本自身所在目录**推导为唯一来源，删除写死兜底项。

**验收**：在源仓按铁律跑——**全 target 重链 → 同步 `libskill_*.so` → 全量 ctest 全绿 + 两个判官**（`static_win32_cross_compile`、`static_test_registration`），并且**在 xyz-agent 路径下跑 e2e 不再触碰 `/root/code/thin_agent`**（可用 `strace`/日志路径断言验证）；改完同步到本仓。
**注意**：源仓改动需**版本递增 + CHANGELOG**；commit 由用户执行。

### T2. 运行时 home 就位（否则"搬完起不来"）

**问题**：后端运行需要 home（`plugins/config/checkpoints/logs/kb`）。本仓目前**没有** home ⇒ 换机器照文档仍缺这一块。

**要做**：
1. 建 `backends/thin_agent_home/`（或用文档化的其它位置），并把 `.gitignore` 规则覆盖到位（`logs/`、`checkpoints/`、`plugins/*.so`、凭据**不得入库**）。
2. 凭据（云通道 `deepseek.env`、KB `access_tokens.yaml`）**单独投放 + `chmod 600`**。
3. 把「三段同步铁律」的 `plugins/<mode>/` 目标指到该 home，并在 README 写明**哪些内容要人工投放**（plugins 来自构建产物；config/checkpoints/logs/kb 运行期自建）。

**验收**：新机器按 README 0→4 步骤能起后端并完成一次真实 WS 往返（ping/心跳 + 一次对话），且 `plugins/` 里的 `.so` 时间戳与本次构建一致。

---

## P2（正确性/一致性/文档完整性）

### T3. `ws_agent.html` 单真源守卫

**现状**：存在两份——`backends/thin_agent/ws_agent.html`（**工程版真源**）与 `frontends/agent_tool/web/ws_agent.html`（思考流副本），靠人工同步（历史已发生过"改了没同步"）。
**要做**：加一个判官（形如 `scripts/check_*_consistency.py`，挂到 ctest）比对两份关键片段，不一致即红；或把副本改为构建期生成。
**验收**：故意改动副本 ⇒ 判官变红；恢复 ⇒ 变绿。

### T4. GGUF 权重上游来源登记

**现状**：`THIRD_PARTY.md §2.5` 只有文件名/字节数/sha256，**确切下载地址未在仓内登记**（CHANGELOG 只记了集成动作与体积），文档已如实标注"以校验值为准"。
**要做**：补上三个权重的上游（HF 仓库 + 量化标识 + 获取命令）。
**验收**：按文档下载得到**字节数与 sha256 完全一致**的文件。

### T5. 前端文档与本仓结构对齐

1. `frontends/zing_agent/BUILD-WINDOWS.md` 里的「**必须初始化 submodule**（`agent_tool/third_party/webview`）」在本仓**已不适用**（webview 已是普通文件、`.gitmodules` 已删）⇒ 需改写为"本仓无需 submodule 步骤"。
2. **Linux 侧前端构建依赖未文档化**（现有指南只覆盖 Windows）⇒ 补一份 Linux 构建说明（webview 的 GTK/WebKit 依赖等），并验证 `frontends/*/build-linux/` 在本仓新路径可构建。
**验收**：按文档在干净环境构建出前端产物。

### T6. e2e 的 Python 依赖未声明

**现状**：e2e 需要 `pytest`、`websocket-client`，但仓内**没有 requirements 文件**（我按文档梳理时发现）。
**要做**：加 `tests/e2e/requirements.txt`（或等价声明）并在 README/子文档引用。
**验收**：新环境 `pip install -r` 后 e2e 可直接跑。

### T7. 前端 / 知识库在本仓新路径的端到端验证（**尚未做过**）

**现状**：已验证的是**后端**（out-of-tree 构建 100%、关键用例 7/7 含真链路 e2e）。前端与 KB 只做了文件级核对，**没在新路径真正构建与运行过**。
**要做**：① 前端 `build-linux` 构建 + 连 `ws://host:port/ws`；② KB `python3 scripts/reindex.py` + `ensure_server.sh` 起服务 + 带 token 的写接口冒烟。
**验收**：两项各自有实测输出（构建成功 + 真实请求响应）。

### T8. 源仓 → 本仓 同步机制

**现状**：本仓由源仓拷贝而来，文档要求"只在一侧修改"，但**没有同步脚本**，靠人记。
**要做**：一条单向 `rsync`（排除 `.gitignore` 所列内容）+ 校验清单（`sha256sum` 对照），并在 README 写明方向与用法；若将来改成以本仓为主，只需把方向倒过来。
**验收**：同步后逐字节对照差异为 0（除有意排除项）。

---

### T9. 源仓 `thin_agent` 的下一轮深度自审（属**源仓**工作，此处仅登记）

上一轮（v0.54.30 / v0.54.31）结束时定的两个候选视角，尚未开工：

1. **`AgentServiceChat` 的 pre-FC 钩子与 `auto_save` 的时序**：v0.54.20 只修过其性能，但"**快照是否拍在正确的时刻**"（钩子相对消息落库/工具调用的先后）未从时序角度审过。
2. **`SandboxExecutor` 的进程组/孤儿清理在真实 kill 路径下的行为**：沙箱铁律在 skill 里，但**没跑过对抗性 kill 实验**（`setpgid` 逃逸/孤儿、`kill(-pid)` 边界）。

**注意**：这两项在**源仓 `/root/code/thin_agent`** 上做（走完整铁律：全 target 重链 → 同步 `libskill_*.so` → ctest + 两判官；版本递增 + CHANGELOG），完成后按 T8 同步到本仓。

### T10. 许可证说明（**需人工确认分发口径**）

**现状**：本仓**顶层没有 `LICENSE`**；第三方许可证说明只零散存在（如 `frontends/agent_tool/third_party/webview/LICENSE` 有，`backends/thin_agent/third_party/mongoose/` 里**没有许可文件**）。

**要做**：若本仓要对外分发/共享，需确认：
1. 是否补顶层 `LICENSE` + 第三方许可汇总（vendored：mongoose、nlohmann/json、webview；外部获取：ONNX Runtime、llama.cpp、GGUF 权重各自的许可）。
2. **mongoose 上游为 GPLv2 / 商业双许可** ⇒ 需确认本项目的分发口径是否合规（这一条**不是技术问题，需你/法务确认**）。

---

## 未决项（需你决策，暂不执行）

| # | 决策点 | 备选 |
|---|---|---|
| D1 | **真源归属**：以后在哪儿开发？ | ①继续以 `/root/code/*` 源仓为主，本仓只做交付快照（现状，配 T8 单向同步）；②以本仓为主，源仓退役 |
| D2 | 大件策略：`models/` 1.7G（gguf 权重） | ①继续本地保留不入库（现状）；②改用 Git LFS 入库；③放到外部存储/制品库，恢复脚本拉取 |
| D3 | 是否补 `LICENSE`（见 T10） | 是 / 否（内部使用）/ 先确认 mongoose 许可口径 |
| D4 | 前端与 KB 是否纳入本仓的常规验证（见 T7） | 是（加进 CI/自测清单）/ 暂不 |

---

## 已知缺口（如实登记，暂无法闭环）

1. **GGUF 三个权重的确切下载源未知**（见 T4）。
2. **前端 Linux 构建依赖未文档化**（见 T5）。
3. **本次"换机器照文档操作"只做了文档审查，未做全新机器实测**（用户当前要求只查文档）——可在有第二台干净机器时执行一次 0→4 全流程作为最终确认。
4. `backends/thin_agent` 里 `AgentServiceUtil.cpp:862` / `IntentOnnx.cpp:40` 的写死路径**当前不影响本仓可用**（相对候选优先、且 `models/` 已在盘中），但属 T1 的隐患，应修。
5. **顶层无 `LICENSE`**，且 `third_party/mongoose/` 无许可文件（上游为 GPLv2/商业双许可）——分发口径需人工确认（见 T10 / D3）。
6. 远端查询方式受限：**WSL 内不通 github**，只能用 Windows 侧 git（`git -C "E:\code\xyz-agent" …`）核对/推送；`\\wsl.localhost\...\mnt\e\...` 路径在 Windows 侧不可访问（Permission denied），要用 **`E:\code\xyz-agent`**。

---

## 已完成（勿重复）

| 项 | 证据 |
|---|---|
| 单仓建立（`.git` 在顶层，子目录 `.git` 已删，无任何嵌套 git） | 提交 `e9d8432`；`find . -name .git -not -path './.git'` → 0 |
| 清理生成物 642M（`build/`、`build-release/`、`.venv/`、`Testing/`、`build-linux/`、KB 日志与 pid） | 同上提交；删前均记录体积 |
| ONNX Runtime 两处符号链接**物化为真文件**（Windows 盘退化） | `15285336` / `12729976` 字节，与源侧目标逐字节一致 |
| 不入库依赖文档化 + sha256 校验表 | 提交 `8bf1525`；`THIRD_PARTY.md` 6/6 与磁盘实物一致 |
| 源码集与源仓逐字节一致（454 文件 0 差异）、大小写冲突 0 组、内容哈希 0 差异 | 见提交 `e9d8432` 提交信息 |
| 后端在新路径 out-of-tree 构建成功 + 关键用例 7/7 | 372 目标 / 0 error；含 `win32 判官 18/18`、`e2e_ws_close_attribution`、`bg_stdin`、`rotating_log_atomicity`、`fs_checkpoint_concurrency` |
| 推送远端 | `https://github.com/wangyujia/xyz-agent.git`（**`master` 分支**）——已推到 `8bf1525`；`4756e9c` 起为待推（见 T0） |
