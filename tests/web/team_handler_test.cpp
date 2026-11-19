// 覆盖 src/apps/web/handlers/team_handler.cpp 的纯组装逻辑(6.1),
// 以及 3.3 的多客户端同序契约:同一个 host 会话分发器上的多个订阅者
// (对应多条团队 WS 连接)按相同顺序收到相同事件流。

#include <gtest/gtest.h>

#include "web/handlers/team_handler.hpp"
#include "session/event_dispatcher.hpp"
#include "team_host/team_service.hpp"

#include <vector>

using namespace acecode;
using namespace acecode::web;
using namespace acecode::team;
using nlohmann::json;

namespace {

TeamMeta sample_team() {
    TeamMeta meta;
    meta.team_id = "team-1";
    meta.name = "研发组";
    meta.invite_seed = "secret-seed";
    meta.created_at = 100;
    meta.created_by = "guid-a";
    meta.updated_at = 200;
    meta.updated_by = "guid-a";
    meta.repos.push_back({"remote", "github.com/Org/Repo"});
    TeamMember a;
    a.guid = "guid-a";
    a.account = "alice@pc-a";
    a.role = "admin";
    a.last_seen = 1'700'000'000; // = now
    a.updated_at = 100;
    meta.members.push_back(a);
    TeamMember b;
    b.guid = "guid-b";
    b.account = "bob@pc-b";
    b.role = "member";
    b.last_seen = 1'700'000'000 - 10;
    b.daily_token_quota = 10'000'000;
    b.quota_used_today = 4'000'000;
    b.quota_day = 1'700'000'000'000 / kQuotaDayMs; // 今天
    b.updated_at = 100;
    meta.members.push_back(b);
    meta.sessions.push_back({"s-1", "调试登录", "guid-a", "running", "", "guid-a", 1, 2});
    return meta;
}

} // namespace

// 场景: 公开元数据绝不包含 invite_seed(凭据种子不下发)
TEST(TeamHandler, PublicMetaOmitsInviteSeed) {
    const auto j = team_meta_public_json(sample_team());
    EXPECT_EQ(j["team_id"], "team-1");
    EXPECT_EQ(j["name"], "研发组");
    EXPECT_FALSE(j.contains("invite_seed"));
    EXPECT_FALSE(j.contains("members"));
    EXPECT_EQ(j["repos"].size(), 1u);
}

