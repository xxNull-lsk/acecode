#include "team_host/quota_ledger.hpp"

#include "utils/utf8_path.hpp"

#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <set>
#include <sstream>

namespace fs = std::filesystem;

namespace acecode::team {
namespace {

std::mutex& append_mutex() {
    static std::mutex mu;
    return mu;
}

fs::path ledger_path(const TeamStore& store) {
    return store.root_dir() / "quota_ledger.jsonl";
}

std::int64_t read_i64(const nlohmann::json& j, const char* key) {
    const auto it = j.find(key);
    if (it == j.end() || !it->is_number_integer()) return 0;
    return it->get<std::int64_t>();
}

std::string read_string(const nlohmann::json& j, const char* key) {
    const auto it = j.find(key);
    return it != j.end() && it->is_string() ? it->get<std::string>() : std::string{};
}

} // namespace

// 本机时间日界:额度按设备本机时区的日历日(0 点)重置,不用 UTC 日。
// 把本机墙上时间按 UTC 解释(timegm / _mkgmtime)得到其"墙上秒数",与真实
// 时间戳之差即当前偏移,含夏令时;day = (now + offset) / 86'400'000 为本机
// 日历日索引(UTC+8 的 22:00 属于本机次日,须加偏移才落到次日的日索引)。
//
// 不要用 mktime 归一 UTC 墙上时间:那会把 UTC 字段当作本地时间重新判定
// DST,在切换窗口(如纽约回拨日 00:00-01:00)算出与真实偏移差 1 小时,
// 把本机 0 点刚过的用量记到前一天。
std::int64_t local_quota_day(std::int64_t now_ms) {
    if (now_ms < 0) return 0;
    const std::time_t seconds = static_cast<std::time_t>(now_ms / 1000);
    std::tm local_tm{};
#ifdef _WIN32
    localtime_s(&local_tm, &seconds);
    const std::time_t wall_seconds = _mkgmtime(&local_tm);
#else
    localtime_r(&seconds, &local_tm);
    const std::time_t wall_seconds = timegm(&local_tm);
#endif
    const auto offset_ms =
        (static_cast<std::int64_t>(wall_seconds) -
         static_cast<std::int64_t>(seconds)) * 1000;
    return (now_ms + offset_ms) / kQuotaDayMs;
}

QuotaLedger::QuotaLedger(std::string data_dir, std::string team_id)
    : data_dir_(std::move(data_dir)), team_id_(std::move(team_id)) {}

nlohmann::json quota_ledger_record_to_json(const QuotaLedgerRecord& record) {
    nlohmann::json j;
    j["v"] = 1;
    j["ts_ms"] = record.timestamp_ms;
    j["day"] = record.day;
    if (!record.session_id.empty()) j["session_id"] = record.session_id;
    if (!record.provider.empty()) j["provider"] = record.provider;
    if (!record.model.empty()) j["model"] = record.model;
    j["usage"] = {
        {"prompt_tokens", record.usage.prompt_tokens},
        {"completion_tokens", record.usage.completion_tokens},
        {"total_tokens", record.usage.total_tokens},
        {"cache_read_tokens", record.usage.cache_read_tokens},
        {"cache_write_tokens", record.usage.cache_write_tokens},
        {"reasoning_tokens", record.usage.reasoning_tokens},
    };
    return j;
}

QuotaLedgerRecord quota_ledger_record_from_json(const nlohmann::json& j) {
    QuotaLedgerRecord record;
    record.timestamp_ms = read_i64(j, "ts_ms");
    record.day = read_i64(j, "day");
    record.session_id = read_string(j, "session_id");
    record.provider = read_string(j, "provider");
    record.model = read_string(j, "model");
    if (auto usage = j.find("usage"); usage != j.end() && usage->is_object()) {
        record.usage.prompt_tokens = static_cast<int>(read_i64(*usage, "prompt_tokens"));
        record.usage.completion_tokens = static_cast<int>(read_i64(*usage, "completion_tokens"));
        record.usage.total_tokens = static_cast<int>(read_i64(*usage, "total_tokens"));
        record.usage.cache_read_tokens = static_cast<int>(read_i64(*usage, "cache_read_tokens"));
        record.usage.cache_write_tokens = static_cast<int>(read_i64(*usage, "cache_write_tokens"));
        record.usage.reasoning_tokens = static_cast<int>(read_i64(*usage, "reasoning_tokens"));
        record.usage.has_data = true;
    }
    return record;
}

bool QuotaLedger::record(const acecode::SessionMeta& session, const acecode::TokenUsage& usage,
                         std::int64_t now_ms, std::string* error) {
    if (session.team_id != team_id_) {
        if (error) *error = "not_team_session";
        return false;
    }
    if (!usage.has_data) {
        if (error) *error = "usage_missing";
        return false;
    }
    QuotaLedgerRecord entry;
    entry.timestamp_ms = now_ms;
    entry.day = local_quota_day(now_ms);
    entry.session_id = session.id;
    entry.provider = session.provider;
    entry.model = session.model;
    entry.usage = usage;

    std::lock_guard<std::mutex> lock(append_mutex());
    std::error_code ec;
    fs::create_directories(path_from_utf8(data_dir_) / "team" / team_id_, ec);
    if (ec) {
        if (error) *error = "team_dir_create_failed";
        return false;
    }
    const TeamStore store(data_dir_, team_id_);
    std::ofstream out(ledger_path(store), std::ios::binary | std::ios::app);
    if (!out.is_open()) {
        if (error) *error = "ledger_write_failed";
        return false;
    }
    std::string line = quota_ledger_record_to_json(entry).dump();
    line.push_back('\n');
    out.write(line.data(), static_cast<std::streamsize>(line.size()));
    out.flush();
    if (!out) {
        if (error) *error = "ledger_write_failed";
        return false;
    }
    return true;
}

std::vector<QuotaLedgerRecord> QuotaLedger::load_all() const {
    std::vector<QuotaLedgerRecord> records;
    const TeamStore store(data_dir_, team_id_);
    std::ifstream input(ledger_path(store), std::ios::binary);
    if (!input.is_open()) return records;
    std::string line;
    while (std::getline(input, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        try {
            records.push_back(quota_ledger_record_from_json(nlohmann::json::parse(line)));
        } catch (...) {
            // 中断写入或损坏行跳过;账本读容错,与 usage ledger 同口径。
        }
    }
    return records;
}

std::int64_t QuotaLedger::used_for_day(std::int64_t day) const {
    std::int64_t total = 0;
    for (const auto& record : load_all()) {
        // 以记录时间戳重算本机日,不信任落盘的 day 字段:旧版本用 UTC 日或
        // 偏移方向错误的日索引写入,按 ts 重算可把存量记录自动归位。
        if (local_quota_day(record.timestamp_ms) == day) {
            total += record.usage.total_tokens;
        }
    }
    return total;
}

DeviceQuotaStatus local_quota_status(const TeamMeta& meta, const std::string& guid,
                                     const QuotaLedger& ledger, std::int64_t now_ms) {
    DeviceQuotaStatus status;
    status.day = local_quota_day(now_ms);
    if (const TeamMember* member = find_member(meta, guid)) {
        status.quota = member->daily_token_quota;
    }
    status.used = ledger.used_for_day(status.day);
    return status;
}

std::vector<TeamQuotaUsage> account_quota_by_team(const std::string& data_dir,
                                                  const std::vector<TeamMeta>& teams,
                                                  std::int64_t now_ms) {
    const auto day = local_quota_day(now_ms);
    std::vector<TeamQuotaUsage> result;
    std::set<std::string> seen;
    for (const auto& meta : teams) {
        if (meta.team_id.empty() || !seen.insert(meta.team_id).second) continue;
        result.push_back({meta.team_id, meta.name,
                          QuotaLedger(data_dir, meta.team_id).used_for_day(day)});
    }
    return result;
}

DeviceQuotaStatus account_quota_status(std::int64_t account_quota,
                                       const std::string& data_dir,
                                       const std::vector<TeamMeta>& teams,
                                       std::int64_t now_ms) {
    DeviceQuotaStatus status;
    status.day = local_quota_day(now_ms);
    status.quota = account_quota < 0 ? kDefaultDailyQuotaTokens : account_quota;
    // 共享池:本机账号下所有团队的账本当日用量求和(团队 ID 去重)。
    std::int64_t used = 0;
    for (const auto& entry : account_quota_by_team(data_dir, teams, now_ms)) {
        used += entry.used;
    }
    status.used = used;
    return status;
}

DeviceQuotaStatus member_quota_snapshot(const TeamMeta& meta, const std::string& guid,
                                        std::int64_t now_ms) {
    DeviceQuotaStatus status;
    status.day = local_quota_day(now_ms);
    const TeamMember* member = find_member(meta, guid);
    if (!member) return status;
    status.quota = member->daily_token_quota;
    status.used = member->quota_day == status.day ? member->quota_used_today : 0;
    return status;
}

} // namespace acecode::team
