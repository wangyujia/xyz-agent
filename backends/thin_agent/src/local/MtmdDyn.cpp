#include "thin_agent/local/MtmdDyn.h"

#include <cctype>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#ifdef THIN_AGENT_WITH_LLAMA_CPP
// 只要类型，不要 dllimport 链接依赖
#pragma push_macro("LLAMA_SHARED")
#undef LLAMA_SHARED
#include "mtmd.h"
#include "mtmd-helper.h"
#pragma pop_macro("LLAMA_SHARED")
#endif

namespace thin_agent {
namespace local {

namespace {

#ifdef _WIN32
void* load_module(const char* name) {
  return reinterpret_cast<void*>(::LoadLibraryA(name));
}
void* sym(void* mod, const char* name) {
  return reinterpret_cast<void*>(
      ::GetProcAddress(reinterpret_cast<HMODULE>(mod), name));
}
#else
void* load_module(const char* name) { return ::dlopen(name, RTLD_LAZY); }
void* sym(void* mod, const char* name) { return ::dlsym(mod, name); }
#endif

template <typename T>
T load_sym(void* mod, const char* name) {
  return reinterpret_cast<T>(sym(mod, name));
}

struct Api {
  void* module = nullptr;

#ifdef THIN_AGENT_WITH_LLAMA_CPP
  using FnContextParamsDefault = mtmd_context_params (*)(void);
  using FnDefaultMarker = const char* (*)(void);
  using FnInitFromFile = mtmd_context* (*)(const char*, const llama_model*,
                                           mtmd_context_params);
  using FnFree = void (*)(mtmd_context*);
  using FnSupportVision = bool (*)(const mtmd_context*);
  using FnGetMarker = const char* (*)(const mtmd_context*);
  using FnInputChunksInit = mtmd_input_chunks* (*)(void);
  using FnInputChunksFree = void (*)(mtmd_input_chunks*);
  using FnTokenize = int32_t (*)(const mtmd_context*, mtmd_input_chunks*,
                                 const mtmd_input_text*,
                                 const mtmd_bitmap* const*, size_t);
  using FnHelperInitOptDefault = mtmd_helper_init_opt (*)(void);
  using FnHelperBitmapFromBuf =
      mtmd_helper_bitmap_wrapper (*)(const mtmd_context*,
                                     const unsigned char*, size_t, bool,
                                     mtmd_helper_init_opt);
  using FnBitmapFree = void (*)(mtmd_bitmap*);
  using FnHelperEvalChunks =
      int32_t (*)(mtmd_context*, llama_context*, const mtmd_input_chunks*,
                  llama_pos, llama_seq_id, int32_t, bool, llama_pos*);

  FnContextParamsDefault context_params_default = nullptr;
  FnDefaultMarker default_marker = nullptr;
  FnInitFromFile init_from_file = nullptr;
  FnFree free_ctx = nullptr;
  FnSupportVision support_vision = nullptr;
  FnGetMarker get_marker = nullptr;
  FnInputChunksInit input_chunks_init = nullptr;
  FnInputChunksFree input_chunks_free = nullptr;
  FnTokenize tokenize = nullptr;
  FnHelperInitOptDefault helper_init_opt_default = nullptr;
  FnHelperBitmapFromBuf helper_bitmap_from_buf = nullptr;
  FnBitmapFree bitmap_free = nullptr;
  FnHelperEvalChunks helper_eval_chunks = nullptr;
#endif

