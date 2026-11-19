#include "team_handler.hpp"

#include "team_host/team_service.hpp"

namespace acecode::web {

nlohmann::json team_meta_public_json(const team::TeamMeta& meta) {
    nlohmann::json j;
    j["team_id"] = meta.team_id;
    j["name"] = meta.name;
    j["created_at"] = meta.created_at;
    if (!meta.created_by.empty()) j["created_by"] = meta.created_by;
    j["updated_at"] = meta.updated_at;
    if (!meta.updated_by.empty()) j["updated_by"] = meta.updated_by;
    auto repos = nlohmann::json::array();
    for (const auto& repo : meta.repos) repos.push_back({{"kind", repo.kind}, {"value", repo.value}});
    j["repos"] = repos;
    return j;
}

nlohmann::json team_member_public_json(const team::TeamMeta& meta,
                                       const team::TeamMember& member,
                                       std::int64_t now_seconds) {
    nlohmann::json j;
    j["guid"] = member.guid;
    j["account"] = member.account;
    j["display_name"] = member.display_name;
    j["role"] = member.role;
    j["online"] = now_seconds - member.last_seen <= team::kMemberStaleSeconds;
    j["last_seen"] = member.last_seen;
    const auto status = team::member_quota_snapshot(meta, member.guid, now_seconds * 1000);
    j["daily_token_quota"] = status.quota;
    j["quota_used_today"] = status.used;
    j["quota_exhausted"] = status.exhausted();
    return j;
}

nlohmann::json team_session_ref_public_json(const team::TeamSessionRef& ref) {
    nlohmann::json j;
    j["session_id"] = ref.session_id;
    if (!ref.title.empty()) j["title"] = ref.title;
    if (!ref.host_guid.empty()) j["host_guid"] = ref.host_guid;
    if (!ref.state.empty()) j["state"] = ref.state;
    if (!ref.pause_reason.empty()) j["pause_reason"] = ref.pause_reason;
    if (!ref.created_by.empty()) j["created_by"] = ref.created_by;
    if (ref.created_at != 0) j["created_at"] = ref.created_at;
    if (ref.updated_at != 0) j["updated_at"] = ref.updated_at;
    return j;
}

nlohmann::json team_state_json(const team::TeamMeta& meta,
                               const std::string& self_guid,
                               const team::QuotaLedger& ledger,
                               std::int64_t now_seconds) {
    nlohmann::json j = team_meta_public_json(meta);
    auto members = nlohmann::json::array();
    for (const auto& member : meta.members)
        members.push_back(team_member_public_json(meta, member, now_seconds));
    j["members"] = members;
    auto sessions = nlohmann::json::array();
    for (const auto& ref : meta.sessions)
        sessions.push_back(team_session_ref_public_json(ref));
    j["sessions"] = sessions;

    // 本机承接能力:额度用尽时发起/切入手稿被拒,前端用它隐藏入口并提示。
    const auto self_status = team::local_quota_status(meta, self_guid, ledger, now_seconds * 1000);
    j["self"] = {
        {"guid", self_guid},
        {"daily_token_quota", self_status.quota},
        {"quota_used_today", self_status.used},
        {"quota_exhausted", self_status.exhausted()},
    };
    return j;
}

std::string validate_team_session_start_body(const nlohmann::json& body) {
    if (!body.is_object()) return "invalid_body";
    // cwd 可缺省(默认 daemon 工作目录),但必须是字符串。
    if (body.contains("cwd") && !body["cwd"].is_string()) return "invalid_cwd";
    return {};
}

} // namespace acecode::web
