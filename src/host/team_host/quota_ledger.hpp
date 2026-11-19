#pragma once

// 团队每日额度账本(host 层,spec capability: team-token-quota)。
//
// 口径:
//   - 每台设备只记"自己作为 host 运行团队会话"产生的 LLM 用量,落
//     <data>/team/<team_id>/quota_ledger.jsonl,一行一条,按 UTC 日分桶;
//   - 非团队会话(SessionMeta::team_id 不匹配)的消耗不入账;
//   - 当日已用 >= 每日额度(默认 30M,0 = 不承接)即额度用尽,该设备停止
//     承接团队会话(host 会话由调用方置 paused,见 TeamService);
//   - 日界用 UTC:day = timestamp_ms / 86'400'000,跨日自然归零。
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
    std::int64_t day = 0;      // UTC 日索引 = timestamp_ms / kQuotaDayMs
    std::string session_id;
    std::string provider;
    std::string model;
    acecode::TokenUsage usage;
};

class QuotaLedger {
public:
    QuotaLedger(std::string data_dir, std::string team_id);

    // 记账:session 必须是本团队的会话(meta.team_id == team_id),usage
    // 必须带数据(has_data);否则拒绝并写 *error,绝不误入普通会话用量。
    // 追加写一行 JSONL(失败返回 false,调用方按"未记账"重试)。
    bool record(const acecode::SessionMeta& session, const acecode::TokenUsage& usage,
                std::int64_t now_ms, std::string* error);

    // 某 UTC 日的累计 total_tokens(账本只记团队会话,天然不含普通会话)。
    std::int64_t used_for_day(std::int64_t day) const;
    std::int64_t used_today(std::int64_t now_ms) const {
        return used_for_day(now_ms / kQuotaDayMs);
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
    std::int64_t day = 0;     // 判定所属 UTC 日索引
    // used >= quota 即用尽(quota=0 时立即用尽 = 当日不承接)。
    bool exhausted() const { return used >= quota; }
};

// 本机状态:额度取成员记录的 daily_token_quota,已用从账本实测。
DeviceQuotaStatus local_quota_status(const TeamMeta& meta, const std::string& guid,
                                     const QuotaLedger& ledger, std::int64_t now_ms);

// 远端成员的最后已知状态:TeamMember 快照;快照不是今天的按已用 0 处理
//(次日该设备自身额度也会归零,不误导用户)。
DeviceQuotaStatus member_quota_snapshot(const TeamMeta& meta, const std::string& guid,
                                        std::int64_t now_ms);

nlohmann::json quota_ledger_record_to_json(const QuotaLedgerRecord& record);
QuotaLedgerRecord quota_ledger_record_from_json(const nlohmann::json& j);

} // namespace acecode::team
