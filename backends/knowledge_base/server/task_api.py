"""知识管家 Task API — Agent-to-Agent 通信端点 (:8901)"""
import sys
import asyncio
from pathlib import Path
from contextlib import asynccontextmanager
from dataclasses import asdict

# 确保 server 包可导入
sys.path.insert(0, str(Path(__file__).parent.parent))

from typing import Optional
from fastapi import FastAPI, HTTPException, Header, Request
from fastapi.responses import JSONResponse
from fastapi.middleware.cors import CORSMiddleware

from server.config import TASK_API_PORT, CORS_ORIGINS
from server.core.policy import get_policy, reload_policy, AgentIdentity
from server.core.task_handler import TaskHandler, TaskResult
from server.core.librarian import ask
from server.core.audit import audit_log


# ============ Lifespan ============

@asynccontextmanager
async def lifespan(app: FastAPI):
    """启动时加载策略"""
    get_policy()  # 触发加载
    yield


app = FastAPI(
    title="知识管家 Task API",
    version="1.0.0",
    description="Agent-to-Agent 任务接口 — 其他 Agent 通过此 API 向知识管家提交任务",
    lifespan=lifespan,
)

# CORS
_cors_origins = (
    ["*"] if CORS_ORIGINS == "*"
    else [o.strip() for o in CORS_ORIGINS.split(",") if o.strip()]
)
app.add_middleware(
    CORSMiddleware,
    allow_origins=_cors_origins,
    allow_methods=["*"],
    allow_headers=["*"],
)

# 全局 TaskHandler
handler = TaskHandler()


# ============ 工具函数 ============

def _identify(request: Request) -> AgentIdentity:
    """从请求 Headers 中识别 Agent 身份"""
    token = request.headers.get("x-agent-token", "")
    policy = get_policy()
    return policy.identify_by_token(token, source="http")


def _result_to_response(result: TaskResult) -> JSONResponse:
    """TaskResult → HTTP Response"""
    status_map = {
        "accepted": 200,
        "rejected": 403,
        "needs_review": 422,
        "error": 400,
    }
    http_status = status_map.get(result.status, 500)

    body = {
        "status": result.status,
        "message": result.message,
        "reviewed": result.reviewed,
    }
    if result.data:
        body["data"] = result.data

    return JSONResponse(content=body, status_code=http_status)


# ============ 核心端点 ============

@app.post("/task")
async def submit_task(request: Request):
    """
    提交任务给知识管家。
    
    Headers:
        X-Agent-Token: Agent 的访问 Token
        X-Agent-ID: (可选) Agent 自报 ID
    
    Body (JSON):
        {
            "action": "search|read|list|ask|write|archive|import",
            ... 操作相关参数
        }
    
    Returns:
        {
            "status": "accepted|rejected|needs_review|error",
            "message": "...",
            "data": {...}  // 可选
        }
    """
    identity = _identify(request)

    try:
        task = await request.json()
    except Exception:
        return JSONResponse(
            status_code=400,
            content={"status": "error", "message": "无效的 JSON 请求体"}
        )

    # 处理任务
    result = handler.handle(task, identity)

    # ask 操作是异步的，需要特殊处理
    if result.status == "accepted" and result.data and result.data.get("_async"):
        question = result.data["question"]
        try:
            ask_result = await ask(question, top_k=5)
            return JSONResponse(content={
                "status": "accepted",
                "message": ask_result.get("answer", ""),
                "data": ask_result,
                "reviewed": False,
            })
        except Exception as e:
            return JSONResponse(
                status_code=500,
                content={"status": "error", "message": f"问答服务出错: {str(e)}"}
            )

    return _result_to_response(result)


# ============ 能力发现（类 A2A Agent Card 简化版）============

@app.get("/task/capabilities")
def get_capabilities():
    """
    返回知识管家的能力描述。
    类似 A2A 的 Agent Card，供其他 Agent 了解可用操作。
    """
    return {
        "agent": {
            "name": "知识管家 (Knowledge Librarian)",
            "version": "1.0.0",
            "description": "产品知识库管理 Agent — 支持搜索、归档、问答、导入",
        },
        "actions": {
            "search": {
                "description": "搜索知识库",
                "params": {"query": "str (必填)", "limit": "int (默认 10)"},
                "permission": "read",
            },
            "read": {
                "description": "读取文档全文",
                "params": {"path": "str (必填)"},
                "permission": "read",
            },
            "list": {
                "description": "列出文档",
                "params": {"directory": "str", "module": "str"},
                "permission": "read",
            },
            "ask": {
                "description": "智能问答（搜索+LLM整合）",
                "params": {"question": "str (必填)"},
                "permission": "ask",
            },
            "write": {
                "description": "写入/更新文档",
                "params": {
                    "path": "str (必填)", "title": "str (必填)",
                    "body": "str (必填)", "tags": "list[str]",
                    "module": "str", "if_match": "str (乐观锁)"
                },
                "permission": "write (contributor 需审核)",
            },
            "archive": {
                "description": "归档内容（write 的友好别名）",
                "params": {"path": "str", "title": "str", "body": "str", "tags": "list", "module": "str"},
                "permission": "write",
            },
            "import": {
                "description": "导入文档（自动备份原文）",
                "params": {"content": "str (必填)", "title": "str (必填)", "target_dir": "str", "tags": "list"},
                "permission": "import",
            },
        },
        "auth": {
            "method": "Header Token",
            "header": "X-Agent-Token",
            "description": "通过预分配 Token 识别身份和权限",
        },
        "roles": ["admin", "writer", "contributor", "reader"],
    }


# ============ 身份查看 ============

@app.get("/task/whoami")
def whoami(request: Request):
    """查看当前 Token 对应的身份和权限"""
    identity = _identify(request)
    return {
        "agent_id": identity.agent_id,
        "role": identity.role,
        "name": identity.name,
        "trust_level": identity.trust_level,
        "permissions": identity.permissions,
        "source": identity.source,
    }


# ============ 管理端点 ============

@app.post("/task/admin/reload-policy")
def admin_reload_policy(request: Request):
    """重新加载访问策略配置（需要 admin 角色）"""
    identity = _identify(request)
    if identity.role != "admin":
        raise HTTPException(status_code=403, detail="需要 admin 权限")

    reload_policy()
    audit_log(action="reload_policy", path="access_tokens.yaml", role="admin", source="task-api")
    return {"status": "ok", "message": "策略配置已重新加载"}


@app.get("/task/admin/agents")
def admin_list_agents(request: Request):
    """列出所有注册的 Agent（需要 admin 角色）"""
    identity = _identify(request)
    if identity.role != "admin":
        raise HTTPException(status_code=403, detail="需要 admin 权限")

    policy = get_policy()
    return {"agents": policy.list_agents()}


# ============ 健康检查 ============

@app.get("/health")
def health():
    return {"status": "ok", "service": "knowledge-librarian-task-api"}


# ============ 启动 ============

if __name__ == "__main__":
    import uvicorn
    uvicorn.run(app, host="0.0.0.0", port=TASK_API_PORT)
