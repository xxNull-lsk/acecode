// 覆盖 src/host/team_host/team_takeover.cpp 与 TeamService::mark_lost_host_sessions
// (tasks 5.1-5.3)。锁定 spec(team-session-handover) 的三条契约:
//   检测只置 paused 绝不自动切换;接管资格(在线+额度)校验;失联用最近副本
//   接管成功且视图指向新 host;git 对齐 fetch+ff-only pull,失败中止。

#include <gtest/gtest.h>

#include "team_host/team_takeover.hpp"
#include "team_host/quota_ledger.hpp"
#include "team/team_store.hpp"
#include "utils/text.hpp"
#include "utils/utf8_path.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;
using namespace acecode::team;

namespace {

struct TempDir {
    fs::path root;
    TempDir() {
        root = fs::temp_directory_path() /
               ("team_takeover_test_" +
                std::to_string(std::chrono::steady_clock::now()
                                   .time_since_epoch()
                                   .count()));
        fs::remove_all(root);
        fs::create_directories(root);
    }
    ~TempDir() { fs::remove_all(root); }
    std::string utf8() const { return acecode::path_to_utf8(root); }
};

struct Clock {
    std::int64_t now = 1'700'000'000;
};

DeviceIdentity identity(const std::string& guid) { return DeviceIdentity{guid, guid + "@pc", guid}; }

TeamMeta team_two_members() {
    TeamMeta meta;
    meta.team_id = "team-1";
    meta.invite_seed = "seed";
    TeamMember a;
    a.guid = "guid-a";
    a.role = "admin";
    a.last_seen = 1'700'000'000;
    a.updated_at = 1;
    meta.members.push_back(a);
    TeamMember b;
    b.guid = "guid-b";
    b.role = "member";
    b.last_seen = 1'700'000'000;
    b.updated_at = 1;
    meta.members.push_back(b);
    meta.repos.push_back({"remote", "r"});
    meta.updated_at = 1;
    meta.updated_by = "guid-a";
    return meta;
}

void write_file(const fs::path& path, const std::string& content) {
    fs::create_directories(path.parent_path());
    std::ofstream(path, std::ios::binary) << content;
}

std::string read_file(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    std::ostringstream buffer;
    buffer << input.rdbuf();
    return buffer.str();
}

int git(const std::string& cmd, const fs::path& cwd) {
    const std::string full = "cd '" + acecode::path_to_utf8(cwd) + "' && " + cmd;
    return std::system(full.c_str());
}

} // namespace

// 5.1 场景: host 心跳超时 → 会话置 paused(失联);host 不变,绝不自动切换;幂等
TEST(TeamTakeover, LostHostOnlyPausesNeverSwitches) {
    TempDir temp;
    Clock clock;
    TeamService observer(temp.utf8(), identity("guid-b"), [&] { return clock.now; });
    TeamMeta meta = team_two_members();
    meta.sessions.push_back({"s-1", "会话", "guid-a", "running", "", "guid-a", 1, 1});
    meta.sessions.push_back({"s-2", "自己的", "guid-b", "running", "", "guid-b", 1, 1});
    ASSERT_TRUE(TeamStore(temp.utf8(), "team-1").save_meta(meta));

    // host A 刚心跳过:不标记
    EXPECT_TRUE(observer.mark_lost_host_sessions("team-1").empty());

    // A 失联超过判定窗口
    clock.now += kMemberStaleSeconds + 1;
    const auto marked = observer.mark_lost_host_sessions("team-1");
    ASSERT_EQ(marked.size(), 1u);
    EXPECT_EQ(marked[0], "s-1");

    const auto stored = observer.load_team("team-1");
    ASSERT_TRUE(stored.has_value());
    const TeamSessionRef* s1 = find_session(*stored, "s-1");
    ASSERT_TRUE(s1);
    EXPECT_EQ(s1->state, "paused");
    EXPECT_EQ(s1->pause_reason, "失联");
    EXPECT_EQ(s1->host_guid, "guid-a"); // 绝不自动切换
    const TeamSessionRef* s2 = find_session(*stored, "s-2");
    ASSERT_TRUE(s2);
    EXPECT_EQ(s2->state, "running"); // 自己 host 的会话不受别人判定影响

    // 幂等
    EXPECT_TRUE(observer.mark_lost_host_sessions("team-1").empty());
}