// 场景: 成员视图带在线/额度状态;离线与额度用尽可区分
TEST(TeamHandler, MemberPublicJson) {
    const TeamMeta meta = sample_team();
    const auto a = team_member_public_json(meta, *find_member(meta, "guid-a"),
                                           1'700'000'000);
    EXPECT_TRUE(a["online"].get<bool>());
    EXPECT_EQ(a["daily_token_quota"], kDefaultDailyQuotaTokens); // 未设置 → 默认 30M
    EXPECT_EQ(a["quota_used_today"], 0);

    const auto b = team_member_public_json(meta, *find_member(meta, "guid-b"),
                                           1'700'000'000);
    EXPECT_TRUE(b["online"].get<bool>());
    EXPECT_EQ(b["daily_token_quota"], 10'000'000);
    EXPECT_EQ(b["quota_used_today"], 4'000'000);
    EXPECT_FALSE(b["quota_exhausted"].get<bool>());

    // 快照不是今天 → 展示归零
    TeamMeta stale_meta = meta;
    find_member(stale_meta, "guid-b")->quota_day = 0;
    const auto stale_view = team_member_public_json(stale_meta, *find_member(stale_meta, "guid-b"),
                                                    1'700'000'000);
    EXPECT_EQ(stale_view["quota_used_today"], 0);
}

// 场景: 状态响应包含会话列表与本机承接能力(self)
TEST(TeamHandler, StateJsonIncludesSelfStatus) {
    TeamMeta meta = sample_team();
    QuotaLedger ledger("/nonexistent-quota-dir", "team-1");
    const auto j = team_state_json(meta, "guid-a", ledger, 1'700'000'000);
    EXPECT_EQ(j["sessions"].size(), 1u);
    EXPECT_EQ(j["sessions"][0]["session_id"], "s-1");
    EXPECT_EQ(j["sessions"][0]["state"], "running");
    EXPECT_EQ(j["self"]["guid"], "guid-a");
    EXPECT_EQ(j["self"]["daily_token_quota"], kDefaultDailyQuotaTokens);
    EXPECT_FALSE(j["self"]["quota_exhausted"].get<bool>());
}

// 场景: 会话发起请求体校验
TEST(TeamHandler, ValidateSessionStartBody) {
    EXPECT_EQ(validate_team_session_start_body(json::parse(R"({"cwd":"/tmp/x"})")), "");
    EXPECT_EQ(validate_team_session_start_body(json::parse("{}")), ""); // cwd 可缺省
    EXPECT_EQ(validate_team_session_start_body(json::parse(R"({"cwd":1})")), "invalid_cwd");
    EXPECT_EQ(validate_team_session_start_body(json::parse("[]")), "invalid_body");
}

// 场景: 创建团队请求体校验(name + cwd 必填,repo_url 可选字符串)
TEST(TeamHandler, ValidateTeamCreateBody) {
    EXPECT_EQ(validate_team_create_body(
        json::parse(R"({"name":"团队","cwd":"/tmp/x"})")), "");
    EXPECT_EQ(validate_team_create_body(
        json::parse(R"({"name":"团队","cwd":"/tmp/x","repo_url":"https://github.com/Org/Repo.git"})")), "");
    EXPECT_EQ(validate_team_create_body(json::parse(R"({"cwd":"/tmp/x"})")), "invalid_name");
    EXPECT_EQ(validate_team_create_body(json::parse(R"({"name":"团队"})")), "invalid_cwd");
    EXPECT_EQ(validate_team_create_body(
        json::parse(R"({"name":"团队","cwd":"/tmp/x","repo_url":1})")), "invalid_repo_url");
    EXPECT_EQ(validate_team_create_body(json::parse("[]")), "invalid_body");
}

// 场景: 加入团队请求体校验(link + cwd 必填)
TEST(TeamHandler, ValidateTeamJoinBody) {
    EXPECT_EQ(validate_team_join_body(
        json::parse(R"({"link":"acecode://team/x","cwd":"/tmp/x"})")), "");
    EXPECT_EQ(validate_team_join_body(json::parse(R"({"cwd":"/tmp/x"})")), "invalid_link");
    EXPECT_EQ(validate_team_join_body(json::parse(R"({"link":"acecode://team/x"})")), "invalid_cwd");
    EXPECT_EQ(validate_team_join_body(json::parse("[]")), "invalid_body");
}

// 场景: 本机定位投影 — 本机会话补 cwd/workspace_hash,远程会话保持稀疏
TEST(TeamHandler, SessionLocalInfoProjection) {
    const auto local = team_session_local_info_json("/tmp/demo-repo", "abcd1234efgh5678");
    EXPECT_EQ(local["cwd"], "/tmp/demo-repo");
    EXPECT_EQ(local["workspace_hash"], "abcd1234efgh5678");

    // no_workspace 会话:只有 cwd,不绑定工作区
    const auto no_ws = team_session_local_info_json("/tmp/scratch", "");
    EXPECT_EQ(no_ws["cwd"], "/tmp/scratch");
    EXPECT_FALSE(no_ws.contains("workspace_hash"));

    EXPECT_TRUE(team_session_local_info_json("", "").empty());
}

// 场景: 发起会话 cwd 缺省链 — 显式 cwd > 本机团队工作目录 > daemon 兜底
TEST(TeamHandler, ResolveSessionCwdChain) {
    const json explicit_cwd = json::parse(R"({"cwd":"/explicit"})");
    EXPECT_EQ(resolve_team_session_cwd(explicit_cwd, "/workdir", "/daemon"), "/explicit");

    const json no_cwd = json::parse("{}");
    EXPECT_EQ(resolve_team_session_cwd(no_cwd, "/workdir", "/daemon"), "/workdir");
    EXPECT_EQ(resolve_team_session_cwd(no_cwd, "", "/daemon"), "/daemon");

    // cwd 显式为空串视同缺省
    const json blank_cwd = json::parse(R"({"cwd":""})");
    EXPECT_EQ(resolve_team_session_cwd(blank_cwd, "/workdir", "/daemon"), "/workdir");

    // 非对象 body 一律走缺省链
    EXPECT_EQ(resolve_team_session_cwd(json::array(), "/workdir", "/daemon"), "/workdir");
}

// 3.3 场景: 同一 host 会话的两个订阅者看到完全一致的事件顺序
// (团队 WS 路由下,每个队员连接都挂在同一个 EventDispatcher 上,
//  顺序由 dispatcher 的 seq 单调 + catch-up 机制保证)。
TEST(TeamHandler, MultipleSubscribersSeeSameOrder) {
    EventDispatcher dispatcher;
    std::vector<std::vector<std::uint64_t>> seqs(2);
    std::vector<SessionClient::SubscriptionId> subs;
    for (int i = 0; i < 2; ++i) {
        subs.push_back(dispatcher.subscribe([&seqs, i](const SessionEvent& event) {
            seqs[i].push_back(event.seq);
        }));
    }
    for (int i = 0; i < 20; ++i) {
        dispatcher.emit(SessionEventKind::Message, json{{"text", std::to_string(i)}});
    }
    // 订阅回调是异步 drain,这里退订会等待在途回调完成。
    for (const auto sub : subs) dispatcher.unsubscribe_and_wait(sub);
    ASSERT_EQ(seqs[0].size(), 20u);
    EXPECT_EQ(seqs[0], seqs[1]);
    for (std::size_t i = 0; i < seqs[0].size(); ++i)
        EXPECT_EQ(seqs[0][i], i + 1);
}