  bool ok() const {
#ifdef THIN_AGENT_WITH_LLAMA_CPP
    return module && context_params_default && init_from_file && free_ctx &&
           support_vision && input_chunks_init && input_chunks_free &&
           tokenize && helper_init_opt_default && helper_bitmap_from_buf &&
           bitmap_free && helper_eval_chunks;
#else
    return false;
#endif
  }
};

Api g_api;
std::once_flag g_once;

void init_api() {
#ifdef THIN_AGENT_WITH_LLAMA_CPP
#ifdef _WIN32
  const char* candidates[] = {"mtmd.dll", "libmtmd.dll"};
#else
  const char* candidates[] = {"libmtmd.so", "libmtmd.dylib", "mtmd.so"};
#endif
  void* mod = nullptr;
  for (const char* c : candidates) {
    mod = load_module(c);
    if (mod) break;
  }
  if (!mod) {
    if (const char* env = std::getenv("THIN_AGENT_MTMD_DLL")) {
      if (env && *env) mod = load_module(env);
    }
  }
  if (!mod) return;
  g_api.module = mod;
  g_api.context_params_default =
      load_sym<Api::FnContextParamsDefault>(mod, "mtmd_context_params_default");
  g_api.default_marker =
      load_sym<Api::FnDefaultMarker>(mod, "mtmd_default_marker");
  g_api.init_from_file =
      load_sym<Api::FnInitFromFile>(mod, "mtmd_init_from_file");
  g_api.free_ctx = load_sym<Api::FnFree>(mod, "mtmd_free");
  g_api.support_vision =
      load_sym<Api::FnSupportVision>(mod, "mtmd_support_vision");
  g_api.get_marker = load_sym<Api::FnGetMarker>(mod, "mtmd_get_marker");
  g_api.input_chunks_init =
      load_sym<Api::FnInputChunksInit>(mod, "mtmd_input_chunks_init");
  g_api.input_chunks_free =
      load_sym<Api::FnInputChunksFree>(mod, "mtmd_input_chunks_free");
  g_api.tokenize = load_sym<Api::FnTokenize>(mod, "mtmd_tokenize");
  g_api.helper_init_opt_default =
      load_sym<Api::FnHelperInitOptDefault>(mod, "mtmd_helper_init_opt_default");
  g_api.helper_bitmap_from_buf = load_sym<Api::FnHelperBitmapFromBuf>(
      mod, "mtmd_helper_bitmap_init_from_buf");
  g_api.bitmap_free = load_sym<Api::FnBitmapFree>(mod, "mtmd_bitmap_free");
  g_api.helper_eval_chunks =
      load_sym<Api::FnHelperEvalChunks>(mod, "mtmd_helper_eval_chunks");
#endif
}

Api& api() {
  std::call_once(g_once, init_api);
  return g_api;
}

}  // namespace

bool mtmd_runtime_available() { return api().ok(); }

std::string resolve_mmproj_path(const std::string& model_path) {
  namespace fs = std::filesystem;
  if (const char* env = std::getenv("THIN_AGENT_MMPROJ")) {
    if (env && *env && fs::exists(env)) return std::string(env);
  }
  if (model_path.empty()) return {};
  std::error_code ec;
  fs::path mp(model_path);
  fs::path dir = mp.parent_path();
  if (dir.empty()) dir = fs::current_path(ec);

  auto lower = [](std::string s) {
    for (auto& c : s)
      c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
  };

  fs::path stem_path = mp;
  stem_path.replace_extension("");
  const std::string stem = stem_path.filename().string();
  const std::string stem_l = lower(stem);

  // 1) 与主模型 stem 强相关的命名（优先，避免同目录错配其它 mmproj）
  const fs::path preferred[] = {
      dir / ("mmproj-" + stem + ".gguf"),
      dir / (stem + ".mmproj.gguf"),
      dir / (stem + "-mmproj-f16.gguf"),
      dir / (stem + "-mmproj-q8_0.gguf"),
      dir / (stem + "-mmproj.gguf"),
  };
  for (const auto& c : preferred) {
    if (fs::exists(c, ec)) return c.string();
  }

  // 2) 同目录：文件名含 mmproj，且与 stem 有公共前缀/关键词
  std::vector<fs::path> loose;
  if (!dir.empty() && fs::is_directory(dir, ec)) {
    for (auto it = fs::directory_iterator(dir, ec);
         !ec && it != fs::directory_iterator(); it.increment(ec)) {
      if (!it->is_regular_file(ec)) continue;
      if (it->path().extension() != ".gguf") continue;
      const std::string name_l = lower(it->path().filename().string());
      if (name_l.find("mmproj") == std::string::npos) continue;
      // 去掉常见量化后缀后看是否共享主干
      bool related = false;
      if (!stem_l.empty() && name_l.find(stem_l) != std::string::npos) {
        related = true;
      } else {
        // 取 stem 去量化后缀的前缀（如 smolvlm-500m-instruct）
        std::string key = stem_l;
        for (const char* suf :
             {"-q8_0", "-q4_k_m", "-q4_0", "-q5_k_m", "-f16", "-fp16"}) {
          auto p = key.rfind(suf);
          if (p != std::string::npos && p + std::strlen(suf) == key.size()) {
            key = key.substr(0, p);
            break;
          }
        }
        if (key.size() >= 6 && name_l.find(key) != std::string::npos)
          related = true;
      }
      if (related) return it->path().string();
      loose.push_back(it->path());
    }
  }
  // 3) 同目录仅有一个 mmproj 时才兜底（子目录单模型布局）
  if (loose.size() == 1) return loose.front().string();
  return {};
}

MtmdSession::~MtmdSession() { reset(); }

void MtmdSession::reset() {
#ifdef THIN_AGENT_WITH_LLAMA_CPP
  if (ctx_ && api().ok() && api().free_ctx) {
    api().free_ctx(static_cast<mtmd_context*>(ctx_));
  }
#endif
  ctx_ = nullptr;
  vision_ = false;
  mmproj_path_.clear();
}

bool MtmdSession::init(llama_model* text_model, const std::string& mmproj_path,
                       int n_threads) {
  reset();
#ifndef THIN_AGENT_WITH_LLAMA_CPP
  (void)text_model;
  (void)mmproj_path;
  (void)n_threads;
  return false;
#else
  if (!text_model || mmproj_path.empty() || !api().ok()) return false;
  auto params = api().context_params_default();
  params.use_gpu = true;
  params.n_threads = n_threads > 0 ? n_threads : 4;
  params.print_timings = false;
  params.warmup = false;
  mtmd_context* c =
      api().init_from_file(mmproj_path.c_str(), text_model, params);
  if (!c) return false;
  ctx_ = c;
  vision_ = api().support_vision(c);
  mmproj_path_ = mmproj_path;
  if (!vision_) {
    reset();
    return false;
  }
  return true;
#endif
}

const char* MtmdSession::media_marker() const {
#ifdef THIN_AGENT_WITH_LLAMA_CPP
  if (ctx_ && api().ok() && api().get_marker) {
    if (const char* m = api().get_marker(static_cast<mtmd_context*>(ctx_))) {
      if (m && *m) return m;
    }
  }
  if (api().ok() && api().default_marker) {
    if (const char* m = api().default_marker()) {
      if (m && *m) return m;
    }
  }
#endif
  return "<__media__>";
}

bool MtmdSession::eval_prompt(
    llama_context* lctx, const std::string& prompt,
    const std::vector<std::vector<uint8_t>>& images) {
#ifndef THIN_AGENT_WITH_LLAMA_CPP
  (void)lctx;
  (void)prompt;
  (void)images;
  return false;
#else
  if (!ready() || !lctx || prompt.empty() || !api().ok()) return false;
  auto* mctx = static_cast<mtmd_context*>(ctx_);

  std::vector<mtmd_bitmap*> bitmaps;
  bitmaps.reserve(images.size());
  auto opt = api().helper_init_opt_default();
  for (const auto& img : images) {
    if (img.empty()) {
      for (auto* b : bitmaps) api().bitmap_free(b);
      return false;
    }
    auto wrap = api().helper_bitmap_from_buf(
        mctx, img.data(), img.size(), /*placeholder=*/false, opt);
    if (!wrap.bitmap) {
      for (auto* b : bitmaps) api().bitmap_free(b);
      return false;
    }
    bitmaps.push_back(wrap.bitmap);
    // video_ctx 若非空通常由 helper 管理；图片场景一般为 nullptr
  }

  mtmd_input_text text;
  text.text = prompt.c_str();
  text.text_len = prompt.size();
  text.add_special = true;
  text.parse_special = true;

  mtmd_input_chunks* chunks = api().input_chunks_init();
  if (!chunks) {
    for (auto* b : bitmaps) api().bitmap_free(b);
    return false;
  }

  const int32_t tok = api().tokenize(
      mctx, chunks, &text,
      bitmaps.empty() ? nullptr
                      : const_cast<const mtmd_bitmap**>(bitmaps.data()),
      bitmaps.size());
  for (auto* b : bitmaps) api().bitmap_free(b);
  bitmaps.clear();

  if (tok != 0) {
    api().input_chunks_free(chunks);
    return false;
  }

  llama_pos new_past = 0;
  const int32_t n_batch = 512;
  const int32_t ev = api().helper_eval_chunks(
      mctx, lctx, chunks, /*n_past=*/0, /*seq_id=*/0, n_batch,
      /*logits_last=*/true, &new_past);
  api().input_chunks_free(chunks);
  return ev == 0;
#endif
}

}  // namespace local
}  // namespace thin_agent
