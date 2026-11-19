#pragma once

// 团队元数据模型(spec capability: team-membership / team-state-sync)。
//
// TeamMeta 是去中心化团队状态的单一载体:每台设备把它存在
// ~/.acecode/team/<team_id>/team.json,经队员间同步逐字段 last-writer-wins
// 收敛。同步仲裁依赖每个可变条目的 updated_at/updated_by — 缺了时间戳的字段
// 会被当作"从未修改",合并时旧值胜出,所以新字段必须带上这对戳。

#include <cstdint>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>

namespace acecode::team {

inline constexpr int kMaxTeamMembers = 50;
inline constexpr int kMaxConcurrentTeamSessions = 5;
inline constexpr std::string_view kRoleAdmin = "admin";
inline constexpr std::string_view kRoleMember = "member";

// 每日贡献额度默认值(spec team-token-quota):每设备 30M LLM tokens/UTC 日。
// 0 = 该设备当日不承接任何团队会话。
inline constexpr std::int64_t kDefaultDailyQuotaTokens = 30'000'000;
// UTC 日索引的时间粒度(与 usage ledger 的日分桶一致)。
inline constexpr std::int64_t kQuotaDayMs = 86'400'000;

// 会话状态:running 正常;paused 等待手动接管(reason 标注失联/额度/异常);
// ended 已结束(仍是列表成员,不再计入并发)。
inline constexpr std::string_view kSessionStateRunning = "running";
inline constexpr std::string_view kSessionStatePaused = "paused";
inline constexpr std::string_view kSessionStateEnded = "ended";

struct TeamMember {
    std::string account;       // 系统用户名+电脑名(展示与查找用)
    std::string guid;          // 设备 GUID,成员身份主键
    std::string display_name;  // 用户可设的显示名,仅展示
    std::string role;          // kRoleAdmin / kRoleMember
    std::string address;       // 最近已知地址(host:port);离线也保留
    std::int64_t last_seen = 0;
    std::int64_t updated_at = 0;
    std::string updated_by;    // 修改该记录的设备 GUID

    // 每日贡献额度(spec team-token-quota):默认 30M,0 = 当日不承接。
    // 字段总是写出(显式 0 与"未设置 30M"语义不同,不能稀疏省略)。
    std::int64_t daily_token_quota = kDefaultDailyQuotaTokens;
    // 最后已知的当日已用 token 数与所属 UTC 日索引(本机上报,展示给其他
    // 队员;离线设备显示这个快照)。quota_day 过期后展示按 0 处理。
    std::int64_t quota_used_today = 0;
    std::int64_t quota_day = 0;
};

struct TeamSessionRef {
    std::string session_id;
    std::string title;
    std::string host_guid;     // 当前权威副本所在设备
    std::string state;         // kSessionState*
    std::string pause_reason;  // 失联 / 额度已用尽 / 异常
    std::string created_by;    // 发起人设备 GUID
    std::int64_t created_at = 0;
    std::int64_t updated_at = 0; // LWW 合并用;状态变更必须更新
};

// 仓库身份:规范化 git remote URL,或无 remote 仓库的本地 UUID。
struct RepoIdentity {
    std::string kind;   // "remote" | "local-id"
    std::string value;
};

struct TeamMeta {
    std::string team_id;
    std::string name;
    std::string invite_seed;
    std::vector<TeamMember> members;
    std::vector<TeamSessionRef> sessions;
    std::vector<RepoIdentity> repos;
    std::int64_t created_at = 0;
    std::string created_by;    // 创建者设备 GUID
    std::int64_t updated_at = 0;
    std::string updated_by;
};

// 角色判断
inline bool is_admin(const TeamMember& member) {
    return member.role == kRoleAdmin;
}

// 团队内成员数上限校验(spec: 成员上限 50)。
inline bool team_full(const TeamMeta& meta) {
    return static_cast<int>(meta.members.size()) >= kMaxTeamMembers;
}

// 进行中的团队会话数(并发准入用;paused 仍占用槽位,ended 释放)。
inline int active_session_count(const TeamMeta& meta) {
    int count = 0;
    for (const auto& session : meta.sessions) {
        if (session.state != kSessionStateEnded) ++count;
    }
    return count;
}

const TeamMember* find_member(const TeamMeta& meta, const std::string& guid);
TeamMember* find_member(TeamMeta& meta, const std::string& guid);
const TeamSessionRef* find_session(const TeamMeta& meta, const std::string& session_id);
TeamSessionRef* find_session(TeamMeta& meta, const std::string& session_id);

// 稀疏序列化:空字符串/零时间戳字段省略,旧版本文件缺字段按默认值读回。
// 时间戳统一用毫秒 Unix epoch(与 ShareRecord 的 created_at 口径一致)。
nlohmann::json member_to_json(const TeamMember& member);
TeamMember member_from_json(const nlohmann::json& j);
nlohmann::json session_ref_to_json(const TeamSessionRef& ref);
TeamSessionRef session_ref_from_json(const nlohmann::json& j);
nlohmann::json repo_identity_to_json(const RepoIdentity& repo);
RepoIdentity repo_identity_from_json(const nlohmann::json& j);
nlohmann::json team_meta_to_json(const TeamMeta& meta);
TeamMeta team_meta_from_json(const nlohmann::json& j);

} // namespace acecode::team
