#include "team_handler.hpp"

#include "team_host/team_service.hpp"
#include "team/team_store.hpp"

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
                               std::int64_t now_seconds,
                               const team::TeamLocalState& local,
                               const team::DeviceQuotaStatus* account_status) {
    nlohmann::json j = team_meta_public_json(meta);
    auto members = nlohmann::json::array();
    for (const auto& member : meta.members) {
        if (!team::member_active(member)) continue;
        members.push_back(team_member_public_json(meta, member, now_seconds));
    }
    j["members"] = members;
    auto sessions = nlohmann::json::array();
    for (const auto& ref : meta.sessions)
        sessions.push_back(team_session_ref_public_json(ref));
    j["sessions"] = sessions;

    // 本机承接能力:额度用尽时发起/切入手稿被拒,前端用它隐藏入口并提示。
    // 传了 account_status(账号级共享池)时以它为准,否则退回成员记录口径。
    const auto self_status = account_status
        ? *account_status
        : team::local_quota_status(meta, self_guid, ledger, now_seconds * 1000);
    j["self"] = {
        {"guid", self_guid},
        {"daily_token_quota", self_status.quota},
        {"quota_used_today", self_status.used},
        {"quota_exhausted", self_status.exhausted()},
    };
    // 本机团队状态:工作目录与归档时间(每设备本地状态,调用方决定是否下发
    // 到别的设备)。
    nlohmann::json local_json = nlohmann::json::object();
    if (!local.work_dir.empty()) local_json["work_dir"] = local.work_dir;
    if (local.archived_at != 0) local_json["archived_at"] = local.archived_at;
    if (!local_json.empty()) j["local"] = std::move(local_json);
    return j;
}

std::string validate_team_session_start_body(const nlohmann::json& body) {
    if (!body.is_object()) return "invalid_body";
    // cwd 可缺省(默认 daemon 工作目录),但必须是字符串。
    if (body.contains("cwd") && !body["cwd"].is_string()) return "invalid_cwd";
    return {};
}

std::string validate_team_create_body(const nlohmann::json& body) {
    if (!body.is_object()) return "invalid_body";
    const auto name = body.find("name");
    if (name == body.end() || !name->is_string() || name->get<std::string>().empty())
        return "invalid_name";
    const auto cwd = body.find("cwd");
    if (cwd == body.end() || !cwd->is_string() || cwd->get<std::string>().empty())
        return "invalid_cwd";
    // repo_url 可省略(工作目录是 git 仓库时从目录解析);给了就必须是字符串。
    const auto repo_url = body.find("repo_url");
    if (repo_url != body.end() && !repo_url->is_string()) return "invalid_repo_url";
    return {};
}

std::string validate_team_join_body(const nlohmann::json& body) {
    if (!body.is_object()) return "invalid_body";
    const auto link = body.find("link");
    if (link == body.end() || !link->is_string() || link->get<std::string>().empty())
        return "invalid_link";
    const auto cwd = body.find("cwd");
    if (cwd == body.end() || !cwd->is_string() || cwd->get<std::string>().empty())
        return "invalid_cwd";
    return {};
}

std::string resolve_team_session_cwd(const nlohmann::json& body,
                                     const std::string& local_work_dir,
                                     const std::string& fallback) {
    if (body.is_object()) {
        const auto cwd = body.find("cwd");
        if (cwd != body.end() && cwd->is_string() && !cwd->get<std::string>().empty())
            return cwd->get<std::string>();
    }
    if (!local_work_dir.empty()) return local_work_dir;
    return fallback;
}

nlohmann::json team_session_local_info_json(const std::string& cwd,
                                            const std::string& workspace_hash) {
    nlohmann::json j = nlohmann::json::object();
    if (cwd.empty()) return j;
    j["cwd"] = cwd;
    if (!workspace_hash.empty()) j["workspace_hash"] = workspace_hash;
    return j;
}

} // namespace acecode::web
