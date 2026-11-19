// 覆盖 src/domain/team/team_meta.cpp。TeamMeta 是去中心化团队状态的单一载体,
// 序列化契约锁定两件事:稀疏字段(空值省略)与旧文件缺字段按默认值读回 —
// 一旦行为漂移,队员间同步会把"从未修改"当成"被清空",逐字段合并会丢数据。

#include <gtest/gtest.h>

#include "team/team_meta.hpp"

namespace acecode::team {

// 场景: 完整 TeamMeta 往返后逐字段一致
TEST(TeamMeta, RoundTripPreservesAllFields) {
    TeamMeta meta;
    meta.team_id = "team-1";
    meta.name = "研发组";
    meta.invite_seed = "seed-abc";
    meta.created_at = 1000;
    meta.created_by = "guid-a";
    meta.updated_at = 2000;
    meta.updated_by = "guid-b";

    TeamMember admin;
    admin.account = "allan@office-pc";
    admin.guid = "guid-a";
    admin.display_name = "老王";
    admin.role = "admin";
    admin.address = "192.168.1.10:12399";
    admin.last_seen = 1500;
    admin.updated_at = 1200;
    admin.updated_by = "guid-a";
    meta.members.push_back(admin);

    TeamMember member;
    member.guid = "guid-b";
    member.role = "member";
    meta.members.push_back(member);

    TeamSessionRef session;
    session.session_id = "20260101_000000_abcd";
    session.title = "修 bug";
    session.host_guid = "guid-a";
    session.state = "paused";
    session.pause_reason = "失联";
    session.created_by = "guid-b";
    session.created_at = 1400;
    meta.sessions.push_back(session);

    RepoIdentity remote;
    remote.kind = "remote";
    remote.value = "github.com/org/repo";
    meta.repos.push_back(remote);
    RepoIdentity local;
    local.kind = "local-id";
    local.value = "b3f2-uuid";
    meta.repos.push_back(local);

    const TeamMeta decoded = team_meta_from_json(team_meta_to_json(meta));
    EXPECT_EQ(decoded.team_id, meta.team_id);
    EXPECT_EQ(decoded.name, meta.name);
    EXPECT_EQ(decoded.invite_seed, meta.invite_seed);
    EXPECT_EQ(decoded.created_at, 1000);
    EXPECT_EQ(decoded.created_by, "guid-a");
    EXPECT_EQ(decoded.updated_at, 2000);
    EXPECT_EQ(decoded.updated_by, "guid-b");

    ASSERT_EQ(decoded.members.size(), 2u);
    EXPECT_EQ(decoded.members[0].account, "allan@office-pc");
    EXPECT_EQ(decoded.members[0].display_name, "老王");
    EXPECT_EQ(decoded.members[0].role, "admin");
    EXPECT_EQ(decoded.members[0].address, "192.168.1.10:12399");
    EXPECT_EQ(decoded.members[0].last_seen, 1500);
    EXPECT_EQ(decoded.members[0].updated_at, 1200);
    EXPECT_EQ(decoded.members[0].updated_by, "guid-a");
    EXPECT_EQ(decoded.members[1].guid, "guid-b");
    EXPECT_EQ(decoded.members[1].role, "member");
    EXPECT_TRUE(decoded.members[1].account.empty());

    ASSERT_EQ(decoded.sessions.size(), 1u);
    EXPECT_EQ(decoded.sessions[0].session_id, "20260101_000000_abcd");
    EXPECT_EQ(decoded.sessions[0].state, "paused");
    EXPECT_EQ(decoded.sessions[0].pause_reason, "失联");
    EXPECT_EQ(decoded.sessions[0].host_guid, "guid-a");

    ASSERT_EQ(decoded.repos.size(), 2u);
    EXPECT_EQ(decoded.repos[0].kind, "remote");
    EXPECT_EQ(decoded.repos[1].value, "b3f2-uuid");
}

// 场景: 旧版本文件缺字段、空对象也能读回,不抛异常
TEST(TeamMeta, MissingFieldsReadAsDefaults) {
    const TeamMeta decoded = team_meta_from_json(nlohmann::json::object());
    EXPECT_TRUE(decoded.team_id.empty());
    EXPECT_TRUE(decoded.members.empty());
    EXPECT_EQ(decoded.created_at, 0);

    const TeamMember member = member_from_json(nlohmann::json::object());
    EXPECT_TRUE(member.guid.empty());
    EXPECT_TRUE(member.role.empty());
    EXPECT_FALSE(is_admin(member));

    const TeamSessionRef ref = session_ref_from_json(nlohmann::json::object());
    EXPECT_TRUE(ref.session_id.empty());

    // 非字符串/非整数类型的杂散字段按缺失处理,与稀疏省略语义一致。
    const TeamMember bad = member_from_json(nlohmann::json{{"guid", 42}, {"last_seen", "no"}});
    EXPECT_TRUE(bad.guid.empty());
    EXPECT_EQ(bad.last_seen, 0);
}

// 场景: 空字符串与零时间戳字段不写出,保持稀疏
TEST(TeamMeta, SparseOmission) {
    TeamMember member;
    member.guid = "g";
    const nlohmann::json j = member_to_json(member);
    EXPECT_FALSE(j.contains("account"));
    EXPECT_FALSE(j.contains("display_name"));
    EXPECT_FALSE(j.contains("last_seen"));

    TeamSessionRef ref;
    ref.session_id = "s";
    const nlohmann::json sj = session_ref_to_json(ref);
    EXPECT_FALSE(sj.contains("state"));
    EXPECT_FALSE(sj.contains("pause_reason"));

    const nlohmann::json mj = team_meta_to_json(TeamMeta{});
    EXPECT_FALSE(mj.contains("members"));
    EXPECT_FALSE(mj.contains("sessions"));
    EXPECT_FALSE(mj.contains("repos"));
    EXPECT_FALSE(mj.contains("created_at"));
}

// 场景: 查找与计数辅助函数
TEST(TeamMeta, LookupAndCountHelpers) {
    TeamMeta meta;
    EXPECT_EQ(find_member(meta, ""), nullptr);
    EXPECT_EQ(find_session(meta, ""), nullptr);
    EXPECT_EQ(active_session_count(meta), 0);
    EXPECT_FALSE(team_full(meta));

    TeamMember admin;
    admin.guid = "guid-a";
    admin.role = "admin";
    meta.members.push_back(admin);
    EXPECT_TRUE(is_admin(*find_member(meta, "guid-a")));
    EXPECT_EQ(find_member(meta, "nope"), nullptr);

    TeamSessionRef running;
    running.session_id = "s1";
    running.state = "running";
    meta.sessions.push_back(running);
    TeamSessionRef paused;
    paused.session_id = "s2";
    paused.state = "paused";
    meta.sessions.push_back(paused);
    TeamSessionRef ended;
    ended.session_id = "s3";
    ended.state = "ended";
    meta.sessions.push_back(ended);
    EXPECT_EQ(active_session_count(meta), 2);
    EXPECT_EQ(find_session(meta, "s2")->state, "paused");
}

} // namespace acecode::team
