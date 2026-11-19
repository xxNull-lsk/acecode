// 覆盖 src/host/team/team_client.cpp。用本地 httplib 服务模拟队员 daemon,
// 锁住三条网络契约:候选地址逐个尝试、团队凭据走独立 header、
// 响应快照的团队 ID/成员身份校验(不能盲信对端数据)。

#include <gtest/gtest.h>

#include <httplib.h>

#include "team_host/team_client.hpp"
#include "team/team_meta.hpp"
#include "team_host/team_service.hpp"
#include "utils/utf8_path.hpp"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <thread>

namespace fs = std::filesystem;
using namespace acecode::team;

namespace {

struct TempDir {
    fs::path root;
    TempDir() {
        root = fs::temp_directory_path() /
               ("team_client_test_" +
                std::to_string(std::chrono::steady_clock::now()
                                   .time_since_epoch()
                                   .count()));
        fs::remove_all(root);
        fs::create_directories(root);
    }
    ~TempDir() { fs::remove_all(root); }
    std::string utf8() const { return acecode::path_to_utf8(root); }
};

struct LocalServer {
    httplib::Server server;
    std::thread thread;
    int port = 0;

    explicit LocalServer(std::function<void(httplib::Server&)> setup) {
        setup(server);
        port = server.bind_to_any_port("127.0.0.1");
        thread = std::thread([this] { server.listen_after_bind(); });
        server.wait_until_ready();
    }
    ~LocalServer() {
        server.stop();
        if (thread.joinable()) thread.join();
    }
};

TeamMeta sample_team() {
    TeamMeta meta;
    meta.team_id = "team-1";
    meta.name = "研发组";
    meta.invite_seed = "seed";
    TeamMember admin;
    admin.guid = "guid-a";
    admin.role = "admin";
    admin.updated_at = 100;
    meta.members.push_back(admin);
    TeamMember joiner;
    joiner.guid = "guid-b";
    joiner.role = "member";
    joiner.updated_at = 200;
    meta.members.push_back(joiner);
    meta.updated_at = 200;
    meta.updated_by = "guid-b";
    return meta;
}

DeviceIdentity self_b() { return DeviceIdentity{"guid-b", "bob@pc", "Bob"}; }

TeamInviteLink invite_for(int port) {
    TeamInviteLink invite;
    invite.team_id = "team-1";
    invite.team_name = "研发组";
    invite.inviter_name = "A";
    invite.inviter_guid = "guid-a";
    invite.addresses = {"127.0.0.1"};
    invite.port = port;
    invite.token = "one-time-token";
    invite.expires_at = 9'999'999'999;
    return invite;
}

} // namespace

// 场景: URL 组装与非法地址拒绝
TEST(TeamClient, BaseUrlValidation) {
    EXPECT_EQ(team_base_url("192.168.1.5", 12399), "http://192.168.1.5:12399");
    EXPECT_EQ(team_base_url("fe80::1", 12399), "http://[fe80::1]:12399");
    EXPECT_THROW(team_base_url("http://evil", 12399), std::invalid_argument);
    EXPECT_THROW(team_base_url("192.168.1.5", 0), std::invalid_argument);
}

// 场景: 加入流程按候选地址逐个尝试,前一个不可达后成功
TEST(TeamClient, JoinTriesCandidatesThenSucceeds) {
    LocalServer server([](httplib::Server& s) {
        s.Post("/api/team/v1/join", [](const httplib::Request& req, httplib::Response& res) {
            const auto body = nlohmann::json::parse(req.body);
            if (body.at("token") != "one-time-token" || body.at("guid") != "guid-b") {
                res.status = 403;
                res.set_content(R"({"error":"invite_invalid"})", "application/json");
                return;
            }
            res.set_content(nlohmann::json{{"snapshot", team_meta_to_json(sample_team())}}.dump(),
                            "application/json");
        });
    });
    auto invite = invite_for(server.port);
    invite.addresses = {"127.0.0.1", "127.0.0.1"}; // 第二个候选仍可用

    const auto fetcher = make_network_join_fetcher(self_b());
    std::string error;
    const auto snapshot = fetcher(invite, &error);
    ASSERT_TRUE(snapshot.has_value()) << error;
    EXPECT_EQ(snapshot->team_id, "team-1");
    EXPECT_EQ(snapshot->members.size(), 2u);
}

