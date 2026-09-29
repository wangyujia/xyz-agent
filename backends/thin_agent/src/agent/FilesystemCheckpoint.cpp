#include "thin_agent/agent/FilesystemCheckpoint.h"
#include "thin_agent/log/LogEvent.h"   // v0.54.20: pre-FC 去重效果的可观测性日志
#include <filesystem>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <set>
#include <sstream>
// v0.54.17: 移除 POSIX-only 头（dirent.h/fcntl.h/sys/stat.h/sys/types.h/unistd.h）——
// 本文件的目录遍历/存在性/文件读/目录创建/mkdir 全部改走 **标准库**（std::filesystem /
// std::ifstream），于是两平台**同一份实现**（Linux 侧由既有单测验证），Windows 无需专属分支。

namespace {
/// v0.54.17: `mkdir(p, 0755)` 是 **POSIX 两参签名**（Windows 只有单参 `_mkdir`，且无 mode 语义）
/// ⇒ 统一走标准库 `create_directories`（best-effort：失败以 error_code 吞掉，与原"忽略返回值"一致）。
inline void ensure_dir_best_effort(const std::string& p) {
  std::error_code ec;
  std::filesystem::create_directories(p, ec);
}
}  // namespace

namespace thin_agent {
namespace agent {

// ══════════════════════════════════════════════════════════════════════════════
// 默认排除列表
// ══════════════════════════════════════════════════════════════════════════════

const std::vector<std::string>& FilesystemCheckpoint::default_excludes() {
  static const std::vector<std::string> ex = {
    // 版本控制
    ".git/", ".svn/", ".hg/",
    // 构建输出
    "build/", "out/", "target/", "dist/", ".next/", ".nuxt/",
    // 缓存
    "__pycache__/", "*.pyc", "*.pyo", ".cache/", ".pytest_cache/",
    ".mypy_cache/", ".ruff_cache/",
    // 虚拟环境
    ".venv/", "venv/", "env/",
    // 二进制 / 库
    "*.so", "*.dylib", "*.dll", "*.o", "*.a", "*.jar", "*.class", "*.exe", "*.obj",
    // 大文件 / 媒体
    "*.mp4", "*.mov", "*.mkv", "*.webm",
    "*.zip", "*.tar", "*.tar.gz", "*.tgz", "*.7z", "*.rar", "*.iso",
    // 密钥 / 环境
    ".env", ".env.*",
    // OS 垃圾
    ".DS_Store", "Thumbs.db",
    // 日志
    "*.log",
    // 覆盖率
    "coverage/", ".coverage",
    // Hermes/thin_agent 自身
    ".hermes/", ".thin_agent/",
    // IDE
    ".vscode/", ".idea/", "*.swp", "*.swo", "*~",
  };
  return ex;
}

// ══════════════════════════════════════════════════════════════════════════════
// 构造 / 路径 helper
// ══════════════════════════════════════════════════════════════════════════════

FilesystemCheckpoint::FilesystemCheckpoint(const std::string& base_dir)
    : base_dir_(base_dir.empty() ? default_checkpoints_dir() : base_dir) {
  // 确保 objects/ 目录存在
  std::string od = objects_dir();
  std::string parent = od.substr(0, od.rfind('/'));
  if (!parent.empty()) {
    ensure_dir_best_effort(parent.c_str());  // best-effort
    ensure_dir_best_effort((parent + "/objects").c_str());
  }
  ensure_dir_best_effort(od.c_str());
}

std::string FilesystemCheckpoint::objects_dir() const {
  return base_dir_ + "/objects";
}

std::string FilesystemCheckpoint::manifest_path(const std::string& ckpt_id) const {
  return checkpoint_dir(base_dir_, ckpt_id) + "/manifest.json";
}

std::string FilesystemCheckpoint::checkpoint_dir(const std::string& base,
                                                  const std::string& ckpt_id) {
  return base + "/" + ckpt_id;
}

std::string FilesystemCheckpoint::blob_path(const std::string& sha256) const {
  // objects/<sha256[:2]>/<sha256>
  return objects_dir() + "/" + sha256.substr(0, 2) + "/" + sha256;
}

// ══════════════════════════════════════════════════════════════════════════════
// SHA-256
// ══════════════════════════════════════════════════════════════════════════════

// ══════════════════════════════════════════════════════════════════════════════
// SHA-256 (自包含实现，零外部依赖)
// ══════════════════════════════════════════════════════════════════════════════

namespace {
// SHA-256 常量
static const uint32_t kSha256K[64] = {
  0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
  0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
  0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
  0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
  0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
  0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
  0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
  0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
};

inline uint32_t rotr32(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

struct Sha256Ctx {
  uint8_t  buf[64];
  uint64_t total;
  uint32_t state[8];
};

void sha256_init(Sha256Ctx* ctx) {
  ctx->total = 0;
  ctx->state[0] = 0x6a09e667; ctx->state[1] = 0xbb67ae85;
  ctx->state[2] = 0x3c6ef372; ctx->state[3] = 0xa54ff53a;
  ctx->state[4] = 0x510e527f; ctx->state[5] = 0x9b05688c;
  ctx->state[6] = 0x1f83d9ab; ctx->state[7] = 0x5be0cd19;
}

void sha256_transform(Sha256Ctx* ctx) {
  uint32_t w[64];
  for (int i = 0; i < 16; ++i)
    w[i] = ((uint32_t)ctx->buf[i*4] << 24) | ((uint32_t)ctx->buf[i*4+1] << 16)
         | ((uint32_t)ctx->buf[i*4+2] << 8) | ctx->buf[i*4+3];
  for (int i = 16; i < 64; ++i) {
    uint32_t s0 = rotr32(w[i-15], 7) ^ rotr32(w[i-15], 18) ^ (w[i-15] >> 3);
    uint32_t s1 = rotr32(w[i-2], 17) ^ rotr32(w[i-2], 19) ^ (w[i-2] >> 10);
    w[i] = w[i-16] + s0 + w[i-7] + s1;
  }
  uint32_t a = ctx->state[0], b = ctx->state[1], c = ctx->state[2], d = ctx->state[3];
  uint32_t e = ctx->state[4], f = ctx->state[5], g = ctx->state[6], h = ctx->state[7];
  for (int i = 0; i < 64; ++i) {
    uint32_t S1 = rotr32(e, 6) ^ rotr32(e, 11) ^ rotr32(e, 25);
    uint32_t ch = (e & f) ^ (~e & g);
    uint32_t t1 = h + S1 + ch + kSha256K[i] + w[i];
    uint32_t S0 = rotr32(a, 2) ^ rotr32(a, 13) ^ rotr32(a, 22);
    uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
    uint32_t t2 = S0 + maj;
    h = g; g = f; f = e; e = d + t1;
    d = c; c = b; b = a; a = t1 + t2;
  }
  ctx->state[0] += a; ctx->state[1] += b; ctx->state[2] += c; ctx->state[3] += d;
  ctx->state[4] += e; ctx->state[5] += f; ctx->state[6] += g; ctx->state[7] += h;
}

void sha256_update(Sha256Ctx* ctx, const uint8_t* data, size_t len) {
  size_t pos = ctx->total % 64;
  ctx->total += len;
  while (len > 0) {
    size_t room = 64 - pos;
    size_t copy = len < room ? len : room;
    memcpy(ctx->buf + pos, data, copy);
    pos += copy; data += copy; len -= copy;
    if (pos == 64) { sha256_transform(ctx); pos = 0; }
  }
}

void sha256_final(Sha256Ctx* ctx, uint8_t hash[32]) {
  size_t pos = ctx->total % 64;
  ctx->buf[pos++] = 0x80;
  if (pos > 56) { memset(ctx->buf + pos, 0, 64 - pos); sha256_transform(ctx); pos = 0; }
  memset(ctx->buf + pos, 0, 56 - pos);
  uint64_t bits = ctx->total * 8;
  for (int i = 0; i < 8; ++i) ctx->buf[56 + i] = (bits >> (56 - i*8)) & 0xFF;
  sha256_transform(ctx);
  for (int i = 0; i < 8; ++i) {
    hash[i*4]   = (ctx->state[i] >> 24) & 0xFF;
    hash[i*4+1] = (ctx->state[i] >> 16) & 0xFF;
    hash[i*4+2] = (ctx->state[i] >>  8) & 0xFF;
    hash[i*4+3] = ctx->state[i]         & 0xFF;
  }
}
}  // namespace

std::string FilesystemCheckpoint::compute_sha256(const std::string& filepath) {
  // v0.54.17: 原 POSIX open/read/close → std::ifstream（两平台同形）。语义保持：
  // 打不开或**读错误** ⇒ 返回空串（原 `n < 0` 分支）。
  std::ifstream in(filepath, std::ios::binary);
  if (!in.is_open()) return "";

  Sha256Ctx ctx;
  sha256_init(&ctx);

  char buf[8192];
  while (in) {
    in.read(buf, sizeof(buf));
    const std::streamsize got = in.gcount();
    if (got > 0) {
      sha256_update(&ctx, reinterpret_cast<const unsigned char*>(buf),
                    static_cast<size_t>(got));
    }
  }
  const bool read_error = in.bad();
  in.close();
  if (read_error) return "";

  uint8_t hash[32];
  sha256_final(&ctx, hash);

  std::ostringstream oss;
  oss << std::hex << std::setfill('0');
  for (int i = 0; i < 32; ++i) {
    oss << std::setw(2) << static_cast<int>(hash[i]);
  }
  return oss.str();
}

// ══════════════════════════════════════════════════════════════════════════════
// Blob 存储 / 恢复
// ══════════════════════════════════════════════════════════════════════════════

bool FilesystemCheckpoint::store_blob(const std::string& src_path,
                                       const std::string& sha256) {
  std::string dst = blob_path(sha256);
  // 如果已存在则跳过（去重）（v0.54.17: fs::exists 两平台同形；且**不跟随符号链接**）
  std::error_code ec_exists;
  if (std::filesystem::exists(dst, ec_exists)) return true;

  // 确保父目录存在（v0.54.17: `mkdir(p, 0755)` 是 POSIX 两参签名，Windows 只有单参 `_mkdir`
  // ⇒ 统一走 `create_directories`（best-effort：失败以 error_code 吞掉，与原"忽略返回值"一致）
  std::string parent = dst.substr(0, dst.rfind('/'));
  std::error_code ec_mkdir;
  std::filesystem::create_directories(parent, ec_mkdir);

  // v0.53.60: 原子拷贝(tmp+rename)——直接 "wb" 打开目标,中途崩溃=
  /// 半截文件,restore 读坏 blob;tmp 写全+rename 原子替换
  FILE* src = ::fopen(src_path.c_str(), "rb");
  if (!src) return false;
  const std::string tmp = dst + ".tmp_ckpt";
  FILE* out = ::fopen(tmp.c_str(), "wb");
  if (!out) { ::fclose(src); return false; }

  char buf[65536];
  size_t n;
  while ((n = ::fread(buf, 1, sizeof(buf), src)) > 0) {
    if (::fwrite(buf, 1, n, out) != n) {
      ::fclose(src); ::fclose(out);
      ::remove(tmp.c_str());
      return false;
    }
  }
  ::fclose(src);
  ::fclose(out);
  // v0.54.17: `::rename` 在 **Windows 上目标已存在时会失败**（POSIX 语义是原子替换）⇒ 改
  // `std::filesystem::rename`（其 Windows 实现走 MoveFileEx + MOVEFILE_REPLACE_EXISTING，与 POSIX
  // 同为"覆盖已存在目标"）。失败仍清理 tmp。
  {
    std::error_code ec_rn;
    std::filesystem::rename(tmp, dst, ec_rn);
    if (ec_rn) {
      std::error_code ec_rm;
      std::filesystem::remove(tmp, ec_rm);
      return false;
    }
  }
  return true;
}

bool FilesystemCheckpoint::restore_blob(const std::string& sha256,
                                         const std::string& dst_path) {
  std::string src = blob_path(sha256);
  // 确保目标父目录存在
  std::string parent = dst_path.substr(0, dst_path.rfind('/'));
  if (!parent.empty() && parent != dst_path) {
    ensure_dir_best_effort(parent.c_str());
  }

  // v0.53.60: 原子写(与 save_blob 同款)——restore 半截文件会把
  /// 工作区直接写坏;tmp+rename
  FILE* in = ::fopen(src.c_str(), "rb");
  if (!in) return false;
  const std::string tmp = dst_path + ".tmp_restore";
  FILE* out = ::fopen(tmp.c_str(), "wb");
  if (!out) { ::fclose(in); return false; }

  char buf[65536];
  size_t n;
  while ((n = ::fread(buf, 1, sizeof(buf), in)) > 0) {
    if (::fwrite(buf, 1, n, out) != n) {
      ::fclose(in); ::fclose(out);
      ::remove(tmp.c_str());
      return false;
    }
  }
  ::fclose(in);
  ::fclose(out);
  {
    std::error_code ec_rn;
    std::filesystem::rename(tmp, dst_path, ec_rn);   // v0.54.17: 同 store_blob 的可移植替换
    if (ec_rn) {
      std::error_code ec_rm;
      std::filesystem::remove(tmp, ec_rm);
      return false;
    }
  }
  return true;
}

// ══════════════════════════════════════════════════════════════════════════════
// 通配符匹配（仅支持 * 前缀/后缀/包含，非完整 glob）
// ══════════════════════════════════════════════════════════════════════════════

static bool wildcard_match(const std::string& pattern, const std::string& text) {
  // 简单模式：前缀* → 后缀匹配；*后缀 → 前缀匹配；*包含* → 子串匹配；精确匹配
  if (pattern == text) return true;

  auto star_first = pattern.find('*');
  auto star_last  = pattern.rfind('*');

  if (star_first == std::string::npos) return false;  // 无通配符且不相等

  if (star_first == 0 && star_last == pattern.size() - 1) {
    // *xxx*
    std::string inner = pattern.substr(1, pattern.size() - 2);
    return text.find(inner) != std::string::npos;
  }

  if (star_first == 0) {
    // *suffix
    std::string suffix = pattern.substr(1);
    return text.size() >= suffix.size() &&
           text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
  }

  if (star_last == pattern.size() - 1) {
    // prefix*
    std::string prefix = pattern.substr(0, star_last);
    return text.compare(0, prefix.size(), prefix) == 0;
  }

  // 多个 * → 降级为子串匹配
  return text.find(pattern.substr(0, star_first)) != std::string::npos;
}

bool FilesystemCheckpoint::is_excluded(const std::string& rel_path,
                                        const std::vector<std::string>& excludes) {
  for (const auto& pat : excludes) {
    // 目录匹配：path/ 或以 pat 开头且紧跟 /
    if (!pat.empty() && pat.back() == '/') {
      std::string dir = pat;
      if (rel_path.find(dir) == 0) return true;
      // 也检查路径的任意组成部分
      if (rel_path.find("/" + dir) != std::string::npos) return true;
      continue;
    }
    // 文件名匹配（取路径最后一段）
    std::string fname = rel_path;
    auto slash = rel_path.rfind('/');
    if (slash != std::string::npos) fname = rel_path.substr(slash + 1);

    if (wildcard_match(pat, fname)) return true;
    if (wildcard_match(pat, rel_path)) return true;
  }
  return false;
}

// ══════════════════════════════════════════════════════════════════════════════
// 目录扫描
// ══════════════════════════════════════════════════════════════════════════════

std::vector<FilesystemCheckpoint::FileEntry>
FilesystemCheckpoint::scan_workdir(const std::string& workdir,
                                    const std::vector<std::string>& excludes) {
  std::vector<FileEntry> results;

  // v0.45.6: 扫描上限 — 防止巨型目录树（如内核源码）卡死服务。
  // 实测：/root/code 含 linux-5.10.y 内核源码，逐个 SHA-256 需数分钟，
  // 每次 handle_chat 前 auto_fc_pre 快照都阻塞全部请求。
  static constexpr size_t kMaxFiles = 20000;  // 最多收录文件数
  static constexpr size_t kMaxDirs = 4000;    // 最多展开目录数

  // 递归扫描 (简易 BFS，避免递归栈溢出)
  std::vector<std::string> dirs = {""};
  while (!dirs.empty()) {
    std::string rel = dirs.back();
    dirs.pop_back();

    std::string full = workdir + "/" + rel;
    if (full.size() > 1 && full.back() == '/') full.pop_back();

    // v0.54.17: opendir/readdir/lstat → std::filesystem（两平台同一份实现）。
    // 语义对齐：`directory_iterator` 本就不产出 "."/".."；用 **symlink_status**（不跟随）判断类型
    // ⇒ 符号链接/socket/fifo/设备一律跳过（与原 lstat + S_ISDIR/S_ISREG 一致）。
    std::error_code ec_dir;
    std::filesystem::directory_iterator it(full, ec_dir);
    if (ec_dir) continue;

    for (const auto& e : it) {
      const std::string name = e.path().filename().string();
      if (name.empty()) continue;

      std::string child_rel = rel.empty() ? name : rel + "/" + name;

      // 检查排除
      if (is_excluded(child_rel, excludes)) continue;

      std::error_code ec_st;
      const auto stt = e.symlink_status(ec_st);
      if (ec_st) continue;

      if (std::filesystem::is_directory(stt)) {
        // 达到目录展开上限：不再深入（已收集的结果保留）
        if (dirs.size() >= kMaxDirs) continue;
        dirs.push_back(child_rel);
      } else if (std::filesystem::is_regular_file(stt)) {
        // 达到文件收录上限：截断，跳过剩余
        if (results.size() >= kMaxFiles) {
          goto scan_done;
        }
        std::error_code ec_sz;
        FileEntry fe;
        fe.path  = child_rel;
        fe.size  = static_cast<size_t>(std::filesystem::file_size(e.path(), ec_sz));
        const auto ft = std::filesystem::last_write_time(e.path(), ec_sz);
        fe.mtime = ft.time_since_epoch().count();
        // v0.54.23: file_clock 与 system_clock **纪元不同**（且 C++17 无 to_sys）⇒ 用两钟 now 的偏移
        // 做标准换算（亚毫秒级误差，足够做"是否明显早于上次快照"的守卫）。Windows 侧 file_clock 纪元
        // 是 1601、Linux 是 Unix 纪元——统一经此换算后即可与 manifest 的 system_clock 毫秒比较。
        // 换算要点：先把"该文件 mtime 相对 file_clock::now() 的偏移"取成 duration，再叠加到
        // system_clock::now() 上（直接相减相加会得到 time_point ⇒ 无法 duration_cast）。
        const auto ft_delta = ft - std::filesystem::file_time_type::clock::now();
        const auto sys_tp =
            std::chrono::system_clock::now() +
            std::chrono::duration_cast<std::chrono::system_clock::duration>(ft_delta);
        fe.mtime_sys_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                              sys_tp.time_since_epoch())
                              .count();
        results.push_back(std::move(fe));
      }
      // 跳过 symlink / socket / fifo / device
    }
  }

scan_done:
  // 排序以保证确定性
  std::sort(results.begin(), results.end(),
            [](const FileEntry& a, const FileEntry& b) { return a.path < b.path; });

