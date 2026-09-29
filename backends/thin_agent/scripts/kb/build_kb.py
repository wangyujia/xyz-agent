#!/usr/bin/env python3
"""Build a SQLite FTS5 knowledge base from source code repos.
Scans C/C++/Python/JS/JSON/YAML files, extracts symbols + comments,
and builds a full-text search index.
"""

import sqlite3
import os
import re
from pathlib import Path

KB_DB = os.path.expanduser("~/.thin_agent/kb/codebase.db")
REPOS = [
    "/root/code/leaptic_app",
    "/root/code/mc3302",
    "/root/code/mc3302_new/mc3302",
]
EXTENSIONS = {".c", ".cpp", ".h", ".hpp", ".py", ".js", ".json", ".yaml", ".yml", ".md", ".txt", ".cmake", ".sh"}

def extract_symbols(content, ext):
    """Extract function names, class names, structs, #define, etc."""
    symbols = set()
    if ext in (".c", ".cpp", ".h", ".hpp"):
        # Function declarations: type name(...)
        for m in re.finditer(r'(?:\w+\s+)+(\w+)\s*\([^)]*\)\s*\{?', content):
            symbols.add(m.group(1))
        # class/struct/enum definitions
        for m in re.finditer(r'(?:class|struct|enum)\s+(\w+)', content):
            symbols.add(m.group(1))
        # #define macros
        for m in re.finditer(r'#define\s+(\w+)', content):
            symbols.add(m.group(1))
        # typedef
        for m in re.finditer(r'typedef\s+.*\s+(\w+)\s*;', content):
            symbols.add(m.group(1))
    return " ".join(sorted(symbols))

def build():
    os.makedirs(os.path.dirname(KB_DB), exist_ok=True)
    conn = sqlite3.connect(KB_DB)
    conn.execute("DROP TABLE IF EXISTS codebase")
    conn.execute("""CREATE VIRTUAL TABLE codebase USING fts5(
        repo, path, filename, extension, symbols, content, tokenize='porter unicode61'
    )""")
    
    count = 0
    for repo in REPOS:
        if not os.path.isdir(repo):
            print(f"SKIP: {repo} (not found)")
            continue
        for root, dirs, files in os.walk(repo):
            # Skip build artifacts and hidden dirs
            dirs[:] = [d for d in dirs if not d.startswith('.') and d not in ('build', 'node_modules', '__pycache__', '.git')]
            for fname in files:
                ext = os.path.splitext(fname)[1].lower()
                if ext not in EXTENSIONS:
                    continue
                fpath = os.path.join(root, fname)
                try:
                    with open(fpath, 'r', errors='replace') as f:
                        content = f.read()
                except Exception:
                    continue
                if len(content) > 500000:  # skip huge files
                    continue
                symbols = extract_symbols(content, ext)
                relpath = os.path.relpath(fpath, repo)
                repo_name = os.path.basename(repo)
                conn.execute(
                    "INSERT INTO codebase (repo, path, filename, extension, symbols, content) VALUES (?,?,?,?,?,?)",
                    (repo_name, relpath, fname, ext, symbols, content)
                )
                count += 1
                if count % 100 == 0:
                    print(f"  {count} files indexed...")
    
    conn.commit()
    conn.close()
    print(f"Done: {count} files indexed to {KB_DB}")

if __name__ == "__main__":
    build()
