#!/usr/bin/env python3
"""
知识库索引重建脚本
扫描所有 .md 文件，jieba 分词后写入 SQLite FTS5

用法:
    python reindex.py          # 全量重建
    python reindex.py --check  # 仅检查哪些文件需要更新
"""

import os
import sys
import re
import sqlite3
import hashlib
import argparse
import yaml

# 知识库根目录
KB_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DB_PATH = os.path.join(KB_ROOT, '.search.db')

# 需要索引的目录（排除 raw/ 的子目录单独处理）
INDEX_DIRS = ['modules', 'concepts', 'decisions', 'bugs', 'raw/prd', 'raw/specs']

try:
    import jieba
    jieba.setLogLevel(20)

    def tokenize(text):
        """jieba 分词，用空格连接"""
        words = jieba.cut_for_search(text)
        return ' '.join(words)
except ImportError:
    print("⚠️ jieba 未安装，使用逐字切分模式")
    print("   安装: pip install jieba")

    def tokenize(text):
        """逐字切分 fallback"""
        return ' '.join(text)


def parse_frontmatter(content):
    """解析 YAML frontmatter"""
    if not content.startswith('---'):
        return {}, content

    end = content.find('---', 3)
    if end == -1:
        return {}, content

    fm_text = content[3:end].strip()
    body = content[end + 3:].strip()

    try:
        fm = yaml.safe_load(fm_text) or {}
    except yaml.YAMLError:
        fm = {}

    return fm, body


def get_md_files():
    """获取所有需要索引的 .md 文件"""
    files = []
    for dir_name in INDEX_DIRS:
        dir_path = os.path.join(KB_ROOT, dir_name)
        if not os.path.exists(dir_path):
            continue
        for fname in os.listdir(dir_path):
            if fname.endswith('.md'):
                files.append(os.path.join(dir_path, fname))

    # 也索引根目录的 SCHEMA.md 和 index.md
    for fname in ['SCHEMA.md', 'index.md']:
        fpath = os.path.join(KB_ROOT, fname)
        if os.path.exists(fpath):
            files.append(fpath)

    return files


def compute_sha256(content):
    """计算内容哈希"""
    return hashlib.sha256(content.encode('utf-8')).hexdigest()


def init_db(conn):
    """初始化数据库表"""
    conn.executescript("""
        DROP TABLE IF EXISTS docs;
        DROP TABLE IF EXISTS docs_meta;

        CREATE VIRTUAL TABLE docs USING fts5(
            title,
            content,
            tags,
            module,
            path UNINDEXED,
            tokenize='unicode61'
        );

        CREATE TABLE docs_meta (
            path TEXT PRIMARY KEY,
            title TEXT,
            type TEXT,
            module TEXT,
            tags TEXT,
            created TEXT,
            updated TEXT,
            sha256 TEXT
        );
    """)


def detect_module(path, fm):
    """检测文件所属模块"""
    # 优先从 frontmatter 获取
    if fm.get('module'):
        return fm['module']

    # 从路径推断
    rel = os.path.relpath(path, KB_ROOT)
    if rel.startswith('modules/'):
        # 文件名就是模块名
        return os.path.splitext(os.path.basename(rel))[0]

    return fm.get('type', '')


def reindex(check_only=False):
    """重建索引"""
    files = get_md_files()

    if not files:
        print("📂 未找到任何 .md 文件")
        return

    if check_only:
        print(f"📂 找到 {len(files)} 个文件待索引")
        for f in files:
            print(f"   {os.path.relpath(f, KB_ROOT)}")
        return

    conn = sqlite3.connect(DB_PATH)
    init_db(conn)

    indexed = 0
    for fpath in files:
        with open(fpath, 'r', encoding='utf-8') as f:
            content = f.read()

        fm, body = parse_frontmatter(content)
        rel_path = os.path.relpath(fpath, KB_ROOT)

        title = fm.get('title', os.path.splitext(os.path.basename(fpath))[0])
        tags = fm.get('tags', [])
        if isinstance(tags, list):
            tags_str = ', '.join(str(t) for t in tags)
        else:
            tags_str = str(tags)

        module = detect_module(fpath, fm)
        doc_type = fm.get('type', '')
        created = fm.get('created', '')
        updated = fm.get('updated', '')
        sha = compute_sha256(content)

        # jieba 分词
        title_tokenized = tokenize(title)
        content_tokenized = tokenize(body)
        tags_tokenized = tokenize(tags_str)

        # 写入 FTS5
        conn.execute(
            "INSERT INTO docs(title, content, tags, module, path) VALUES (?, ?, ?, ?, ?)",
            (title_tokenized, content_tokenized, tags_tokenized, module, rel_path)
        )

        # 写入元数据
        conn.execute(
            "INSERT INTO docs_meta(path, title, type, module, tags, created, updated, sha256) "
            "VALUES (?, ?, ?, ?, ?, ?, ?, ?)",
            (rel_path, title, doc_type, module, tags_str, created, updated, sha)
        )

        indexed += 1

    conn.commit()
    conn.close()

    print(f"✅ 索引重建完成: {indexed} 个文件已索引")
    print(f"   数据库: {DB_PATH}")


def main():
    parser = argparse.ArgumentParser(description='知识库索引重建')
    parser.add_argument('--check', action='store_true', help='仅检查待索引文件')
    args = parser.parse_args()

    reindex(check_only=args.check)


if __name__ == '__main__':
    main()