// 5.2 场景: 目标资格校验 — 未知成员 / 离线 / 额度用尽均拒绝
TEST(TeamTakeover, TargetEligibility) {
    const TeamMeta meta = team_two_members();
    const std::int64_t now = 1'700'000'000;
    EXPECT_EQ(check_takeover_target(meta, "guid-x", now).error, "member_not_found");
    EXPECT_EQ(check_takeover_target(meta, "guid-b", now).ok, true);

    TeamMeta offline = meta;
    find_member(offline, "guid-b")->last_seen = now - kMemberStaleSeconds - 1;
    EXPECT_EQ(check_takeover_target(offline, "guid-b", now).error, "member_offline");

    TeamMeta exhausted = meta;
    find_member(exhausted, "guid-b")->daily_token_quota = 0; // 0 = 当日不承接
    EXPECT_EQ(check_takeover_target(exhausted, "guid-b", now).error, "quota_exhausted");
}

// 5.2 场景: 源失联 → 用最近副本接管成功,视图指向新 host;resume 被调用
TEST(TeamTakeover, OfflineTakeoverFromReplica) {
    TempDir temp;
    TempDir project;
    TempDir repo;
    ASSERT_EQ(git("git init -q . && git config user.email t@t && git config user.name t && "
                  "git commit -q --allow-empty -m base", repo.root), 0);
    Clock clock;
    TeamService device_b(temp.utf8(), identity("guid-b"), [&] { return clock.now; });
    TeamMeta meta = team_two_members();
    // A 已失联(会话因此暂停)
    find_member(meta, "guid-a")->last_seen = clock.now - kMemberStaleSeconds - 10;
    // A host 的会话,已暂停(失联)
    meta.sessions.push_back({"s-1", "会话", "guid-a", "paused", "失联", "guid-a", 1, 1});
    // A 的最后已知快照:用量未超
    find_member(meta, "guid-a")->quota_used_today = 1'000'000;
    find_member(meta, "guid-a")->quota_day = clock.now * 1000 / kQuotaDayMs;
    ASSERT_TRUE(TeamStore(temp.utf8(), "team-1").save_meta(meta));

    // 本机最近副本(host 同步落下的:源 jsonl/meta 为空内容 + 源 HEAD)
    const TeamStore store(temp.utf8(), "team-1");
    const fs::path source_jsonl = temp.root / "src.jsonl";
    write_file(source_jsonl, "{\"role\":\"user\"}\n");
    const fs::path source_meta = temp.root / "src.meta.json";
    write_file(source_meta, R"({"id":"s-1","team_id":"team-1"})");
    const auto synced = sync_session_replica(store, "s-1", acecode::path_to_utf8(source_jsonl),
                                             acecode::path_to_utf8(source_meta),
                                             ""); // 副本未记录 HEAD → 接管跳过对齐
    ASSERT_TRUE(synced) << synced.error;
    write_file(store.replica_dir("s-1") / "s-1.jsonl", "{\"role\":\"user\"}\n");
    write_file(store.replica_dir("s-1") / "s-1.meta.json", R"({"id":"s-1","team_id":"team-1"})");

    int resumes = 0;
    TeamTakeover takeover(device_b, nullptr, [&](const std::string& sid) {
        ++resumes;
        return sid == "s-1";
    });
    const auto outcome = takeover.take_over("team-1", "s-1", acecode::path_to_utf8(repo.root),
                                            acecode::path_to_utf8(project.root));
    ASSERT_TRUE(outcome) << outcome.error;
    EXPECT_EQ(resumes, 1);
    EXPECT_EQ(outcome.ref.host_guid, "guid-b");
    EXPECT_EQ(outcome.ref.state, "running");
    EXPECT_TRUE(outcome.ref.pause_reason.empty());

    // 数据落到 project_dir;副本内容原样
    EXPECT_EQ(read_file(project.root / "s-1.jsonl"), "{\"role\":\"user\"}\n");
    EXPECT_EQ(read_file(project.root / "s-1.meta.json"), R"({"id":"s-1","team_id":"team-1"})");

    // 团队状态:切 host + 恢复
    const auto stored = device_b.load_team("team-1");
    ASSERT_TRUE(stored.has_value());
    const TeamSessionRef* ref = find_session(*stored, "s-1");
    ASSERT_TRUE(ref);
    EXPECT_EQ(ref->host_guid, "guid-b");
    EXPECT_EQ(ref->state, "running");
}

