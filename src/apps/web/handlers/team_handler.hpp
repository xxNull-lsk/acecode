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
// 本机当日额度判定(承接能力)。
nlohmann::json team_state_json(const team::TeamMeta& meta,
                               const std::string& self_guid,
                               const team::QuotaLedger& ledger,
                               std::int64_t now_seconds);

// POST /api/team/<id>/session 请求体校验:cwd 必填。返回 error 空串 = 合法。
std::string validate_team_session_start_body(const nlohmann::json& body);

} // namespace acecode::web
