// 覆盖 team 配置块(add-team-workdir-sidebar 反馈轮):
//   - team.enabled:功能开关,默认关闭;关闭时 daemon 不启动团队模块
//   - team.account_daily_token_quota:本机登录账号的每日 token 额度(默认 30M)
// 走真实的 save_config → load_config_from_path 往返,写临时目录,不碰 ~/.acecode。

#include <gtest/gtest.h>

#include "config/config.hpp"
#include "config/settings_mutations.hpp"

#include <nlohmann/json.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;
using namespace acecode;

namespace {

fs::path make_temp_dir() {
    auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    fs::path dir = fs::temp_directory_path() /
        ("acecode-config-team-" + std::to_string(now));
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    return dir;
}

void write_text(const fs::path& path, const std::string& text) {
    std::ofstream ofs(path, std::ios::binary | std::ios::trunc);
    ofs << text;
}

nlohmann::json read_json(const fs::path& path) {
    std::ifstream ifs(path, std::ios::binary);
    return nlohmann::json::parse(ifs);
}

}  // namespace

// 场景:默认值 —— 功能关闭、账号额度 30M。
TEST(ConfigTeam, DefaultsAreDisabledWithDefaultQuota) {
    AppConfig cfg;
    EXPECT_FALSE(cfg.team.enabled);
    EXPECT_EQ(cfg.team.account_daily_token_quota, 30'000'000);
}

// 场景:config.json 显式开启 + 自定义额度;保存后只在非默认时落盘(sparse)。
TEST(ConfigTeam, RoundTripIsSparse) {
    const fs::path dir = make_temp_dir();
    const std::string path = (dir / "config.json").string();
    write_text(path, R"({"team":{"enabled":true,"account_daily_token_quota":50000000}})");
    AppConfig cfg = load_config_from_path(path);
    EXPECT_TRUE(cfg.team.enabled);
    EXPECT_EQ(cfg.team.account_daily_token_quota, 50'000'000);

    save_config(cfg, path);
    const nlohmann::json saved = read_json(path);
    ASSERT_TRUE(saved.contains("team"));
    EXPECT_EQ(saved["team"]["enabled"], true);
    EXPECT_EQ(saved["team"]["account_daily_token_quota"], 50'000'000);

    // 全默认 → team 块整体不落盘
    AppConfig defaults;
    const std::string path2 = (dir / "config-default.json").string();
    save_config(defaults, path2);
    EXPECT_FALSE(read_json(path2).contains("team"));
    fs::remove_all(dir);
}

// 场景:额度类型非法(非整数)被忽略,回到默认值。
TEST(ConfigTeam, InvalidQuotaFallsBackToDefault) {
    const fs::path dir = make_temp_dir();
    const std::string path = (dir / "config.json").string();
    write_text(path, R"({"team":{"account_daily_token_quota":"lots"}})");
    AppConfig cfg = load_config_from_path(path);
    EXPECT_EQ(cfg.team.account_daily_token_quota, 30'000'000);
    EXPECT_FALSE(cfg.team.enabled);
    fs::remove_all(dir);
}

// 场景:settings_mutations 即时保存(team 功能开关 + 账号额度)。
TEST(ConfigTeam, SettingsMutationRoundTrip) {
    const fs::path dir = make_temp_dir();
    const std::string path = (dir / "config.json").string();
    AppConfig live;

    SettingsMutationOptions options;
    options.config_path = path;
    options.live_config = &live;
    const auto enabled_result = set_team_enabled(true, options);
    ASSERT_TRUE(enabled_result.ok) << enabled_result.error;
    EXPECT_TRUE(live.team.enabled);

    const auto quota_result = set_team_account_quota(45'000'000, options);
    ASSERT_TRUE(quota_result.ok) << quota_result.error;
    EXPECT_EQ(live.team.account_daily_token_quota, 45'000'000);

    // 幂等:同值不产生 changed
    EXPECT_FALSE(set_team_enabled(true, options).changed);
    EXPECT_FALSE(set_team_account_quota(45'000'000, options).changed);

    // 负值拒绝,不改配置
    const auto invalid = set_team_account_quota(-1, options);
    EXPECT_FALSE(invalid.ok);
    EXPECT_EQ(live.team.account_daily_token_quota, 45'000'000);

    // 落盘可读回
    AppConfig loaded = load_config_from_path(path);
    EXPECT_TRUE(loaded.team.enabled);
    EXPECT_EQ(loaded.team.account_daily_token_quota, 45'000'000);
    fs::remove_all(dir);
}
