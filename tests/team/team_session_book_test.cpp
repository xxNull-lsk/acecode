// 覆盖 src/host/team/team_session_book.cpp 与 TeamService 的会话引用操作
// (tasks 3.1/3.2/3.4/3.5)。关键契约:
//   准入:仓库未关联拒绝、第 6 个并发拒绝、ended 释放槽位;
//   引用生命周期:发起=host、状态变更带 updated_at、切换 host 后双端收敛;
//   消息中继:忙时按 FIFO 入队不丢失、投递失败停在队首、发送者标注;
//   副本同步:增量追加、副本内容与 host 一致。

#include <gtest/gtest.h>

#include "team_host/team_session_book.hpp"
#include "team_host/team_service.hpp"
#include "team/team_store.hpp"
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
               ("team_session_book_test_" +
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

TeamMeta team_with_repo(const std::string& team_id = "team-1") {
    TeamMeta meta;
    meta.team_id = team_id;
    meta.name = "研发组";
    meta.invite_seed = "seed";
    TeamMember a;
    a.guid = "guid-a";
    a.role = "admin";
    a.updated_at = 1;
    meta.members.push_back(a);
    TeamMember b;
    b.guid = "guid-b";
    b.role = "member";
    b.updated_at = 1;
    meta.members.push_back(b);
    meta.repos.push_back({"remote", "github.com/Org/Repo"});
    meta.updated_at = 1;
    meta.updated_by = "guid-a";
    return meta;
}

void write_file(const fs::path& path, const std::string& content) {
    std::ofstream(path, std::ios::binary) << content;
}

std::string read_file(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    std::ostringstream buffer;
    buffer << input.rdbuf();
    return buffer.str();
}

} // namespace

// 3.1 场景: 仓库未关联 / 会话 ID 为空 / 第 6 个并发会话被拒
TEST(TeamSessionBook, AdmissionChecks) {
    const TeamMeta meta = team_with_repo();
    EXPECT_EQ(check_session_admission(meta, RepoIdentity{"remote", "github.com/Org/Repo"}, "s1").ok,
              true);
    EXPECT_EQ(check_session_admission(meta, RepoIdentity{"remote", "github.com/Org/Other"}, "s1")
                  .error,
              "repo_not_linked");
    EXPECT_EQ(check_session_admission(meta, RepoIdentity{"remote", "github.com/Org/Repo"}, "")
                  .error,
              "invalid_session");

    // 已有 5 个进行中会话(paused 占槽,ended 不占)
    TeamMeta busy = meta;
    for (int i = 0; i < kMaxConcurrentTeamSessions; ++i) {
        busy.sessions.push_back({"s" + std::to_string(i), "", "guid-a", "running", "", "guid-a",
                                 1, 1});
    }
    EXPECT_EQ(check_session_admission(busy, RepoIdentity{"remote", "github.com/Org/Repo"}, "s6")
                  .error,
              "too_many_concurrent_sessions");
    busy.sessions[0].state = "ended";
    EXPECT_EQ(check_session_admission(busy, RepoIdentity{"remote", "github.com/Org/Repo"}, "s6").ok,
              true);
}

// 3.1 场景: 发起团队会话登记 session_refs,发起设备成为 host;Service 层拒绝重复登记
TEST(TeamSessionBook, StartSessionRegistersRef) {
    TempDir temp;
    Clock clock;
    TeamService service(temp.utf8(), identity("guid-a"), [&] { return clock.now; });
    ASSERT_TRUE(service.apply_snapshot(team_with_repo()));

    const auto started = service.start_session("team-1",
        RepoIdentity{"remote", "github.com/Org/Repo"}, "s-1", "调试登录");
    ASSERT_TRUE(started) << started.error;
    EXPECT_EQ(started.ref.host_guid, "guid-a");
    EXPECT_EQ(started.ref.state, "running");
    EXPECT_EQ(started.ref.created_by, "guid-a");
    EXPECT_EQ(started.ref.updated_at, clock.now);

    const auto stored = service.load_team("team-1");
    ASSERT_TRUE(stored.has_value());
    ASSERT_EQ(stored->sessions.size(), 1u);
    EXPECT_EQ(stored->sessions[0].session_id, "s-1");
    EXPECT_EQ(stored->updated_by, "guid-a");

    // 非关联仓库 / 重复登记
    EXPECT_EQ(service.start_session("team-1", RepoIdentity{"remote", "github.com/Org/X"}, "s-2", "")
                  .error,
              "repo_not_linked");
    EXPECT_EQ(service.start_session("team-1", RepoIdentity{"remote", "github.com/Org/Repo"}, "s-1", "")
                  .error,
              "session_already_registered");
}

