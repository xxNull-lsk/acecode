#pragma once

// 团队服务(host 层):团队状态机 + 成员管理 + 元数据收敛。
//
// 设计边界(见 design.md 决策 4/6):
//   - 本层是纯逻辑 + 本机持久化,不直接开 socket;网络动作由调用方注入
//     SnapshotFetcher 或由 Web 路由桥接,便于单测稳定覆盖状态迁移。
//   - 元数据收敛用逐字段 last-writer-wins:成员按 updated_at,last_seen 取
//     max(在线状态独立于元数据修改,不制造 LWW 冲突)。
//   - 团队凭据由 invite_seed 派生 HMAC,和 daemon token 完全隔离:团队凭据
//     只能访问团队路由,不能通过 auth.hpp 的 require_auth。

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "team/team_meta.hpp"
#include "team/team_invite.hpp"
#include "team/team_store.hpp"
#include "team/repo_identity.hpp"

namespace acecode::team {

// 团队请求头(与 daemon 的 X-ACECode-Token 并列,互不通用)。
inline constexpr std::string_view kTeamTokenHeader = "X-ACECode-Team-Token";
// 在线判定:最后心跳超过该秒数即视为离线(只影响展示与切换目标选择)。
inline constexpr std::int64_t kMemberStaleSeconds = 120;

struct DeviceIdentity {
    std::string guid;          // 设备 GUID(身份主键)
    std::string account;       // 用户名+电脑名
    std::string display_name;  // 可为空,展示时回退到 account
};

struct TeamOpResult {
    bool ok = false;
    std::string error;
    explicit operator bool() const { return ok; }
    static TeamOpResult success() { return {true, {}}; }
    static TeamOpResult fail(std::string message) { return {false, std::move(message)}; }
};

struct InviteIssueResult {
    TeamInviteLink invite;
    std::string error;
    explicit operator bool() const { return error.empty(); }
};

struct SessionRefResult {
    TeamSessionRef ref;
    std::string error;
    explicit operator bool() const { return error.empty(); }
};

// 加入时从邀请者拉全量快照;网络实现由调用方注入(fetcher 抛错/返回空都按
// 失败处理)。测试用假 fetcher 覆盖加入流程的状态迁移。
using SnapshotFetcher = std::function<std::optional<TeamMeta>(const TeamInviteLink& invite,
                                                             std::string* error)>;

class TeamService {
public:
    TeamService(std::string data_dir, DeviceIdentity self,
                std::function<std::int64_t()> now_seconds = default_clock);

    static std::int64_t default_clock();

    const DeviceIdentity& self() const { return self_; }
    const std::string& data_dir() const { return data_dir_; }
    std::int64_t now() const { return now_seconds_(); }
    TeamStore store_for(const std::string& team_id) const;

    // 本机所有团队(扫描 data_dir/team/*/team.json),按创建时间升序。
    std::vector<TeamMeta> load_teams() const;
    std::optional<TeamMeta> load_team(const std::string& team_id) const;

    // 创建团队:创建者为管理员;生成团队 ID 与邀请种子并落盘。
    std::optional<TeamMeta> create_team(const std::string& name, std::string* error);

    // 签发一次性邀请码(仅管理员)。now 取注入时钟。
    InviteIssueResult issue_invite(const std::string& team_id,
                                   const std::vector<std::string>& addresses,
                                   int port);

    // 邀请者侧:验证 token 并消费,登记新成员,返回要发给加入者的全量快照。
    struct JoinAccept {
        TeamMeta snapshot;
        std::string error;
        explicit operator bool() const { return error.empty(); }
    };
    JoinAccept accept_join(const std::string& team_id, const std::string& token,
                           const DeviceIdentity& joiner);

    // 加入者侧:用注入的 fetcher 从邀请者拉全量并落盘。
    TeamOpResult join_team(const TeamInviteLink& invite, const SnapshotFetcher& fetch);

    // 加入者侧:直接应用一份快照(供 fetcher 之外的路径与测试)。
    TeamOpResult apply_snapshot(const TeamMeta& snapshot);

