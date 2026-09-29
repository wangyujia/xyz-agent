"""搜索引擎 — jieba 分词 + SQLite FTS5"""
import sqlite3
import hashlib
from pathlib import Path
from typing import Optional

import jieba

from server.config import DB_PATH, KB_ROOT, MAX_SEARCH_RESULTS, SNIPPET_LENGTH


def _get_db() -> sqlite3.Connection:
    """获取数据库连接（WAL 模式支持并发读）"""
    DB_PATH.parent.mkdir(parents=True, exist_ok=True)
    conn = sqlite3.connect(str(DB_PATH), timeout=10)
    conn.execute("PRAGMA journal_mode=WAL")
    conn.execute("PRAGMA busy_timeout=5000")
    conn.row_factory = sqlite3.Row
    return conn


def _tokenize(text: str) -> str:
    """jieba 搜索模式分词，空格连接"""
    words = jieba.cut_for_search(text)
    return " ".join(w.strip() for w in words if w.strip())


def ensure_db():
    """确保 FTS5 表存在"""
    conn = _get_db()
    conn.execute("""
        CREATE VIRTUAL TABLE IF NOT EXISTS kb_fts USING fts5(
            path, title, tags, content,
            tokenize='unicode61'
        )
    """)
    conn.execute("""
        CREATE TABLE IF NOT EXISTS kb_meta (
            path TEXT PRIMARY KEY,
            title TEXT,
            tags TEXT,
            content_hash TEXT,
            updated_at TEXT
        )
    """)
    conn.commit()
    conn.close()


def reindex_file(filepath: Path, title: str = "", tags: str = "", content: str = ""):
    """索引单个文件"""
    conn = _get_db()
    rel_path = str(filepath.relative_to(KB_ROOT))
    content_hash = hashlib.sha256(content.encode()).hexdigest()

    # 检查是否需要更新
    row = conn.execute(
        "SELECT content_hash FROM kb_meta WHERE path = ?", (rel_path,)
    ).fetchone()

    if row and row["content_hash"] == content_hash:
        conn.close()
        return False  # 未变化

    # 分词处理
    tokenized_content = _tokenize(content)
    tokenized_title = _tokenize(title)
    tokenized_tags = _tokenize(tags)

    # 删除旧记录
    conn.execute("DELETE FROM kb_fts WHERE path = ?", (rel_path,))
    conn.execute("DELETE FROM kb_meta WHERE path = ?", (rel_path,))

    # 插入新记录
    conn.execute(
        "INSERT INTO kb_fts (path, title, tags, content) VALUES (?, ?, ?, ?)",
        (rel_path, tokenized_title, tokenized_tags, tokenized_content),
    )
    conn.execute(
        "INSERT INTO kb_meta (path, title, tags, content_hash, updated_at) VALUES (?, ?, ?, ?, datetime('now'))",
        (rel_path, title, tags, content_hash),
    )
    conn.commit()
    conn.close()
    return True


def remove_from_index(rel_path: str):
    """从索引中删除"""
    conn = _get_db()
    conn.execute("DELETE FROM kb_fts WHERE path = ?", (rel_path,))
    conn.execute("DELETE FROM kb_meta WHERE path = ?", (rel_path,))
    conn.commit()
    conn.close()


def search(query: str, limit: Optional[int] = None) -> list[dict]:
    """搜索知识库，返回匹配结果"""
    if limit is None:
        limit = MAX_SEARCH_RESULTS

    conn = _get_db()
    tokenized_query = _tokenize(query)

    results = []

    # FTS5 搜索
    try:
        rows = conn.execute(
            """
            SELECT path, rank, snippet(kb_fts, 3, '>>>', '<<<', '...', 64) as snippet
            FROM kb_fts
            WHERE kb_fts MATCH ?
            ORDER BY rank
            LIMIT ?
            """,
            (tokenized_query, limit),
        ).fetchall()

        for row in rows:
            results.append({
                "path": row["path"],
                "score": -row["rank"],  # FTS5 rank 是负数，越小越好
                "snippet": row["snippet"],
            })
    except sqlite3.OperationalError:
        # FTS 匹配失败时 fallback 到 LIKE
        pass

    # 如果 FTS5 无结果，fallback LIKE 搜索
    if not results:
        like_pattern = f"%{query}%"
        rows = conn.execute(
            """
            SELECT path FROM kb_meta
            WHERE title LIKE ? OR tags LIKE ?
            LIMIT ?
            """,
            (like_pattern, like_pattern, limit),
        ).fetchall()

        for row in rows:
            results.append({
                "path": row["path"],
                "score": 0.5,
                "snippet": "",
            })

    conn.close()
    return results


def reindex_all() -> int:
    """重建全量索引，返回索引文件数"""
    import yaml

    ensure_db()
    count = 0

    for md_file in KB_ROOT.rglob("*.md"):
        # 跳过 raw/ 目录和隐藏文件
        rel = md_file.relative_to(KB_ROOT)
        if str(rel).startswith("raw/") or str(rel).startswith("."):
            continue
        if md_file.name.startswith("."):
            continue

        content = md_file.read_text(encoding="utf-8")
        title = ""
        tags = ""

        # 解析 frontmatter
        if content.startswith("---"):
            parts = content.split("---", 2)
            if len(parts) >= 3:
                try:
                    fm = yaml.safe_load(parts[1])
                    title = fm.get("title", "")
                    tags_list = fm.get("tags", [])
                    if isinstance(tags_list, list):
                        tags = " ".join(tags_list)
                    else:
                        tags = str(tags_list)
                    content = parts[2]
                except yaml.YAMLError:
                    pass

        if not title:
            title = md_file.stem.replace("-", " ").replace("_", " ")

        if reindex_file(md_file, title, tags, content):
            count += 1

    return count
