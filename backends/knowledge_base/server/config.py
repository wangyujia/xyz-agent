"""
Knowledge Base Server 配置
"""
import os
from pathlib import Path

# 知识库根目录
KB_ROOT = Path(os.environ.get("KB_ROOT", str(Path(__file__).parent.parent)))

# SQLite 数据库路径
DB_PATH = KB_ROOT / ".search.db"

# raw 目录
RAW_DIR = KB_ROOT / "raw"

# ============ 权限 Token（分角色）============
# ADMIN: 拥有所有权限（reindex, delete, 管理操作）
# WRITE: 写入和导入（知识管家 Agent 用）
# 只读操作无需 Token

ADMIN_TOKEN = os.environ.get("KB_ADMIN_TOKEN", "")
WRITE_TOKEN = os.environ.get("KB_WRITE_TOKEN", "kb-default-token-change-me")

# 向后兼容：如果未设置 ADMIN_TOKEN，默认与 WRITE_TOKEN 相同
if not ADMIN_TOKEN:
    ADMIN_TOKEN = WRITE_TOKEN

# LLM 配置（/ask 接口用）
LLM_API_BASE = os.environ.get("KB_LLM_API_URL", "http://localhost:11434/v1")
LLM_API_KEY = os.environ.get("KB_LLM_API_KEY", "")
LLM_MODEL = os.environ.get("KB_LLM_MODEL", "qwen2.5:7b")

# 服务配置
HOST = os.environ.get("KB_HOST", "0.0.0.0")
PORT = int(os.environ.get("KB_PORT", "8900"))

# Task API 端口（Agent-to-Agent 通信）
TASK_API_PORT = int(os.environ.get("KB_TASK_API_PORT", "8901"))

# CORS 配置
# 生产环境设置为具体域名列表，逗号分隔。留空或 "*" 表示允许所有（仅 demo 用）。
CORS_ORIGINS = os.environ.get("KB_CORS_ORIGINS", "*")

# 搜索参数
MAX_SEARCH_RESULTS = 50
SNIPPET_LENGTH = 64
SEARCH_DEFAULT_LIMIT = 10
SEARCH_MAX_LIMIT = 50

# /ask 接口参数
ASK_TOP_K_DOCS = 5
ASK_MAX_CONTEXT_CHARS = 8000

# 审计日志
AUDIT_LOG_PATH = KB_ROOT / "audit.log"
