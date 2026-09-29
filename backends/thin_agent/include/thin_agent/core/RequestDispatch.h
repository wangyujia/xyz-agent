#pragma once
// RequestDispatch.h — WS 请求线程归属判据（v0.53.82）
//
// 背景（长任务白名单律，家族第 5 次复发）：ws_agent_main 曾用一张**手维护的
// 长任务类型清单**（is_long_running）决定"投递 worker 队列 / 在 mongoose 事件
// 循环内联执行"。清单每漏一个类型，该请求就跑在事件循环里——期间心跳停发、
// accept 停摆、全部连接冻结：
//   v0.52.9  agent_decompose* 系
//   v0.53.19 spawn_agent
//   v0.53.59 orch 编排系（debate/dag/orchestrate/kanban_run/goal_auto_reason）
//   v0.53.82 又查出 6 处漏网：chat_approve（批准后续跑 FC 循环，最长数十次
//            LLM 调用）/ agent_decompose（LLM 150s 超时窗）/ cron_reply
//            （内含完整 handle_chat）/ switch_model（加载 GGUF 秒到分钟级）/
//            summarize（云端 LLM）/ task_submit（ActionExecutor+重试退避 sleep）
//
// 结论：白名单**方向反了**——穷举"谁重"必然漏，穷举"谁必须即时"才是收敛的。
// 故本头文件改为默认异步：只有下方 kInstantTypes 里的毫秒级、无 LLM/网络/
// 进程/模型加载的类型允许在事件循环内联；**未知类型一律入队**（fail-safe：
// 判断失误的代价从"整服务冻结"降为"多一跳队列延迟"）。
#include <cstddef>
#include <map>
#include <string>
#include <vector>

namespace thin_agent {

/// 是否需要在 mongoose 事件循环内**即时**处理（毫秒级、纯内存/本地轻量读）。
/// 除此之外的请求（含未知类型）一律投递 worker 队列。
bool is_instant_ws_request(const std::string& type);

/// 反向判据（ws_agent_main 用）：true = 必须离事件循环。
inline bool ws_request_needs_worker(const std::string& type) {
  return !is_instant_ws_request(type);
}

/// 即时类型清单（供测试与自描述接口使用，便于契约锁定）。
const char* const* instant_ws_request_types(std::size_t* count);

/// 不可选下标（队列空或全部 session 都在飞）。
inline constexpr std::size_t kNoPickableJob = static_cast<std::size_t>(-1);

/// 同 session 串行调度判据（v0.53.37 ws_agent_main 落地，v0.53.82 提为共享
/// 判据并补齐 agent_api——孪生拷贝律：两个 WS 服务端各写一份必然漂移）：
/// 返回 queued_sids 中第一个"其 session 无在飞任务"的下标；全部被占返回
/// kNoPickableJob。语义：同一 session 的请求必须按序串行（一条 FC 跑几十秒时
/// 第二条并发进入会让记忆/槽位交错），不同 session 可并行占满 worker 池。
std::size_t pick_unblocked_job(const std::vector<std::string>& queued_sids,
                               const std::map<std::string, int>& inflight);

}  // namespace thin_agent
