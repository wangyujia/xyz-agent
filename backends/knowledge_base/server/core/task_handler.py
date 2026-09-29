"""任务处理器 — 管家审核逻辑（Layer 2: LLM 判断力）"""
from dataclasses import dataclass
from typing import Optional

from server.core.policy import AgentIdentity, Decision, get_policy
from server.core.searcher import search
from server.core.writer import write_document, ConflictError
from server.core.importer import import_raw_text
from server.core.reader import read_document, list_documents
from server.core.audit import audit_log


# ============ 任务类型 ============

TASK_ACTIONS = {
    "search": "read",
    "read": "read",
    "list": "read",
    "ask": "ask",
    "write": "write",
    "import": "import",
    "archive": "write",     # archive = write 的友好别名
}


# ============ 任务结果 ============

@dataclass
class TaskResult:
    """任务执行结果"""
    status: str           # accepted / rejected / needs_review / error
    message: str          # 给调用方的回复
    data: Optional[dict] = None  # 结构化数据
    reviewed: bool = False       # 是否经过管家审核


# ============ 审核引擎（Layer 2）============

class ReviewEngine:
    """
    管家审核逻辑。
    当 trust_level=review 时，执行内容质量和合理性检查。
    这里用规则实现基础审核，真正的 LLM 判断在管家 Agent Skill 中定义。
    """

    @staticmethod
    def review_write(title: str, body: str, path: str, tags: list, module: str) -> TaskResult:
        """审核写入请求"""
        issues = []

        # 1. 内容质量检查
        if not title or len(title.strip()) < 2:
            issues.append("标题过短或为空")
        if not body or len(body.strip()) < 30:
            issues.append("正文内容过短（需至少 30 字）")

        # 2. 路径合理性检查
        valid_dirs = ["modules/", "bugs/", "concepts/", "decisions/", "requirements/"]
        if path and not any(path.startswith(d) for d in valid_dirs):
            # 自动修正：根据内容猜测目录
            suggested_dir = ReviewEngine._suggest_directory(title, tags)
            if suggested_dir:
                issues.append(f"路径 '{path}' 不在标准目录中，建议使用 '{suggested_dir}'")

        # 3. 重复检测
        if title:
            existing = search(title, limit=3)
            for doc in existing:
                # 如果标题相似度很高（简单判断：标题出现在搜索结果中）
                if doc.get("title") and title.lower() in doc["title"].lower():
                    issues.append(f"可能与现有文档重复: {doc['path']} ({doc['title']})")
                    break

        # 4. 标签规范
        if not tags:
            issues.append("缺少标签（建议至少 2 个）")

        if issues:
            return TaskResult(
                status="needs_review",
                message="管家审核发现以下问题：\n" + "\n".join(f"  • {i}" for i in issues)
                        + "\n\n请修正后重新提交，或联系管理员直接写入。",
                data={"issues": issues},
                reviewed=True,
            )

        return TaskResult(status="accepted", message="审核通过", reviewed=True)

    @staticmethod
    def _suggest_directory(title: str, tags: list) -> str:
        """根据标题和标签建议目录"""
        title_lower = title.lower()
        tags_lower = [t.lower() for t in tags] if tags else []

        if any(w in title_lower for w in ["bug", "缺陷", "问题", "crash", "异常"]):
            return "bugs/"
        if any(w in title_lower for w in ["决策", "选型", "方案"]):
            return "decisions/"
        if any(w in title_lower for w in ["概念", "原理", "协议"]):
            return "concepts/"
        if any(w in title_lower for w in ["需求", "prd", "功能"]):
            return "requirements/"
        if "bug" in tags_lower:
            return "bugs/"
        return "modules/"


# ============ 主任务处理器 ============