// 3.1 场景: SessionMeta 的团队标记;普通会话保持为空
TEST(TeamSessionBook, MarkSessionMeta) {
    acecode::SessionMeta meta;
    mark_team_session_meta(meta, "team-1", "host");
    EXPECT_EQ(meta.team_id, "team-1");
    EXPECT_EQ(meta.team_role, "host");
    mark_team_session_meta(meta, "", "");
    EXPECT_TRUE(meta.team_id.empty());
    EXPECT_TRUE(meta.team_role.empty());
}

// 3.2 场景: 结束/暂停变更状态;ended 释放并发槽位后能再发起
TEST(TeamSessionBook, SessionStateLifecycle) {
    TempDir temp;
    Clock clock;
    TeamService service(temp.utf8(), identity("guid-a"), [&] { return clock.now; });
    ASSERT_TRUE(service.apply_snapshot(team_with_repo()));
    ASSERT_TRUE(service.start_session("team-1", RepoIdentity{"remote", "github.com/Org/Repo"},
                                      "s-1", "x"));

    EXPECT_TRUE(service.set_session_state("team-1", "s-1", "paused", "失联"));
    auto paused = service.load_team("team-1");
    ASSERT_TRUE(paused.has_value());
    EXPECT_EQ(paused->sessions[0].state, "paused");
    EXPECT_EQ(paused->sessions[0].pause_reason, "失联");

    clock.now += 10;
    EXPECT_TRUE(service.set_session_state("team-1", "s-1", "ended", ""));
    const auto ended = service.load_team("team-1");
    ASSERT_TRUE(ended.has_value());
    EXPECT_EQ(ended->sessions[0].state, "ended");
    EXPECT_EQ(ended->sessions[0].pause_reason, "");
    EXPECT_EQ(ended->sessions[0].updated_at, clock.now);
    EXPECT_EQ(active_session_count(*ended), 0);

    // 未知会话/非法状态
    EXPECT_EQ(service.set_session_state("team-1", "s-x", "ended", "").error, "session_not_found");
    EXPECT_EQ(service.set_session_state("team-1", "s-1", "flying", "").error, "invalid_state");
}

// 3.2 场景: 切换 host 后另一台设备经 merge_remote 收敛,列表一致
TEST(TeamSessionBook, SwitchHostConvergesAcrossDevices) {
    TempDir host_dir;
    TempDir member_dir;
    Clock clock;
    TeamService host(host_dir.utf8(), identity("guid-a"), [&] { return clock.now; });
    TeamService member(member_dir.utf8(), identity("guid-b"), [&] { return clock.now; });
    ASSERT_TRUE(member.apply_snapshot(team_with_repo()));

    // host 从 member 的快照创建并切换权威副本到 guid-b
    ASSERT_TRUE(host.apply_snapshot(team_with_repo()));
    ASSERT_TRUE(host.start_session("team-1", RepoIdentity{"remote", "github.com/Org/Repo"},
                                   "s-1", "x"));
    clock.now += 5;
    EXPECT_TRUE(host.set_session_host("team-1", "s-1", "guid-b"));
    EXPECT_EQ(host.set_session_host("team-1", "s-1", "guid-x").error, "member_not_found");

    // member 拉取 host 的最新快照合并:会话列表与 host 一致
    std::string error;
    const auto merged = member.merge_remote("team-1", *host.load_team("team-1"), &error);
    ASSERT_TRUE(merged.has_value()) << error;
    ASSERT_EQ(merged->sessions.size(), 1u);
    EXPECT_EQ(merged->sessions[0].host_guid, "guid-b");
    EXPECT_EQ(merged->sessions[0].updated_at, clock.now);
    const auto member_stored = member.load_team("team-1");
    ASSERT_TRUE(member_stored.has_value());
    EXPECT_EQ(member_stored->sessions[0].host_guid, "guid-b");
}

