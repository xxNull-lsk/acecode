#include "team/team_meta.hpp"

#include <algorithm>

namespace acecode::team {
namespace {

std::string opt_string(const nlohmann::json& j, const char* key) {
    if (auto it = j.find(key); it != j.end() && it->is_string()) return *it;
    return {};
}

std::int64_t opt_int(const nlohmann::json& j, const char* key) {
    if (auto it = j.find(key); it != j.end() && it->is_number_integer())
        return it->get<std::int64_t>();
    return 0;
}

// 字符串字段只在有值时写出;空值省略让同步 diff 与旧版本读取都简单。
void set_opt_string(nlohmann::json& j, const char* key, const std::string& value) {
    if (!value.empty()) j[key] = value;
}

void set_opt_int(nlohmann::json& j, const char* key, std::int64_t value) {
    if (value != 0) j[key] = value;
}

} // namespace

const TeamMember* find_member(const TeamMeta& meta, const std::string& guid) {
    if (guid.empty()) return nullptr;
    auto it = std::find_if(meta.members.begin(), meta.members.end(),
        [&](const TeamMember& m) { return m.guid == guid; });
    return it == meta.members.end() ? nullptr : &*it;
}

TeamMember* find_member(TeamMeta& meta, const std::string& guid) {
    if (guid.empty()) return nullptr;
    auto it = std::find_if(meta.members.begin(), meta.members.end(),
        [&](const TeamMember& m) { return m.guid == guid; });
    return it == meta.members.end() ? nullptr : &*it;
}

const TeamSessionRef* find_session(const TeamMeta& meta, const std::string& session_id) {
    if (session_id.empty()) return nullptr;
    auto it = std::find_if(meta.sessions.begin(), meta.sessions.end(),
        [&](const TeamSessionRef& s) { return s.session_id == session_id; });
    return it == meta.sessions.end() ? nullptr : &*it;
}

TeamSessionRef* find_session(TeamMeta& meta, const std::string& session_id) {
    if (session_id.empty()) return nullptr;
    auto it = std::find_if(meta.sessions.begin(), meta.sessions.end(),
        [&](const TeamSessionRef& s) { return s.session_id == session_id; });
    return it == meta.sessions.end() ? nullptr : &*it;
}

nlohmann::json member_to_json(const TeamMember& member) {
    nlohmann::json j;
    set_opt_string(j, "account", member.account);
    set_opt_string(j, "guid", member.guid);
    set_opt_string(j, "display_name", member.display_name);
    set_opt_string(j, "role", member.role);
    set_opt_string(j, "address", member.address);
    set_opt_int(j, "last_seen", member.last_seen);
    set_opt_int(j, "updated_at", member.updated_at);
    set_opt_string(j, "updated_by", member.updated_by);
    // 额度字段:quota 总是写出(显式 0 有语义);用量快照非零才写。
    j["daily_token_quota"] = member.daily_token_quota;
    set_opt_int(j, "quota_used_today", member.quota_used_today);
    set_opt_int(j, "quota_day", member.quota_day);
    return j;
}

TeamMember member_from_json(const nlohmann::json& j) {
    TeamMember member;
    member.account = opt_string(j, "account");
    member.guid = opt_string(j, "guid");
    member.display_name = opt_string(j, "display_name");
    member.role = opt_string(j, "role");
    member.address = opt_string(j, "address");
    member.last_seen = opt_int(j, "last_seen");
    member.updated_at = opt_int(j, "updated_at");
    member.updated_by = opt_string(j, "updated_by");
    // 老版本成员记录没有该字段:按默认 30M 读回。
    if (auto it = j.find("daily_token_quota");
        it != j.end() && it->is_number_integer() && it->get<std::int64_t>() >= 0) {
        member.daily_token_quota = it->get<std::int64_t>();
    }
    member.quota_used_today = opt_int(j, "quota_used_today");
    member.quota_day = opt_int(j, "quota_day");
    return member;
}

nlohmann::json session_ref_to_json(const TeamSessionRef& ref) {
    nlohmann::json j;
    set_opt_string(j, "session_id", ref.session_id);
    set_opt_string(j, "title", ref.title);
    set_opt_string(j, "host_guid", ref.host_guid);
    set_opt_string(j, "state", ref.state);
    set_opt_string(j, "pause_reason", ref.pause_reason);
    set_opt_string(j, "created_by", ref.created_by);
    set_opt_int(j, "created_at", ref.created_at);
    set_opt_int(j, "updated_at", ref.updated_at);
    return j;
}

TeamSessionRef session_ref_from_json(const nlohmann::json& j) {
    TeamSessionRef ref;
    ref.session_id = opt_string(j, "session_id");
    ref.title = opt_string(j, "title");
    ref.host_guid = opt_string(j, "host_guid");
    ref.state = opt_string(j, "state");
    ref.pause_reason = opt_string(j, "pause_reason");
    ref.created_by = opt_string(j, "created_by");
    ref.created_at = opt_int(j, "created_at");
    ref.updated_at = opt_int(j, "updated_at");
    return ref;
}

nlohmann::json repo_identity_to_json(const RepoIdentity& repo) {
    nlohmann::json j;
    set_opt_string(j, "kind", repo.kind);
    set_opt_string(j, "value", repo.value);
    return j;
}

RepoIdentity repo_identity_from_json(const nlohmann::json& j) {
    RepoIdentity repo;
    repo.kind = opt_string(j, "kind");
    repo.value = opt_string(j, "value");
    return repo;
}

nlohmann::json team_meta_to_json(const TeamMeta& meta) {
    nlohmann::json j;
    set_opt_string(j, "team_id", meta.team_id);
    set_opt_string(j, "name", meta.name);
    set_opt_string(j, "invite_seed", meta.invite_seed);
    if (!meta.members.empty()) {
        nlohmann::json members = nlohmann::json::array();
        for (const auto& member : meta.members) members.push_back(member_to_json(member));
        j["members"] = std::move(members);
    }
    if (!meta.sessions.empty()) {
        nlohmann::json sessions = nlohmann::json::array();
        for (const auto& session : meta.sessions) sessions.push_back(session_ref_to_json(session));
        j["sessions"] = std::move(sessions);
    }
    if (!meta.repos.empty()) {
        nlohmann::json repos = nlohmann::json::array();
        for (const auto& repo : meta.repos) repos.push_back(repo_identity_to_json(repo));
        j["repos"] = std::move(repos);
    }
    set_opt_int(j, "created_at", meta.created_at);
    set_opt_string(j, "created_by", meta.created_by);
    set_opt_int(j, "updated_at", meta.updated_at);
    set_opt_string(j, "updated_by", meta.updated_by);
    return j;
}

TeamMeta team_meta_from_json(const nlohmann::json& j) {
    TeamMeta meta;
    meta.team_id = opt_string(j, "team_id");
    meta.name = opt_string(j, "name");
    meta.invite_seed = opt_string(j, "invite_seed");
    if (auto it = j.find("members"); it != j.end() && it->is_array()) {
        for (const auto& entry : *it) meta.members.push_back(member_from_json(entry));
    }
    if (auto it = j.find("sessions"); it != j.end() && it->is_array()) {
        for (const auto& entry : *it) meta.sessions.push_back(session_ref_from_json(entry));
    }
    if (auto it = j.find("repos"); it != j.end() && it->is_array()) {
        for (const auto& entry : *it) meta.repos.push_back(repo_identity_from_json(entry));
    }
    meta.created_at = opt_int(j, "created_at");
    meta.created_by = opt_string(j, "created_by");
    meta.updated_at = opt_int(j, "updated_at");
    meta.updated_by = opt_string(j, "updated_by");
    return meta;
}

} // namespace acecode::team
