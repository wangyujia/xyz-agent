#!/usr/bin/env python3
"""
知识库文档导入脚本
将原始文档导入 raw/ 并生成对应的知识页

用法:
    python import.py --type prd --title "相册功能PRD" --file /path/to/doc.md
    python import.py --type spec --title "WebSocket协议规范" --stdin < doc.txt
    echo "文档内容" | python import.py --type prd --title "标题" --stdin
"""

import os
import sys
import hashlib
import argparse
from datetime import datetime

# 知识库根目录
KB_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def slugify(title):
    """标题转文件名（简单处理）"""
    import re
    # 保留英文字母、数字、中文
    slug = re.sub(r'[^\w\u4e00-\u9fff-]', '-', title.lower())
    slug = re.sub(r'-+', '-', slug).strip('-')
    return slug or 'untitled'


def compute_sha256(content):
    """计算内容哈希"""
    return hashlib.sha256(content.encode('utf-8')).hexdigest()


def import_doc(title, content, doc_type='prd', tags=None):
    """
    导入文档到知识库

    Args:
        title: 文档标题
        content: 文档正文
        doc_type: 文档类型 (prd / spec)
        tags: 标签列表

    Returns:
        dict: 导入结果信息
    """
    now = datetime.now()
    date_str = now.strftime('%Y-%m-%d')
    time_str = now.strftime('%Y-%m-%d %H:%M')

    # 1. 确定存储路径
    slug = slugify(title)
    if doc_type == 'prd':
        raw_dir = os.path.join(KB_ROOT, 'raw', 'prd')
    elif doc_type == 'spec':
        raw_dir = os.path.join(KB_ROOT, 'raw', 'specs')
    else:
        raw_dir = os.path.join(KB_ROOT, 'raw', doc_type)

    os.makedirs(raw_dir, exist_ok=True)
    raw_path = os.path.join(raw_dir, f'{slug}.md')

    # 避免覆盖
    if os.path.exists(raw_path):
        base, ext = os.path.splitext(raw_path)
        i = 2
        while os.path.exists(f"{base}-v{i}{ext}"):
            i += 1
        raw_path = f"{base}-v{i}{ext}"

    # 2. 计算 sha256
    sha = compute_sha256(content)

    # 3. 写入 raw/（带 frontmatter）
    tags_str = ', '.join(tags) if tags else doc_type
    raw_content = f"""---
title: {title}
type: {doc_type}
ingested: {date_str}
sha256: {sha}
tags: [{tags_str}]
---

{content}
"""
    with open(raw_path, 'w', encoding='utf-8') as f:
        f.write(raw_content)

    # 4. 更新 log.md
    log_path = os.path.join(KB_ROOT, 'log.md')
    rel_raw = os.path.relpath(raw_path, KB_ROOT)
    log_entry = f"[{time_str}] 导入 | {title} → {rel_raw}\n"

    with open(log_path, 'a', encoding='utf-8') as f:
        f.write(log_entry)

    # 5. 返回结果
    result = {
        'raw_path': rel_raw,
        'title': title,
        'type': doc_type,
        'sha256': sha,
        'size': len(content),
    }

    return result


def main():
    parser = argparse.ArgumentParser(description='知识库文档导入')
    parser.add_argument('--title', '-t', required=True, help='文档标题')
    parser.add_argument('--type', '-T', default='prd', choices=['prd', 'spec', 'protocol'],
                        help='文档类型')
    parser.add_argument('--tags', nargs='*', help='标签列表')
    parser.add_argument('--file', '-f', help='文档文件路径')
    parser.add_argument('--stdin', action='store_true', help='从 stdin 读取内容')

    args = parser.parse_args()

    # 读取内容
    if args.file:
        with open(args.file, 'r', encoding='utf-8') as f:
            content = f.read()
    elif args.stdin:
        content = sys.stdin.read()
    else:
        print("❌ 需要指定 --file 或 --stdin")
        sys.exit(1)

    if not content.strip():
        print("❌ 文档内容为空")
        sys.exit(1)

    # 导入
    result = import_doc(args.title, content, doc_type=args.type, tags=args.tags)

    print(f"✅ 导入成功:")
    print(f"   标题: {result['title']}")
    print(f"   路径: {result['raw_path']}")
    print(f"   类型: {result['type']}")
    print(f"   大小: {result['size']} 字符")
    print(f"   SHA256: {result['sha256'][:16]}...")
    print()
    print("💡 下一步:")
    print("   1. 根据内容创建/更新 modules/ 知识页")
    print("   2. 运行 python scripts/reindex.py 重建索引")


if __name__ == '__main__':
    main()
