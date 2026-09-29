#!/usr/bin/env python3
"""Jira-to-KB bridge: pull a Jira issue, search the codebase KB, output a diagnosis.

Usage: jira_kb_bridge.py SMAR-21685
       jira_kb_bridge.py --recent SMAR 5   → check N most recent bugs

This is a consumer of kb_diagnose.py — it does NOT embed KB search logic.
"""

import json
import os
import re
import subprocess
import sys

KB_DIAGNOSE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "kb_diagnose.py")


def kb_search(query, limit=3):
    """Call kb_diagnose.py and parse its JSON output."""
    try:
        result = subprocess.run(
            ["python3", KB_DIAGNOSE, query],
            capture_output=True, text=True, timeout=30,
        )
        return json.loads(result.stdout)
    except Exception as e:
        return {"error": str(e), "matches": 0, "results": []}


def get_issue(issue_key):
    """Get Jira issue details via jira-cli."""
    try:
        result = subprocess.run(
            ["jira", "issue", "view", issue_key, "--plain"],
            capture_output=True, text=True, timeout=30,
        )
        return result.stdout
    except Exception as e:
        return None


def extract_keywords(issue_text):
    """Extract error-related keywords from issue text."""
    error_keywords = []
    for line in issue_text.split("\n"):
        line_lower = line.lower()
        if any(
            w in line_lower
            for w in ["error", "crash", "fail", "exception", "segfault",
                       "null", "timeout", "assert", "abort", "overflow"]
        ):
            words = re.findall(r"\b[a-zA-Z_][a-zA-Z0-9_]{3,}\b", line)
            error_keywords.extend(words)
    return error_keywords


def list_recent(project, count):
    """List recent bugs for a project via jira-cli."""
    try:
        result = subprocess.run(
            ["jira", "issue", "list", "-p", project, "-t", "Bug",
             "--order-by", "created", "--reverse", "-n", str(count), "--plain"],
            capture_output=True, text=True, timeout=30,
        )
        return result.stdout
    except Exception as e:
        return None


def diagnose(issue_key):
    """Pull Jira → extract keywords → search KB → return diagnosis."""
    issue_text = get_issue(issue_key)
    if issue_text is None:
        return {"issue": issue_key, "error": "jira-cli failed"}

    keywords = extract_keywords(issue_text)
    if not keywords:
        return {
            "issue": issue_key,
            "error_keywords": [],
            "kb_matches": {},
            "suggestion": "No error keywords found in issue.",
        }

    all_matches = {}
    seen = set()
    for kw in keywords[:8]:  # top 8 keywords
        if kw in seen or len(kw) < 4:
            continue
        seen.add(kw)
        r = kb_search(kw, limit=3)
        if r.get("matches", 0) > 0:
            all_matches[kw] = r["results"]

    diagnosis = {
        "issue": issue_key,
        "error_keywords": keywords[:15],
        "kb_matches": all_matches,
        "suggestion": "",
    }

    if all_matches:
        files = []
        for kw, results in all_matches.items():
            for r in results:
                symbols = r.get("symbols", "")[:60]
                files.append(f"{r['path']} ({symbols})")
        diagnosis["suggestion"] = (
            "Check these files for related code:\n" +
            "\n".join(f"  - {f}" for f in files[:8])
        )
    else:
        diagnosis["suggestion"] = (
            "No matching code found in KB. Manual investigation needed."
        )

    return diagnosis


if __name__ == "__main__":
    if len(sys.argv) < 2:
        print("Usage: jira_kb_bridge.py <ISSUE-KEY>")
        print("       jira_kb_bridge.py --recent <PROJECT> <COUNT>")
        sys.exit(1)

    if sys.argv[1] == "--recent":
        if len(sys.argv) < 4:
            print("Usage: jira_kb_bridge.py --recent <PROJECT> <COUNT>")
            sys.exit(1)
        project = sys.argv[2]
        count = int(sys.argv[3])
        output = list_recent(project, count)
        if output is None:
            print(json.dumps({"error": "jira-cli failed"}))
        else:
            # Extract issue keys from listing
            keys = re.findall(rf"{project}-\d+", output)
            results = []
            for key in keys[:5]:
                results.append(diagnose(key))
            print(json.dumps(results, indent=2, ensure_ascii=False))
        sys.exit(0)

    issue_key = sys.argv[1]
    result = diagnose(issue_key)
    print(json.dumps(result, indent=2, ensure_ascii=False))
