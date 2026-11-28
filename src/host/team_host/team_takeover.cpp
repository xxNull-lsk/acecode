#include "team_host/team_takeover.hpp"

#include "team_host/quota_ledger.hpp"
#include "utils/atomic_file.hpp"
#include "utils/text.hpp"
#include "utils/utf8_path.hpp"
#include "worktree/worktree_manager.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

namespace acecode::team {
namespace {

bool is_valid_head_commit(const std::string& head) {
    if (head.empty()) return true; // 未提供 = 跳过对齐
    if (head.size() != 40 && head.size() != 64) return false;
    return std::all_of(head.begin(), head.end(), [](unsigned char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
}

std::string read_all(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input.is_open()) return {};
    std::ostringstream buffer;
    buffer << input.rdbuf();
    return buffer.str();
}

} // namespace

TakeoverTargetCheck check_takeover_target(const TeamMeta& meta, const std::string& target_guid,
                                          std::int64_t now_seconds) {
    TakeoverTargetCheck result;
    const TeamMember* member = find_member(meta, target_guid);
    if (!member || !member_active(*member)) {
        result.error = "member_not_found";
        return result;
    }
    if (member_quota_snapshot(meta, target_guid, now_seconds * 1000).exhausted()) {
        result.error = "quota_exhausted";
        return result;
    }
    const std::int64_t now = now_seconds;
    if (now - member->last_seen > kMemberStaleSeconds) {
        result.error = "member_offline";
        return result;
    }
    result.ok = true;
    return result;
}

std::string align_repo_to_head(const std::string& cwd, const std::string& head_commit) {
    if (!is_valid_head_commit(head_commit)) return "invalid_head_commit";
    if (head_commit.empty()) return {};
    if (worktree::find_git_root(cwd).empty()) return "not_git_repo";

    const auto head_now = worktree::run_git({"rev-parse", "HEAD"}, cwd, 10000);
    if (!head_now.ok()) return "head_read_failed";
    if (utils::trim_ascii_copy(head_now.out) == head_commit) return {}; // 已对齐

    // 目标提交已存在于本地仓库时跳过 fetch(本地仓库、克隆过、或同步过)。
    bool present = worktree::run_git({"cat-file", "-e", head_commit + "^{commit}"}, cwd, 10000).ok();
    if (!present) {
        const auto fetched = worktree::run_git({"fetch", "origin"}, cwd, 60000, true);
        if (!fetched.ok()) return "git_fetch_failed";
        present = worktree::run_git({"cat-file", "-e", head_commit + "^{commit}"}, cwd, 10000).ok();
        if (!present) return "git_fetch_failed";
    }
    // --ff-only 对齐到该提交:落后场景直接快进;分叉/冲突/未提交改动冲突直接
    // 失败,绝不产生自动 merge commit,不改写未提交改动。
    const auto merged = worktree::run_git({"merge", "--ff-only", head_commit}, cwd, 60000, true);
    if (!merged.ok()) return "git_pull_failed";

    const auto after = worktree::run_git({"rev-parse", "HEAD"}, cwd, 10000);
    if (!after.ok()) return "head_read_failed";
    if (utils::trim_ascii_copy(after.out) != head_commit) return "repo_diverged";
    return {};
}

TeamTakeover::TeamTakeover(TeamService& service, SourceDataFetcher fetch, SessionResumer resume,
                           std::function<bool()> account_quota_exhausted)
    : service_(service), fetch_(std::move(fetch)), resume_(std::move(resume)),
      account_quota_exhausted_(std::move(account_quota_exhausted)) {}

TakeoverOutcome TeamTakeover::take_over(const std::string& team_id,
                                        const std::string& session_id,
                                        const std::string& cwd,
                                        const std::string& project_dir) {
    TakeoverOutcome outcome;
    auto meta = service_.load_team(team_id);
    if (!meta) {
        outcome.error = "team_not_found";
        return outcome;
    }
    const TeamSessionRef* ref = find_session(*meta, session_id);
    if (!ref) {
        outcome.error = "session_not_found";
        return outcome;
    }
    if (ref->state == kSessionStateEnded) {
        outcome.error = "session_ended";
        return outcome;
    }
    if (ref->host_guid == service_.self().guid) {
        outcome.error = "already_host";
        return outcome;
    }
    // 资格:本机当日额度未用尽(0 额度即不承接)。账号级共享池优先
    // (额度属于登录账号);未注入账号判定时退回成员记录口径。
    if (account_quota_exhausted_ && account_quota_exhausted_()) {
        outcome.error = "quota_exhausted";
        return outcome;
    }
    QuotaLedger ledger(service_.data_dir(), team_id);
    if (local_quota_status(*meta, service_.self().guid, ledger,
                           service_.now() * 1000)
            .exhausted()) {
        outcome.error = "quota_exhausted";
        return outcome;
    }

    // 取数:源 host 在线走注入 fetcher;失联用本机最近副本。
    SessionSourceData data;
    const TeamMember* source_member = find_member(*meta, ref->host_guid);
    const bool source_online =
        source_member && service_.member_online(*meta, ref->host_guid);
    if (source_online) {
        if (!fetch_) {
            outcome.error = "data_unavailable";
            return outcome;
        }
        data = fetch_(*meta, *ref);
        if (!data) {
            outcome.error = data.error.empty() ? "data_unavailable" : data.error;
            return outcome;
        }
    } else {
        const TeamStore store(service_.data_dir(), team_id);
        const fs::path replica = store.replica_dir(session_id);
        data.jsonl = read_all(replica / (session_id + ".jsonl"));
        data.meta = read_all(replica / (session_id + ".meta.json"));
        data.head_commit = utils::trim_ascii_copy(read_all(replica / (session_id + ".head")));
        if (data.jsonl.empty() && data.meta.empty()) {
            outcome.error = "no_replica";
            return outcome;
        }
    }

    // 落数据(源 meta 已带 team_role=host;内容原样落地)。
    if (!atomic_write_file(path_from_utf8(project_dir) / (session_id + ".jsonl"), data.jsonl) ||
        (!data.meta.empty() &&
         !atomic_write_file(path_from_utf8(project_dir) / (session_id + ".meta.json"),
                            data.meta))) {
        outcome.error = "write_failed";
        return outcome;
    }

    // git 对齐:失败即中止,会话保持暂停(还没有切 host)。
    const std::string align_error = align_repo_to_head(cwd, data.head_commit);
    if (!align_error.empty()) {
        outcome.error = "repo_align_failed: " + align_error;
        return outcome;
    }

    // 切 host → 恢复 running → resume。
    if (!service_.set_session_host(team_id, session_id, service_.self().guid)) {
        outcome.error = "host_switch_failed";
        return outcome;
    }
    if (!service_.set_session_state(team_id, session_id, kSessionStateRunning, "")) {
        outcome.error = "state_write_failed";
        return outcome;
    }
    if (resume_ && !resume_(session_id)) {
        outcome.error = "resume_failed";
        return outcome;
    }

    auto updated = service_.load_team(team_id);
    if (updated) {
        if (const TeamSessionRef* after = find_session(*updated, session_id))
            outcome.ref = *after;
    }
    outcome.ok = true;
    return outcome;
}

} // namespace acecode::team
