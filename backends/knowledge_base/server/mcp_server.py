"""知识库 MCP Server — 为 AI Agent 提供原生工具接口（带权限管控）"""
import sys
import json
import asyncio
from pathlib import Path

# 确保 server 包可导入
sys.path.insert(0, str(Path(__file__).parent.parent))

from mcp.server import Server
from mcp.server.stdio import stdio_server
from mcp.server.models import InitializationOptions
from mcp.types import Tool, TextContent, ServerCapabilities

from server.config import WRITE_TOKEN, ADMIN_TOKEN
from server.core.searcher import search, reindex_all, ensure_db
from server.core.reader import read_document, list_documents
from server.core.writer import write_document, ConflictError
from server.core.importer import import_raw_text
from server.core.librarian import ask
from server.core.audit import audit_log

# 创建 MCP Server
mcp = Server("knowledge-base")


# ============ 权限验证 ============

def _verify_write_token(token: str) -> str:
    """验证写权限 Token，返回角色名。无效则返回空字符串。"""
    if token == ADMIN_TOKEN:
        return "admin"
    if token == WRITE_TOKEN:
        return "writer"
    return ""


def _verify_admin_token(token: str) -> str:
    """验证管理员 Token，返回角色名。无效则返回空字符串。"""
    if token == ADMIN_TOKEN:
        return "admin"
    return ""


# ============ 工具定义 ============

TOOLS = [
    Tool(
        name="kb_search",
        description="搜索知识库。输入关键词，返回匹配的文档路径和摘要。",
        inputSchema={
            "type": "object",
            "properties": {
                "query": {"type": "string", "description": "搜索关键词"},
                "limit": {"type": "integer", "description": "返回结果数", "default": 10},
            },
            "required": ["query"],
        },
    ),
    Tool(
        name="kb_read",
        description="读取知识库文档的完整内容。传入文档相对路径。",
        inputSchema={
            "type": "object",
            "properties": {
                "path": {"type": "string", "description": "文档相对路径，如 modules/album.md"},
            },
            "required": ["path"],
        },
    ),
    Tool(
        name="kb_list",
        description="列出知识库文档。可按目录或模块过滤。",
        inputSchema={
            "type": "object",
            "properties": {
                "directory": {"type": "string", "description": "目录过滤", "default": ""},
                "module": {"type": "string", "description": "模块过滤", "default": ""},
            },
        },
    ),
    Tool(
        name="kb_ask",
        description="向知识管家提问。会搜索相关文档并用 LLM 整合回答。",
        inputSchema={
            "type": "object",
            "properties": {
                "question": {"type": "string", "description": "问题"},
            },
            "required": ["question"],
        },
    ),
    Tool(
        name="kb_write",
        description="写入/更新知识库文档。需要 token 参数验证写权限。",
        inputSchema={
            "type": "object",
            "properties": {
                "path": {"type": "string", "description": "相对路径"},
                "title": {"type": "string", "description": "文档标题"},
                "body": {"type": "string", "description": "Markdown 正文"},
                "tags": {"type": "string", "description": "逗号分隔的标签", "default": ""},
                "module": {"type": "string", "description": "所属模块", "default": ""},
                "if_match": {"type": "string", "description": "乐观锁hash", "default": ""},
                "token": {"type": "string", "description": "写权限 Token (WRITE 或 ADMIN)"},
            },
            "required": ["path", "title", "body", "token"],
        },
    ),
    Tool(
        name="kb_import",
        description="导入文档到知识库。需要 token 参数验证写权限。",
        inputSchema={
            "type": "object",
            "properties": {
                "content": {"type": "string", "description": "文档内容"},
                "title": {"type": "string", "description": "文档标题"},
                "target_dir": {"type": "string", "description": "目标目录", "default": "modules"},
                "tags": {"type": "string", "description": "逗号分隔的标签", "default": ""},
                "module": {"type": "string", "description": "所属模块", "default": ""},
                "token": {"type": "string", "description": "写权限 Token (WRITE 或 ADMIN)"},
            },
            "required": ["content", "title", "token"],
        },
    ),
    Tool(
        name="kb_reindex",
        description="重建知识库全量索引。需要 token 参数验证 ADMIN 权限。",
        inputSchema={
            "type": "object",
            "properties": {
                "token": {"type": "string", "description": "管理员 Token (ADMIN only)"},
            },
            "required": ["token"],
        },
    ),
]


# ============ 注册处理器 ============

@mcp.list_tools()
async def handle_list_tools() -> list[Tool]:
    return TOOLS


