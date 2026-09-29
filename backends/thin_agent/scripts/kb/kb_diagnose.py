#!/usr/bin/env python3
"""Search the thin_agent codebase KB via FTS5.
Usage: kb_diagnose.py "segfault in camera_init" → returns matching files + symbols
       kb_diagnose.py --count               → show total indexed documents

This is a pure KB search engine — no external tool dependencies.
For Jira integration, use jira_kb_bridge.py instead.
"""

import sqlite3
import json
import os
import sys

KB_DB = os.path.expanduser("~/.thin_agent/kb/codebase.db")


def search(query, limit=10):
    """FTS5 search in codebase KB."""
    if not os.path.exists(KB_DB):
        return {"error": "KB not built. Run build_kb.py first."}

    conn = sqlite3.connect(KB_DB)
    try:
        rows = conn.execute(
            "SELECT path, filename, extension, symbols, "
            "substr(content, 1, 300) "
            "FROM codebase WHERE codebase MATCH ? ORDER BY rank LIMIT ?",
            (query, limit),
        ).fetchall()
    except sqlite3.OperationalError:
        # FTS5 syntax error → fall back to LIKE
        like_q = f"%{query}%"
        rows = conn.execute(
            "SELECT path, filename, extension, symbols, "
            "substr(content, 1, 300) "
            "FROM codebase WHERE content LIKE ? OR symbols LIKE ? LIMIT ?",
            (like_q, like_q, limit),
        ).fetchall()

    results = []
    for row in rows:
        results.append(
            {
                "path": row[0],
                "file": row[1],
                "ext": row[2],
                "symbols": row[3],
                "snippet": row[4],
            }
        )
    conn.close()
    return {"query": query, "matches": len(results), "results": results}


if __name__ == "__main__":
    if len(sys.argv) < 2:
        print("Usage: kb_diagnose.py <query>")
        print("       kb_diagnose.py --count")
        sys.exit(1)

    if sys.argv[1] == "--count":
        conn = sqlite3.connect(KB_DB)
        count = conn.execute("SELECT COUNT(*) FROM codebase").fetchone()[0]
        conn.close()
        print(json.dumps({"documents": count}, indent=2))
        sys.exit(0)

    query = " ".join(sys.argv[1:])
    result = search(query)
    print(json.dumps(result, indent=2, ensure_ascii=False))
