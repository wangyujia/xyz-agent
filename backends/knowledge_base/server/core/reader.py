"""文档读取器 — 解析 frontmatter、计算 hash"""
import hashlib
from pathlib import Path
from typing import Optional

import yaml

from server.config import KB_ROOT


def _content_hash(content: str) -> str:
    """计算内容 sha256"""
    return hashlib.sha256(content.encode("utf-8")).hexdigest()


def _parse_frontmatter(content: str) -> tuple[dict, str]:
    """解析 YAML frontmatter，返回 (metadata, body)"""
    if not content.startswith("---"):
        return {}, content

    parts = content.split("---", 2)
    if len(parts) < 3:
        return {}, content

    try:
        fm = yaml.safe_load(parts[1]) or {}
    except yaml.YAMLError:
        fm = {}

    body = parts[2].strip()
    return fm, body


def read_document(rel_path: str) -> Optional[dict]:
    """
    读取知识库文档。

    Args:
        rel_path: 相对于 KB_ROOT 的路径 (如 modules/album.md)

    Returns:
        {"path", "title", "body", "content", "frontmatter", "sha256"} 或 None
    """
    filepath = KB_ROOT / rel_path

    # 安全检查
    try:
        filepath.resolve().relative_to(KB_ROOT.resolve())
    except ValueError:
        return None

    if not filepath.exists():
        return None

    content = filepath.read_text(encoding="utf-8")
    fm, body = _parse_frontmatter(content)
    sha = _content_hash(content)

    return {
        "path": rel_path,
        "title": fm.get("title", filepath.stem.replace("-", " ").replace("_", " ")),
        "body": body,
        "content": content,
        "frontmatter": fm,
        "sha256": sha,
    }


def list_documents(directory: str = "", module: Optional[str] = None) -> list[dict]:
    """列出知识库文档"""
    if directory:
        scan_dir = KB_ROOT / directory
    else:
        scan_dir = KB_ROOT

    if not scan_dir.exists():
        return []

    results = []
    for md_file in sorted(scan_dir.rglob("*.md")):
        # 跳过 raw/ server/ 隐藏文件
        rel = str(md_file.relative_to(KB_ROOT))
        if rel.startswith(("raw/", "server/", ".")):
            continue
        if md_file.name.startswith("."):
            continue

        content = md_file.read_text(encoding="utf-8")
        fm, _ = _parse_frontmatter(content)

        # 模块过滤
        if module and fm.get("module") != module:
            continue

        results.append({
            "path": rel,
            "title": fm.get("title", md_file.stem),
            "module": fm.get("module", ""),
            "tags": fm.get("tags", []),
            "updated": fm.get("updated", ""),
        })

    return results
