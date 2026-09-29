"""文档导入器 — 从原始文本/文件导入到知识库"""
import re
from datetime import datetime
from pathlib import Path
from typing import Optional

from server.config import KB_ROOT, RAW_DIR
from server.core.writer import write_document


def _slugify(text: str) -> str:
    """生成文件名友好的 slug"""
    # 保留中文、字母、数字
    text = text.strip().lower()
    text = re.sub(r'[^\w\u4e00-\u9fff-]', '-', text)
    text = re.sub(r'-+', '-', text)
    return text.strip('-')[:60]


def import_raw_text(
    content: str,
    title: str,
    target_dir: str = "modules",
    tags: Optional[list[str]] = None,
    module: Optional[str] = None,
    save_raw: bool = True,
) -> dict:
    """
    导入原始文本到知识库。
    
    流程：
    1. (可选) 保存原始内容到 raw/
    2. 写入 Markdown 知识页到目标目录
    
    Args:
        content: 原始文本内容
        title: 文档标题
        target_dir: 目标目录 (modules/app, bugs, decisions 等)
        tags: 标签列表
        module: 所属模块
        save_raw: 是否保存原始副本
    
    Returns:
        {"path": ..., "raw_path": ..., "hash": ...}
    """
    slug = _slugify(title)
    if not slug:
        slug = datetime.now().strftime("%Y%m%d-%H%M%S")

    # 保存原始文本
    raw_path = None
    if save_raw:
        raw_file = RAW_DIR / target_dir / f"{slug}.md"
        raw_file.parent.mkdir(parents=True, exist_ok=True)
        raw_file.write_text(content, encoding="utf-8")
        raw_path = str(raw_file.relative_to(KB_ROOT))

    # 写入知识页
    rel_path = f"{target_dir}/{slug}.md"
    result = write_document(
        rel_path=rel_path,
        title=title,
        body=content,
        tags=tags,
        module=module,
    )

    result["raw_path"] = raw_path
    return result


def import_feishu_doc(
    doc_content: str,
    title: str,
    target_dir: str = "modules",
    tags: Optional[list[str]] = None,
    module: Optional[str] = None,
) -> dict:
    """
    导入飞书文档内容。
    
    飞书文档通过粘贴/API获取纯文本后调用此函数。
    后续 Phase 2 可通过飞书 API 自动拉取。
    """
    # 清理飞书格式（去掉多余空行等）
    cleaned = re.sub(r'\n{3,}', '\n\n', doc_content.strip())

    return import_raw_text(
        content=cleaned,
        title=title,
        target_dir=target_dir,
        tags=tags,
        module=module,
        save_raw=True,
    )
