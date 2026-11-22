#pragma once

// /api/team* 路由的纯组装逻辑(可单测)。Crow 路由注册在 routes_team.cpp。
//
// 鉴权边界(openspec add-decentralized-team 决策 6):
//   - 本机管理路由走 require_auth(daemon token);
//   - 队员互访路由(/api/team/v1/*、/ws/team/*)只认 X-ACECode-Team-Token,
//     由 TeamService::authorize 按团队派生凭据校验,与 daemon token 互不通用。

#include "team_host/quota_ledger.hpp"
#include "team/team_meta.hpp"

#include <cstdint>
#include <nlohmann/json.hpp>
#include <string>

namespace acecode::web {

// 团队元数据公开视图:剔除 invite_seed(凭据种子绝不下发)。
nlohmann::json team_meta_public_json(const team::TeamMeta& meta);

// 成员视图:基本信息 + 在线状态 + 每日额度与最后已知当日已用。
nlohmann::json team_member_public_json(const team::TeamMeta& meta,
                                       const team::TeamMember& member,
                                       std::int64_t now_seconds);

// 会话引用公开视图(列表与事件共用字段口径)。
nlohmann::json team_session_ref_public_json(const team::TeamSessionRef& ref);

// GET /api/team 与 GET /api/team/<id> 的响应:公开元数据 + 成员额度状态 +
// 本机当日额度判定(承接能力)。local 为本机团队状态(工作目录),由调用方
// 决定是否携带(绝不随广播发给其他设备);account_status 非空时 self 以
// 账号级共享池为准(额度属于登录账号,账号下所有团队共享),空则退回
// 成员记录口径。
nlohmann::json team_state_json(const team::TeamMeta& meta,
                               const std::string& self_guid,
                               const team::QuotaLedger& ledger,
                               std::int64_t now_seconds,
                               const team::TeamLocalState& local = {},
                               const team::DeviceQuotaStatus* account_status = nullptr);

// POST /api/team/<id>/session 请求体校验:cwd 必填。返回 error 空串 = 合法。
std::string validate_team_session_start_body(const nlohmann::json& body);

// POST /api/team 请求体校验:name 与 cwd 必填,repo_url 可选字符串。
std::string validate_team_create_body(const nlohmann::json& body);

// POST /api/team/join 请求体校验:link 与 cwd 必填。
std::string validate_team_join_body(const nlohmann::json& body);

// 团队会话发起 cwd 缺省链:显式 cwd → 本机团队工作目录 → 兜底(daemon
// 工作目录)。返回最终生效的 cwd。
std::string resolve_team_session_cwd(const nlohmann::json& body,
                                     const std::string& local_work_dir,
                                     const std::string& fallback);

// 团队会话引用的本机定位投影:cwd/workspace_hash 由路由层从本机会话
// meta 读出后传入,返回 {cwd, workspace_hash} 覆盖层,供侧边栏把本机
// 存在的会话当普通会话打开;两者皆空返回空对象(远程托管的会话保持
// 稀疏)。no_workspace 会话只带 cwd、不带 workspace_hash。
nlohmann::json team_session_local_info_json(const std::string& cwd,
                                            const std::string& workspace_hash);

} // namespace acecode::web
