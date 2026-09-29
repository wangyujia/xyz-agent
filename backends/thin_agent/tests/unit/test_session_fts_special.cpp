// v0.53.53: SessionStore FTS 特字符查询回归
// 背景:裸绑用户串,含 +/引号/NEAR 即 fts5 语法错→step 报错→
// 静默空结果(搜"什么是C++"必空)。修:step 错误时降级整串短语查询。
#include "test_macros.h"
#include "thin_agent/core/SessionStore.h"
using thin_agent::SessionStore;
int main() {
  const char* db = "/tmp/test_fts_special.db";
  ::remove(db);
  SessionStore st;
  ASSERT_TRUE("open", st.open(db));
  ASSERT_TRUE("record", st.record_message("s1", "user", "什么是C++的移动语义?"));
  ASSERT_TRUE("record2", st.record_message("s2", "user", "plain hello world"));
  // 特字符整串(修前=fts5 syntax error→0 条)
  auto r1 = st.search("什么是C++的移动语义?", 5);
  ASSERT_TRUE("特字符整串可搜(降级短语)", r1.size() >= 1);
  // 拉丁普通词(裸绑定合法,词查询语义保留)
  auto r2 = st.search("hello", 5);
  ASSERT_TRUE("拉丁词可搜", r2.size() >= 1);
  // FTS 保留字(不崩即可)
  auto r3 = st.search("NEAR(a b)", 5);
  ASSERT_TRUE("保留字不崩", r3.size() >= 0);
  return TEST_REPORT();
}
