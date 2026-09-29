# 方案 C 设计稿：审批后自动续跑 FC 循环（v0.52.x 待议）

> 状态：**设计稿，未实现**。审批语义变更需要用户拍板后再动。

## 问题

当前语义（v0.49.0）：批准 = 单独执行该工具，**不续跑 FC 循环**。多步编程
任务（写文件→编译→运行）每步危险操作后用户须再发"继续"驱动模型。实测
（e2e 驱动日志 chat-2/chat-8）：continue 话术不当时模型会反问"要做什么"
或重试已批准过的写文件，一个 LRU 任务需要 3-5 次人工干预。

AgentLoop 子代理通道已有 `continue_after_approval`（批准后带工具结果
重入循环），主 FC 通道没有——同族行为不对称（与 v0.50.3/v0.51.2/v0.51.4
修过的 fast/main 不对称同构）。

## 方案

**批准后带工具结果重入 FC 循环**（对齐 AgentLoop 语义）：

1. `PendingApproval` 增加 `resume_messages`（暂停时的完整 messages 快照）
   ——暂停时机在 FC 循环内，messages 就在手边，快照成本一次 vector 拷贝。
2. `handle_chat_approve` 批准并执行工具后，不直接返回"✅已执行"，而是
   把工具结果 append 进 resume_messages，调 `run_function_calling_loop`
   续跑（迭代预算取剩余值，同样存 PendingApproval 供二次审批）。
3. **同轮多危险工具**：只批第一个，其余按现状逐个弹（保守）。
4. **迭代预算**：快照里存已用迭代数，续跑上限 = 原上限 - 已用，防无限循环。
5. **拒绝路径不变**：返回"已取消"（当前行为）。

## 风险与对冲

| 风险 | 对冲 |
|---|---|
| 批一次=放行整段后续自主行为 | 每个新的危险工具仍弹审批（循环内检查不变）；用户可控停在任一步 |
| resume_messages 快照内存 | 与会话 messages 同量级，已有 token budget 管理 |
| 审批 TTL 内消息过期 | 快照带时间戳，TTL 检查沿用 5min |
| 与 cron 无人值守路径交互 | cron 不走暂停（v0.52.1 自动拒绝），无交互 |

## 改动面估计

- `AgentService.h`：PendingApproval +2 字段（resume_messages、iters_used）
- `AgentService.cpp`：fc_approval_cb 暂停处快照（~10 行）；FC 循环签名加
  resume 入参（~15 行）；handle_chat_approve 续跑分支（~40 行）
- 单测：扩 test_hitl_fast_path / test_cron_unattended_hitl（批准续跑→
  二次审批→完成；迭代预算耗尽收尾）

预计 200 行内，一个版本（v0.53.0）。

## 备选：轻量替代（不动审批语义）

approve 响应 text 里附带结构化"下一步建议"（工具结果+原任务摘要），让
前端/驱动脚本可用固定话术续跑——只改 approve 的 text 组装（~20 行），
不改语义。适合先落这个、观察满意度再决定是否上完整方案 C。
