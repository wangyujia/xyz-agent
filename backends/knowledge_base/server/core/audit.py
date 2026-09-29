"""审计日志模块 — 记录所有写操作"""
import json
from datetime import datetime, timezone
from pathlib import Path
from typing import Optional

from server.config import AUDIT_LOG_PATH


def audit_log(
    action: str,
    path: str = "",
    role: str = "unknown",
    source: str = "http",
    detail: Optional[str] = None,
) -> None:
    """
    追加一条审计记录到 audit.log。
    
    Args:
        action: 操作类型 (write, delete, import, reindex)
        path: 操作的文档路径
        role: 鉴权角色 (admin, writer, anonymous)
        source: 调用来源 (http, mcp)
        detail: 附加说明
    """
    entry = {
        "time": datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
        "action": action,
        "path": path,
        "role": role,
        "source": source,
    }
    if detail:
        entry["detail"] = detail

    try:
        with open(AUDIT_LOG_PATH, "a", encoding="utf-8") as f:
            f.write(json.dumps(entry, ensure_ascii=False) + "\n")
    except Exception:
        # 审计日志写入失败不应阻塞主流程
        pass