// 5.2 场景: 无副本且源失联 → 拒绝;自己已是 host → 拒绝;已结束 → 拒绝
TEST(TeamTakeover, TakeoverRejections) {
    TempDir temp;
    TempDir project;
    Clock clock;
    TeamService device_b(temp.utf8(), identity("guid-b"), [&] { return clock.now; });
    TeamMeta meta = team_two_members();
    // A 失联(无副本场景)
    find_member(meta, "guid-a")->last_seen = clock.now - kMemberStaleSeconds - 10;
    meta.sessions.push_back({"s-1", "失联无副本", "guid-a", "paused", "失联", "guid-a", 1, 1});
    meta.sessions.push_back({"s-2", "自己host", "guid-b", "running", "", "guid-b", 1, 1});
    meta.sessions.push_back({"s-3", "已结束", "guid-a", "ended", "", "guid-a", 1, 1});
    ASSERT_TRUE(TeamStore(temp.utf8(), "team-1").save_meta(meta));

    TeamTakeover takeover(device_b, nullptr, nullptr);
    EXPECT_EQ(takeover.take_over("team-1", "s-1", "/tmp", acecode::path_to_utf8(project.root)).error,
              "no_replica");
    EXPECT_EQ(takeover.take_over("team-1", "s-2", "/tmp", acecode::path_to_utf8(project.root)).error,
              "already_host");
    EXPECT_EQ(takeover.take_over("team-1", "s-3", "/tmp", acecode::path_to_utf8(project.root)).error,
              "session_ended");
    EXPECT_EQ(takeover.take_over("team-1", "s-x", "/tmp", acecode::path_to_utf8(project.root)).error,
              "session_not_found");

    // 自己额度用尽(0 额度)也拒绝
    ASSERT_TRUE(device_b.set_daily_quota("team-1", "guid-b", 0));
    EXPECT_EQ(takeover.take_over("team-1", "s-1", "/tmp", acecode::path_to_utf8(project.root)).error,
              "quota_exhausted");
}

// 5.2 场景: 源在线但 fetch 失败 → 中止,状态保持 paused,host 不变
TEST(TeamTakeover, FetchFailureAborts) {
    TempDir temp;
    TempDir project;
    Clock clock;
    TeamService device_b(temp.utf8(), identity("guid-b"), [&] { return clock.now; });
    TeamMeta meta = team_two_members();
    meta.sessions.push_back({"s-1", "会话", "guid-a", "paused", "异常", "guid-a", 1, 1});
    ASSERT_TRUE(TeamStore(temp.utf8(), "team-1").save_meta(meta));

    TeamTakeover takeover(device_b,
        [](const TeamMeta&, const TeamSessionRef&) {
            SessionSourceData data;
            data.error = "source_refused";
            return data;
        }, nullptr);
    const auto outcome = takeover.take_over("team-1", "s-1", "/tmp",
                                            acecode::path_to_utf8(project.root));
    ASSERT_FALSE(outcome);
    EXPECT_EQ(outcome.error, "source_refused");
    const auto stored = device_b.load_team("team-1");
    ASSERT_TRUE(stored.has_value());
    const TeamSessionRef* ref = find_session(*stored, "s-1");
    ASSERT_TRUE(ref);
    EXPECT_EQ(ref->host_guid, "guid-a"); // 未切换
    EXPECT_EQ(ref->state, "paused");     // 保持暂停
    EXPECT_TRUE(fs::is_empty(project.root)); // 没有落数据
}

