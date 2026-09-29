#!/usr/bin/env python3
"""
知识库搜索脚本
使用 jieba 分词 + SQLite FTS5 全文检索

用法:
    python search.py "查询关键词"
    python search.py "相册 断开" --limit 5
    python search.py "协议" --module websocket
"""

import sys
import os
import sqlite3
import argparse

# 知识库根目录
KB_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DB_PATH = os.path.join(KB_ROOT, '.search.db')

try:
    import jieba
    jieba.setLogLevel(20)  # 抑制 jieba 加载日志

    def tokenize_query(text):
        """对查询进行 jieba 分词"""
        words = jieba.cut_for_search(text)
        # 过滤掉单字符和空白
        tokens = [w.strip() for w in words if len(w.strip()) > 1]
        if not tokens:
            # fallback: 如果分词后为空，使用原文
            return text
        return ' '.join(tokens)
except ImportError:
    def tokenize_query(text):
        """无 jieba 时直接返回原文"""
        return text


def search(query, limit=10, module=None):
    """执行搜索"""
    if not os.path.exists(DB_PATH):
        print("❌ 索引不存在，请先运行 reindex.py")
        sys.exit(1)

    conn = sqlite3.connect(DB_PATH)
    conn.row_factory = sqlite3.Row

    # 分词
    tokenized = tokenize_query(query)

    # 构建查询
    if module:
        sql = """
            SELECT path, title, module, tags,
                   snippet(docs, 1, '→', '←', '...', 40) as excerpt,
                   rank
            FROM docs
            WHERE docs MATCH ?
            AND module = ?
            ORDER BY rank
            LIMIT ?
        """
        params = (tokenized, module, limit)
    else:
        sql = """
            SELECT path, title, module, tags,
                   snippet(docs, 1, '→', '←', '...', 40) as excerpt,
                   rank
            FROM docs
            WHERE docs MATCH ?
            ORDER BY rank
            LIMIT ?
        """
        params = (tokenized, limit)

    try:
        cursor = conn.execute(sql, params)
        results = cursor.fetchall()
    except sqlite3.OperationalError as e:
        # FTS5 语法错误时 fallback 到简单匹配
        print(f"⚠️ FTS5 查询失败 ({e})，尝试简单匹配...")
        sql = """
            SELECT path, title, module, tags,
                   substr(content, 1, 200) as excerpt,
                   0 as rank
            FROM docs
            WHERE content LIKE ?
            LIMIT ?
        """
        params = (f'%{query}%', limit)
        cursor = conn.execute(sql, params)
        results = cursor.fetchall()

    conn.close()

    if not results:
        print(f"🔍 未找到匹配「{query}」的结果")
        return

    print(f"🔍 搜索「{query}」→ 分词「{tokenized}」→ {len(results)} 条结果:\n")
    for i, row in enumerate(results, 1):
        print(f"  {i}. [{row['module'] or ''}] {row['title']}")
        print(f"     路径: {row['path']}")
        if row['tags']:
            print(f"     标签: {row['tags']}")
        if row['excerpt']:
            excerpt = row['excerpt'].replace('\n', ' ')[:120]
            print(f"     摘要: {excerpt}")
        print()


def main():
    parser = argparse.ArgumentParser(description='知识库搜索')
    parser.add_argument('query', help='搜索关键词')
    parser.add_argument('--limit', '-l', type=int, default=10, help='返回结果数量')
    parser.add_argument('--module', '-m', help='按模块过滤')
    args = parser.parse_args()

    search(args.query, limit=args.limit, module=args.module)


if __name__ == '__main__':
    main()
