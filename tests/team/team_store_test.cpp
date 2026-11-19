// 覆盖 src/domain/team/team_store.cpp。团队本机目录是去中心化状态的落点:
// team.json 损坏可重拉(返回空),邀请码账本损坏必须 fail-closed — 这两句话
// 各对应一组用例,不能互相渗透。

#include <gtest/gtest.h>

#include "team/team_store.hpp"
#include "utils/atomic_file.hpp"
#include "utils/utf8_path.hpp"

#include <filesystem>
#include <nlohmann/json.hpp>

namespace fs = std::filesystem;

namespace acecode::team {

namespace {

struct TempDir {
    fs::path root;
    TempDir() {
        root = fs::temp_directory_path() /
               ("team_store_test_" +
                std::to_string(std::chrono::steady_clock::now()
                                   .time_since_epoch()
                                   .count()));
        fs::remove_all(root);
        fs::create_directories(root);
    }
    ~TempDir() { fs::remove_all(root); }
    std::string utf8() const { return path_to_utf8(root); }
};

TeamMeta sample_meta() {
    TeamMeta meta;
    meta.team_id = "team-1";
    meta.name = "研发组";
    meta.invite_seed = "seed";
    TeamMember member;
    member.guid = "guid-a";
    member.role = "admin";
    member.updated_at = 1;
    member.updated_by = "guid-a";
    meta.members.push_back(member);
    meta.created_at = 2;
    meta.created_by = "guid-a";
    return meta;
}

} // namespace

// 场景: 首次保存后 team.json 落在 <data_dir>/team/<team_id>/ 下并可读回
TEST(TeamStore, SaveAndLoadMetaRoundTrip) {
    TempDir temp;
    TeamStore store(temp.utf8(), "team-1");

    EXPECT_FALSE(store.load_meta().has_value()); // 未创建前是空
    const TeamMeta meta = sample_meta();
    ASSERT_TRUE(store.save_meta(meta));

    const fs::path expected = temp.root / "team" / "team-1" / "team.json";
    EXPECT_TRUE(fs::exists(expected));
    ASSERT_TRUE(store.load_meta().has_value());
    EXPECT_EQ(store.load_meta()->name, "研发组");
    EXPECT_EQ(store.load_meta()->members.size(), 1u);
}

// 场景: team.json 损坏 → 返回空(同步层按"没有本地快照"重拉),不抛异常
TEST(TeamStore, CorruptMetaReadsAsMissing) {
    TempDir temp;
    TeamStore store(temp.utf8(), "team-1");
    fs::create_directories(temp.root / "team" / "team-1");
    atomic_write_file(path_to_utf8(temp.root / "team" / "team-1" / "team.json"),
                      "{not json");

    EXPECT_FALSE(store.load_meta().has_value());
    EXPECT_FALSE(store.load_meta().has_value()); // 幂等
}

// 场景: 邀请码账本记账、查找与双重失效
TEST(TeamStore, InviteLedgerConsumeOnce) {
    TempDir temp;
    TeamStore store(temp.utf8(), "team-1");

    InviteLedgerRecord record;
    record.token = "tok-1";
    record.issued_at = 1000;
    record.expires_at = 1000 + kInviteTtlSeconds;
    ASSERT_TRUE(store.save_invites({record}));

    auto found = store.find_invite("tok-1");
    ASSERT_TRUE(found.has_value());
    EXPECT_TRUE(invite_usable(*found, 1000));
    // 15 分钟内有效;超过即过期
    EXPECT_TRUE(invite_usable(*found, record.expires_at));
    EXPECT_FALSE(invite_usable(*found, record.expires_at + 1));

    // 消费一次成功;第二次幂等失败
    EXPECT_TRUE(store.consume_invite("tok-1", "guid-b", 1500));
    EXPECT_FALSE(store.consume_invite("tok-1", "guid-c", 1501));
    found = store.find_invite("tok-1");
    ASSERT_TRUE(found.has_value());
    EXPECT_EQ(found->consumed_at, 1500);
    EXPECT_EQ(found->consumed_by, "guid-b");
    EXPECT_FALSE(invite_usable(*found, 1500));
}

// 场景: 未知 token / 空 token 查找与消费一律失败
TEST(TeamStore, InviteUnknownTokenFailsClosed) {
    TempDir temp;
    TeamStore store(temp.utf8(), "team-1");
    EXPECT_FALSE(store.find_invite("nope").has_value());
    EXPECT_FALSE(store.find_invite("").has_value());
    EXPECT_FALSE(store.consume_invite("nope", "guid", 1));
    EXPECT_FALSE(store.consume_invite("", "guid", 1));
}

// 场景: 账本损坏 → fail-closed,查找与消费全部失败(不能因文件坏而放行)
TEST(TeamStore, CorruptInviteLedgerFailsClosed) {
    TempDir temp;
    TeamStore store(temp.utf8(), "team-1");
    fs::create_directories(temp.root / "team" / "team-1");
    atomic_write_file(path_to_utf8(temp.root / "team" / "team-1" / "invite_ledger.json"),
                      "[{broken");

    const InviteLedgerLoad loaded = store.load_invites();
    EXPECT_TRUE(loaded.has_file);
    EXPECT_FALSE(loaded.error.empty());
    EXPECT_TRUE(loaded.records.empty());
    EXPECT_FALSE(store.find_invite("tok-1").has_value());
    EXPECT_FALSE(store.consume_invite("tok-1", "guid", 1));
}

// 场景: 账本里混入缺 token 的坏记录 → 整体 fail-closed,不是部分可用
TEST(TeamStore, InvalidInviteRecordFailsWholeLedger) {
    TempDir temp;
    TeamStore store(temp.utf8(), "team-1");
    fs::create_directories(temp.root / "team" / "team-1");
    atomic_write_file(path_to_utf8(temp.root / "team" / "team-1" / "invite_ledger.json"),
                      R"([{"token":"tok-1","expires_at":900},{"expires_at":900}])");

    const InviteLedgerLoad loaded = store.load_invites();
    EXPECT_FALSE(loaded.error.empty());
    EXPECT_TRUE(loaded.records.empty());
    EXPECT_FALSE(store.consume_invite("tok-1", "guid", 1));
}

// 场景: 本机团队状态 local.json 往返;损坏/缺失按"未设置"处理
TEST(TeamStore, LocalStateRoundTrip) {
    TempDir temp;
    TeamStore store(temp.utf8(), "team-1");

    const TeamLocalState missing = store.load_local_state();
    EXPECT_TRUE(missing.work_dir.empty()); // 缺文件 = 未设置

    TeamLocalState state;
    state.work_dir = "/home/a/projects/demo";
    ASSERT_TRUE(store.save_local_state(state));

    const fs::path expected = temp.root / "team" / "team-1" / "local.json";
    EXPECT_TRUE(fs::exists(expected));
    const TeamLocalState loaded = store.load_local_state();
    EXPECT_EQ(loaded.work_dir, "/home/a/projects/demo");

    // 覆盖写生效
    TeamLocalState next;
    next.work_dir = "/home/b/other";
    ASSERT_TRUE(store.save_local_state(next));
    EXPECT_EQ(store.load_local_state().work_dir, "/home/b/other");

    // 空 work_dir 序列化为不带字段的对象,读回仍是空
    ASSERT_TRUE(store.save_local_state(TeamLocalState{}));
    EXPECT_TRUE(store.load_local_state().work_dir.empty());
}

// 场景: local.json 损坏 → 按"未设置"处理,不抛异常
TEST(TeamStore, CorruptLocalStateReadsAsUnset) {
    TempDir temp;
    TeamStore store(temp.utf8(), "team-1");
    fs::create_directories(temp.root / "team" / "team-1");
    atomic_write_file(path_to_utf8(temp.root / "team" / "team-1" / "local.json"),
                      "{broken");
    EXPECT_TRUE(store.load_local_state().work_dir.empty());
}

// 场景: 会话副本目录按会话 ID 创建,team_id 之间互不可见
TEST(TeamStore, ReplicaDirIsolatedPerTeam) {
    TempDir temp;
    TeamStore store_a(temp.utf8(), "team-a");
    TeamStore store_b(temp.utf8(), "team-b");

    const fs::path dir_a = store_a.replica_dir("s1");
    const fs::path dir_b = store_b.replica_dir("s1");
    EXPECT_TRUE(fs::exists(dir_a));
    EXPECT_TRUE(fs::exists(dir_b));
    EXPECT_NE(dir_a, dir_b);
    EXPECT_EQ(dir_a, temp.root / "team" / "team-a" / "sessions" / "s1");
}

} // namespace acecode::team