// 5.3 场景: 目标落后于源 → fetch+pull 快进到源 HEAD;分叉 → 中止
TEST(TeamTakeover, GitAlignment) {
    TempDir source;
    TempDir target;
    ASSERT_EQ(git("git init -q . && git config user.email t@t && git config user.name t && "
                  "echo v1 > f.txt && git add . && git commit -qm v1", source.root), 0);
    ASSERT_EQ(git("git clone -q '" + acecode::path_to_utf8(source.root) + "' .", target.root), 0);

    // 源前进一个提交
    ASSERT_EQ(git("echo v2 > f.txt && git add . && git commit -qm v2", source.root), 0);
    const std::string head = acecode::utils::trim_ascii_copy(
        read_file(source.root / ".git" / "refs" / "heads" / "master").empty()
            ? read_file(source.root / ".git" / "refs" / "heads" / "main")
            : read_file(source.root / ".git" / "refs" / "heads" / "master"));

    // 目标落后:ff-only pull 应快进到源 HEAD
    EXPECT_EQ(align_repo_to_head(acecode::path_to_utf8(target.root), head), "");
    const std::string target_head = acecode::utils::trim_ascii_copy(
        read_file(target.root / ".git" / "refs" / "heads" / "master").empty()
            ? read_file(target.root / ".git" / "refs" / "heads" / "main")
            : read_file(target.root / ".git" / "refs" / "heads" / "master"));
    EXPECT_EQ(target_head, head);

    // 分叉:目标本地提交后,源 HEAD 已不在快进线上 → 中止
    ASSERT_EQ(git("echo local > g.txt && git add . && git commit -qm local", target.root), 0);
    ASSERT_EQ(git("echo v3 > f.txt && git add . && git commit -qm v3", source.root), 0);
    const std::string head3 = acecode::utils::trim_ascii_copy(
        read_file(source.root / ".git" / "refs" / "heads" / "master").empty()
            ? read_file(source.root / ".git" / "refs" / "heads" / "main")
            : read_file(source.root / ".git" / "refs" / "heads" / "master"));
    const std::string align_error = align_repo_to_head(acecode::path_to_utf8(target.root), head3);
    EXPECT_FALSE(align_error.empty());

    // 未提供 HEAD → no-op;非法 HEAD → 拒绝;非 git 目录 → 拒绝
    TempDir plain;
    EXPECT_EQ(align_repo_to_head(acecode::path_to_utf8(plain.root), ""), "");
    EXPECT_EQ(align_repo_to_head(acecode::path_to_utf8(plain.root), "zz"), "invalid_head_commit");
    EXPECT_EQ(align_repo_to_head(acecode::path_to_utf8(plain.root),
                                 "0123456789abcdef0123456789abcdef01234567"), "not_git_repo");
}

// 5.3 场景: 源提交已在本地(无 origin remote)时跳过 fetch,ff-only 直接对齐
TEST(TeamTakeover, GitAlignmentSkipsFetchWhenCommitPresent) {
    TempDir source;
    TempDir target;
    ASSERT_EQ(git("git init -q . && git config user.email t@t && git config user.name t && "
                  "echo v1 > f.txt && git add . && git commit -qm v1", source.root), 0);
    ASSERT_EQ(git("git init -q . && git config user.email t@t && git config user.name t", target.root), 0);
    // 目标是独立仓库,没有 origin:把源的对象直接搬过来(cat-file 可见)。
    ASSERT_EQ(git("git fetch -q '" + acecode::path_to_utf8(source.root) + "' && "
                  "git merge --ff-only FETCH_HEAD", target.root), 0);
    // 源前进一个提交;目标在无 remote 的仓库里通过一次性的 fetch 拿到它
    ASSERT_EQ(git("echo v2 > f.txt && git add . && git commit -qm v2", source.root), 0);
    const std::string head2 = acecode::utils::trim_ascii_copy(
        read_file(source.root / ".git" / "refs" / "heads" / "master").empty()
            ? read_file(source.root / ".git" / "refs" / "heads" / "main")
            : read_file(source.root / ".git" / "refs" / "heads" / "master"));
    // 目标仓库没有 origin:fetch 路径会被"提交已在本地"检查跳过前先失败,
    // 这里模拟"提交已存在"的场景:直接把对象接进来。
    ASSERT_EQ(git("git fetch -q '" + acecode::path_to_utf8(source.root) + "'", target.root), 0);
    // 现在 head2 已在目标本地:align 无需 origin 即可对齐
    EXPECT_EQ(align_repo_to_head(acecode::path_to_utf8(target.root), head2), "");
}