    // 增量同步:把远端状态逐字段合并进本地并落盘。返回合并后的元数据。
    std::optional<TeamMeta> merge_remote(const std::string& team_id, const TeamMeta& remote,
                                         std::string* error);

    // 角色管理:actor 必须是管理员;只能设置 admin/member。
    TeamOpResult set_role(const std::string& team_id, const std::string& actor_guid,
                          const std::string& target_guid, const std::string& role);

    // 心跳:更新成员最后在线时间与地址,不修改元数据版本戳。
    TeamOpResult touch_member(const std::string& team_id, const std::string& guid,
                              const std::string& address);

    // 在线判定(基于 last_seen 与 now 的差)。
    bool member_online(const TeamMeta& meta, const std::string& guid) const;

    // 团队凭据校验:header 值是否与 invite_seed 派生值一致(常量时间比较)。
    bool authorize(const std::string& team_id, const std::string& header_value) const;

    // ---- 团队会话引用(spec capability: team-session) ----
    // 发起:校验仓库关联与并发上限,登记 session_refs,发起设备成为初始 host。
    SessionRefResult start_session(const std::string& team_id, const RepoIdentity& repo,
                                   const std::string& session_id, const std::string& title);
    // 状态变更(结束/暂停,reason 标注失联/额度/异常);ended 释放并发槽位。
    TeamOpResult set_session_state(const std::string& team_id, const std::string& session_id,
                                   std::string_view state, const std::string& pause_reason);
    // 手动接管后切换权威副本所在设备(P5 入口,P3 提供)。
    TeamOpResult set_session_host(const std::string& team_id, const std::string& session_id,
                                  const std::string& new_host_guid);

    // 关联项目(成员设备把自己的仓库身份登记进团队;幂等)。发起会话的
    // 前置条件:repo_identities 匹配才允许标记团队会话。
    TeamOpResult register_repo(const std::string& team_id, const RepoIdentity& repo);

    // ---- 每日额度(spec capability: team-token-quota) ----
    // 成员自行修改每日额度(0 = 当日不承接);只能改自己的,负值拒绝。
    TeamOpResult set_daily_quota(const std::string& team_id, const std::string& actor_guid,
                                 std::int64_t quota);
    // 本机用量上报:写自己成员记录的当日已用快照,随元数据同步供他人展示。
    TeamOpResult report_quota_usage(const std::string& team_id, std::int64_t used_today,
                                    std::int64_t day);
    // 本机额度用尽:把本机 host 的进行中团队会话置 paused(额度已用尽)。
    // 返回被暂停的会话 ID(调用方广播通知);数据不清除。
    std::vector<std::string> pause_sessions_for_quota(const std::string& team_id);
    // UTC 日切换后额度归零:把本机 host 的 paused(额度已用尽) 会话恢复 running。
    // 仅恢复配额原因的暂停,不动失联/异常等其他原因。
    std::vector<std::string> resume_quota_paused_sessions(const std::string& team_id);

    // ---- 失联检测(spec team-session-handover: 只标记,不自动切换) ----
    // 把 host 心跳超时(成员 last_seen 超过 kMemberStaleSeconds,或 host 成员
    // 已不在团队)的进行中会话置 paused(失联)。任何设备都可执行,幂等;
    // 返回本次新暂停的会话 ID(调用方广播通知)。绝不修改 host 指向。
    std::vector<std::string> mark_lost_host_sessions(const std::string& team_id);

private:
    std::string data_dir_;
    DeviceIdentity self_;
    std::function<std::int64_t()> now_seconds_;

    std::optional<TeamMeta> load_or_error(const std::string& team_id, std::string* error) const;
};

// 团队凭据派生与校验(设计决策 6)。token = hex(HMAC-SHA256(invite_seed, "team:"+team_id))。
std::string derive_team_token(const std::string& invite_seed, const std::string& team_id);
bool verify_team_token(const TeamMeta& meta, std::string_view token);

} // namespace acecode::team
