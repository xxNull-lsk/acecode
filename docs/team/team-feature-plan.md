# 团队功能（去中心化团队协作）— 实施方案

日期：2026-11-17
状态：方案已与用户确认，待按 OpenSpec 流程创建 change 后实施

## 需求确认记录

| 需求点 | 决策 |
|---|---|
| 设备互联 | 仅局域网/VPN 直连（邀请码含 IP:端口），不做 NAT 穿透/中继 |
| token 额度语义 | LLM tokens；每台设备跑团队会话消耗**它自己贡献**的每日额度（默认 30M），超出则该设备当日暂停承接团队会话 |
| UI 入口 | 第一版 Web 端，TUI 后续跟进 |
| 会话切换 | **不自动切换，必须手动发起**；host 失联/超额度/多次异常时，会话暂停并通知队员，由队员手动触发切换 |
| 邀请码有效期 | 从创建起最多 15 分钟有效，且一次性 |
| 新成员信息来源 | 从**邀请者**拉取全量 TeamState |
| 团队信息存储 | 独立于项目，存 `~/.acecode/team/<team_id>/`，**不**存项目目录 |
| 项目关联方式 | 规范化后的 git remote URL 匹配（无 remote 的本地仓库用仓库 UUID 兜底）；队员路径可不同但须同一 git 仓库 |
| 迁移时 git 对齐 | 接管流程自动执行 `git fetch/pull` 对齐到源设备 HEAD（不自动 commit/push） |
| 规模 | 单团队最多 50 人，最多 5 个并发团队会话 |
| 队员会话消息 | 作为会话消息流里的 human 消息（代替操作者输入，直接喂给 AI 并触发回合） |
| 邀请码 | 一次性，含邀请者 IP:端口 + 一次性 token，仅管理员可生成 |
| 账户身份 | 系统用户名+电脑名，可设显示名 |
| 元数据字段 | 团队名称、成员列表、会话列表、创建时间/人、最后更新时间/人、邀请码种子 |

## 架构落点

| 层 | 位置 | 内容 |
|---|------|------|
| 领域层 | `src/domain/team/` | TeamMeta、邀请码编解码、配额账本类型 |
| 宿主层 | `src/host/team_host/` | TeamService（团队状态机）、成员连接管理、会话托管、手动会话切换 |
| Web 层 | `src/apps/web/routes/routes_team*.cpp` + `web/handlers/` | REST + WS 路由 |
| 前端 | `web/src/` | 团队管理页 + 团队会话视图 |
| 存储 | `~/.acecode/team/<team_id>/` | 见下文数据模型 |

复用现有设施：
- Crow WS 路由模式（参照 `src/apps/web/routes/routes_peers_ws.cpp`）
- token 认证（`src/apps/web/auth.hpp`）
- 用量账本（`src/domain/session/session_usage_ledger.hpp`）
- 日额度先例（`src/host/session_share/share_store.hpp` 的 `begin_question()`）
- 邀请码编解码（`src/domain/share/invite_link.hpp`）
- 配置惯例（`config_mutation`、设置页即时保存规则）
- 网卡地址发现（`src/apps/web/remote_web.hpp`）

## 数据模型

### TeamMeta（`~/.acecode/team/<team_id>/team.json`）

- `team_id`（GUID）、名称、创建时间/创建人、最后更新时间/最后更新人
- `members[]`：账户名（用户名+机器名）、设备 GUID（设备身份主键，防重名）、显示名、角色（admin/member）、设备地址、最后在线时间
- `repo_identities[]`：规范化 git remote URL + 本地仓库 UUID 兜底
- `session_refs[]`：团队会话列表（session_id、标题、当前 host 设备、状态）
- `invite_seed`：邀请码签名种子

### 各设备本地运行态（同目录）

- `invite_ledger.json`：本机签发的一次性邀请码及消费状态
- `quota_ledger`：本设备每日团队 token 消耗（复用 usage ledger 记录，按本机时间日重置）
- `sessions/<session_id>/`：复制过来的团队会话数据（接管时落地）

## 核心机制

### 1. 邀请与加入
- 只有 admin 可生成邀请码：`acecode://team/` 扩展现有格式，编码团队 ID、团队名、邀请者设备地址+端口、一次性 token（签名防伪造）、过期时间（创建起最多 15 分钟）。
- 邀请码双重失效：15 分钟过期或被消费一次即作废，两者先到为准。
- 新加入者从邀请者拉全量 TeamState 快照（成员+会话列表+种子），落地本地目录，回执入册成为 member。
- 创建者默认 admin；admin 可提升其他成员为 admin。

