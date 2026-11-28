// 覆盖 src/host/team/team_service.cpp。团队状态机的关键契约:
//   创建者=管理员、仅管理员能签发/提升、邀请码 15 分钟一次性、
//   加入从邀请者拉全量并回执消费、元数据 LWW 收敛不丢字段、
//   心跳不影响元数据版本戳、团队凭据与 daemon 凭据隔离。
// 网络用注入 fetcher 替代,测试不依赖 socket。

#include <gtest/gtest.h>

#include "platform/crypto/digest.hpp"
#include "team_host/team_service.hpp"
#include "team/team_store.hpp"
#include "team/team_meta.hpp"
#include "team/repo_identity.hpp"
#include "utils/utf8_path.hpp"

#include <chrono>
#include <filesystem>
#include <memory>

namespace fs = std::filesystem;
using namespace acecode::team;

namespace {

struct TempDir {
    fs::path root;
    TempDir() {
        root = fs::temp_directory_path() /
               ("team_service_test_" +
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

DeviceIdentity identity(const std::string& guid, const std::string& account = "user@pc") {
    return DeviceIdentity{guid, account, "显示名-" + guid};
}

TeamInviteLink make_invite(const TeamMeta& meta, const DeviceIdentity& inviter,
                           std::int64_t now) {
    TeamInviteLink invite;
    invite.team_id = meta.team_id;
    invite.team_name = meta.name;
    invite.inviter_name = inviter.display_name;
    invite.inviter_guid = inviter.guid;
    invite.addresses = {"127.0.0.1"};
    invite.port = 12399;
    invite.token = "token-fake";
    invite.expires_at = now + kInviteTtlSeconds;
    return invite;
}

} // namespace

// 2.1 / 2.3 场景: 创建团队 → 创建者是管理员、落盘到 <data>/team/<id>
TEST(TeamService, CreateTeamRegistersCreatorAsAdmin) {
    TempDir temp;
    Clock clock;
    auto service = std::make_unique<TeamService>(temp.utf8(), identity("guid-a"),
        [&] { return clock.now; });

    std::string error;
    const auto meta = service->create_team("研发组", &error);
    ASSERT_TRUE(meta.has_value()) << error;
    EXPECT_EQ(meta->name, "研发组");
    EXPECT_EQ(meta->members.size(), 1u);
    EXPECT_TRUE(is_admin(meta->members[0]));
    EXPECT_EQ(meta->members[0].guid, "guid-a");
    EXPECT_FALSE(meta->invite_seed.empty());
    EXPECT_EQ(meta->created_by, "guid-a");
    EXPECT_TRUE(fs::exists(temp.root / "team" / meta->team_id / "team.json"));

    ASSERT_EQ(service->load_teams().size(), 1u);
    ASSERT_TRUE(service->load_team(meta->team_id).has_value());
}

// 2.1 场景: 未知团队的读取与操作失败,不抛异常
TEST(TeamService, UnknownTeamOperationsFail) {
    TempDir temp;
    Clock clock;
    TeamService service(temp.utf8(), identity("guid-a"), [&] { return clock.now; });
    EXPECT_FALSE(service.load_team("nope").has_value());
    EXPECT_EQ(service.issue_invite("nope", {"127.0.0.1"}, 12399).error, "team_not_found");
    EXPECT_FALSE(service.set_role("nope", "guid-a", "guid-a", "member"));
    EXPECT_FALSE(service.touch_member("nope", "guid-a", ""));
    EXPECT_FALSE(service.accept_join("nope", "t", identity("guid-b")));
}

// 2.2 场景: 团队凭据由 invite_seed 派生,可校验、错误值被拒、与空值隔离
TEST(TeamService, TeamCredentialDerivation) {
    TempDir temp;
    Clock clock;
    TeamService service(temp.utf8(), identity("guid-a"), [&] { return clock.now; });
    std::string error;
    const auto meta = service.create_team("组", &error);
    ASSERT_TRUE(meta.has_value());

    const std::string token = derive_team_token(meta->invite_seed, meta->team_id);
    EXPECT_EQ(token.size(), 64u);
    EXPECT_EQ(token, derive_team_token(meta->invite_seed, meta->team_id)); // 确定性
    EXPECT_TRUE(verify_team_token(*meta, token));
    EXPECT_TRUE(service.authorize(meta->team_id, token));

    // 错误 seed / 错误团队 / 空值都拒绝;团队凭据不能当作另一团队的凭据。
    EXPECT_FALSE(verify_team_token(*meta, "deadbeef"));
    EXPECT_FALSE(verify_team_token(*meta, ""));
    EXPECT_FALSE(service.authorize(meta->team_id, ""));
    TeamMeta other = *meta;
    other.invite_seed = "another-seed";
    EXPECT_FALSE(verify_team_token(other, token));
    other = *meta;
    other.team_id = "other-team";
    EXPECT_FALSE(verify_team_token(other, token));
}

// 2.4 场景: 仅管理员能签发邀请码;签发记录入账本且 15 分钟有效
TEST(TeamService, OnlyAdminCanIssueInvite) {
    TempDir temp;
    Clock clock;
    TeamService host(temp.utf8(), identity("guid-a"), [&] { return clock.now; });
    std::string error;
    const auto meta = host.create_team("组", &error);
    ASSERT_TRUE(meta.has_value());

    const auto issued = host.issue_invite(meta->team_id, {"192.168.1.5"}, 12399);
    ASSERT_TRUE(issued) << issued.error;
    EXPECT_EQ(issued.invite.team_id, meta->team_id);
    EXPECT_EQ(issued.invite.inviter_guid, "guid-a");
    EXPECT_EQ(issued.invite.expires_at, clock.now + kInviteTtlSeconds);
    EXPECT_EQ(issued.invite.port, 12399);

    const InviteLedgerLoad ledger = TeamStore(temp.utf8(), meta->team_id).load_invites();
    ASSERT_EQ(ledger.records.size(), 1u);
    EXPECT_EQ(ledger.records[0].token, issued.invite.token);
    EXPECT_TRUE(invite_usable(ledger.records[0], clock.now));

    // 普通成员(第二个设备视角)不能签发:先把 guid-b 加进团队
    const auto joined = host.accept_join(meta->team_id, issued.invite.token, identity("guid-b"));
    ASSERT_TRUE(joined) << joined.error;
    // 普通成员(第二个设备视角)不能签发:先把 guid-b 加进团队。
    // 独立 data_dir 模拟另一台设备:成员先落地自己的团队快照(设备身份
    // 持久化在各自的 team/ 下)。
    TempDir member_dir;
    TeamService member_service(member_dir.utf8(), identity("guid-b"), [&] { return clock.now; });
    ASSERT_TRUE(member_service.apply_snapshot(*host.load_team(meta->team_id)));
    const auto member_issue = member_service.issue_invite(meta->team_id, {"192.168.1.6"}, 12399);
    EXPECT_EQ(member_issue.error, "not_admin");
}

// 2.4 / 2.5 场景: 邀请码一次性 + 过期拒绝 + 加入者拿到全量快照
TEST(TeamService, InviteIsOneTimeAndJoinPullsSnapshot) {
    TempDir host_dir;
    TempDir joiner_dir;
    Clock clock;
    TeamService host(host_dir.utf8(), identity("guid-a"), [&] { return clock.now; });
    std::string error;
    const auto meta = host.create_team("组", &error);
    ASSERT_TRUE(meta.has_value());
    const auto issued = host.issue_invite(meta->team_id, {"127.0.0.1"}, 12399);
    ASSERT_TRUE(issued) << issued.error;

    auto invite = issued.invite;
    TeamService joiner(joiner_dir.utf8(), identity("guid-b", "bob@b-pc"),
        [&] { return clock.now; });
    const auto fetch = [&](const TeamInviteLink& in, std::string* fetch_error) {
        auto accepted = host.accept_join(in.team_id, in.token, identity("guid-b", "bob@b-pc"));
        if (!accepted) {
            *fetch_error = accepted.error;
            return std::optional<TeamMeta>{};
        }
        return std::optional<TeamMeta>{accepted.snapshot};
    };
    const auto joined = joiner.join_team(invite, fetch);
    ASSERT_TRUE(joined) << joined.error;

    const auto local = joiner.load_team(meta->team_id);
    ASSERT_TRUE(local.has_value());
    EXPECT_EQ(local->name, "组");
    EXPECT_EQ(local->members.size(), 2u);
    ASSERT_TRUE(find_member(*local, "guid-b"));
    EXPECT_EQ(find_member(*local, "guid-b")->role, "member");
    EXPECT_EQ(find_member(*local, "guid-b")->account, "bob@b-pc");

    // 第二次使用同一 token:已消费,拒绝
    const auto second = host.accept_join(meta->team_id, invite.token, identity("guid-c"));
    EXPECT_EQ(second.error, "invite_invalid");
    // 新签发一条邀请码,越过 15 分钟后消费:过期拒绝
    const auto later = host.issue_invite(meta->team_id, {"127.0.0.1"}, 12399);
    ASSERT_TRUE(later) << later.error;
    clock.now = later.invite.expires_at + 1;
    EXPECT_EQ(host.accept_join(meta->team_id, later.invite.token, identity("guid-d")).error,
              "invite_invalid");
}

// 2.5 场景: 团队成员已满 50 人时拒绝加入
TEST(TeamService, JoinRejectedWhenTeamFull) {
    TempDir temp;
    Clock clock;
    TeamService host(temp.utf8(), identity("guid-owner"), [&] { return clock.now; });
    std::string error;
    auto meta = host.create_team("满员队", &error);
    ASSERT_TRUE(meta.has_value());
    // 直接写入 50 人(创建者 + 49)
    for (int i = 1; i < kMaxTeamMembers; ++i) {
        TeamMember member;
        member.guid = "guid-" + std::to_string(i);
        member.role = "member";
        member.updated_at = clock.now;
        meta->members.push_back(member);
    }
    ASSERT_TRUE(TeamStore(temp.utf8(), meta->team_id).save_meta(*meta));
    const auto issued = host.issue_invite(meta->team_id, {"127.0.0.1"}, 12399);
    ASSERT_TRUE(issued) << issued.error;

    const auto accepted = host.accept_join(meta->team_id, issued.invite.token,
                                           identity("guid-51"));
    EXPECT_EQ(accepted.error, "team_full");
}

// 2.6 场景: 双端并发修改逐字段 LWW 合并不丢字段,last_seen 取 max
TEST(TeamService, MergeRemoteConvergesPerField) {
    TempDir temp;
    Clock clock;
    TeamService service(temp.utf8(), identity("guid-a"), [&] { return clock.now; });
    std::string error;
    auto meta = service.create_team("旧名", &error);
    ASSERT_TRUE(meta.has_value());

    TeamMeta remote = *meta;
    // B 端把团队名、成员角色改得更新
    remote.name = "新名";
    remote.updated_at = clock.now + 10;
    remote.updated_by = "guid-b";
    ASSERT_TRUE(find_member(remote, "guid-a"));
    find_member(remote, "guid-a")->role = "member";
    find_member(remote, "guid-a")->updated_at = clock.now + 10;
    find_member(remote, "guid-a")->updated_by = "guid-b";
    // 新成员 C
    TeamMember c;
    c.guid = "guid-c";
    c.role = "member";
    c.updated_at = clock.now + 5;
    c.updated_by = "guid-b";
    c.last_seen = clock.now + 5;
    remote.members.push_back(c);
    // 新会话引用与仓库身份
    remote.sessions.push_back({"s1", "会话", "guid-b", "running", "", "guid-b", clock.now, clock.now});
    remote.repos.push_back({"remote", "github.com/Org/Repo"});

    const auto merged = service.merge_remote(meta->team_id, remote, &error);
    ASSERT_TRUE(merged.has_value()) << error;
    EXPECT_EQ(merged->name, "新名");
    EXPECT_EQ(merged->members.size(), 2u);
    EXPECT_EQ(find_member(*merged, "guid-a")->role, "member");
    ASSERT_TRUE(find_member(*merged, "guid-c"));
    ASSERT_EQ(merged->sessions.size(), 1u);
    EXPECT_EQ(merged->sessions[0].state, "running");
    ASSERT_EQ(merged->repos.size(), 1u);
    EXPECT_TRUE(team_matches_repo(*merged, RepoIdentity{"remote", "github.com/Org/Repo"}));

    // 落盘的合并结果与返回值一致(重复 merge 幂等)
    const auto again = service.merge_remote(meta->team_id, remote, &error);
    ASSERT_TRUE(again.has_value());
    EXPECT_EQ(again->members.size(), merged->members.size());
    const auto stored = service.load_team(meta->team_id);
    ASSERT_TRUE(stored.has_value());
    EXPECT_EQ(stored->name, "新名");
}

// 2.6 场景: 心跳只更新 last_seen/地址,不改变元数据版本戳
TEST(TeamService, HeartbeatDoesNotBumpMetadataVersion) {
    TempDir temp;
    Clock clock;
    TeamService service(temp.utf8(), identity("guid-a"), [&] { return clock.now; });
    std::string error;
    const auto meta = service.create_team("组", &error);
    ASSERT_TRUE(meta.has_value());
    const std::int64_t original_updated = meta->updated_at;
    EXPECT_TRUE(service.member_online(*meta, "guid-a"));

    clock.now = original_updated + 30;
    ASSERT_TRUE(service.touch_member(meta->team_id, "guid-a", "10.0.0.2:12399"));
    const auto reloaded = service.load_team(meta->team_id);
    ASSERT_TRUE(reloaded.has_value());
    EXPECT_EQ(reloaded->updated_at, original_updated);
    EXPECT_EQ(find_member(*reloaded, "guid-a")->last_seen, clock.now);
    EXPECT_EQ(find_member(*reloaded, "guid-a")->address, "10.0.0.2:12399");

    // 超过 stale 窗口判定离线
    clock.now += kMemberStaleSeconds + 1;
    EXPECT_FALSE(service.member_online(*reloaded, "guid-a"));
    EXPECT_FALSE(service.member_online(*reloaded, "guid-unknown"));
}

// 2.7 场景: 管理员可提升/降级;普通成员与非法角色被拒
TEST(TeamService, RoleManagementRequiresAdmin) {
    TempDir temp;
    Clock clock;
    TeamService host(temp.utf8(), identity("guid-a"), [&] { return clock.now; });
    std::string error;
    const auto meta = host.create_team("组", &error);
    ASSERT_TRUE(meta.has_value());
    const auto issued = host.issue_invite(meta->team_id, {"127.0.0.1"}, 12399);
    ASSERT_TRUE(issued) << issued.error;
    ASSERT_TRUE(host.accept_join(meta->team_id, issued.invite.token, identity("guid-b")));

    // 普通成员不能改角色
    EXPECT_EQ(host.set_role(meta->team_id, "guid-b", "guid-a", "member").error, "not_admin");
    // 管理员提升 B
    EXPECT_TRUE(host.set_role(meta->team_id, "guid-a", "guid-b", "admin"));
    auto promoted = host.load_team(meta->team_id);
    ASSERT_TRUE(promoted.has_value());
    EXPECT_TRUE(is_admin(*find_member(*promoted, "guid-b")));
    EXPECT_EQ(promoted->updated_by, "guid-a");

    // 新管理员可以降级原管理员
    EXPECT_TRUE(host.set_role(meta->team_id, "guid-b", "guid-a", "member"));
    auto demoted = host.load_team(meta->team_id);
    ASSERT_TRUE(demoted.has_value());
    EXPECT_FALSE(is_admin(*find_member(*demoted, "guid-a")));

    // 非法角色与未知成员
    EXPECT_EQ(host.set_role(meta->team_id, "guid-b", "guid-a", "owner").error, "invalid_role");
    EXPECT_EQ(host.set_role(meta->team_id, "guid-b", "guid-x", "admin").error, "member_not_found");
}

// 关联项目:幂等登记仓库身份,重复登记不重复落条目
TEST(TeamService, RegisterRepoIsIdempotent) {
    TempDir temp;
    Clock clock;
    TeamService service(temp.utf8(), identity("guid-a"), [&] { return clock.now; });
    std::string error;
    const auto meta = service.create_team("组", &error);
    ASSERT_TRUE(meta.has_value());

    EXPECT_TRUE(service.register_repo(meta->team_id, RepoIdentity{"remote", "github.com/Org/Repo"}));
    EXPECT_TRUE(service.register_repo(meta->team_id, RepoIdentity{"remote", "github.com/Org/Repo"}));
    const auto stored = service.load_team(meta->team_id);
    ASSERT_TRUE(stored.has_value());
    ASSERT_EQ(stored->repos.size(), 1u);
    EXPECT_TRUE(team_matches_repo(*stored, RepoIdentity{"remote", "github.com/Org/Repo"}));
    EXPECT_EQ(service.register_repo(meta->team_id, RepoIdentity{}).error, "invalid_repo");
    EXPECT_EQ(service.register_repo("nope", RepoIdentity{"remote", "x"}).error, "team_not_found");
}

namespace {

TeamMember make_member(const std::string& guid, const std::string& role,
                       std::int64_t joined_at) {
    TeamMember member;
    member.guid = guid;
    member.account = guid + "@pc";
    member.role = role;
    member.joined_at = joined_at;
    member.last_seen = joined_at;
    member.updated_at = joined_at;
    member.updated_by = guid;
    return member;
}

TeamSessionRef make_session(const std::string& id, const std::string& host_guid,
                            std::int64_t updated_at) {
    TeamSessionRef ref;
    ref.session_id = id;
    ref.title = id;
    ref.host_guid = host_guid;
    ref.state = std::string(kSessionStateRunning);
    ref.created_by = host_guid;
    ref.created_at = updated_at;
    ref.updated_at = updated_at;
    return ref;
}

} // namespace

// 退出场景: 暂停本机 host 会话、写墓碑、最后管理员自动转让给最早加入者
TEST(TeamService, LeavePausesHostedSessionsAndTransfersAdmin) {
    TempDir temp;
    Clock clock;
    TeamService service(temp.utf8(), identity("guid-a"), [&] { return clock.now; });
    std::string error;
    auto meta = service.create_team("组", &error);
    ASSERT_TRUE(meta.has_value());
    meta->members.push_back(make_member("guid-c", "member", clock.now + 20));
    meta->members.push_back(make_member("guid-b", "member", clock.now + 10));
    meta->sessions.push_back(make_session("s-local", "guid-a", clock.now));
    meta->sessions.push_back(make_session("s-remote", "guid-b", clock.now));
    ASSERT_TRUE(TeamStore(temp.utf8(), meta->team_id).save_meta(*meta));

    clock.now += 100;
    const auto left = service.leave_team(meta->team_id);
    ASSERT_TRUE(left) << left.error;
    ASSERT_EQ(left.paused_sessions.size(), 1u);
    EXPECT_EQ(left.paused_sessions[0], "s-local");
    EXPECT_EQ(left.promoted_guid, "guid-b"); // joined_at 更早

    const auto self = find_member(left.snapshot, "guid-a");
    ASSERT_TRUE(self);
    EXPECT_FALSE(member_active(*self));
    EXPECT_EQ(self->left_at, clock.now);
    EXPECT_EQ(self->role, "member");
    EXPECT_EQ(find_member(left.snapshot, "guid-b")->role, "admin");
    EXPECT_EQ(find_member(left.snapshot, "guid-c")->role, "member");

    const auto* local_ref = find_session(left.snapshot, "s-local");
    ASSERT_TRUE(local_ref);
    EXPECT_EQ(local_ref->state, "paused");
    EXPECT_EQ(local_ref->pause_reason, "设备已退出");
    EXPECT_EQ(find_session(left.snapshot, "s-remote")->state, "running");

    // 退出不等于归档: 团队仍在活跃列表里,直到调用方完成本地归档。
    EXPECT_EQ(service.load_teams().size(), 1u);
    EXPECT_TRUE(service.archive_team(meta->team_id));
    EXPECT_TRUE(service.load_teams().empty());
    auto archived = service.load_archived_teams();
    ASSERT_EQ(archived.size(), 1u);
    EXPECT_TRUE(archived[0].invite_seed.empty()); // 不再持有团队凭据
    EXPECT_TRUE(service.load_team(meta->team_id)->invite_seed.empty());
}

// 退出场景: 团队还有其他管理员时不再提升普通成员
TEST(TeamService, LeaveDoesNotPromoteWhenAnotherAdminExists) {
    TempDir temp;
    Clock clock;
    TeamService service(temp.utf8(), identity("guid-a"), [&] { return clock.now; });
    std::string error;
    auto meta = service.create_team("组", &error);
    ASSERT_TRUE(meta.has_value());
    meta->members.push_back(make_member("guid-b", "admin", clock.now + 10));
    meta->members.push_back(make_member("guid-c", "member", clock.now + 20));
    ASSERT_TRUE(TeamStore(temp.utf8(), meta->team_id).save_meta(*meta));

    clock.now += 100;
    const auto left = service.leave_team(meta->team_id);
    ASSERT_TRUE(left) << left.error;
    EXPECT_TRUE(left.promoted_guid.empty());
    EXPECT_EQ(find_member(left.snapshot, "guid-b")->role, "admin");
    EXPECT_EQ(find_member(left.snapshot, "guid-c")->role, "member");
}

// 退出场景: 非成员与重复退出被拒;活跃团队不能被直接删除
TEST(TeamService, LeaveRequiresActiveMemberAndDeleteRequiresArchive) {
    TempDir temp;
    Clock clock;
    TeamService service(temp.utf8(), identity("guid-a"), [&] { return clock.now; });
    std::string error;
    const auto meta = service.create_team("组", &error);
    ASSERT_TRUE(meta.has_value());

    EXPECT_EQ(service.delete_archived_team(meta->team_id).error, "team_not_archived");
    ASSERT_TRUE(service.leave_team(meta->team_id));
    EXPECT_TRUE(service.leave_team(meta->team_id)); // 幂等:可继续完成归档
    EXPECT_TRUE(service.archive_team(meta->team_id));

    TempDir other_dir;
    TeamService other(other_dir.utf8(), identity("guid-x"), [&] { return clock.now; });
    EXPECT_EQ(other.leave_team(meta->team_id).error, "team_not_found");
    EXPECT_TRUE(service.delete_archived_team(meta->team_id));
    EXPECT_FALSE(service.load_team(meta->team_id).has_value());
    EXPECT_TRUE(service.load_archived_teams().empty());
}

// 退出同步场景: 墓碑经 LWW 收敛;重新加入复用原成员记录
TEST(TeamService, LeaveTombstoneMergesAndRejoinReactivatesMember) {
    TempDir a_dir;
    TempDir b_dir;
    Clock clock;
    TeamService a(a_dir.utf8(), identity("guid-a"), [&] { return clock.now; });
    std::string error;
    const auto created = a.create_team("组", &error);
    ASSERT_TRUE(created.has_value());
    auto with_b = *created;
    with_b.members.push_back(make_member("guid-b", "member", clock.now + 10));
    ASSERT_TRUE(TeamStore(a_dir.utf8(), created->team_id).save_meta(with_b));

    TeamService b(b_dir.utf8(), identity("guid-b"), [&] { return clock.now; });
    ASSERT_TRUE(b.apply_snapshot(with_b));

    clock.now += 100;
    const auto left = a.leave_team(created->team_id);
    ASSERT_TRUE(left) << left.error;
    // B 收到退出快照后把 A 视为已退出,但保留墓碑记录。
    const auto merged = b.merge_remote(created->team_id, left.snapshot, &error);
    ASSERT_TRUE(merged.has_value()) << error;
    ASSERT_TRUE(find_member(*merged, "guid-a"));
    EXPECT_FALSE(member_active(*find_member(*merged, "guid-a")));
    EXPECT_TRUE(is_admin(*find_member(*merged, "guid-b")));

    // B 现在是管理员,签发邀请码后 A 用同一 GUID 重新加入。
    const auto issued = b.issue_invite(created->team_id, {"127.0.0.1"}, 12399);
    ASSERT_TRUE(issued) << issued.error;
    const auto rejoined = b.accept_join(created->team_id, issued.invite.token,
                                        identity("guid-a", "alice@a-pc"));
    ASSERT_TRUE(rejoined) << rejoined.error;
    EXPECT_EQ(rejoined.snapshot.members.size(), 2u); // 不产生重复成员
    const auto* a_member = find_member(rejoined.snapshot, "guid-a");
    ASSERT_TRUE(a_member);
    EXPECT_TRUE(member_active(*a_member));
    EXPECT_EQ(a_member->left_at, 0);
    EXPECT_EQ(a_member->role, "member");
    EXPECT_EQ(a_member->account, "alice@a-pc");
}

// 退出后本地团队状态归档;重新加入时清掉归档状态
TEST(TeamService, RejoinClearsLocalArchiveState) {
    TempDir temp;
    Clock clock;
    TeamService service(temp.utf8(), identity("guid-a"), [&] { return clock.now; });
    std::string error;
    const auto meta = service.create_team("组", &error);
    ASSERT_TRUE(meta.has_value());
    ASSERT_TRUE(service.leave_team(meta->team_id));
    ASSERT_TRUE(service.archive_team(meta->team_id));
    EXPECT_TRUE(service.is_team_archived(meta->team_id));

    ASSERT_TRUE(service.apply_snapshot(*service.load_team(meta->team_id)));
    EXPECT_FALSE(service.is_team_archived(meta->team_id));
}

// 退出场景: 旧数据 joined_at=0 视为更早加入,优先于带时间戳的新成员
TEST(TeamService, LeavePromotesLegacyMemberBeforeNewerJoin) {
    TempDir temp;
    Clock clock;
    TeamService service(temp.utf8(), identity("guid-a"), [&] { return clock.now; });
    std::string error;
    auto meta = service.create_team("组", &error);
    ASSERT_TRUE(meta.has_value());
    meta->members.push_back(make_member("guid-legacy", "member", 0));
    meta->members.push_back(make_member("guid-new", "member", clock.now + 10));
    ASSERT_TRUE(TeamStore(temp.utf8(), meta->team_id).save_meta(*meta));

    clock.now += 100;
    const auto left = service.leave_team(meta->team_id);
    ASSERT_TRUE(left) << left.error;
    EXPECT_EQ(left.promoted_guid, "guid-legacy");
}
