"""知识库 HTTP API — FastAPI 主入口"""
import sys
from pathlib import Path
from contextlib import asynccontextmanager

# 确保 server 包可导入
sys.path.insert(0, str(Path(__file__).parent.parent))

from typing import Optional
from fastapi import FastAPI, HTTPException, Header, Query
from fastapi.responses import JSONResponse, FileResponse
from fastapi.staticfiles import StaticFiles
from fastapi.middleware.cors import CORSMiddleware
from pydantic import BaseModel

from server.config import HOST, PORT, ADMIN_TOKEN, WRITE_TOKEN, CORS_ORIGINS
from server.core.searcher import search, reindex_all, ensure_db
from server.core.reader import read_document, list_documents
from server.core.writer import write_document, delete_document, ConflictError
from server.core.importer import import_raw_text, import_feishu_doc
from server.core.librarian import ask
from server.core.audit import audit_log


# ============ Lifespan（替代 deprecated on_event）============

@asynccontextmanager
async def lifespan(app: FastAPI):
    """启动时确保数据库存在"""
    ensure_db()
    yield


app = FastAPI(
    title="Leaptic 产品知识库",
    version="1.1.0",
    description="产品知识库 HTTP API — 支持搜索、读取、写入、导入、智能问答（带权限管控）",
    lifespan=lifespan,
)

# ============ CORS 配置 ============
# 生产环境通过 KB_CORS_ORIGINS 环境变量设置允许的域名列表（逗号分隔）
# demo 模式默认 "*" 允许所有

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


# ============ 分角色认证 ============

def _check_write_auth(x_kb_token: Optional[str] = Header(None)) -> str:
    """
    验证写权限。WRITE_TOKEN 或 ADMIN_TOKEN 均可通过。
    返回角色名称: 'admin' | 'writer'
    """
    if not x_kb_token:
        raise HTTPException(status_code=403, detail="Missing X-KB-Token header")
    if x_kb_token == ADMIN_TOKEN:
        return "admin"
    if x_kb_token == WRITE_TOKEN:
        return "writer"
    raise HTTPException(status_code=403, detail="Invalid X-KB-Token")


def _check_admin_auth(x_kb_token: Optional[str] = Header(None)) -> str:
    """
    验证管理员权限。仅 ADMIN_TOKEN 可通过。
    """
    if not x_kb_token:
        raise HTTPException(status_code=403, detail="Missing X-KB-Token header")
    if x_kb_token == ADMIN_TOKEN:
        return "admin"
    if x_kb_token == WRITE_TOKEN:
        raise HTTPException(
            status_code=403,
            detail="This operation requires ADMIN privileges. WRITE token is insufficient."
        )
    raise HTTPException(status_code=403, detail="Invalid X-KB-Token")


# ============ 请求模型 ============

class WriteRequest(BaseModel):
    path: str
    title: str
    body: str
    tags: Optional[list[str]] = None
    module: Optional[str] = None
    if_match: Optional[str] = None  # 乐观锁 hash


class ImportRequest(BaseModel):
    content: str
    title: str
    target_dir: str = "modules"
    tags: Optional[list[str]] = None
    module: Optional[str] = None
    save_raw: bool = True


class AskRequest(BaseModel):
    question: str
    top_k: int = 5


# ============ 只读接口（无认证）============

@app.get("/api/search")
def api_search(
    q: str = Query(..., description="搜索关键词"),
    limit: int = Query(20, ge=1, le=100),
):
    """搜索知识库"""
    results = search(q, limit=limit)
    return {"query": q, "count": len(results), "results": results}


@app.get("/api/doc/{path:path}")
def api_read(path: str):
    """读取文档"""
    doc = read_document(path)
    if not doc:
        raise HTTPException(status_code=404, detail=f"Document not found: {path}")
    return doc


