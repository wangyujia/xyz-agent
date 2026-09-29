"""知识管家 — LLM 整合问答"""
import httpx
from typing import Optional

from server.config import LLM_API_BASE, LLM_MODEL, LLM_API_KEY
from server.core.searcher import search
from server.core.reader import read_document


SYSTEM_PROMPT = """你是产品知识库的知识管家。用户会问你关于产品的问题，你需要根据提供的知识库文档内容来回答。

规则：
1. 只根据提供的文档内容回答，不要编造信息
2. 如果文档中没有相关信息，明确说"知识库中未找到相关信息"
3. 回答要简洁、准确、有条理
4. 如果多个文档有相关信息，综合整理后回答
5. 标注信息来源（文档路径）"""


async def ask(question: str, top_k: int = 5) -> dict:
    """
    知识管家问答接口。
    
    流程：
    1. 搜索相关文档
    2. 读取 top-k 文档内容
    3. 调用 LLM 整合回答
    
    Args:
        question: 用户问题
        top_k: 参考文档数量
    
    Returns:
        {"answer": ..., "sources": [...], "model": ...}
    """
    # 1. 搜索相关文档
    search_results = search(question, limit=top_k)

    if not search_results:
        return {
            "answer": "知识库中未找到与该问题相关的文档。",
            "sources": [],
            "model": LLM_MODEL,
        }

    # 2. 读取文档内容
    context_parts = []
    sources = []
    for result in search_results:
        doc = read_document(result["path"])
        if doc:
            context_parts.append(
                f"### 文档: {doc['title']} ({result['path']})\n\n{doc['body'][:2000]}"
            )
            sources.append({
                "path": result["path"],
                "title": doc["title"],
                "score": result["score"],
            })

    if not context_parts:
        return {
            "answer": "搜索到了相关索引但无法读取文档内容。",
            "sources": [],
            "model": LLM_MODEL,
        }

    # 3. 构建 prompt
    context_text = "\n\n---\n\n".join(context_parts)
    user_message = f"""基于以下知识库文档回答问题。

## 知识库文档

{context_text}

## 问题

{question}

请基于上述文档内容回答："""

    # 4. 调用 LLM
    try:
        async with httpx.AsyncClient(timeout=60.0) as client:
            resp = await client.post(
                f"{LLM_API_BASE}/chat/completions",
                headers={
                    "Authorization": f"Bearer {LLM_API_KEY}",
                    "Content-Type": "application/json",
                },
                json={
                    "model": LLM_MODEL,
                    "messages": [
                        {"role": "system", "content": SYSTEM_PROMPT},
                        {"role": "user", "content": user_message},
                    ],
                    "temperature": 0.3,
                    "max_tokens": 2000,
                },
            )
            resp.raise_for_status()
            data = resp.json()
            answer = data["choices"][0]["message"]["content"]
    except Exception as e:
        answer = f"LLM 调用失败: {str(e)}。\n\n以下是搜索到的相关文档供参考：\n" + "\n".join(
            f"- {s['title']} ({s['path']})" for s in sources
        )

    return {
        "answer": answer,
        "sources": sources,
        "model": LLM_MODEL,
    }