@mcp.call_tool()
async def handle_call_tool(name: str, arguments: dict) -> list[TextContent]:
    """分发工具调用（含权限校验和审计日志）"""

    if name == "kb_search":
        results = search(arguments["query"], limit=arguments.get("limit", 10))
        if not results:
            text = "未找到匹配的文档。"
        else:
            lines = [f"搜索 '{arguments['query']}' 找到 {len(results)} 个结果：\n"]
            for r in results:
                snippet = r["snippet"][:100] if r.get("snippet") else "(无摘要)"
                lines.append(f"- **{r['path']}** (score: {r['score']:.2f})\n  {snippet}")
            text = "\n".join(lines)

    elif name == "kb_read":
        doc = read_document(arguments["path"])
        if not doc:
            text = f"文档不存在: {arguments['path']}"
        else:
            fm = doc.get("frontmatter", {})
            header = f"# {doc['title']}\n"
            tags = fm.get("tags", [])
            if tags:
                header += f"标签: {', '.join(tags)}\n"
            module = fm.get("module", "")
            if module:
                header += f"模块: {module}\n"
            header += f"Hash: {doc['sha256']}\n\n---\n\n"
            text = header + doc["body"]

    elif name == "kb_list":
        docs = list_documents(
            directory=arguments.get("directory", ""),
            module=arguments.get("module") or None,
        )
        if not docs:
            text = "未找到文档。"
        else:
            lines = [f"共 {len(docs)} 个文档：\n"]
            for d in docs:
                tags_str = f" [{', '.join(d['tags'])}]" if d.get("tags") else ""
                lines.append(f"- {d['path']} — {d['title']}{tags_str}")
            text = "\n".join(lines)

    elif name == "kb_ask":
        result = await ask(arguments["question"], top_k=5)
        text = result["answer"]
        if result.get("sources"):
            text += "\n\n---\n来源: " + ", ".join(
                f"{s['title']} ({s['path']})" for s in result["sources"]
            )

    elif name == "kb_write":
        # 权限校验
        token = arguments.get("token", "")
        role = _verify_write_token(token)
        if not role:
            text = "❌ 权限拒绝：无效的写权限 Token。请在 token 参数中提供正确的 KB_WRITE_TOKEN 或 KB_ADMIN_TOKEN。"
        else:
            tags_str = arguments.get("tags", "")
            tags_list = [t.strip() for t in tags_str.split(",") if t.strip()] if tags_str else None
            module_val = arguments.get("module") or None
            match_val = arguments.get("if_match") or None

            try:
                result = write_document(
                    rel_path=arguments["path"],
                    title=arguments["title"],
                    body=arguments["body"],
                    tags=tags_list,
                    module=module_val,
                    if_match=match_val,
                )
                action = "create" if result["created"] else "update"
                audit_log(action=action, path=arguments["path"], role=role, source="mcp")
                action_cn = "创建" if result["created"] else "更新"
                text = f"✅ {action_cn}成功: {result['path']}\nHash: {result['hash']}"
            except ConflictError as e:
                text = f"⚠️ 冲突！文档已被修改。\n当前Hash: {e.current_hash}\n请用最新hash重试。"
            except ValueError as e:
                text = f"❌ 错误: {str(e)}"

    elif name == "kb_import":
        # 权限校验
        token = arguments.get("token", "")
        role = _verify_write_token(token)
        if not role:
            text = "❌ 权限拒绝：无效的写权限 Token。请在 token 参数中提供正确的 KB_WRITE_TOKEN 或 KB_ADMIN_TOKEN。"
        else:
            tags_str = arguments.get("tags", "")
            tags_list = [t.strip() for t in tags_str.split(",") if t.strip()] if tags_str else None
            module_val = arguments.get("module") or None

            result = import_raw_text(
                content=arguments["content"],
                title=arguments["title"],
                target_dir=arguments.get("target_dir", "modules"),
                tags=tags_list,
                module=module_val,
            )
            audit_log(action="import", path=result.get("path", ""), role=role, source="mcp")
            text = f"✅ 导入成功\n知识页: {result['path']}\n原始备份: {result['raw_path']}\nHash: {result['hash']}"

    elif name == "kb_reindex":
        # 管理员权限校验
        token = arguments.get("token", "")
        role = _verify_admin_token(token)
        if not role:
            text = "❌ 权限拒绝：reindex 需要 ADMIN 权限。WRITE token 不足以执行此操作。"
        else:
            count = reindex_all()
            audit_log(action="reindex", path="", role=role, source="mcp", detail=f"indexed {count} files")
            text = f"✅ 索引重建完成，共索引 {count} 个文件。"

    else:
        text = f"未知工具: {name}"

    return [TextContent(type="text", text=text)]


# ============ 入口 ============

async def main():
    ensure_db()
    async with stdio_server() as (read_stream, write_stream):
        init_options = InitializationOptions(
            server_name="knowledge-base",
            server_version="1.1.0",
            capabilities=ServerCapabilities(tools={}),
        )
        await mcp.run(read_stream, write_stream, init_options)


if __name__ == "__main__":
    asyncio.run(main())