@app.get("/api/list")
def api_list(
    directory: str = Query("", description="目录过滤"),
    module: Optional[str] = Query(None, description="模块过滤"),
):
    """列出文档"""
    docs = list_documents(directory=directory, module=module)
    return {"count": len(docs), "documents": docs}


@app.post("/api/ask")
async def api_ask(req: AskRequest):
    """知识管家智能问答"""
    result = await ask(req.question, top_k=req.top_k)
    return result


# ============ 写接口（需要 WRITE 或 ADMIN Token）============

@app.post("/api/doc")
def api_write(req: WriteRequest, x_kb_token: Optional[str] = Header(None)):
    """写入/更新文档（需要 WRITE 权限）"""
    role = _check_write_auth(x_kb_token)

    try:
        result = write_document(
            rel_path=req.path,
            title=req.title,
            body=req.body,
            tags=req.tags,
            module=req.module,
            if_match=req.if_match,
        )
        action = "create" if result["created"] else "update"
        audit_log(action=action, path=req.path, role=role, source="http")
        status_code = 201 if result["created"] else 200
        return JSONResponse(content=result, status_code=status_code)
    except ConflictError as e:
        return JSONResponse(
            status_code=409,
            content={
                "error": "conflict",
                "message": "Content has been modified by another writer",
                "current_hash": e.current_hash,
                "current_content": e.current_content,
            },
        )
    except ValueError as e:
        raise HTTPException(status_code=400, detail=str(e))


@app.post("/api/import")
def api_import(req: ImportRequest, x_kb_token: Optional[str] = Header(None)):
    """导入文档（需要 WRITE 权限）"""
    role = _check_write_auth(x_kb_token)

    result = import_raw_text(
        content=req.content,
        title=req.title,
        target_dir=req.target_dir,
        tags=req.tags,
        module=req.module,
        save_raw=req.save_raw,
    )
    audit_log(action="import", path=result.get("path", ""), role=role, source="http")
    return JSONResponse(content=result, status_code=201)


# ============ 管理接口（仅 ADMIN Token）============

@app.delete("/api/doc/{path:path}")
def api_delete(path: str, x_kb_token: Optional[str] = Header(None)):
    """删除文档（需要 ADMIN 权限）"""
    role = _check_admin_auth(x_kb_token)

    if delete_document(path):
        audit_log(action="delete", path=path, role=role, source="http")
        return {"deleted": path}
    raise HTTPException(status_code=404, detail=f"Document not found: {path}")


@app.post("/api/reindex")
def api_reindex(x_kb_token: Optional[str] = Header(None)):
    """重建全量索引（需要 ADMIN 权限）"""
    role = _check_admin_auth(x_kb_token)

    count = reindex_all()
    audit_log(action="reindex", path="", role=role, source="http", detail=f"indexed {count} files")
    return {"reindexed": count}


# ============ 权限查看接口（调试用）============

@app.get("/api/auth/check")
def api_auth_check(x_kb_token: Optional[str] = Header(None)):
    """检查当前 Token 的权限等级"""
    if not x_kb_token:
        return {"role": "anonymous", "permissions": ["read", "search", "ask"]}
    if x_kb_token == ADMIN_TOKEN:
        return {"role": "admin", "permissions": ["read", "search", "ask", "write", "import", "delete", "reindex"]}
    if x_kb_token == WRITE_TOKEN:
        return {"role": "writer", "permissions": ["read", "search", "ask", "write", "import"]}
    return {"role": "invalid", "permissions": []}


# ============ Web UI ============

STATIC_DIR = Path(__file__).parent / "static"


@app.get("/", include_in_schema=False)
async def web_ui():
    """浏览器访问根路径 → 知识库搜索页"""
    return FileResponse(STATIC_DIR / "index.html")


app.mount("/static", StaticFiles(directory=str(STATIC_DIR)), name="static")

# ============ 启动 ============

if __name__ == "__main__":
    import uvicorn
    uvicorn.run(app, host=HOST, port=PORT)
