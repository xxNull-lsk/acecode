#include "team_host/team_service.hpp"

#include "platform/crypto/digest.hpp"
#include "platform/crypto/secure_random.hpp"
#include "utils/atomic_file.hpp"
#include "utils/text.hpp"
#include "utils/utf8_path.hpp"
#include "utils/uuid.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>

namespace fs = std::filesystem;

namespace acecode::team {
namespace {

fs::path teams_root(const std::string& data_dir) {
    return path_from_utf8(data_dir) / "team";
}

// 设备 GUID 持久化:<data>/team/device_guid。daemon 运行时 GUID 每次启动
// 都会重新生成,不能当成员身份;团队模块自持一份稳定身份,数据目录即设备。
// 首次启动采用传入的 guid 落盘,之后一直复用;传入为空时生成新的。
std::string load_or_create_device_guid(const std::string& data_dir,
                                       const std::string& provided) {
    const fs::path file = teams_root(data_dir) / "device_guid";
    std::error_code ec;
    if (fs::exists(file, ec)) {
        std::ifstream input(file);
        if (input) {
            std::ostringstream buffer;
            buffer << input.rdbuf();
            const std::string guid = utils::trim_ascii_copy(buffer.str());
            if (!guid.empty()) return guid;
        }
    }
    fs::create_directories(file.parent_path(), ec);
    const std::string guid = provided.empty() ? generate_uuid() : provided;
    if (atomic_write_file(path_to_utf8(file), guid + "\n")) return guid;
    // 写不下去就退回进程内 GUID:至少本进程内自洽,不产生空 guid。
    return provided.empty() ? generate_uuid() : provided;
}

// 常量时间比较:凭据校验不能因为早退泄露前缀匹配长度。
bool constant_time_equal(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    volatile unsigned char mismatch = 0;
    for (std::size_t i = 0; i < a.size(); ++i)
        mismatch |= static_cast<unsigned char>(a[i] ^ b[i]);
    return mismatch == 0;
}

// LWW 时间戳是秒级:同一秒内的退出/重新加入不能让墓碑被旧值盖回。
std::int64_t next_updated_at(std::int64_t previous, std::int64_t now) {
    return std::max(now, previous + 1);
}

} // namespace

std::int64_t TeamService::default_clock() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

TeamService::TeamService(std::string data_dir, DeviceIdentity self,
                         std::function<std::int64_t()> now_seconds)
    : data_dir_(std::move(data_dir)), self_(std::move(self)),
      now_seconds_(now_seconds ? std::move(now_seconds) : default_clock) {
    // 设备身份必须跨 daemon 重启稳定(daemon 运行时 GUID 每次启动都重新生成):
    // 团队成员身份主键持久化在 <data>/team/device_guid,一次采用终身使用。
    self_.guid = load_or_create_device_guid(data_dir_, self_.guid);
}

TeamStore TeamService::store_for(const std::string& team_id) const {
    return TeamStore(data_dir_, team_id);
}

std::optional<TeamMeta> TeamService::load_team(const std::string& team_id) const {
    if (team_id.empty()) return std::nullopt;
    return store_for(team_id).load_meta();
}

std::vector<TeamMeta> TeamService::load_teams(bool include_archived) const {
    std::vector<TeamMeta> teams;
    std::error_code ec;
    const fs::path root = teams_root(data_dir_);
    if (!fs::exists(root, ec)) return teams;
    for (const auto& entry : fs::directory_iterator(root, ec)) {
        if (ec) break;
        if (!entry.is_directory(ec)) continue;
        TeamStore store(data_dir_, path_to_utf8(entry.path().filename()));
        auto meta = store.load_meta();
        if (!meta || meta->team_id.empty()) continue;
        if (!include_archived && store.load_local_state().archived_at != 0) continue;
        teams.push_back(std::move(*meta));
    }
    std::sort(teams.begin(), teams.end(), [](const TeamMeta& a, const TeamMeta& b) {
        if (a.created_at != b.created_at) return a.created_at < b.created_at;
        return a.team_id < b.team_id;
    });
    return teams;
}

std::vector<TeamMeta> TeamService::load_archived_teams() const {
    std::vector<TeamMeta> archived;
    for (auto& meta : load_teams(/*include_archived=*/true)) {
        if (store_for(meta.team_id).load_local_state().archived_at != 0)
            archived.push_back(std::move(meta));
    }
    return archived;
}

bool TeamService::is_team_archived(const std::string& team_id) const {
    if (team_id.empty()) return false;
    return store_for(team_id).load_local_state().archived_at != 0;
}

std::optional<TeamMeta> TeamService::create_team(const std::string& name, std::string* error) {
    const std::string trimmed = name.empty() ? std::string("团队") : name;
    if (trimmed.size() > 256) {
        if (error) *error = "invalid_team_name";
        return std::nullopt;
    }
    const std::int64_t now = now_seconds_();
    TeamMeta meta;
    meta.team_id = generate_uuid();
    meta.name = trimmed;
    meta.invite_seed = platform::secure_random_token(32);
    if (meta.invite_seed.empty()) {
        if (error) *error = "secure_random_unavailable";
        return std::nullopt;
    }
    TeamMember creator;
    creator.guid = self_.guid;
    creator.account = self_.account;
    creator.display_name = self_.display_name;
    creator.role = std::string(kRoleAdmin);
    creator.last_seen = now;
    creator.joined_at = now;
    creator.updated_at = now;
    creator.updated_by = self_.guid;
    meta.members.push_back(std::move(creator));
    meta.created_at = now;
    meta.created_by = self_.guid;
    meta.updated_at = now;
    meta.updated_by = self_.guid;

    if (!store_for(meta.team_id).save_meta(meta)) {
        if (error) *error = "team_write_failed";
        return std::nullopt;
    }
    return meta;
}

InviteIssueResult TeamService::issue_invite(const std::string& team_id,
                                            const std::vector<std::string>& addresses,
                                            int port) {
    InviteIssueResult result;
    const std::int64_t now = now_seconds_();
    auto meta = load_team(team_id);
    if (!meta) {
        result.error = "team_not_found";
        return result;
    }
    const TeamMember* self = find_member(*meta, self_.guid);
    if (!self || !member_active(*self) || !is_admin(*self)) {
        result.error = "not_admin";
        return result;
    }
    const std::string token = platform::secure_random_token(32);
    if (token.empty()) {
        result.error = "secure_random_unavailable";
        return result;
    }
    TeamStore store = store_for(team_id);
    InviteLedgerLoad ledger = store.load_invites();
    if (!ledger.error.empty()) {
        result.error = ledger.error;
        return result;
    }
    InviteLedgerRecord record;
    record.token = token;
    record.issued_at = now;
    record.expires_at = now + kInviteTtlSeconds;
    ledger.records.push_back(std::move(record));
    if (!store.save_invites(ledger.records)) {
        result.error = "invite_write_failed";
        return result;
    }
    result.invite.team_id = meta->team_id;
    result.invite.team_name = meta->name;
    result.invite.inviter_name = self_.display_name.empty() ? self_.account : self_.display_name;
    result.invite.inviter_guid = self_.guid;
    result.invite.addresses = addresses;
    result.invite.port = port;
    result.invite.token = token;
    result.invite.expires_at = record.expires_at;
    return result;
}

TeamService::JoinAccept TeamService::accept_join(const std::string& team_id,
                                                 const std::string& token,
                                                 const DeviceIdentity& joiner) {
    JoinAccept accepted;
    const std::int64_t now = now_seconds_();
    auto meta = load_team(team_id);
    if (!meta) {
        accepted.error = "team_not_found";
        return accepted;
    }
    if (joiner.guid.empty()) {
        accepted.error = "invalid_joiner";
        return accepted;
    }
    TeamMember* existing = find_member(*meta, joiner.guid);
    if (existing && member_active(*existing)) {
        accepted.error = "already_member";
        return accepted;
    }
    if (team_full(*meta)) {
        accepted.error = "team_full";
        return accepted;
    }
    TeamStore store = store_for(team_id);
    if (!store.consume_invite(token, joiner.guid, now)) {
        accepted.error = "invite_invalid";
        return accepted;
    }
    if (existing) {
        // 退出后重新加入:复用墓碑,清掉退出时间;角色回退为普通成员。
        existing->account = joiner.account;
        existing->display_name = joiner.display_name;
        existing->role = std::string(kRoleMember);
        existing->last_seen = now;
        existing->joined_at = now;
        existing->left_at = 0;
        existing->address.clear();
        existing->updated_at = next_updated_at(existing->updated_at, now);
        existing->updated_by = self_.guid;
    } else {
        TeamMember member;
        member.account = joiner.account;
        member.guid = joiner.guid;
        member.display_name = joiner.display_name;
        member.role = std::string(kRoleMember);
        member.last_seen = now;
        member.joined_at = now;
        member.updated_at = now;
        member.updated_by = self_.guid;
        meta->members.push_back(std::move(member));
    }
    meta->updated_at = next_updated_at(meta->updated_at, now);
    meta->updated_by = self_.guid;
    if (!store.save_meta(*meta)) {
        accepted.error = "team_write_failed";
        return accepted;
    }
    accepted.snapshot = std::move(*meta);
    return accepted;
}

TeamOpResult TeamService::join_team(const TeamInviteLink& invite, const SnapshotFetcher& fetch) {
    std::string fetch_error;
    std::optional<TeamMeta> snapshot;
    if (fetch) snapshot = fetch(invite, &fetch_error);
    if (!snapshot || snapshot->team_id.empty()) {
        return TeamOpResult::fail(fetch_error.empty() ? "snapshot_unavailable" : fetch_error);
    }
    if (snapshot->team_id != invite.team_id) return TeamOpResult::fail("team_id_mismatch");
    if (!find_active_member(*snapshot, self_.guid)) return TeamOpResult::fail("self_not_in_team");
    return apply_snapshot(*snapshot);
}

TeamOpResult TeamService::apply_snapshot(const TeamMeta& snapshot) {
    if (snapshot.team_id.empty()) return TeamOpResult::fail("invalid_snapshot");
    if (!store_for(snapshot.team_id).save_meta(snapshot)) {
        return TeamOpResult::fail("team_write_failed");
    }
    // 重新加入(或加入)后团队不再是归档状态;保留本机工作目录,只清归档时间。
    TeamStore store = store_for(snapshot.team_id);
    TeamLocalState local = store.load_local_state();
    if (local.archived_at != 0) {
        local.archived_at = 0;
        if (!store.save_local_state(local)) return TeamOpResult::fail("team_write_failed");
    }
    return TeamOpResult::success();
}

std::optional<TeamMeta> TeamService::merge_remote(const std::string& team_id,
                                                  const TeamMeta& remote,
                                                  std::string* error) {
    auto local = load_team(team_id);
    if (!local) {
        if (error) *error = "team_not_found";
        return std::nullopt;
    }
    bool changed = false;

    // 成员:按 updated_at 逐字段整条替换;last_seen 独立取 max。
    for (const auto& incoming : remote.members) {
        if (incoming.guid.empty()) continue;
        TeamMember* existing = find_member(*local, incoming.guid);
        if (!existing) {
            local->members.push_back(incoming);
            changed = true;
            continue;
        }
        const std::int64_t incoming_seen = incoming.last_seen;
        if (incoming.updated_at > existing->updated_at ||
            (incoming.updated_at == existing->updated_at &&
             incoming.updated_by > existing->updated_by)) {
            const std::int64_t keep_last_seen = std::max(existing->last_seen, incoming_seen);
            const std::string keep_address =
                incoming.address.empty() ? existing->address : incoming.address;
            *existing = incoming;
            existing->last_seen = keep_last_seen;
            existing->address = keep_address;
            changed = true;
        } else if (incoming_seen > existing->last_seen) {
            existing->last_seen = incoming_seen;
            if (!incoming.address.empty()) existing->address = incoming.address;
            changed = true;
        }
    }

    // 会话引用:按 session_id 合并,更新时较新者胜;不存在则新增,不删除
    //(结束态由 state 表达,删除会让其他设备丢记录)。
    for (const auto& incoming : remote.sessions) {
        if (incoming.session_id.empty()) continue;
        TeamSessionRef* existing = find_session(*local, incoming.session_id);
        if (!existing) {
            local->sessions.push_back(incoming);
            changed = true;
            continue;
        }
        if (incoming.updated_at > existing->updated_at) {
            *existing = incoming;
            changed = true;
        }
    }

    // 仓库身份:并集去重。
    for (const auto& repo : remote.repos) {
        if (repo.kind.empty() || repo.value.empty()) continue;
        const bool known = std::any_of(local->repos.begin(), local->repos.end(),
            [&](const RepoIdentity& r) { return r.kind == repo.kind && r.value == repo.value; });
        if (!known) {
            local->repos.push_back(repo);
            changed = true;
        }
    }

    if (local->invite_seed.empty()) {
        local->invite_seed = remote.invite_seed;
        if (!remote.invite_seed.empty()) changed = true;
    }
    // 顶层名称:整条团队元数据的 updated_at 较新者胜。
    if (remote.updated_at > local->updated_at) {
        local->name = remote.name.empty() ? local->name : remote.name;
        local->updated_at = remote.updated_at;
        local->updated_by = remote.updated_by;
        changed = true;
    }

    if (changed && !store_for(team_id).save_meta(*local)) {
        if (error) *error = "team_write_failed";
        return std::nullopt;
    }
    return local;
}

TeamOpResult TeamService::set_role(const std::string& team_id, const std::string& actor_guid,
                                   const std::string& target_guid, const std::string& role) {
    if (role != kRoleAdmin && role != kRoleMember) return TeamOpResult::fail("invalid_role");
    auto meta = load_team(team_id);
    if (!meta) return TeamOpResult::fail("team_not_found");
    const TeamMember* actor = find_member(*meta, actor_guid);
    if (!actor || !member_active(*actor) || !is_admin(*actor)) return TeamOpResult::fail("not_admin");
    TeamMember* target = find_member(*meta, target_guid);
    if (!target || !member_active(*target)) return TeamOpResult::fail("member_not_found");
    if (target->role == role) return TeamOpResult::success();

    const std::int64_t now = now_seconds_();
    target->role = role;
    target->updated_at = now;
    target->updated_by = actor_guid;
    meta->updated_at = now;
    meta->updated_by = actor_guid;
    if (!store_for(team_id).save_meta(*meta)) return TeamOpResult::fail("team_write_failed");
    return TeamOpResult::success();
}

TeamOpResult TeamService::touch_member(const std::string& team_id, const std::string& guid,
                                       const std::string& address) {
    if (guid.empty()) return TeamOpResult::fail("invalid_member");
    auto meta = load_team(team_id);
    if (!meta) return TeamOpResult::fail("team_not_found");
    TeamMember* member = find_member(*meta, guid);
    if (!member || !member_active(*member)) return TeamOpResult::fail("member_not_found");
    // 心跳不参与 LWW:只更新 last_seen/address,两侧合并时取 max。
    member->last_seen = now_seconds_();
    if (!address.empty()) member->address = address;
    if (!store_for(team_id).save_meta(*meta)) return TeamOpResult::fail("team_write_failed");
    return TeamOpResult::success();
}

bool TeamService::member_online(const TeamMeta& meta, const std::string& guid) const {
    const TeamMember* member = find_active_member(meta, guid);
    if (!member) return false;
    return now_seconds_() - member->last_seen <= kMemberStaleSeconds;
}

bool TeamService::authorize(const std::string& team_id, const std::string& header_value) const {
    auto meta = load_team(team_id);
    if (!meta) return false;
    return verify_team_token(*meta, header_value);
}

SessionRefResult TeamService::start_session(const std::string& team_id,
                                                         const RepoIdentity& repo,
                                                         const std::string& session_id,
                                                         const std::string& title) {
    SessionRefResult result;
    auto meta = load_team(team_id);
    if (!meta) {
        result.error = "team_not_found";
        return result;
    }
    if (!find_active_member(*meta, self_.guid)) {
        result.error = "not_member";
        return result;
    }
    if (!team_matches_repo(*meta, repo)) {
        result.error = "repo_not_linked";
        return result;
    }
    if (session_id.empty()) {
        result.error = "invalid_session";
        return result;
    }
    if (find_session(*meta, session_id)) {
        result.error = "session_already_registered";
        return result;
    }
    if (active_session_count(*meta) >= kMaxConcurrentTeamSessions) {
        result.error = "too_many_concurrent_sessions";
        return result;
    }

    TeamSessionRef ref;
    ref.session_id = session_id;
    ref.title = title;
    ref.host_guid = self_.guid;
    ref.state = std::string(kSessionStateRunning);
    ref.created_by = self_.guid;
    ref.created_at = now_seconds_();
    ref.updated_at = ref.created_at;
    meta->sessions.push_back(ref);
    meta->updated_at = ref.updated_at;
    meta->updated_by = self_.guid;
    if (!store_for(team_id).save_meta(*meta)) {
        result.error = "team_write_failed";
        return result;
    }
    result.ref = std::move(ref);
    return result;
}

TeamOpResult TeamService::register_repo(const std::string& team_id, const RepoIdentity& repo) {
    if (repo.kind.empty() || repo.value.empty()) return TeamOpResult::fail("invalid_repo");
    auto meta = load_team(team_id);
    if (!meta) return TeamOpResult::fail("team_not_found");
    if (!find_active_member(*meta, self_.guid)) return TeamOpResult::fail("not_member");
    if (team_matches_repo(*meta, repo)) return TeamOpResult::success(); // 幂等

    const std::int64_t now = now_seconds_();
    meta->repos.push_back(repo);
    meta->updated_at = now;
    meta->updated_by = self_.guid;
    if (!store_for(team_id).save_meta(*meta)) return TeamOpResult::fail("team_write_failed");
    return TeamOpResult::success();
}

TeamOpResult TeamService::set_session_state(const std::string& team_id,
                                            const std::string& session_id,
                                            std::string_view state,
                                            const std::string& pause_reason) {
    if (state != kSessionStateRunning && state != kSessionStatePaused &&
        state != kSessionStateEnded)
        return TeamOpResult::fail("invalid_state");
    auto meta = load_team(team_id);
    if (!meta) return TeamOpResult::fail("team_not_found");
    if (!find_active_member(*meta, self_.guid)) return TeamOpResult::fail("not_member");
    TeamSessionRef* ref = find_session(*meta, session_id);
    if (!ref) return TeamOpResult::fail("session_not_found");

    const std::int64_t now = now_seconds_();
    ref->state = std::string(state);
    ref->pause_reason = state == kSessionStatePaused ? pause_reason : std::string{};
    ref->updated_at = now;
    meta->updated_at = now;
    meta->updated_by = self_.guid;
    if (!store_for(team_id).save_meta(*meta)) return TeamOpResult::fail("team_write_failed");
    return TeamOpResult::success();
}

TeamOpResult TeamService::set_session_host(const std::string& team_id,
                                           const std::string& session_id,
                                           const std::string& new_host_guid) {
    if (new_host_guid.empty()) return TeamOpResult::fail("invalid_member");
    auto meta = load_team(team_id);
    if (!meta) return TeamOpResult::fail("team_not_found");
    if (!find_active_member(*meta, self_.guid)) return TeamOpResult::fail("not_member");
    TeamSessionRef* ref = find_session(*meta, session_id);
    if (!ref) return TeamOpResult::fail("session_not_found");
    if (!find_active_member(*meta, new_host_guid)) return TeamOpResult::fail("member_not_found");
    if (ref->host_guid == new_host_guid) return TeamOpResult::success();

    const std::int64_t now = now_seconds_();
    ref->host_guid = new_host_guid;
    ref->updated_at = now;
    meta->updated_at = now;
    meta->updated_by = self_.guid;
    if (!store_for(team_id).save_meta(*meta)) return TeamOpResult::fail("team_write_failed");
    return TeamOpResult::success();
}

TeamOpResult TeamService::set_daily_quota(const std::string& team_id,
                                          const std::string& actor_guid,
                                          std::int64_t quota) {
    if (quota < 0) return TeamOpResult::fail("invalid_quota");
    auto meta = load_team(team_id);
    if (!meta) return TeamOpResult::fail("team_not_found");
    TeamMember* member = find_member(*meta, actor_guid);
    if (!member || !member_active(*member)) return TeamOpResult::fail("member_not_found");
    if (member->daily_token_quota == quota) return TeamOpResult::success();

    const std::int64_t now = now_seconds_();
    member->daily_token_quota = quota;
    member->updated_at = now;
    member->updated_by = actor_guid;
    meta->updated_at = now;
    meta->updated_by = actor_guid;
    if (!store_for(team_id).save_meta(*meta)) return TeamOpResult::fail("team_write_failed");
    return TeamOpResult::success();
}

TeamOpResult TeamService::report_quota_usage(const std::string& team_id, std::int64_t used_today,
                                             std::int64_t day) {
    if (used_today < 0 || day < 0) return TeamOpResult::fail("invalid_usage");
    auto meta = load_team(team_id);
    if (!meta) return TeamOpResult::fail("team_not_found");
    TeamMember* member = find_member(*meta, self_.guid);
    if (!member || !member_active(*member)) return TeamOpResult::fail("member_not_found");
    if (member->quota_day == day && member->quota_used_today == used_today)
        return TeamOpResult::success();

    // 用量快照与心跳同理,不 bump 团队级 updated_at;成员记录带 updated_at
    // 供 LWW(本机是它自己快照的唯一写者,较新者胜)。
    const std::int64_t now = now_seconds_();
    member->quota_used_today = used_today;
    member->quota_day = day;
    member->updated_at = now;
    member->updated_by = self_.guid;
    if (!store_for(team_id).save_meta(*meta)) return TeamOpResult::fail("team_write_failed");
    return TeamOpResult::success();
}

std::vector<std::string> TeamService::pause_sessions_for_quota(const std::string& team_id) {
    auto meta = load_team(team_id);
    if (!meta) return {};
    std::vector<std::string> paused;
    const std::int64_t now = now_seconds_();
    bool changed = false;
    for (auto& ref : meta->sessions) {
        if (ref.state != kSessionStateRunning || ref.host_guid != self_.guid) continue;
        ref.state = std::string(kSessionStatePaused);
        ref.pause_reason = "额度已用尽";
        ref.updated_at = now;
        paused.push_back(ref.session_id);
        changed = true;
    }
    if (changed) {
        meta->updated_at = now;
        meta->updated_by = self_.guid;
        if (!store_for(team_id).save_meta(*meta)) return {};
    }
    return paused;
}

std::vector<std::string> TeamService::resume_quota_paused_sessions(const std::string& team_id) {
    auto meta = load_team(team_id);
    if (!meta) return {};
    std::vector<std::string> resumed;
    const std::int64_t now = now_seconds_();
    bool changed = false;
    for (auto& ref : meta->sessions) {
        if (ref.state != kSessionStatePaused || ref.pause_reason != "额度已用尽" ||
            ref.host_guid != self_.guid)
            continue;
        ref.state = std::string(kSessionStateRunning);
        ref.pause_reason.clear();
        ref.updated_at = now;
        resumed.push_back(ref.session_id);
        changed = true;
    }
    if (changed) {
        meta->updated_at = now;
        meta->updated_by = self_.guid;
        if (!store_for(team_id).save_meta(*meta)) return {};
    }
    return resumed;
}

std::vector<std::string> TeamService::mark_lost_host_sessions(const std::string& team_id) {
    auto meta = load_team(team_id);
    if (!meta) return {};
    std::vector<std::string> marked;
    const std::int64_t now = now_seconds_();
    bool changed = false;
    for (auto& ref : meta->sessions) {
        if (ref.state != kSessionStateRunning) continue;
        // 本机自己 host 的会话不判定失联(host 知道自己活着)。
        if (ref.host_guid == self_.guid) continue;
        const TeamMember* host = find_active_member(*meta, ref.host_guid);
        if (host && now - host->last_seen <= kMemberStaleSeconds) continue;
        ref.state = std::string(kSessionStatePaused);
        ref.pause_reason = "失联";
        ref.updated_at = next_updated_at(ref.updated_at, now);
        marked.push_back(ref.session_id);
        changed = true;
    }
    if (changed) {
        meta->updated_at = next_updated_at(meta->updated_at, now);
        meta->updated_by = self_.guid;
        if (!store_for(team_id).save_meta(*meta)) return {};
    }
    return marked;
}

// 退出团队:本机状态机部分。网络推送由路由层负责,保证这里可单测。
LeaveResult TeamService::leave_team(const std::string& team_id) {
    LeaveResult result;
    auto meta = load_team(team_id);
    if (!meta) {
        result.error = "team_not_found";
        return result;
    }
    TeamMember* self = find_member(*meta, self_.guid);
    if (!self) {
        result.error = "not_member";
        return result;
    }
    if (!member_active(*self)) {
        // 幂等:上一次退出可能已写墓碑但归档失败,重试只需继续归档。
        result.snapshot = *meta;
        return result;
    }

    const std::int64_t now = now_seconds_();

    // 1. 本机 host 的进行中会话先暂停:其他队员看到后可手动接管。
    for (auto& ref : meta->sessions) {
        if (ref.state != kSessionStateRunning || ref.host_guid != self_.guid) continue;
        ref.state = std::string(kSessionStatePaused);
        ref.pause_reason = "设备已退出";
        ref.updated_at = next_updated_at(ref.updated_at, now);
        result.paused_sessions.push_back(ref.session_id);
    }

    // 2. 最后管理员退出时,把最早加入的活跃成员提升为管理员。只在团队
    //    将失去全部管理员时触发;还有其他管理员则保持原样。
    if (is_admin(*self)) {
        bool has_other_admin = false;
        std::size_t replacement = meta->members.size();
        for (std::size_t i = 0; i < meta->members.size(); ++i) {
            const auto& member = meta->members[i];
            if (member.guid == self_.guid || !member_active(member)) continue;
            if (is_admin(member)) {
                has_other_admin = true;
                break;
            }
            if (replacement == meta->members.size()) {
                replacement = i;
                continue;
            }
            const auto& best = meta->members[replacement];
            // joined_at == 0 是旧数据:视为比任何带时间的新成员更早加入。
            if (best.joined_at > 0 &&
                (member.joined_at == 0 || member.joined_at < best.joined_at)) {
                replacement = i;
            }
        }
        if (!has_other_admin && replacement < meta->members.size()) {
            auto& promoted = meta->members[replacement];
            promoted.role = std::string(kRoleAdmin);
            promoted.updated_at = next_updated_at(promoted.updated_at, now);
            promoted.updated_by = self_.guid;
            result.promoted_guid = promoted.guid;
        }
    }

    // 3. 本机成员写成墓碑。保留记录供同步与重新加入;不再计入名额与在线。
    self->role = std::string(kRoleMember);
    self->left_at = now;
    self->address.clear();
    self->updated_at = next_updated_at(self->updated_at, now);
    self->updated_by = self_.guid;

    meta->updated_at = next_updated_at(meta->updated_at, now);
    meta->updated_by = self_.guid;
    if (!store_for(team_id).save_meta(*meta)) {
        result.error = "team_write_failed";
        return result;
    }
    result.snapshot = std::move(*meta);
    return result;
}

TeamOpResult TeamService::archive_team(const std::string& team_id) {
    auto meta = load_team(team_id);
    if (!meta) return TeamOpResult::fail("team_not_found");
    TeamStore store = store_for(team_id);
    TeamLocalState local = store.load_local_state();
    if (local.archived_at == 0) {
        local.archived_at = now_seconds_();
        if (!store.save_local_state(local)) return TeamOpResult::fail("team_write_failed");
    }
    // 退出后本机不再持有团队凭据:清掉 invite_seed,避免归档快照还能访问
    // 团队互访路由。重新加入时由邀请者快照恢复种子。
    if (!meta->invite_seed.empty()) {
        meta->invite_seed.clear();
        if (!store.save_meta(*meta)) return TeamOpResult::fail("team_write_failed");
    }
    return TeamOpResult::success();
}

TeamOpResult TeamService::delete_archived_team(const std::string& team_id) {
    if (team_id.empty()) return TeamOpResult::fail("invalid_team");
    TeamStore store = store_for(team_id);
    if (!store.load_meta()) return TeamOpResult::fail("team_not_found");
    if (store.load_local_state().archived_at == 0)
        return TeamOpResult::fail("team_not_archived");
    std::error_code ec;
    fs::remove_all(store.root_dir(), ec);
    if (ec || fs::exists(store.root_dir(), ec))
        return TeamOpResult::fail("team_delete_failed");
    return TeamOpResult::success();
}

std::string derive_team_token(const std::string& invite_seed, const std::string& team_id) {
    if (invite_seed.empty() || team_id.empty()) return {};
    std::string digest = platform::hmac_sha256(invite_seed, "team:" + team_id);
    if (digest.size() != 32) return {};
    static const char* kHex = "0123456789abcdef";
    std::string hex;
    hex.reserve(64);
    for (unsigned char c : digest) {
        hex.push_back(kHex[c >> 4]);
        hex.push_back(kHex[c & 0x0f]);
    }
    return hex;
}

bool verify_team_token(const TeamMeta& meta, std::string_view token) {
    if (meta.invite_seed.empty() || token.empty()) return false;
    const std::string expected = derive_team_token(meta.invite_seed, meta.team_id);
    if (expected.empty()) return false;
    return constant_time_equal(expected, std::string(token));
}

} // namespace acecode::team
