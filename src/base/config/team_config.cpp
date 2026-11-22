#include "config.hpp"

#include <nlohmann/json.hpp>

namespace acecode {

// 团队协作配置(add-team-workdir-sidebar 反馈轮):解析与稀疏保存,
// 与 swarm/image_generation 等分文件配置同一模式,避免 config.cpp 超长。

void load_team_config_json(const nlohmann::json& j, TeamConfig& out) {
    if (!j.is_object()) return;
    if (j.contains("enabled") && j["enabled"].is_boolean()) {
        out.enabled = j["enabled"].get<bool>();
    }
    if (j.contains("account_daily_token_quota") &&
        j["account_daily_token_quota"].is_number_integer()) {
        out.account_daily_token_quota = j["account_daily_token_quota"].get<std::int64_t>();
    }
}

nlohmann::json team_config_to_json(const TeamConfig& cfg) {
    const TeamConfig defaults;
    nlohmann::json j = nlohmann::json::object();
    if (cfg.enabled != defaults.enabled)
        j["enabled"] = cfg.enabled;
    if (cfg.account_daily_token_quota != defaults.account_daily_token_quota)
        j["account_daily_token_quota"] = cfg.account_daily_token_quota;
    return j;
}

} // namespace acecode
