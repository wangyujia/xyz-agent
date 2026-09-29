/* write_line_probe.c — v0.54.24 日志行原子性**机制级探针**（LD_PRELOAD）
 *
 * 目的：把"一行 = 一次写出"这条不变量变成**可判定的取证**，而不是靠概率抓撕裂。
 *
 * 为什么拦 `fwrite`/`fputc` 而不是 `write`：glibc 的 stdio 内部调用 `write` **不经过 PLT**
 * （内部绑定），LD_PRELOAD 拦不到，实测拦 write 得到 0 笔。而 `RotatingLogBuf` 是从**应用侧**
 * 调用 std::fwrite/fputc（PLT 解析）⇒ 可插桩，且配合日志文件 `_IONBF`（一次 fwrite = 一次 write
 * 系统调用），"fwrite 一笔"就是"write 一笔"。
 *
 * 判定：对写向 **agent_svc.log** 的每一笔：
 *   LINE <bytes> —— 该笔以 '\n' 结尾（完整行边界）✓
 *   PART <bytes> —— 不以 '\n' 结尾（行被拆开写出 ⇒ 并发读者可能看到半行）✗
 * 用法：LD_PRELOAD=<this.so> TA_WRITE_TRACE=/tmp/trace.txt ./build/thin_agent ...
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static size_t (*real_fwrite)(const void*, size_t, size_t, FILE*);
static int (*real_fputc)(int, FILE*);
static __thread int in_hook;
static FILE* trace_fp;
static int trace_init;

static const char* trace_path(void) { return getenv("TA_WRITE_TRACE"); }

static void ensure_trace(void) {
  if (trace_init) return;
  trace_init = 1;
  const char* p = trace_path();
  if (p && *p) trace_fp = fopen(p, "a");
}

/* 该 FILE* 是否就是服务日志（按 fd 目标路径判定） */
static int is_service_log(FILE* f) {
  if (f == NULL) return 0;
  const int fd = fileno(f);
  if (fd <= 2) return 0;
  char link[64];
  char target[PATH_MAX];
  (void)snprintf(link, sizeof(link), "/proc/self/fd/%d", fd);
  const ssize_t ln = readlink(link, target, sizeof(target) - 1);
  if (ln <= 0) return 0;
  target[ln] = '\0';
  return strstr(target, "agent_svc.log") != NULL;
}

static void record(int ends_with_newline, size_t bytes, const char* buf) {
  if (in_hook) return;
  in_hook = 1;
  ensure_trace();
  if (trace_fp) {
    if (ends_with_newline) {
      (void)fprintf(trace_fp, "LINE %zu\n", bytes);
    } else {
      /* 记下片段开头（可打印化），便于定位"谁把一行拆开了" */
      char snip[48];
      size_t k = 0;
      for (size_t i = 0; i < bytes && k + 1 < sizeof(snip); ++i) {
        const char ch = buf[i];
        snip[k++] = (ch >= 32 && ch < 127) ? ch : '.';
      }
      snip[k] = '\0';
      (void)fprintf(trace_fp, "PART %zu [%s]\n", bytes, snip);
    }
    (void)fflush(trace_fp);
  }
  in_hook = 0;
}

size_t fwrite(const void* ptr, size_t size, size_t nmemb, FILE* stream) {
  if (!real_fwrite) real_fwrite = (size_t(*)(const void*, size_t, size_t, FILE*))dlsym(RTLD_NEXT, "fwrite");
  const size_t r = real_fwrite(ptr, size, nmemb, stream);
  const size_t bytes = size * nmemb;
  if (bytes > 0 && is_service_log(stream)) {
    const char* b = (const char*)ptr;
    record(b[bytes - 1] == '\n', bytes, b);
  }
  return r;
}

int fputc(int c, FILE* stream) {
  if (!real_fputc) real_fputc = (int(*)(int, FILE*))dlsym(RTLD_NEXT, "fputc");
  const int r = real_fputc(c, stream);
  char cb = (char)c;
  if (is_service_log(stream)) record(c == '\n', 1, &cb);
  return r;
}
