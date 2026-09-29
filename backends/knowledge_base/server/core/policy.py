"""访问策略引擎 — 身份识别 + 权限校验"""
from dataclasses import dataclass, field
from pathlib import Path
from typing import Optional

import yaml

from server.config import KB_ROOT


# ============ 数据模型 ============

@dataclass
class AgentIdentity:
    """识别出的 Agent 身份"""
    agent_id: str
    role: str
    name: str
    trust_level: str  # full / trusted / review
    permissions: list[str] = field(default_factory=list)
    source: str = ""  # http / feishu / mcp


@dataclass
class Decision:
    """权限校验结果"""
    allowed: bool
    reason: str = ""
    requires_review: bool = False  # 需要管家 LLM 审核


# ============ 策略引擎 ============

class AccessPolicy:
    """加载 access_tokens.yaml 并提供身份识别和权限校验"""

    def __init__(self, config_path: Optional[Path] = None):
        self._config_path = config_path or (KB_ROOT / "access_tokens.yaml")
        self._config: dict = {}
        self._roles: dict = {}
        self._agents: dict = {}
        self._feishu_users: dict = {}
        self._default_role: str = "reader"
        self.reload()

    def reload(self):
        """重新加载配置文件"""
        if not self._config_path.exists():
            # 无配置文件时用默认值
            self._config = {}
            self._roles = {
                "admin": {"permissions": ["read", "search", "ask", "write", "import", "delete", "reindex"], "trust_level": "full"},
                "writer": {"permissions": ["read", "search", "ask", "write", "import"], "trust_level": "trusted"},
                "contributor": {"permissions": ["read", "search", "ask", "suggest_write"], "trust_level": "review"},
                "reader": {"permissions": ["read", "search", "ask"], "trust_level": "full"},
            }
            self._agents = {}
            self._feishu_users = {}
            return

        with open(self._config_path, "r", encoding="utf-8") as f:
            self._config = yaml.safe_load(f) or {}

        self._roles = self._config.get("roles", {})
        self._agents = self._config.get("agents", {})
        self._feishu_users = self._config.get("feishu_users", {})
        self._default_role = self._config.get("default_role", "reader")

    def identify_by_token(self, token: str, source: str = "http") -> AgentIdentity:
        """通过 Token 识别 Agent 身份"""
        agent_info = self._agents.get(token)
        if agent_info:
            role_name = agent_info["role"]
            role_def = self._roles.get(role_name, {})
            return AgentIdentity(
                agent_id=agent_info["agent_id"],
                role=role_name,
                name=agent_info.get("name", agent_info["agent_id"]),
                trust_level=role_def.get("trust_level", "full"),
                permissions=role_def.get("permissions", []),
                source=source,
            )

        # Token 不在配置中 → 默认角色
        return self._make_default_identity(source)

    def identify_by_feishu_user(self, user_id: str) -> AgentIdentity:
        """通过飞书 user_id 识别身份"""
        user_info = self._feishu_users.get(user_id)
        if user_info:
            role_name = user_info["role"]
            role_def = self._roles.get(role_name, {})
            return AgentIdentity(
                agent_id=f"feishu:{user_id}",
                role=role_name,
                name=user_info.get("name", f"feishu-user-{user_id[:8]}"),
                trust_level=role_def.get("trust_level", "full"),
                permissions=role_def.get("permissions", []),
                source="feishu",
            )

        return self._make_default_identity("feishu")

    def authorize(self, identity: AgentIdentity, action: str) -> Decision:
        """
        校验身份是否有权执行指定操作。
        
        Returns:
            Decision(allowed=True/False, requires_review=...)
        """
        # 操作是否在权限列表中
        if action in identity.permissions:
            return Decision(allowed=True)

        # 特殊：contributor 的 suggest_write 覆盖 write/import
        if action in ("write", "import") and "suggest_write" in identity.permissions:
            return Decision(
                allowed=True,
                requires_review=True,
                reason=f"Agent '{identity.name}' ({identity.role}) 只有 suggest_write 权限，需要管家审核后执行"
            )

        return Decision(
            allowed=False,
            reason=f"Agent '{identity.name}' (角色: {identity.role}) 无权执行 '{action}' 操作。"
                   f" 当前权限: {', '.join(identity.permissions)}"
        )

    def get_role_info(self, role_name: str) -> dict:
        """获取角色定义"""
        return self._roles.get(role_name, {})

    def list_agents(self) -> list[dict]:
        """列出所有已注册的 Agent"""
        result = []
        for token_prefix, info in self._agents.items():
            # 不暴露完整 Token，只显示前 8 位
            result.append({
                "agent_id": info["agent_id"],
                "name": info.get("name", ""),
                "role": info["role"],
                "token_prefix": token_prefix[:8] + "...",
            })
        return result

    def _make_default_identity(self, source: str) -> AgentIdentity:
        """构造默认角色身份"""
        role_def = self._roles.get(self._default_role, {})
        return AgentIdentity(
            agent_id="anonymous",
            role=self._default_role,
            name="未识别来源",
            trust_level=role_def.get("trust_level", "full"),
            permissions=role_def.get("permissions", ["read", "search", "ask"]),
            source=source,
        )


# ============ 全局单例 ============

_policy: Optional[AccessPolicy] = None


def get_policy() -> AccessPolicy:
    """获取全局策略实例（懒加载）"""
    global _policy
    if _policy is None:
        _policy = AccessPolicy()
    return _policy


def reload_policy():
    """重新加载策略配置"""
    global _policy
    _policy = AccessPolicy()