class TaskHandler:
    """
    处理从 Task API 收到的任务请求。
    
    流程:
      1. 识别身份 (policy layer)
      2. 权限校验 (policy layer)
      3. 审核 (review engine, 仅 contributor)
      4. 执行
      5. 审计日志
    """

    def __init__(self):
        self.review_engine = ReviewEngine()

    def handle(self, task: dict, identity: AgentIdentity) -> TaskResult:
        """
        处理任务。
        
        Args:
            task: 任务请求 dict (包含 action + 参数)
            identity: 已识别的身份
        
        Returns:
            TaskResult
        """
        action = task.get("action", "")
        if not action:
            return TaskResult(status="error", message="缺少 action 字段")

        # 映射到权限动作
        perm_action = TASK_ACTIONS.get(action)
        if not perm_action:
            return TaskResult(
                status="error",
                message=f"未知操作: {action}。支持的操作: {', '.join(TASK_ACTIONS.keys())}"
            )

        # 权限校验
        policy = get_policy()
        decision = policy.authorize(identity, perm_action)

        if not decision.allowed:
            audit_log(
                action=f"denied:{action}",
                path=task.get("path", ""),
                role=identity.role,
                source=identity.source,
                detail=decision.reason,
            )
            return TaskResult(status="rejected", message=decision.reason)

        # 需要审核？
        if decision.requires_review:
            review_result = self._do_review(action, task, identity)
            if review_result.status != "accepted":
                audit_log(
                    action=f"review_blocked:{action}",
                    path=task.get("path", ""),
                    role=identity.role,
                    source=identity.source,
                    detail=review_result.message,
                )
                return review_result

        # 执行任务
        return self._execute(action, task, identity)

    def _do_review(self, action: str, task: dict, identity: AgentIdentity) -> TaskResult:
        """执行审核"""
        if action in ("write", "archive"):
            return self.review_engine.review_write(
                title=task.get("title", ""),
                body=task.get("body", ""),
                path=task.get("path", ""),
                tags=task.get("tags", []),
                module=task.get("module", ""),
            )
        if action == "import":
            return self.review_engine.review_write(
                title=task.get("title", ""),
                body=task.get("content", ""),
                path=task.get("target_dir", "modules") + "/imported.md",
                tags=task.get("tags", []),
                module=task.get("module", ""),
            )
        # 其他操作不需要审核
        return TaskResult(status="accepted", message="")

    def _execute(self, action: str, task: dict, identity: AgentIdentity) -> TaskResult:
        """执行具体操作"""
        try:
            if action == "search":
                return self._exec_search(task)
            elif action == "read":
                return self._exec_read(task)
            elif action == "list":
                return self._exec_list(task)
            elif action == "ask":
                return self._exec_ask(task)
            elif action in ("write", "archive"):
                return self._exec_write(task, identity)
            elif action == "import":
                return self._exec_import(task, identity)
            else:
                return TaskResult(status="error", message=f"操作 {action} 暂未实现")
        except Exception as e:
            return TaskResult(status="error", message=f"执行出错: {str(e)}")

    def _exec_search(self, task: dict) -> TaskResult:
        query = task.get("query", "")
        if not query:
            return TaskResult(status="error", message="search 操作需要 query 参数")
        results = search(query, limit=task.get("limit", 10))
        return TaskResult(
            status="accepted",
            message=f"搜索 '{query}' 找到 {len(results)} 个结果",
            data={"results": results},
        )

    def _exec_read(self, task: dict) -> TaskResult:
        path = task.get("path", "")
        if not path:
            return TaskResult(status="error", message="read 操作需要 path 参数")
        doc = read_document(path)
        if not doc:
            return TaskResult(status="error", message=f"文档不存在: {path}")
        return TaskResult(
            status="accepted",
            message=f"读取成功: {path}",
            data=doc,
        )

    def _exec_list(self, task: dict) -> TaskResult:
        docs = list_documents(
            directory=task.get("directory", ""),
            module=task.get("module"),
        )
        return TaskResult(
            status="accepted",
            message=f"共 {len(docs)} 个文档",
            data={"documents": docs},
        )

    def _exec_ask(self, task: dict) -> TaskResult:
        # ask 是异步的，在 Task API 层做 asyncio.run
        question = task.get("question", "")
        if not question:
            return TaskResult(status="error", message="ask 操作需要 question 参数")
        # 返回占位，实际在 task_api 里异步执行
        return TaskResult(
            status="accepted",
            message="",
            data={"question": question, "_async": True},
        )

    def _exec_write(self, task: dict, identity: AgentIdentity) -> TaskResult:
        path = task.get("path", "")
        title = task.get("title", "")
        body = task.get("body", "")

        if not path or not title or not body:
            return TaskResult(status="error", message="write 操作需要 path, title, body 参数")

        tags = task.get("tags")
        if isinstance(tags, str):
            tags = [t.strip() for t in tags.split(",") if t.strip()]

        try:
            result = write_document(
                rel_path=path,
                title=title,
                body=body,
                tags=tags,
                module=task.get("module"),
                if_match=task.get("if_match"),
            )
            action_cn = "创建" if result["created"] else "更新"
            audit_log(
                action="create" if result["created"] else "update",
                path=path,
                role=identity.role,
                source=identity.source,
                detail=f"by {identity.name} ({identity.agent_id})",
            )
            return TaskResult(
                status="accepted",
                message=f"✅ {action_cn}成功: {result['path']}",
                data=result,
            )
        except ConflictError as e:
            return TaskResult(
                status="error",
                message=f"冲突：文档已被修改。当前 Hash: {e.current_hash}",
                data={"current_hash": e.current_hash},
            )
        except ValueError as e:
            return TaskResult(status="error", message=str(e))

    def _exec_import(self, task: dict, identity: AgentIdentity) -> TaskResult:
        content = task.get("content", "")
        title = task.get("title", "")
        if not content or not title:
            return TaskResult(status="error", message="import 操作需要 content, title 参数")

        tags = task.get("tags")
        if isinstance(tags, str):
            tags = [t.strip() for t in tags.split(",") if t.strip()]

        result = import_raw_text(
            content=content,
            title=title,
            target_dir=task.get("target_dir", "modules"),
            tags=tags,
            module=task.get("module"),
        )
        audit_log(
            action="import",
            path=result.get("path", ""),
            role=identity.role,
            source=identity.source,
            detail=f"by {identity.name} ({identity.agent_id})",
        )
        return TaskResult(
            status="accepted",
            message=f"✅ 导入成功: {result['path']}",
            data=result,
        )
