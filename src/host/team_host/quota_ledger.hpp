#pragma once

// 团队每日额度账本(host 层,spec capability: team-token-quota)。
//
// 口径:
//   - 每台设备只记"自己作为 host 运行团队会话"产生的 LLM 用量,落
//     <data>/team/<team_id>/quota_ledger.jsonl,一行一条,按本机时间日分桶;
//   - 非团队会话(SessionMeta::team_id 不匹配)的消耗不入账;
//   - 当日已用 >= 每日额度(默认 30M,0 = 不承接)即额度用尽,该设备停止
//     承接团队会话(host 会话由调用方置 paused,见 TeamService);
//   - 日界用设备本机时区:day = local_quota_day(timestamp_ms),本机 0 点跨日
//     自然归零。
//
// 记录结构复用 usage ledger 的 TokenUsage 语义(prompt/completion/cache 等),
// 但只存判定额度所需的最小字段,避免把 usage.jsonl 整个搬进团队目录。

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "llm/llm_provider.hpp"
#include "session/session_storage.hpp"
#include "team/team_meta.hpp"
#include "team/team_store.hpp"

namespace acecode::team {

struct QuotaLedgerRecord {
    std::int64_t timestamp_ms = 0;
    std::int64_t day = 0;      // 本机日索引 = local_quota_day(timestamp_ms)
    std::string session_id;
    std::string provider;
    std::string model;
    acecode::TokenUsage usage;
};

// 本机时间日界:day = 本机日历日索引(时区/夏令时按设备当前设置)。
// 团队成员一般在同一局域网,时区一致;远端成员快照的 quota_day 由其
// 设备按自己的本机日计算上报。
std::int64_t local_quota_day(std::int64_t now_ms);

class QuotaLedger {
public:
    QuotaLedger(std::string data_dir, std::string team_id);

    // 记账:session 必须是本团队的会话(meta.team_id == team_id),usage
    // 必须带数据(has_data);否则拒绝并写 *error,绝不误入普通会话用量。
    // 追加写一行 JSONL(失败返回 false,调用方按"未记账"重试)。
    bool record(const acecode::SessionMeta& session, const acecode::TokenUsage& usage,
                std::int64_t now_ms, std::string* error);

    // 某本机日的累计 total_tokens(账本只记团队会话,天然不含普通会话)。
    // 记录按 ts 重算本机日,落盘 day 字段仅作参考。
    std::int64_t used_for_day(std::int64_t day) const;
    std::int64_t used_today(std::int64_t now_ms) const {
        return used_for_day(local_quota_day(now_ms));
    }

    // 全量记录(查询/展示用);损坏行跳过,不抛异常。
    std::vector<QuotaLedgerRecord> load_all() const;

private:
    std::string data_dir_;
    std::string team_id_;
};

// ---- 额度状态判定 ----

struct DeviceQuotaStatus {
    std::int64_t quota = 0;   // 当日有效额度(每日设置)
    std::int64_t used = 0;    // 当日已用(本机实测或最后已知快照)
    std::int64_t day = 0;     // 判定所属本机日索引
    // used >= quota 即用尽(quota=0 时立即用尽 = 当日不承接)。
    bool exhausted() const { return used >= quota; }
};

// 本机状态:额度取成员记录的 daily_token_quota,已用从账本实测。
DeviceQuotaStatus local_quota_status(const TeamMeta& meta, const std::string& guid,
                                     const QuotaLedger& ledger, std::int64_t now_ms);

// 账号级状态(add-team-workdir-sidebar 反馈轮):额度属于本机登录账号而非
// 设备,账号下的所有团队共享同一个池——额度取账号配置,已用为各团队账本
// 当日之和。teams 传本机全部团队(由调用方 load_teams)。
DeviceQuotaStatus account_quota_status(std::int64_t account_quota,
                                       const std::string& data_dir,
                                       const std::vector<TeamMeta>& teams,
                                       std::int64_t now_ms);

// 共享池的团队明细:每个团队当日已用(与 account_quota_status 同口径,
// 团队 ID 去重)。使用情况页按团队列出共享池消耗时用。
struct TeamQuotaUsage {
    std::string team_id;
    std::string name;
    std::int64_t used = 0;
};
std::vector<TeamQuotaUsage> account_quota_by_team(const std::string& data_dir,
                                                  const std::vector<TeamMeta>& teams,
                                                  std::int64_t now_ms);

// 远端成员的最后已知状态:TeamMember 快照;快照日与查看设备的本机日不一致
// 时按已用 0 处理(该设备自己的额度也已跨日归零,不误导用户)。
DeviceQuotaStatus member_quota_snapshot(const TeamMeta& meta, const std::string& guid,
                                        std::int64_t now_ms);

nlohmann::json quota_ledger_record_to_json(const QuotaLedgerRecord& record);
QuotaLedgerRecord quota_ledger_record_from_json(const nlohmann::json& j);

} // namespace acecode::team
