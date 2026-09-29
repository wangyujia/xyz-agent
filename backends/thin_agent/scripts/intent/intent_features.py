"""Shared 64-dim intent feature extraction (must stay aligned with IntentOnnx.cpp).
   dims 0-15: legacy 16-dim keyword/slot features
   dims 16-63: char 1/2/3-gram hash buckets (48 bins, 8 salt seeds)
"""

from __future__ import annotations

import re
from typing import Any

import numpy as np

FEATURE_DIM = 64
LEGACY_DIM = 16
NGRAM_BINS = 48
# 8 salt seeds — match IntentOnnx.cpp kHashSalt
HASH_SALTS = [17, 31, 53, 79, 97, 113, 131, 157]

FEATURE_NAMES = [
    "weather_kw",
    "news_kw",
    "profile_kw",
    "status_kw",
    "memory_kw",
    "general_kw",
    "referential",
    "ctx_weather",
    "ctx_news",
    "has_city",
    "has_topic",
    "text_len_norm",
    "has_latin",
    "event_kw",
    "memory_kw_dup",
    "bias",
] + [f"ngram_h{i}" for i in range(NGRAM_BINS)]

MARKERS = [
    "刚才", "刚刚", "上次", "之前", "最后", "上一",
    "哪里", "哪儿", "哪个", "哪家",
    "什么方向", "什么话题", "哪个主题", "什么主题",
    "的呢", "如何", "怎么样", "怎样",
    "how", "what about", "last", "previous",
]

KEYWORD_GROUPS: dict[str, list[str]] = {
    "weather": ["天气", "weather", "天况", "气温", "温度"],
    "news": ["新闻", "头条", "快讯", "热点", "news"],
    "profile": ["你是谁", "你叫什么", "介绍", "能力", "who", "profile", "yourself", "ability"],
    "status": ["状态", "模型", "status", "model", "provider", "你的模型", "本地模型", "onnx"],
    "memory": ["记忆", "memory", "短期记忆", "会话记忆"],
    "general": ["查一下", "搜一下", "帮我查", "search"],
    "event": ["事件", "event"],
}

CITIES = ["上海", "深圳", "北京", "广州", "成都", "杭州", "shang hai", "shanghai", "shenzhen", "beijing"]
TOPICS = ["ai", "科技", "财经", "本地", "体育", "tech"]


def normalize(text: str) -> str:
    t = text.lower().strip()
    t = re.sub(r"[\s\?\!\.，。！？、]+", " ", t)
    return t


def keyword_hit(norm: str, words: list[str]) -> float:
    best = 0.0
    for w in words:
        w = normalize(w)
        if not w:
            continue
        if w in norm:
            best = max(best, 1.0)
        elif len(w) >= 2 and w[:2] in norm:
            best = max(best, 0.35)
    return best


def has_referential(norm: str) -> bool:
    return any(m in norm for m in MARKERS)


def detect_city(norm: str) -> bool:
    return any(c in norm for c in CITIES)


def detect_topic(norm: str) -> bool:
    return any(t in norm for t in TOPICS)


def char_ngrams(text: str, n: int) -> list[str]:
    """Character-level n-grams (strips spaces)."""
    s = text.replace(" ", "")
    return [s[i:i + n] for i in range(len(s) - n + 1)]


def fnv1a_32(s: str) -> int:
    """FNV-1a 32-bit hash — deterministic, matches C++ IntentOnnx.cpp."""
    h = 0x811C9DC5
    for ch in s:
        h = ((h ^ ord(ch)) * 0x01000193) & 0xFFFFFFFF
    return h


def extract_ngram_bins(norm: str) -> list[float]:
    """Hash char 1/2/3-grams into NGRAM_BINS buckets with 8 salt seeds (FNV-1a)."""
    bins = [0.0] * NGRAM_BINS
    for n in [1, 2, 3]:
        for ng in char_ngrams(norm, n):
            h = fnv1a_32(ng)
            for salt in HASH_SALTS:
                idx = (h ^ salt) % NGRAM_BINS
                bins[idx] += 1.0
    mx = max(bins) or 1.0
    return [min(v / mx, 1.0) for v in bins]


def extract_features(
    text: str,
    last_intent: str = "",
    last_slots: dict[str, Any] | None = None,
) -> np.ndarray:
    del last_slots  # reserved for future slot-aware features
    norm = normalize(text)
    feat = np.zeros(FEATURE_DIM, dtype=np.float32)
    # legacy 16-dim
    feat[0] = max(keyword_hit(norm, KEYWORD_GROUPS["weather"]), keyword_hit(norm, ["查天气", "看天气"]))
    feat[1] = keyword_hit(norm, KEYWORD_GROUPS["news"])
    feat[2] = keyword_hit(norm, KEYWORD_GROUPS["profile"])
    feat[3] = keyword_hit(norm, KEYWORD_GROUPS["status"])
    feat[4] = keyword_hit(norm, KEYWORD_GROUPS["memory"])
    feat[5] = keyword_hit(norm, KEYWORD_GROUPS["general"])
    feat[6] = 1.0 if has_referential(norm) else 0.0
    if last_intent == "weather":
        feat[7] = 1.0
    if last_intent == "news":
        feat[8] = 1.0
    feat[9] = 1.0 if detect_city(norm) else 0.0
    feat[10] = 1.0 if detect_topic(norm) else 0.0
    feat[11] = min(len(norm), 48) / 48.0
    feat[12] = 1.0 if re.search(r"[a-z]", norm) else 0.0
    feat[13] = keyword_hit(norm, KEYWORD_GROUPS["event"])
    feat[14] = keyword_hit(norm, KEYWORD_GROUPS["memory"])
    feat[15] = 1.0
    # ngram hash bins (dims 16-63)
    ngram_bins = extract_ngram_bins(norm)
    for i, v in enumerate(ngram_bins):
        feat[LEGACY_DIM + i] = v
    return feat