// 3.4 场景: host 忙时消息按 FIFO 入队,flush 按顺序投递不丢失
TEST(TeamMessageRelay, BusyMessagesQueueInOrder) {
    std::vector<std::string> delivered;
    TeamMessageRelay relay([&](const TeamInboundMessage& message) {
        delivered.push_back(message.text);
        return true;
    });

    auto make = [&](const std::string& text) {
        TeamInboundMessage message;
        message.team_id = "team-1";
        message.session_id = "s-1";
        message.sender_guid = "guid-b";
        message.sender_name = "Bob";
        message.text = text;
        return message;
    };

    EXPECT_EQ(relay.receive(make("先跑测试"), true), RelayOutcome::Queued);
    EXPECT_EQ(relay.receive(make("再看日志"), true), RelayOutcome::Queued);
    EXPECT_EQ(relay.pending(), 2u);
    EXPECT_EQ(relay.flush(), 2u);
    ASSERT_EQ(delivered.size(), 2u);
    EXPECT_EQ(delivered[0], "先跑测试");
    EXPECT_EQ(delivered[1], "再看日志");
    EXPECT_EQ(relay.pending(), 0u);
    EXPECT_EQ(relay.flush(), 0u);
}

// 3.4 场景: 空闲时直接投递;投递失败停在队首,flush 重试保序
TEST(TeamMessageRelay, DeliverFailureKeepsHeadAndOrder) {
    std::vector<std::string> delivered;
    bool accept = true;
    TeamMessageRelay relay([&](const TeamInboundMessage& message) {
        if (!accept) return false;
        delivered.push_back(message.text);
        return true;
    });

    auto make = [&](const std::string& text) {
        TeamInboundMessage message;
        message.session_id = "s-1";
        message.sender_guid = "guid-b";
        message.text = text;
        return message;
    };

    EXPECT_EQ(relay.receive(make("m1"), false), RelayOutcome::Delivered);
    accept = false;
    EXPECT_EQ(relay.receive(make("m2"), false), RelayOutcome::Queued);
    EXPECT_EQ(relay.receive(make("m3"), true), RelayOutcome::Queued);
    EXPECT_EQ(relay.flush(), 0u); // 队首 m2 失败,m3 不能跳过
    accept = true;
    EXPECT_EQ(relay.flush(), 2u);
    ASSERT_EQ(delivered.size(), 3u);
    EXPECT_EQ(delivered[1], "m2");
    EXPECT_EQ(delivered[2], "m3");
}

// 3.4 场景: 非法消息拒绝;容量满时入队失败(调用方须重试);暂停期间保留队列
TEST(TeamMessageRelay, RejectionsAndPausedQueue) {
    std::vector<std::string> delivered;
    TeamMessageRelay relay([&](const TeamInboundMessage& message) {
        delivered.push_back(message.text);
        return true;
    }, 2);

    TeamInboundMessage invalid;
    invalid.text = "no session";
    EXPECT_EQ(relay.receive(invalid, false), RelayOutcome::Rejected);

    auto make = [&](const std::string& text) {
        TeamInboundMessage message;
        message.session_id = "s-1";
        message.sender_guid = "b";
        message.text = text;
        return message;
    };
    relay.set_paused(true);
    EXPECT_EQ(relay.receive(make("p1"), true), RelayOutcome::Queued);
    EXPECT_EQ(relay.receive(make("p2"), true), RelayOutcome::Queued);
    EXPECT_EQ(relay.receive(make("p3"), true), RelayOutcome::Queued); // 容量 2 已满
    EXPECT_EQ(relay.pending(), 2u);
    EXPECT_EQ(relay.flush(), 0u); // 暂停时不投递,消息保留
    relay.set_paused(false);
    EXPECT_EQ(relay.flush(), 2u);
}

