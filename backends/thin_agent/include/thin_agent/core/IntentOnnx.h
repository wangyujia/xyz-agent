#pragma once

#include <string>

#include <nlohmann/json.hpp>

#include "thin_agent/core/IntentScorer.h"

namespace thin_agent {

/// 本地 ONNX 意图模型推理（当前为 profile 置信度 stub）。
/// AgentService::classify_local_intent 在规则不确定时可选调用，成功则 intent_backend=onnx。
bool infer_profile_confidence_onnx(const std::string& model_path,
                                   double* confidence,
                                   std::string* error);

/// 多意图 ONNX 分类（特征向量 + intent_multiclass.onnx）。
/// 返回 JSON：intent、confidence、backend=onnx、slots；失败或禁用时 intent=unknown。
nlohmann::json classify_intent_onnx(const std::string& text, const DialogContext* dialog = nullptr);

/// 是否启用 ONNX 多意图（可通过 THIN_AGENT_INTENT_ONNX=0 关闭）。
bool intent_onnx_enabled();

}  // namespace thin_agent
