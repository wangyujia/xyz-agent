"""文档写入器 — 乐观锁 + frontmatter 管理"""
import hashlib
from datetime import datetime
from pathlib import Path
from typing import Optional

import yaml

from server.config import KB_ROOT
from server.core.reader import _parse_frontmatter, _content_hash
from server.core.searcher import reindex_file


class ConflictError(Exception):
    """乐观锁冲突"""
    def __init__(self, current_hash: str, current_content: str):
        self.current_hash = current_hash
        self.current_content = current_content
        super().__init__("Content conflict: hash mismatch")


def write_document(
    rel_path: str,
    title: str,
    body: str,
    tags: Optional[list[str]] = None,
    module: Optional[str] = None,
    if_match: Optional[str] = None,
) -> dict:
    """
    写入/更新文档。
    
    Args:
        rel_path: 相对路径 (如 modules/app/ws-protocol.md)
        title: 文档标题
        body: Markdown 正文
        tags: 标签列表
        module: 所属模块
        if_match: 乐观锁 — 期望的当前内容 hash，不匹配则 409
    
    Returns:
        {"path": ..., "hash": ..., "created": bool}
    
    Raises:
        ConflictError: hash 不匹配
        ValueError: 路径不合法
    """
    filepath = KB_ROOT / rel_path

    # 安全检查
    try:
        filepath.resolve().relative_to(KB_ROOT.resolve())
    except ValueError:
        raise ValueError(f"Path traversal not allowed: {rel_path}")

    # 不允许写入 raw/ 和 server/
    if rel_path.startswith(("raw/", "server/")):
        raise ValueError(f"Cannot write to protected directory: {rel_path}")

    # 乐观锁检查
    is_create = not filepath.exists()
    if not is_create and if_match:
        current_content = filepath.read_text(encoding="utf-8")
        current_hash = _content_hash(current_content)
        if current_hash != if_match:
            raise ConflictError(current_hash, current_content)

    # 构建 frontmatter
    now = datetime.now().strftime("%Y-%m-%d")
    meta = {
        "title": title,
        "updated": now,
    }
    if tags:
        meta["tags"] = tags
    if module:
        meta["module"] = module

    if is_create:
        meta["created"] = now
    else:
        # 保留原 created
        old_content = filepath.read_text(encoding="utf-8")
        old_meta, _ = _parse_frontmatter(old_content)
        if old_meta.get("created"):
            meta["created"] = old_meta["created"]
        else:
            meta["created"] = now

    # 生成完整内容
    frontmatter = yaml.dump(meta, allow_unicode=True, default_flow_style=False).strip()
    full_content = f"---\n{frontmatter}\n---\n\n{body}\n"

    # 写入
    filepath.parent.mkdir(parents=True, exist_ok=True)
    filepath.write_text(full_content, encoding="utf-8")

    # 更新索引
    tags_str = " ".join(tags) if tags else ""
    reindex_file(filepath, title, tags_str, body)

    return {
        "path": rel_path,
        "hash": _content_hash(full_content),
        "created": is_create,
    }


def delete_document(rel_path: str) -> bool:
    """删除文档"""
    filepath = KB_ROOT / rel_path

    if not filepath.exists():
        return False

    # 安全检查
    try:
        filepath.resolve().relative_to(KB_ROOT.resolve())
    except ValueError:
        return False

    if rel_path.startswith(("raw/", "server/")):
        return False

    filepath.unlink()

    # 从索引删除
    from server.core.searcher import remove_from_index
    remove_from_index(rel_path)

    return True