// 3.4 场景: 消息标注发送者(展示名优先,否则 GUID)
TEST(TeamMessageRelay, LabelsSender) {
    TeamInboundMessage message;
    message.sender_guid = "guid-b";
    message.sender_name = "Bob";
    message.text = "帮我跑下测试";
    EXPECT_EQ(TeamMessageRelay::labeled_text(message), "Bob: 帮我跑下测试");
    message.sender_name.clear();
    EXPECT_EQ(TeamMessageRelay::labeled_text(message), "guid-b: 帮我跑下测试");
}

// 5.4 场景: 会话事件载荷 — 暂停带原因、host 变更带新 host,字段直接来自引用
TEST(TeamSessionEvent, PayloadFields) {
    TeamSessionRef ref;
    ref.session_id = "s-1";
    ref.title = "调试登录";
    ref.host_guid = "guid-b";
    ref.state = "paused";
    ref.pause_reason = "失联";
    ref.updated_at = 1'700'000'123;

    const auto paused = team_session_event_json("paused", ref);
    EXPECT_EQ(paused["kind"], "paused");
    EXPECT_EQ(paused["session_id"], "s-1");
    EXPECT_EQ(paused["title"], "调试登录");
    EXPECT_EQ(paused["host_guid"], "guid-b");
    EXPECT_EQ(paused["state"], "paused");
    EXPECT_EQ(paused["pause_reason"], "失联");
    EXPECT_EQ(paused["updated_at"], 1'700'000'123);

    // host 变更事件:无暂停原因时不写该字段
    ref.state = "running";
    ref.pause_reason.clear();
    const auto switched = team_session_event_json("host_changed", ref);
    EXPECT_EQ(switched["kind"], "host_changed");
    EXPECT_FALSE(switched.contains("pause_reason"));
}

// 3.5 场景: 副本同步 — 首次全量,再写后只追加尾部,内容与 host 一致
TEST(TeamSessionBook, ReplicaSyncIncremental) {
    TempDir temp;
    TempDir project;
    const TeamStore store(temp.utf8(), "team-1");

    const fs::path jsonl = project.root / "s-1.jsonl";
    const fs::path meta = project.root / "s-1.meta.json";
    write_file(jsonl, "{\"role\":\"user\"}\n");
    write_file(meta, R"({"id":"s-1","team_id":"team-1","team_role":"host"})");

    const auto first = sync_session_replica(store, "s-1", acecode::path_to_utf8(jsonl),
                                            acecode::path_to_utf8(meta));
    ASSERT_TRUE(first) << first.error;
    EXPECT_EQ(first.appended_bytes, 16);
    EXPECT_TRUE(first.meta_rewritten);

    const fs::path replica_dir = store.replica_dir("s-1");
    ASSERT_EQ(read_file(replica_dir / "s-1.jsonl"), read_file(jsonl));
    ASSERT_EQ(read_file(replica_dir / "s-1.meta.json"), read_file(meta));

    // host 追加两条消息后再次同步:只搬增量,副本内容仍一致
    const std::string appended = "{\"role\":\"assistant\"}\n{\"role\":\"user\"}\n";
    {
        std::ofstream out(jsonl, std::ios::binary | std::ios::app);
        out << appended;
    }
    const auto second = sync_session_replica(store, "s-1", acecode::path_to_utf8(jsonl),
                                             acecode::path_to_utf8(meta));
    ASSERT_TRUE(second) << second.error;
    EXPECT_EQ(second.appended_bytes, static_cast<std::int64_t>(appended.size()));
    EXPECT_FALSE(second.meta_rewritten); // meta 未变
    EXPECT_EQ(read_file(replica_dir / "s-1.jsonl"), read_file(jsonl));

    // 源文件被重写(比副本小)时整体覆盖
    write_file(jsonl, "{\"role\":\"user\"}\n");
    const auto third = sync_session_replica(store, "s-1", acecode::path_to_utf8(jsonl),
                                            acecode::path_to_utf8(meta));
    ASSERT_TRUE(third) << third.error;
    EXPECT_EQ(read_file(replica_dir / "s-1.jsonl"), read_file(jsonl));

    // 源缺失报错
    EXPECT_EQ(sync_session_replica(store, "s-1",
                                   acecode::path_to_utf8(project.root / "missing.jsonl"), "")
                  .error,
              "source_unreadable");
    EXPECT_EQ(sync_session_replica(store, "", "", "").error, "invalid_session");
}
