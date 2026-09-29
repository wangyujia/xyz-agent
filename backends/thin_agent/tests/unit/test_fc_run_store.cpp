// test_fc_run_store：v0.52.26 断点续跑 #2 持久化层回归
//
// 覆盖：建表/创建 run/增量追加消息/终态/重启清扫(interrupted)/
// 可恢复加载（最新优先）/审批落盘-取走-过期清理/会话隔离。

#include "../../include/thin_agent/core/FcRunStore.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unistd.h>

using thin_agent::FcRun;
using thin_agent::FcRunStore;
using thin_agent::FcPendingApproval;

static int g_failures = 0;
#define CHECK(cond, msg) \
  do { if (!(cond)) { ++g_failures; std::printf("FAIL: %s\n", msg); } \
       else { std::printf("PASS: %s\n", msg); } } while (0)

int main() {
  const char* tmp = "/tmp/test_fc_run_store.db";
  ::unlink(tmp);

  FcRunStore store;
  CHECK(store.open(tmp), "open+建表");

  // 1) 创建 run + 增量追加
  FcRun r1;
  r1.run_id = "run_A";
  r1.session_id = "sess_1";
  r1.user_text = "帮我修一个 bug";
  r1.status = "running";
  nlohmann::json msgs = nlohmann::json::array();
  msgs.push_back({{"role", "user"}, {"content", "帮我修"}});
  r1.messages = msgs;
  CHECK(store.create_run(r1), "create_run(running)");

  nlohmann::json add1 = nlohmann::json::array();
  add1.push_back({{"role", "assistant"}, {"content", "看代码中"}});
  add1.push_back({{"role", "tool"}, {"content", "read ok"}});
  CHECK(store.append_messages("run_A", 3, add1), "append_messages(2条)");

  // 2) 重启清扫：running → interrupted
  CHECK(store.mark_interrupted_on_boot() == 1, "boot清扫 running→interrupted");

  // 3) 加载可恢复（消息合并正确+iter 更新）
  FcRun loaded = store.load_resumable("sess_1");
  CHECK(loaded.run_id == "run_A", "load_resumable 命中");
  CHECK(loaded.iter == 3, "iter=3 回读");
  CHECK(loaded.messages.is_array() && loaded.messages.size() == 3,
        "messages 合并 3 条");

  // 4) 二次清扫零命中（幂等）
  CHECK(store.mark_interrupted_on_boot() == 0, "二次清扫幂等");

  // 5) done 的 run 不可恢复
  CHECK(store.finish_run("run_A", "done"), "finish_run(done)");
  FcRun none = store.load_resumable("sess_1");
  CHECK(none.run_id.empty(), "done 后无 resumable");

  // 6) 会话隔离：sess_2 不受 sess_1 影响
  FcRun r2;
  r2.run_id = "run_B";
  r2.session_id = "sess_2";
  r2.status = "running";
  CHECK(store.create_run(r2), "create_run(sess_2)");
  store.mark_interrupted_on_boot();
  FcRun lb = store.load_resumable("sess_2");
  CHECK(lb.run_id == "run_B", "会话隔离加载");
  FcRun lc = store.load_resumable("不存在的会话");
  CHECK(lc.run_id.empty(), "未知会话返回空");

  // 7) 审批：落盘→取走（取走即删）→再取为空
  FcPendingApproval pa;
  pa.run_id = "run_B";
  pa.session_id = "sess_2";
  pa.tool_name = "shell_exec";
  pa.tool_args = "{\"cmd\":\"rm -rf /\"}";
  pa.fc_iterations_used = 5;
  pa.user_text = "危险操作请求";
  CHECK(store.save_approval(pa), "save_approval");
  FcPendingApproval got = store.take_approval("sess_2");
  CHECK(got.run_id == "run_B" && got.tool_name == "shell_exec",
        "take_approval 取到");
  CHECK(got.fc_iterations_used == 5, "审批 iterations 回读");
  FcPendingApproval empty = store.take_approval("sess_2");
  CHECK(empty.run_id.empty(), "取走即删");

  // 8) 审批过期清理
  pa.created_at_ms = 1;  // 远古时间 → 必过期
  store.save_approval(pa);
  CHECK(store.purge_expired_approvals(300) == 1, "过期审批清理");
  CHECK(store.take_approval("sess_2").run_id.empty(), "清理后取不到");

  // 9) v0.53.81 幽灵续跑不变量：审批型 run 在三条出口（续跑完成/拒绝/
  // 超时）收敛终态后，重启扫描【不得】把它改判 interrupted 而复活。
  // 此前出口不写终态 → 重启后 mark_interrupted_on_boot 一律改判
  // interrupted → load_resumable 命中【已完成的 run】=幽灵续跑。
  FcRun r3;
  r3.run_id = "run_C";
  r3.session_id = "sess_3";
  r3.status = "running";
  CHECK(store.create_run(r3), "create_run(sess_3)");
  CHECK(store.finish_run("run_C", "done"), "出口收敛 finish_run(done)");
  CHECK(store.mark_interrupted_on_boot() == 0,
        "已收敛的 run 不被 boot 扫描改判（幽灵续跑闸）");
  CHECK(store.load_resumable("sess_3").run_id.empty(),
        "已收敛 run 不可恢复");

  // 10) 三个出口的终态词汇均可落盘（denied/expired 同 done/failed 语义）
  CHECK(store.finish_run("run_B", "denied"), "出口收敛 finish_run(denied)");
  FcRun r4;
  r4.run_id = "run_D";
  r4.session_id = "sess_4";
  r4.status = "running";
  store.create_run(r4);
  CHECK(store.finish_run("run_D", "expired"), "出口收敛 finish_run(expired)");
  CHECK(store.mark_interrupted_on_boot() == 0,
        "denied/expired 亦不被 boot 扫描复活");

  ::unlink(tmp);
  if (g_failures == 0) std::printf("ALL PASS\n");
  return g_failures == 0 ? 0 : 1;
}
