# 不入库的第三方依赖与外部资源（THIRD-PARTY / NON-COMMITTED）

> 本仓**只提交源码/文档/脚本/测试/配置/小资源**（见 `.gitignore`）。下面这些依赖与资源**按约定不入库**——体积大、可由官方分发或本仓脚本再生、或属凭据。
> **克隆本仓后按本文恢复即可得到完整可构建/可运行的状态。**
> 恢复时**务必用文中的 `sha256` 校验**（尤其是模型文件）。

---

## 1. 一览表

| # | 依赖 / 资源 | 路径（相对仓根） | 体积 | 入库 | 何时需要 | 恢复方式 |
|---|---|---|---|---|---|---|
| 1 | **llama.cpp 源码树 / Windows 预编译** | `third_party/llama.cpp/` 或 `C:\deps-thin-agent\llama.cpp\` | 220M+ | ❌ | `-DTHIN_AGENT_WITH_LLAMA_CPP=ON` 进程内 GGUF | §2.1 |
| 2 | **ONNX Runtime 预编译库（x86_64）** | `backends/thin_agent/third_party/onnxruntime/lib/` | 15M | ❌（**头文件已入库**） | 链接 `thin_agent`（ONNX 意图推理） | §2.2（官方 release，手动放置） |
| 3 | **ONNX Runtime（aarch64）** | `backends/thin_agent/third_party/onnxruntime-aarch64/` | 25M | ❌ | `bash build.sh cam`（设备/ARM 版） | §2.3（**build.sh 自动下载**） |
| 4 | **意图分类模型** | `backends/thin_agent/models/intent/onnx/model.onnx` | 91,011,230 B | ❌ | ONNX 意图推理（本地级联第 2 级，~50ms） | §2.4（**本仓脚本可再生成**） |
| 5 | **GGUF 本地模型** | `backends/thin_agent/models/gguf/` | ~2.3G（Aurum+Qwen） | ❌ | 进程内离线 / 外挂 llama-server | §2.5（GitHub release + 校验） |
| 6 | **aarch64 离线打包产物** | `backends/thin_agent/dist/aarch64/` | 36M | ❌ | 设备侧离线部署 | §2.6（`deploy.sh --dist` 重建） |
| 7 | **知识库索引** | `backends/knowledge_base/{kb.db,.search.db}` | 221K | ❌ | KB 全文检索 | §2.7（`scripts/reindex.py` 重建） |
| 8 | **凭据** | `backends/knowledge_base/access_tokens.yaml`、`$THIN_AGENT_HOME/deepseek.env` | 小 | ❌（安全） | 运行时 | §3（单独投放 `chmod 600`） |

**不影响构建的**：`frontends/` 的 webview 与 nlohmann/json **已作为普通文件入库**（webview 原先来自上游子模块，本仓已 vendored，无需恢复）。

---

## 2. 逐项恢复步骤

### 2.1 llama.cpp（可选）

#### Linux / 源码静态库

本仓的 `third_party/llama.cpp/` 可为**上游检出**（历史固定 commit，供静态 `.a` 链接）：

```
上游   https://github.com/ggerganov/llama.cpp.git
commit 4f37f519722aa3242eecb7649466b4a4a2d6d6da   ("server: accept null sampling params (#25538)", 2026-07-10)
```

```bash
cd backends/thin_agent
git clone https://github.com/ggerganov/llama.cpp.git third_party/llama.cpp
git -C third_party/llama.cpp checkout 4f37f519722aa3242eecb7649466b4a4a2d6d6da
cmake -B <build> -DTHIN_AGENT_WITH_LLAMA_CPP=ON
# 或：-DLLAMA_CPP_DIR=/path/to/llama.cpp
```

#### Windows 预编译 DLL（本机推荐）

约定路径（不入库）：

| 路径 | 内容 |
|---|---|
| `C:\deps-thin-agent\llama.cpp\bin\` | `llama.dll`、`ggml*.dll`、`llama-server.exe`、`libomp.dll` 等 |
| `C:\deps-thin-agent\llama.cpp\lib\` | 导入库 `llama.lib` / `ggml.lib`（可由 DLL 生成） |
| `C:\deps-thin-agent\llama.cpp-src\` | 与 DLL **同 commit** 的头文件树（`include/llama.h`、`ggml/include`） |

本机已验证的发行版：`llama-server --version` → build **12503**，commit **`4577932fab595eaa8b27ad71734d5de1f3c07d8c`**（Vulkan Windows 包）。头文件：

```powershell
git clone --filter=blob:none https://github.com/ggml-org/llama.cpp.git C:\deps-thin-agent\llama.cpp-src
git -C C:\deps-thin-agent\llama.cpp-src fetch --depth 1 origin 4577932fab595eaa8b27ad71734d5de1f3c07d8c
git -C C:\deps-thin-agent\llama.cpp-src checkout 4577932fab595eaa8b27ad71734d5de1f3c07d8c
```

无 `.lib` 时在「x64 Native Tools」里对 `bin\llama.dll` / `ggml.dll`：`dumpbin /exports` → 写 `.def` → `lib /def:... /machine:x64`。CMake 默认会找上述路径；详见 `script/windows/README.md`。

> 该目录整体被 `backends/thin_agent/.gitignore`（与顶层 `.gitignore`）忽略；**默认 OFF**，不 clone 不影响其余全部功能与测试。

### 2.2 ONNX Runtime 预编译库（x86_64，15M）

- **已入库**：`third_party/onnxruntime/include/*.h`（C/C++ API 头文件）
- **未入库**：`third_party/onnxruntime/lib/libonnxruntime.so*`（二进制）

```bash
ver=1.18.1
dest=backends/thin_agent/third_party/onnxruntime/lib
curl -fsSL -o /tmp/ort.tgz \
  "https://github.com/microsoft/onnxruntime/releases/download/v${ver}/onnxruntime-linux-x64-${ver}.tgz"
tar -xzf /tmp/ort.tgz -C /tmp
mkdir -p "$dest"
cp /tmp/onnxruntime-linux-x64-${ver}/lib/libonnxruntime.so.${ver} "$dest/"
# ⚠ 关键：官方包里的 libonnxruntime.so 是指向 .so.${ver} 的符号链接。
#    在 Windows 盘（/mnt/*，DrvFs/9p）上符号链接会退化成 0 字节文件 ⇒ 必须显式物化：
cp "$dest/libonnxruntime.so.${ver}" "$dest/libonnxruntime.so"
```

校验（`sha256sum`）：

| 文件 | 字节数 | sha256 |
|---|---|---|
| `third_party/onnxruntime/lib/libonnxruntime.so.1.18.1` | 15,285,336 | `1147caa734d19f7549f90a659aa9ca444366eeb80a596900d19d242145a9f4c9` |

### 2.3 ONNX Runtime（aarch64，25M）

**无需手动操作**：`bash build.sh cam` 首次运行会自动下载并安装（实现见 `build.sh` 的 `ensure_onnxruntime_aarch64()`，版本变量 `ONNX_VER` 默认 `1.18.1`）：

```
https://github.com/microsoft/onnxruntime/releases/download/v1.18.1/onnxruntime-linux-aarch64-1.18.1.tgz
```

校验：

| 文件 | 字节数 | sha256 |
|---|---|---|
| `third_party/onnxruntime-aarch64/lib/libonnxruntime.so.1.18.1` | 12,729,976 | `006437cc7da2dd9a8a116e3fb0c96fe9c4ec8645fccbb731f3b02082a6fbf70e` |

### 2.4 意图分类模型（91M，可由本仓再生成）

`models/intent/onnx/model.onnx` 是 12 类意图分类器（基座 `cross-encoder/ms-marco-MiniLM-L-12-v2`，`BertForSequenceClassification`）。本仓自带完整流水线：

```bash
cd backends/thin_agent
python3 scripts/intent/retrain_intent_12class.py    # 训练（需 transformers/torch/onnx 环境）
python3 scripts/intent/export_intent_onnx.py        # 导出 → models/intent/onnx/model.onnx
python3 scripts/intent/eval_golden.py              # 黄金集评测（通过即视为等价）
```

校验（**原仓导出件**）：

| 文件 | 字节数 | sha256 |
|---|---|---|
| `models/intent/onnx/model.onnx` | 91,011,230 | `5d3e70fd0c9ff14b9b5169a51e957b7a9c74897afd0a35ce4bd318150c1d4d4a` |

> 重新训练/导出会得到**不同**的 hash（属预期）——此时以 `eval_golden.py` 通过为准。
> **兜底已入库**：`models/intent/intent_multiclass.onnx`、`intent_stub.onnx`、`labels.json`、`tokenizer.json` 等小文件都在仓内 ⇒ 没有训练环境时，代码会自动退化到这些小模型（`resolve_intent_model_path()` 的候选顺序）。

### 2.5 GGUF 本地模型

放置到 `backends/thin_agent/models/gguf/`（`.gitignore` 忽略）。用途：

- **Windows 推荐**：进程内 `GgufModel`（`offline_demo` + `THIN_AGENT_WITH_LLAMA_CPP=ON`）懒加载 **Aurum → Qwen**；或外挂 `llama-server` + `lmstudio_demo`。
- **历史 Linux 级联**：Template → ONNX intent → Qwen → Gemma（见 CHANGELOG）；云模式不需要 GGUF。

#### 已核验下载（Windows 本机，2026-11-18）

```powershell
$dir = "backends/thin_agent/models/gguf"
New-Item -ItemType Directory -Force -Path $dir | Out-Null
curl.exe --ssl-no-revoke -L --fail -o "$dir/aurum-brain-q4_k_m.gguf" `
  "https://github.com/aurum-lab/aurum-brain-ai/releases/download/daily-2026.11.18-07/aurum-brain-q4_k_m.gguf"
curl.exe --ssl-no-revoke -L --fail -o "$dir/qwen2.5-0.5b-instruct-q2_k.gguf" `
  "https://github.com/ozuakutku/qwen2.5-0.5b-gguf/releases/download/v1.0.0/qwen2.5-0.5b.gguf"
```

| 本地文件 | 字节数 | sha256 | 上游 |
|---|---|---|---|
| `aurum-brain-q4_k_m.gguf` | 1,929,902,400 | `ef68a4d9c31f9b5c1186979416aa5856557fa9821917d3fda8d5f36c8624786d` | [aurum-lab/aurum-brain-ai `daily-2026.11.18-07`](https://github.com/aurum-lab/aurum-brain-ai/releases/download/daily-2026.11.18-07/aurum-brain-q4_k_m.gguf) |
| `qwen2.5-0.5b-instruct-q2_k.gguf` | 397,807,936 | `c5396e06af294bd101b30dce59131a76d2b773e76950acc870eda801d3ab0515` | [ozuakutku/qwen2.5-0.5b-gguf `v1.0.0` 的 `qwen2.5-0.5b.gguf`](https://github.com/ozuakutku/qwen2.5-0.5b-gguf/releases/download/v1.0.0/qwen2.5-0.5b.gguf)（保存时改名） |

> Aurum release 标签可能按日滚动；若链接 404，到同一仓库 Releases 页取最新 `*_q4_k_m.gguf`，并以新文件的 sha256 更新本表。

#### 历史登记（Linux/CHANGELOG，地址曾未入库）

| 文件 | 字节数 | sha256 | 说明 |
|---|---|---|---|
| `qwen2.5-0.5b-instruct-q2_k.gguf` | 415,182,688 | `9ee36184e616dfc76df4f5dd66f908dbde6979524ae36e6cefb67f532f798cb8` | 与上表 Windows 件**不是同一文件** |
| `qwen2.5-0.5b-instruct-q4_k_m.gguf` | 491,400,032 | `74a4da8c9fdbcd15bd1f6d01d621410d31c6fc00986f5eb687824e7b93d7a9db` | 可选更高质量量化；**确切 URL 仍未登记** |
| `gemma-3-1b-it-q4_k_m.gguf` | 806,058,272 | `e4304281685198621755dac1851c040ff86b071e997cc214bfb9e8aca7f8dec2` | 代码仍可注册；文件缺失则跳过；**确切 URL 仍未登记** |

### 2.6 aarch64 离线打包产物（36M）

```bash
cd backends/thin_agent
bash deploy.sh --dist            # 产出 dist/aarch64/（可用 DIST_DIR 覆盖目标目录）
```

### 2.7 知识库索引（221K）

正文（markdown）已入库，DB 是**派生索引**：

```bash
cd backends/knowledge_base
python3 scripts/reindex.py       # 重建索引（另有 scripts/import.py、scripts/search.py、ensure_server.sh）
```

---

## 3. 凭据（非第三方，但同样不入库）

| 凭据 | 位置 | 投放要求 |
|---|---|---|
| KB 访问令牌 | `backends/knowledge_base/access_tokens.yaml` | 单独投放，`chmod 600`；**不要把真值提交** |
| 云通道凭据 | `$THIN_AGENT_HOME/deepseek.env` | `chmod 600`（Hermes/服务读取） |

自检（应输出空）：

```bash
cd <repo-root> && git ls-files | grep -E 'access_tokens|\.env($|\.)' || echo "OK：无凭据入库"
```

---

## 4. 一键校验（恢复后执行）

```bash
cd backends/thin_agent
sha256sum models/gguf/*.gguf models/intent/onnx/model.onnx \
  third_party/onnxruntime/lib/libonnxruntime.so.1.18.1 \
  | grep -v '^$'
# 与本文表格逐行比对（字节数用 stat -c%s 再核一遍）
```

Windows：

```powershell
Get-FileHash backends\thin_agent\models\gguf\*.gguf -Algorithm SHA256
```

## 5. 注意事项

1. **Windows 盘（`/mnt/*`，DrvFs/9p）**：符号链接会**退化为 0 字节文件** ⇒ 所有 `.so` 形式的依赖（见 §2.2）**必须物化**；本仓当前已是物化后的真文件（见 README「已知差异」）。**重新拷贝/恢复时务必重做这一步**，否则链接会失败。
2. **不要在本仓内构建**：构建产物应由 CMake 写到 Linux 侧 ext4（out-of-tree），例如
   `cmake -S backends/thin_agent -B ~/build_xyz && cmake --build ~/build_xyz -j$(nproc)`；
   本仓已忽略 `build*/`，不要把构建目录提交进来。
3. **`*.sh` 在 Windows 盘上没有执行位**：用 `bash xxx.sh` 调用。
4. 恢复后的自检：构建通过 + `ctest` 中两个判官（`static_win32_cross_compile`、`static_test_registration`）为绿。