// 场景: 对端返回的团队 ID 不匹配 → 拒绝,不落地
TEST(TeamClient, JoinRejectsMismatchedTeamId) {
    LocalServer server([](httplib::Server& s) {
        s.Post("/api/team/v1/join", [](const httplib::Request&, httplib::Response& res) {
            TeamMeta meta = sample_team();
            meta.team_id = "another-team";
            res.set_content(nlohmann::json{{"snapshot", team_meta_to_json(meta)}}.dump(),
                            "application/json");
        });
    });
    auto invite = invite_for(server.port);
    const auto fetcher = make_network_join_fetcher(self_b());
    std::string error;
    EXPECT_FALSE(fetcher(invite, &error).has_value());
    EXPECT_EQ(error, "team_id_mismatch");
}

// 场景: 快照里没有加入者自己 → 拒绝
TEST(TeamClient, JoinRejectsSnapshotWithoutSelf) {
    LocalServer server([](httplib::Server& s) {
        s.Post("/api/team/v1/join", [](const httplib::Request&, httplib::Response& res) {
            TeamMeta meta = sample_team();
            meta.members.pop_back(); // 去掉 guid-b
            res.set_content(nlohmann::json{{"snapshot", team_meta_to_json(meta)}}.dump(),
                            "application/json");
        });
    });
    const auto fetcher = make_network_join_fetcher(self_b());
    std::string error;
    EXPECT_FALSE(fetcher(invite_for(server.port), &error).has_value());
    EXPECT_EQ(error, "self_not_in_team");
}

// 场景: 所有候选都不可达 → connection_failed,不抛异常
TEST(TeamClient, JoinAllCandidatesUnreachable) {
    const auto fetcher = make_network_join_fetcher(self_b());
    std::string error;
    EXPECT_FALSE(fetcher(invite_for(1), &error).has_value());
    EXPECT_FALSE(error.empty());
}

// 场景: 状态拉取携带团队凭据头,响应被解析;错误状态映射为错误串
TEST(TeamClient, PullStateUsesTeamTokenHeader) {
    std::atomic<bool> saw_header{false};
    LocalServer server([&](httplib::Server& s) {
        s.Get("/api/team/v1/state", [&](const httplib::Request& req, httplib::Response& res) {
            const auto header = req.get_header_value("X-ACECode-Team-Token");
            if (header != "team-credential") {
                res.status = 403;
                res.set_content(R"({"error":"forbidden"})", "application/json");
                return;
            }
            saw_header.store(true);
            if (req.get_param_value("team_id") != "team-1") {
                res.status = 404;
                res.set_content(R"({"error":"team_not_found"})", "application/json");
                return;
            }
            res.set_content(nlohmann::json{{"snapshot", team_meta_to_json(sample_team())}}.dump(),
                            "application/json");
        });
    });

    const auto pulled = pull_team_state("127.0.0.1", server.port, "team-1", "team-credential");
    ASSERT_TRUE(pulled) << pulled.error;
    EXPECT_TRUE(saw_header.load());
    EXPECT_EQ(pulled.state.name, "研发组");

    const auto rejected = pull_team_state("127.0.0.1", server.port, "team-1", "wrong");
    EXPECT_FALSE(rejected);
    EXPECT_EQ(rejected.error, "state_rejected");
}

// 场景: sync_with_peer 拉取 + LWW 合并进本地服务
TEST(TeamClient, SyncWithPeerMergesRemoteState) {
    TempDir temp;
    TeamService service(temp.utf8(), self_b(), [] { return 1'700'000'000; });
    ASSERT_TRUE(service.apply_snapshot(sample_team()));

    TeamMeta remote = sample_team();
    remote.name = "远端改名";
    remote.updated_at = 300;
    remote.updated_by = "guid-a";
    LocalServer server([&](httplib::Server& s) {
        s.Get("/api/team/v1/state", [&](const httplib::Request&, httplib::Response& res) {
            res.set_content(nlohmann::json{{"snapshot", team_meta_to_json(remote)}}.dump(),
                            "application/json");
        });
    });

    const auto synced = sync_with_peer(service, "127.0.0.1", server.port, "team-1", "token");
    ASSERT_TRUE(synced) << synced.error;
    const auto local = service.load_team("team-1");
    ASSERT_TRUE(local.has_value());
    EXPECT_EQ(local->name, "远端改名");
}
