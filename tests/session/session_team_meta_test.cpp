// 覆盖 SessionMeta 的团队字段(openspec: add-decentralized-team, task 1.5)。
// 契约:普通会话不写 team_id/team_role;参与团队的会话往返后逐字段一致;
// 老 meta 无字段时按空值读回 — 三条都锁死,避免团队标记污染普通会话列表。

#include <gtest/gtest.h>

#include "session/session_storage.hpp"
#include "utils/utf8_path.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>

namespace fs = std::filesystem;
using acecode::SessionMeta;
using acecode::SessionStorage;

namespace {

struct TempDir {
    fs::path root;
    TempDir() {
        root = fs::temp_directory_path() /
               ("session_team_meta_test_" +
                std::to_string(std::chrono::steady_clock::now()
                                   .time_since_epoch()
                                   .count()));
        fs::remove_all(root);
        fs::create_directories(root);
    }
    ~TempDir() { fs::remove_all(root); }
};

SessionMeta base_meta() {
    SessionMeta meta;
    meta.id = "20260101_120000_abcd";
    meta.cwd = "/tmp/proj";
    meta.created_at = "2026-01-01T12:00:00Z";
    meta.updated_at = "2026-01-01T12:00:00Z";
    meta.provider = "openai";
    meta.model = "gpt-5";
    return meta;
}

} // namespace

// 场景: 团队会话的 team_id/team_role 往返一致
TEST(SessionTeamMeta, RoundTripTeamFields) {
    TempDir temp;
    SessionMeta meta = base_meta();
    meta.team_id = "team-1";
    meta.team_role = "host";
    const std::string path = acecode::path_to_utf8(temp.root / "m.meta.json");

    ASSERT_TRUE(SessionStorage::write_meta(path, meta));
    const SessionMeta read = SessionStorage::read_meta(path);
    EXPECT_EQ(read.team_id, "team-1");
    EXPECT_EQ(read.team_role, "host");
    EXPECT_EQ(read.id, meta.id);
}

// 场景: 普通会话不写团队字段(稀疏),读回为空
TEST(SessionTeamMeta, OrdinarySessionOmitsTeamFields) {
    TempDir temp;
    const std::string path = acecode::path_to_utf8(temp.root / "m.meta.json");
    ASSERT_TRUE(SessionStorage::write_meta(path, base_meta()));

    std::ifstream input(path);
    const nlohmann::json j = nlohmann::json::parse(input);
    EXPECT_FALSE(j.contains("team_id"));
    EXPECT_FALSE(j.contains("team_role"));

    const SessionMeta read = SessionStorage::read_meta(path);
    EXPECT_TRUE(read.team_id.empty());
    EXPECT_TRUE(read.team_role.empty());
}

// 场景: 老 meta 没有团队字段时按空值读回,不抛异常
TEST(SessionTeamMeta, LegacyMetaWithoutTeamFieldsReadsEmpty) {
    TempDir temp;
    const fs::path path = temp.root / "legacy.meta.json";
    std::ofstream(path) << R"({"id":"20250101-010101-abcd","cwd":"/tmp/x"})";
    const SessionMeta read = SessionStorage::read_meta(acecode::path_to_utf8(path));
    EXPECT_EQ(read.id, "20250101-010101-abcd");
    EXPECT_TRUE(read.team_id.empty());
    EXPECT_TRUE(read.team_role.empty());
}