### 2. 启动发现与同步
- 启动时读 `~/.acecode/team/<team_id>/` 本地信息起服务 → 扫描本地项目，按规范化 remote URL 匹配团队 → 向成员地址逐个连接 → 连上任一队员后增量同步（版本号+最后更新时间，last-writer-wins 广播）。
- 本地优先，后台同步收敛。

### 3. 团队会话
- 会话 meta 加 `team_id` 标记；权威副本只在 host 设备，host 是唯一写者。
- 其他设备经 WS 流实时查看，发起设备优先显示。
- 队员消息 → 转发到 host → 追加为 human 消息 → 触发 AI 回合。
- 准入：50 人 / 5 并发会话。

### 4. token 配额
- 每设备每日贡献额度默认 30M（可改），团队会话在 host 设备产生的消耗记在该设备自己头上。
- 当日额度用尽 → 该设备暂停承接团队会话（会话暂停，不自动切换，等待手动处理）。

### 5. 会话切换（仅手动）
- **不做自动切换**：host 心跳可检测失联、当日额度超限、多次长时间异常，但只用于状态展示与会话暂停标记，切换一律由队员手动发起。
- host 异常时会话进入「暂停」状态，各设备 UI 通知所有队员，标注原因（失联 / 超额度 / 异常）。
- 手动切换流程（UI 按钮）：队员选择一台在线且有剩余额度的目标设备 → 复制会话数据（jsonl+meta）→ 目标设备自动 `git fetch/pull` 对齐到源设备 HEAD（不自动 commit/push）→ resume → 广播新 host → 各设备 UI 通知。
- 若会话在暂停期间无响应输入，AI 回合不启动，直至接管完成后恢复。

## Web UI（第一版）

- 团队管理页：创建团队、邀请码生成（展示剩余有效时间，15 分钟内有效且一次性）、成员列表+角色管理、显示名设置、每日贡献额度设置（即时保存，无保存按钮）
- 团队会话列表：发起、查看（任意设备）、host 设备标识
- 会话视图：队员发消息入口、手动切换设备按钮、暂停/切换/配额事件通知条（含暂停原因标注）

## 实施阶段（OpenSpec change）

1. **P1 领域与存储**：身份 util 提升、TeamMeta、邀请码编解码、本地团队目录
2. **P2 成员网络**：成员连接、元数据同步、加入流程、邀请码消费、角色管理
3. **P3 团队会话**：发起、跨设备查看、队员消息、准入限制
4. **P4 配额**：日额度账本、超限拦截
5. **P5 会话切换**：host 心跳与暂停状态、手动接管流程、git fetch/pull 对齐
6. **P6 Web UI**：管理页与会话视图、docs/daemon-api.md 更新

## 设计约束

- **API key 永不跨设备**：切换设备后用目标设备自己的 provider key。
- 局域网明文 + 团队凭据（邀请码种子派生 token），不做 TLS（沿用既有协作边界，链接格式已预留扩展）。
- 团队信息不进项目目录，迁移一致性靠 git remote URL 匹配 + fetch/pull。
- host 是团队会话唯一写者，天然避免并发写冲突；不引入共识算法，广播 + last-writer-wins 即可（50 人规模）。
- 迁移只 fetch/pull，不自动 commit/push，不动队员未提交的工作区改动。

## 相关既有代码（探索结论）

- 会话存储：`src/domain/session/session_storage.hpp`（SessionMeta、jsonl、meta.json）
- peer 连接：`src/host/peers/peer_store.hpp`、`peer_tools.hpp`
- 会话分享：`src/host/session_share/share_store.hpp`、`src/domain/share/share_projection.hpp`
- 邀请链接：`src/domain/share/invite_link.hpp`（`acecode://connect/`）
- 远程 web：`src/apps/web/remote_web.hpp`、`remote_web_proxy.hpp`
- 身份函数（需提升为公共 util）：`src/adapters/feedback/feedback_upload.cpp` 的 `current_computer_name()` / `current_login_name()`
- daemon 运行时：`~/.acecode/run/`（daemon.pid/port/guid/token）、`src/base/utils/constants.hpp`（DEFAULT_WEB_PORT=12399）
- 既有 OpenSpec 参照：`openspec/changes/add-acecode-invite-links/`、`add-mesh-swarm-mode/`