  return results;
}

// ══════════════════════════════════════════════════════════════════════════════
// 保存
// ══════════════════════════════════════════════════════════════════════════════

std::string FilesystemCheckpoint::save(const std::string& workdir,
                                        const std::vector<std::string>& custom_excludes,
                                        const std::string& label,
                                        int max_checkpoints) {
  std::unique_lock<std::mutex> lock(mu_);

  // 合并排除列表
  std::vector<std::string> excludes = default_excludes();
  excludes.insert(excludes.end(), custom_excludes.begin(), custom_excludes.end());

  // 扫描
  auto files = scan_workdir(workdir, excludes);

  // 生成 checkpoint_id
  auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count();
  // v0.54.23: **id 必须唯一** —— 原来只用 now_ms，**同一毫秒内的两次 save 会拿到同一个 id**，
  // 第二次直接**覆盖**第一次的 manifest（blob 又是内容寻址 ⇒ 旧状态不可恢复）= 快照静默丢失。
  // 修法：毫秒 + 进程内单调序号；并对"目标 manifest 已存在"继续递增（跨进程/时钟回拨同样安全）。
  // 参照：CheckpointManager（内存版）早已用 `..._<seq>` 防碰撞，文件版此前漏了这一步。
  static std::atomic<uint64_t> ckpt_seq{0};
  std::error_code ec_probe;   // v0.54.27: exists 用 ec 版，避免权限异常打断存档
  std::string ckpt_id;
  do {
    std::ostringstream id_ss;
    id_ss << "ckpt_" << now_ms << "_" << ckpt_seq.fetch_add(1);
    ckpt_id = id_ss.str();
  } while (std::filesystem::exists(manifest_path(ckpt_id), ec_probe));   // v0.54.27: ec 版不抛异常

  // v0.54.20: **mtime+size 去重** —— 同一个 workdir 的上一次快照里，若某文件 size 与 mtime 均未变，
  // 直接复用其 sha256 并跳过 `compute_sha256` + `store_blob`。**语义完全不变**（manifest 字段、内容寻址
  // 与回滚行为一致）：sha256 只是文件内容的身份，未变文件的内容身份必然未变；blob 也是按 sha256 存的
  // （`store_blob` 命中已有对象会直接返回）。
  // 动因（实测）：workdir=/root/code（44 万文件，caps 20000 文件/4000 目录）时每轮 pre-FC 要
  // 11.6→26s（随仓库增长单调恶化，直接把 e2e_shutdown_notice 的 20s 预算顶爆），而其中绝大多数
  // 文件自上次快照以来**根本没变** —— 之前是每轮无条件全量重哈希 + 全量拷贝。
  std::map<std::string, FileEntry> prev;
  int64_t best_timestamp = 0;   // v0.54.23: 上一次快照的保存时刻（system_clock 毫秒）
  {
    auto snaps = list_nolock();  // 调用方已持锁（save() 开头）；返回的每个 Snapshot 自带 files
    const Snapshot* best = nullptr;
    for (const auto& sn : snaps) {
      if (sn.workdir != workdir) continue;
      if (best == nullptr || sn.timestamp > best->timestamp) best = &sn;
    }
    if (best != nullptr) {
      best_timestamp = best->timestamp;   // v0.54.23: 竞态守卫用
      for (const auto& f : best->files) prev[f.path] = f;
    }
  }

  int reused = 0;
  int hashed = 0;
  for (auto& fe : files) {
    std::string full_path = workdir + "/" + fe.path;
    auto it = prev.find(fe.path);
    // v0.54.23 **竞态守卫**：仅当"size 与 mtime 均未变"**且**"该 mtime 明显早于上一次快照的保存时刻"
    // 才复用。否则存在同秒竞态：文件在"上次哈希之后、mtime 粒度同一格之内"被改写且大小不变 ⇒ 复用旧
    // sha256 ⇒ 回滚内容不正确。守卫按 git index 的 racy-timestamp 规则取 2s 余量（兼容粒度 ≤2s 的 FS）；
    // 换算失败/旧 manifest（mtime_sys_ms==0）时同样保守重哈希。
    const bool racy_safe = it != prev.end() && it->second.mtime_sys_ms > 0 &&
                           it->second.mtime_sys_ms + 2000 < best_timestamp;
    if (it != prev.end() && it->second.size == fe.size && it->second.mtime == fe.mtime &&
        racy_safe && !it->second.sha256.empty()) {
      fe.sha256 = it->second.sha256;   // 未变 ⇒ 复用（不读文件、不拷贝）
      ++reused;
      continue;
    }
    fe.sha256 = compute_sha256(full_path);
    if (fe.sha256.empty()) continue;
    store_blob(full_path, fe.sha256);
    ++hashed;
  }
  last_reused_ = reused;   // v0.54.23: 测试/诊断可观察
  // 可观测性：去重效果必须能被日志验证（否则"变快了"无从归因）
  log_event("fs-checkpoint", LogLevel::Info, "pre-FC 扫描去重",
            {{"total", static_cast<int>(files.size())},
             {"reused", reused},
             {"hashed", hashed},
             {"prev_files", static_cast<int>(prev.size())}});

  // 写 manifest
  std::string ckpt_dir = checkpoint_dir(base_dir_, ckpt_id);
  ensure_dir_best_effort(ckpt_dir.c_str());

  nlohmann::json manifest;
  manifest["checkpoint_id"] = ckpt_id;
  manifest["workdir"] = workdir;
  manifest["label"] = label;
  manifest["timestamp"] = now_ms;
  manifest["files"] = nlohmann::json::array();
  for (const auto& fe : files) {
    nlohmann::json jf;
    jf["path"] = fe.path;
    jf["sha256"] = fe.sha256;
    jf["size"] = fe.size;
    jf["mtime"] = fe.mtime;
    jf["mtime_sys_ms"] = fe.mtime_sys_ms;   // v0.54.23（旧 manifest 缺此字段 → 0 → 保守重哈希）
    manifest["files"].push_back(jf);
  }

  std::string mp = manifest_path(ckpt_id);
  // v0.54.31: **原子写 manifest**（tmp + rename）。旧实现就地 `std::ofstream ofs(mp)` 覆盖：并发读者
  // （另一个进程、或任何直接读该文件的代码）会看到**被截断的 manifest** —— 实测 1500 文件/321KB
  // manifest 时被观测到 **1010 次**截断态；解析失败 ⇒ 该快照被 list/prune **静默丢弃**（回滚时表现为
  // "快照不存在"，比崩溃更隐蔽）。进程在写中途被杀同样会留下半截文件。
  // 与同文件里 blob（v0.53.60）与 workdir（v0.53.60/v0.54.17）一致：tmp 写全 + 可移植 rename 替换。
  const std::string tmp_mp = mp + ".tmp";
  {
    std::ofstream ofs(tmp_mp, std::ios::binary | std::ios::trunc);
    if (!ofs) return "";
    ofs << manifest.dump(2);
    ofs.flush();
    if (!ofs) {                       // 写失败（磁盘满等）：清掉 tmp，**保留旧的完整 manifest**
      std::error_code ec_del;
      std::filesystem::remove(tmp_mp, ec_del);
      return "";
    }
  }
  {
    std::error_code ec_rn;
    std::filesystem::rename(tmp_mp, mp, ec_rn);   // v0.54.17: 可移植（Windows 亦为原子替换）
    if (ec_rn) {
      std::error_code ec_del;
      std::filesystem::remove(tmp_mp, ec_del);
      return "";
    }
  }

  // 修剪旧快照（释放锁后调用，因为 prune 内部需要加锁）
  lock.unlock();
  if (max_checkpoints > 0) {
    prune(max_checkpoints);
  }

  return ckpt_id;
}

int FilesystemCheckpoint::last_reused_count() const {
  std::lock_guard<std::mutex> lock(mu_);
  return last_reused_;
}

// ══════════════════════════════════════════════════════════════════════════════
// 回滚
// ══════════════════════════════════════════════════════════════════════════════

FilesystemCheckpoint::RollbackResult
FilesystemCheckpoint::rollback(const std::string& checkpoint_id, bool dry_run) {
  std::lock_guard<std::mutex> lock(mu_);
  return rollback_nolock(checkpoint_id, dry_run);
}

FilesystemCheckpoint::RollbackResult
FilesystemCheckpoint::rollback_nolock(const std::string& checkpoint_id, bool dry_run) {
  RollbackResult result;

  std::string mp = manifest_path(checkpoint_id);
  std::ifstream ifs(mp);
  if (!ifs) {
    result.error = "checkpoint not found: " + checkpoint_id;
    return result;
  }

  nlohmann::json manifest;
  try {
    ifs >> manifest;
  } catch (...) {
    result.error = "invalid manifest: " + checkpoint_id;
    return result;
  }

  std::string workdir = manifest.value("workdir", "");
  if (workdir.empty()) {
    result.error = "manifest missing workdir";
    return result;
  }

  auto files = manifest.value("files", nlohmann::json::array());

  // 收集快照中的文件清单
  std::set<std::string> ckpt_paths;
  for (const auto& jf : files) {
    std::string path = jf.value("path", "");
    if (!path.empty()) ckpt_paths.insert(path);
    }

  // 扫描当前目录中的文件
  std::set<std::string> current_paths;
  {
    auto current_files = scan_workdir(workdir, {});  // 无排除以获取完整列表
    for (const auto& fe : current_files) {
      current_paths.insert(fe.path);
    }

    // 回滚文件
    for (const auto& jf : files) {
      std::string path = jf.value("path", "");
      std::string sha256 = jf.value("sha256", "");
      if (path.empty() || sha256.empty()) continue;

      std::string full_path = workdir + "/" + path;
      std::string current_hash = compute_sha256(full_path);

      if (!current_hash.empty() && current_hash == sha256) {
        result.skipped.push_back(path);
      } else {
        result.restored.push_back(path);
        if (!dry_run) {
          restore_blob(sha256, full_path);
        }
      }
    }

    // 新增文件（当前有但快照无）
    for (const auto& p : current_paths) {
      if (ckpt_paths.count(p) == 0) {
        result.added.push_back(p);
      }
    }

    // 删除文件（快照有但当前无）
    for (const auto& p : ckpt_paths) {
      if (current_paths.count(p) == 0) {
        result.removed.push_back(p);
      }
    }
  }

  result.ok = true;
  return result;
}

// ══════════════════════════════════════════════════════════════════════════════
// 差异
// ══════════════════════════════════════════════════════════════════════════════

nlohmann::json FilesystemCheckpoint::diff(const std::string& checkpoint_id) {
  std::lock_guard<std::mutex> lock(mu_);

  auto rb = rollback_nolock(checkpoint_id, /*dry_run=*/true);
  nlohmann::json result;
  result["checkpoint_id"] = checkpoint_id;
  result["changed"] = nlohmann::json::array();

  for (const auto& p : rb.restored) {
    nlohmann::json item;
    item["path"] = p;
    item["status"] = "modified";
    result["changed"].push_back(item);
  }
  for (const auto& p : rb.added) {
    nlohmann::json item;
    item["path"] = p;
    item["status"] = "added";
    result["changed"].push_back(item);
  }
  for (const auto& p : rb.removed) {
    nlohmann::json item;
    item["path"] = p;
    item["status"] = "removed";
    result["changed"].push_back(item);
  }

  result["summary"] = {
    {"modified", rb.restored.size()},
    {"added", rb.added.size()},
    {"removed", rb.removed.size()},
    {"unchanged", rb.skipped.size()},
  };

  return result;
}

// ══════════════════════════════════════════════════════════════════════════════
// 管理
// ══════════════════════════════════════════════════════════════════════════════

std::vector<FilesystemCheckpoint::Snapshot>
FilesystemCheckpoint::list() const {
  std::lock_guard<std::mutex> lock(mu_);
  return list_nolock();
}

std::vector<FilesystemCheckpoint::Snapshot>
FilesystemCheckpoint::list_nolock() const {
  std::vector<Snapshot> results;

  // v0.54.17: opendir/readdir → std::filesystem（两平台同形）。`directory_iterator` 不产出
  // "."/".."；原样跳过 "objects"（blob 目录，非快照）。
  std::error_code ec_dir;
  std::filesystem::directory_iterator it(base_dir_, ec_dir);
  if (ec_dir) return results;

  for (const auto& e : it) {
    std::string name = e.path().filename().string();
    if (name.empty() || name == "objects") continue;

    std::string mp = manifest_path(name);
    std::ifstream ifs(mp);
    if (!ifs) continue;

    try {
      nlohmann::json manifest = nlohmann::json::parse(ifs);
      Snapshot snap;
      snap.checkpoint_id = manifest.value("checkpoint_id", name);
      snap.workdir       = manifest.value("workdir", "");
      snap.label         = manifest.value("label", "");
      snap.timestamp     = manifest.value("timestamp", 0LL);

      auto files = manifest.value("files", nlohmann::json::array());
      for (const auto& jf : files) {
        FileEntry fe;
        fe.path   = jf.value("path", "");
        fe.sha256 = jf.value("sha256", "");
        fe.size   = jf.value("size", 0ULL);
        fe.mtime  = jf.value("mtime", 0LL);
        // v0.54.23: **必须反序列化** —— 漏读会让去重竞态守卫恒判"不安全"⇒ 去重永久失效（实测踩到）
        fe.mtime_sys_ms = jf.value("mtime_sys_ms", 0LL);
        snap.files.push_back(fe);
      }
      results.push_back(std::move(snap));
    } catch (...) {
      // 跳过损坏的 manifest
    }
  }

  // 按时间倒序
  std::sort(results.begin(), results.end(),
            [](const Snapshot& a, const Snapshot& b) {
              return a.timestamp > b.timestamp;
            });
  return results;
}

bool FilesystemCheckpoint::remove(const std::string& checkpoint_id) {
  std::lock_guard<std::mutex> lock(mu_);

  std::string ckpt_dir = checkpoint_dir(base_dir_, checkpoint_id);
  // 递归删除
  std::string cmd = "rm -rf " + ckpt_dir;
  return (::system(cmd.c_str()) == 0);

  // 注意：这里没有删除 objects/ 中的 blob（可能需要 GC）
  // 简单实现：blob 不删除，让 prune 时统一处理
}

int FilesystemCheckpoint::prune(int keep_count) {
  std::lock_guard<std::mutex> lock(mu_);
  auto snaps = list_nolock();  // 已按时间倒序
  if (static_cast<int>(snaps.size()) <= keep_count) return 0;

  int removed = 0;
  for (int i = keep_count; i < static_cast<int>(snaps.size()); ++i) {
    std::string ckpt_dir = checkpoint_dir(base_dir_, snaps[i].checkpoint_id);
    // v0.53.80: rm -rf 拼接→filesystem::remove_all——system("rm -rf "+path)
    /// 是自家代码里的裸 shell(与本仓 grade_shell_risk 哲学相悖,ckpt_id
    /// 被污染即任意删除断言面);remove_all 等价且无 shell
    std::error_code ec;
    if (std::filesystem::remove_all(ckpt_dir, ec) > 0) ++removed;
  }
  // v0.53.80: blob GC——prune 只删快照目录,objects/ 内容寻址 blob 成
  /// 孤儿永不回收(实测 3.1G 堆积);扫描存活 manifest 引用集,删未引用
  std::set<std::string> live;
  for (int i = 0; i < keep_count && i < static_cast<int>(snaps.size()); ++i) {
    std::ifstream mf(manifest_path(snaps[i].checkpoint_id));
    if (!mf) continue;
    try {
      auto man = nlohmann::json::parse(mf);
      if (man.contains("files") && man["files"].is_array()) {
        for (const auto& jf : man["files"]) {
          if (jf.contains("sha256") && jf["sha256"].is_string())
            live.insert(jf["sha256"].get<std::string>());
        }
      }
    } catch (...) {
      // manifest 畸形:保守跳过(不 GC 该快照的引用)
    }
  }
  std::error_code ec2;
  for (auto it = std::filesystem::recursive_directory_iterator(
           objects_dir(), ec2);
       it != std::filesystem::recursive_directory_iterator(); ++it) {
    if (ec2 || !it->is_regular_file(ec2)) continue;
    const std::string fname = it->path().filename().string();
    // 文件名=sha256;未被任何存活快照引用=孤儿
    if (live.count(fname) == 0) {
      std::error_code rm_ec;
      std::filesystem::remove(it->path(), rm_ec);
    }
  }
  return removed;
}

nlohmann::json FilesystemCheckpoint::stats() const {
  std::lock_guard<std::mutex> lock(mu_);
  auto snaps = list_nolock();

  nlohmann::json s;
  s["total_checkpoints"] = snaps.size();
  s["checkpoints"] = nlohmann::json::array();
  for (const auto& snap : snaps) {
    nlohmann::json js;
    js["id"] = snap.checkpoint_id;
    js["label"] = snap.label;
    js["timestamp"] = snap.timestamp;
    js["file_count"] = snap.files.size();
    s["checkpoints"].push_back(js);
  }
  return s;
}

// ══════════════════════════════════════════════════════════════════════════════
// 自动保存（turn 级速率限制）
// ══════════════════════════════════════════════════════════════════════════════

std::string FilesystemCheckpoint::auto_save(const std::string& workdir,
                                              const std::string& label) {
  {
    std::lock_guard<std::mutex> lock(mu_);
    if (!turn_dirty_ || turn_saved_) return "";
    turn_saved_ = true;
  }
  // 在锁外调用 save（save 内部加锁，避免死锁）
  return save(workdir, {}, label, 20);
}

}  // namespace agent
}  // namespace thin_agent
