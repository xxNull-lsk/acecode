// 覆盖 src/host/team_host/quota_ledger.cpp 与 TeamService 的额度操作
// (tasks 4.1-4.4)。锁定 spec(team-token-quota) 的四条契约:
//   记账只认团队会话、本机时间日分桶跨日归零、用尽即暂停承接(含 0 额度)、
//   额度/用量展示取本机实测或最后已知快照。

#include <gtest/gtest.h>

#include "team_host/quota_ledger.hpp"
#include "team_host/team_service.hpp"
#include "team_host/team_session_book.hpp"
#include "team/team_store.hpp"
#include "utils/utf8_path.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;
using namespace acecode::team;
using acecode::SessionMeta;
using acecode::TokenUsage;

namespace {

struct TempDir {
    fs::path root;
    TempDir() {
        root = fs::temp_directory_path() /
               ("quota_ledger_test_" +
                std::to_string(std::chrono::steady_clock::now()
                                   .time_since_epoch()
                                   .count()));
        fs::remove_all(root);
        fs::create_directories(root);
    }
    ~TempDir() { fs::remove_all(root); }
    std::string utf8() const { return acecode::path_to_utf8(root); }
};

struct Clock {
    std::int64_t now = 1'700'000'000;
};

DeviceIdentity identity(const std::string& guid) { return DeviceIdentity{guid, guid + "@pc", guid}; }

SessionMeta team_session(const std::string& id, const std::string& team_id = "team-1") {
    SessionMeta meta;
    meta.id = id;
    meta.team_id = team_id;
    meta.team_role = "host";
    meta.provider = "openai";
    meta.model = "gpt-5";
    return meta;
}

TokenUsage usage(int total) {
    TokenUsage usage;
    usage.prompt_tokens = total / 2;
    usage.completion_tokens = total - total / 2;
    usage.total_tokens = total;
    usage.has_data = true;
    return usage;
}

TeamMeta team_with_member(const std::string& guid = "guid-a") {
    TeamMeta meta;
    meta.team_id = "team-1";
    meta.invite_seed = "seed";
    TeamMember member;
    member.guid = guid;
    member.role = "admin";
    member.updated_at = 1;
    meta.members.push_back(member);
    meta.repos.push_back({"remote", "r"});
    meta.updated_at = 1;
    meta.updated_by = guid;
    return meta;
}

std::int64_t day_of(std::int64_t now_ms) { return local_quota_day(now_ms); }

} // namespace

// 4.1 场景: 记账只计本团队的会话;普通会话与无用量数据不入账
TEST(QuotaLedger, RecordsOnlyTeamSessions) {
    TempDir temp;
    QuotaLedger ledger(temp.utf8(), "team-1");
    std::string error;

    EXPECT_TRUE(ledger.record(team_session("s-1"), usage(2'000'000), 1'700'000'000'000, &error))
        << error;

    // 普通会话(非本团队)拒绝
    EXPECT_FALSE(ledger.record(team_session("s-2", ""), usage(1'000), 1'700'000'000'000, &error));
    EXPECT_EQ(error, "not_team_session");
    // 其他团队也拒绝
    EXPECT_FALSE(ledger.record(team_session("s-2", "team-2"), usage(1'000),
                               1'700'000'000'000, &error));
    EXPECT_EQ(error, "not_team_session");
    // 无用量数据拒绝
    TokenUsage no_data;
    no_data.has_data = false;
    EXPECT_FALSE(ledger.record(team_session("s-3"), no_data, 1'700'000'000'000, &error));
    EXPECT_EQ(error, "usage_missing");

    EXPECT_EQ(ledger.used_today(1'700'000'000'000), 2'000'000);
}

// 4.1 场景: 按本机时间日分桶,跨日归零;持久化可重读
TEST(QuotaLedger, DayBucketsResetAndPersist) {
    TempDir temp;
    const std::int64_t day1_ms = 1'700'000'000'000; // 本机日 N
    {
        QuotaLedger ledger(temp.utf8(), "team-1");
        std::string error;
        ASSERT_TRUE(ledger.record(team_session("s-1"), usage(2'000'000), day1_ms, &error)) << error;
        ASSERT_TRUE(ledger.record(team_session("s-1"), usage(1'500'000),
                                  day1_ms + 60'000, &error)) << error;
        EXPECT_EQ(ledger.used_today(day1_ms), 3'500'000);
    }
    const std::int64_t day2_ms = day1_ms + 86'400'000;
    {
        // 新实例重读账本:昨日用量不计入今天
        QuotaLedger ledger(temp.utf8(), "team-1");
        EXPECT_EQ(ledger.used_today(day2_ms), 0);
        EXPECT_EQ(ledger.used_for_day(day_of(day1_ms)), 3'500'000);
        // 今天再记 1M:今天累计只有这 1M
        std::string error;
        ASSERT_TRUE(ledger.record(team_session("s-1"), usage(1'000'000), day2_ms, &error));
        EXPECT_EQ(ledger.used_today(day2_ms), 1'000'000);
        const auto records = ledger.load_all();
        EXPECT_EQ(records.size(), 3u);
    }
}

// 4.1 场景: 损坏行跳过,不抛异常(与 usage ledger 同容错口径)
TEST(QuotaLedger, ToleratesCorruptLines) {
    TempDir temp;
    const fs::path path = temp.root / "team" / "team-1" / "quota_ledger.jsonl";
    fs::create_directories(path.parent_path());
    {
        std::ofstream out(path, std::ios::binary);
        out << "{not json}\n";
        // ts=1ms,day 字段写一个不匹配的值:读取按 ts 重算本机日,day 仅参考。
        out << quota_ledger_record_to_json(QuotaLedgerRecord{1, 2, "s-1", "", "", usage(500)}).dump()
            << "\n";
    }
    QuotaLedger ledger(temp.utf8(), "team-1");
    const auto records = ledger.load_all();
    ASSERT_EQ(records.size(), 1u);
    EXPECT_EQ(records[0].usage.total_tokens, 500);
    EXPECT_EQ(ledger.used_for_day(local_quota_day(1)), 500);
    // 存量记录的 day 字段错误(如旧版 UTC 日)不影响当日累计
    EXPECT_EQ(ledger.used_for_day(2), 0);
}

// 本机时间日界:UTC+8 的 22:00 属于本机次日;重置点为本机 0 点而非 UTC 0 点。
TEST(QuotaLedger, LocalDayFollowsMachineTimezone) {
#ifdef _WIN32
    char prev_tz[128] = {};
    size_t prev_size = 0;
    getenv_s(&prev_size, prev_tz, sizeof(prev_tz), "TZ");
    // MSVC 的 TZ 只认 POSIX 写法(CST-8 = UTC+8),不认 IANA 名。
    _putenv_s("TZ", "CST-8");
    _tzset();
#else
    const char* prev = getenv("TZ");
    const std::string prev_tz = prev ? prev : "";
    setenv("TZ", "Asia/Shanghai", 1);
    tzset();
#endif
    // UTC 2023-11-14 22:13:20 = 本机(UTC+8) 2023-11-15 06:13:20 → 本机日索引 = UTC 日 + 1
    const std::int64_t cst_morning = 1'700'000'000'000;
    EXPECT_EQ(local_quota_day(cst_morning), cst_morning / kQuotaDayMs + 1);
    // UTC 2023-11-14 15:58:00 = 本机 23:58(还是 11-14) → 与 UTC 日索引相同
    const std::int64_t cst_late_evening = 1'699'977'480'000;
    EXPECT_EQ(local_quota_day(cst_late_evening), cst_late_evening / kQuotaDayMs);
    // UTC 2023-11-14 20:00:00 = 本机 2023-11-15 04:00 → 本机日索引 = UTC 日 + 1
    const std::int64_t cst_dawn = 1'699'992'000'000;
    EXPECT_EQ(local_quota_day(cst_dawn), cst_dawn / kQuotaDayMs + 1);
#ifdef _WIN32
    _putenv_s("TZ", prev_tz && prev_tz[0] ? prev_tz : "");
    _tzset();
#else
    if (!prev_tz.empty()) setenv("TZ", prev_tz.c_str(), 1);
    else unsetenv("TZ");
    tzset();
#endif
}

// 夏令时回拨日的 0 点:本机 00:30 EDT 仍属于当天。旧实现用 mktime 把 UTC
// 墙上时间按本地时间归一,切换窗口内偏移差 1 小时,会把这半小时记到前一天。
#ifndef _WIN32
TEST(QuotaLedger, LocalDaySurvivesDstFallBack) {
    const char* prev = getenv("TZ");
    const std::string prev_tz = prev ? prev : "";
    setenv("TZ", "America/New_York", 1);
    tzset();
    // 2023-11-05T04:30Z = 本机 2023-11-05 00:30 EDT(UTC-4),UTC 与本机同日
    const std::int64_t just_after_midnight = 1'699'158'600'000;
    EXPECT_EQ(local_quota_day(just_after_midnight), just_after_midnight / kQuotaDayMs);
    // 2023-11-05T05:30Z / 06:30Z = 回拨前后两个 01:30,仍属当天
    const std::int64_t first_one_thirty = 1'699'162'200'000;
    const std::int64_t second_one_thirty = 1'699'165'800'000;
    EXPECT_EQ(local_quota_day(first_one_thirty), first_one_thirty / kQuotaDayMs);
    EXPECT_EQ(local_quota_day(second_one_thirty), second_one_thirty / kQuotaDayMs);
    // 2023-11-05T03:30Z = 本机 2023-11-04 23:30 EDT,本机日索引比 UTC 小 1
    const std::int64_t previous_evening = 1'699'155'000'000;
    EXPECT_EQ(local_quota_day(previous_evening), previous_evening / kQuotaDayMs - 1);
    if (!prev_tz.empty()) setenv("TZ", prev_tz.c_str(), 1);
    else unsetenv("TZ");
    tzset();
}
#endif

// 4.2 场景: 默认额度 30M;成员可自行修改(含 0);只能改自己的
TEST(QuotaService, DailyQuotaDefaultsAndSelfService) {
    TempDir temp;
    Clock clock;
    TeamService service(temp.utf8(), identity("guid-a"), [&] { return clock.now; });
    ASSERT_TRUE(service.apply_snapshot(team_with_member()));

    const auto meta = service.load_team("team-1");
    ASSERT_TRUE(meta.has_value());
    EXPECT_EQ(meta->members[0].daily_token_quota, kDefaultDailyQuotaTokens); // 默认 30M

    EXPECT_TRUE(service.set_daily_quota("team-1", "guid-a", 10'000'000));
    EXPECT_EQ(service.load_team("team-1")->members[0].daily_token_quota, 10'000'000);

    // 0 = 当日不承接;负值拒绝;不能替别人改
    EXPECT_TRUE(service.set_daily_quota("team-1", "guid-a", 0));
    EXPECT_EQ(service.load_team("team-1")->members[0].daily_token_quota, 0);
    EXPECT_EQ(service.set_daily_quota("team-1", "guid-a", -1).error, "invalid_quota");

    TeamService other(temp.utf8(), identity("guid-b"), [&] { return clock.now; });
    other.apply_snapshot(team_with_member());
    EXPECT_EQ(other.set_daily_quota("team-1", "guid-b", 5'000'000).error, "member_not_found");
}

// 4.2/4.4 场景: 用量上报后其他设备经 LWW 合并看到最后已知快照;跨日显示归零
TEST(QuotaService, UsageSnapshotSyncsAndResetsAcrossDays) {
    TempDir a_dir;
    TempDir b_dir;
    Clock clock;
    TeamService device_a(a_dir.utf8(), identity("guid-a"), [&] { return clock.now; });
    TeamService device_b(b_dir.utf8(), identity("guid-b"), [&] { return clock.now; });
    ASSERT_TRUE(device_a.apply_snapshot(team_with_member()));
    ASSERT_TRUE(device_b.apply_snapshot(team_with_member()));

    const std::int64_t now_ms = clock.now * 1000;
    ASSERT_TRUE(device_a.report_quota_usage("team-1", 20'000'000, day_of(now_ms)));

    // B 从 A 合并成员记录
    std::string error;
    ASSERT_TRUE(device_b.merge_remote("team-1", *device_a.load_team("team-1"), &error)) << error;

    const auto status_b = member_quota_snapshot(*device_b.load_team("team-1"), "guid-a", now_ms);
    EXPECT_EQ(status_b.quota, kDefaultDailyQuotaTokens);
    EXPECT_EQ(status_b.used, 20'000'000);
    EXPECT_FALSE(status_b.exhausted());

    // 次日:快照过期,展示归零
    const std::int64_t tomorrow_ms = now_ms + 86'400'000;
    const auto status_next = member_quota_snapshot(*device_b.load_team("team-1"), "guid-a",
                                                   tomorrow_ms);
    EXPECT_EQ(status_next.used, 0);
    EXPECT_EQ(status_next.day, day_of(tomorrow_ms));
}

// 4.3 场景: 本机额度状态 = 设置额度 + 账本实测已用;用尽判定含 0 额度
TEST(QuotaService, LocalStatusExhausted) {
    TempDir temp;
    const std::int64_t now_ms = 1'700'000'000'000;
    QuotaLedger ledger(temp.utf8(), "team-1");
    std::string error;
    ASSERT_TRUE(ledger.record(team_session("s-1"), usage(30'000'000), now_ms, &error));

    TeamMeta meta = team_with_member();
    meta.members[0].daily_token_quota = 30'000'000;
    const auto status = local_quota_status(meta, "guid-a", ledger, now_ms);
    EXPECT_EQ(status.used, 30'000'000);
    EXPECT_TRUE(status.exhausted());

    meta.members[0].daily_token_quota = 29'999'999;
    EXPECT_TRUE(local_quota_status(meta, "guid-a", ledger, now_ms).exhausted());
    meta.members[0].daily_token_quota = 0;
    EXPECT_TRUE(local_quota_status(meta, "guid-a", ledger, now_ms).exhausted()); // 0 = 不承接
    meta.members[0].daily_token_quota = 50'000'000;
    EXPECT_FALSE(local_quota_status(meta, "guid-a", ledger, now_ms).exhausted());
}

// 账号级共享池(add-team-workdir-sidebar 反馈轮):额度属于登录账号,
// 已用为本机所有团队账本当日之和;负额度回退默认值。
TEST(QuotaService, AccountPoolSumsAcrossTeams) {
    TempDir temp;
    const std::int64_t now_ms = 1'700'000'000'000;
    ASSERT_TRUE(QuotaLedger(temp.utf8(), "team-1")
                    .record(team_session("s-1"), usage(10'000'000), now_ms, nullptr));
    ASSERT_TRUE(QuotaLedger(temp.utf8(), "team-2")
                    .record(team_session("s-2", "team-2"), usage(15'000'000), now_ms, nullptr));

    TeamMeta team1 = team_with_member();
    team1.team_id = "team-1";
    team1.name = "研发组";
    TeamMeta team2 = team_with_member();
    team2.team_id = "team-2";

    const auto status = account_quota_status(30'000'000, temp.utf8(), {team1, team2}, now_ms);
    EXPECT_EQ(status.used, 25'000'000); // 两团队之和 = 共享池已用
    EXPECT_FALSE(status.exhausted());

    // 团队明细:与总和同口径,各团队各自账本当日之和;重复团队 ID 去重
    const auto breakdown = account_quota_by_team(temp.utf8(), {team1, team2, team1}, now_ms);
    ASSERT_EQ(breakdown.size(), 2u);
    EXPECT_EQ(breakdown[0].team_id, "team-1");
    EXPECT_EQ(breakdown[0].name, "研发组");
    EXPECT_EQ(breakdown[0].used, 10'000'000);
    EXPECT_EQ(breakdown[1].team_id, "team-2");
    EXPECT_EQ(breakdown[1].used, 15'000'000);

    // 超过账号额度即用尽(单团队未超也会被共享池拦住)
    EXPECT_TRUE(account_quota_status(20'000'000, temp.utf8(), {team1, team2}, now_ms)
                    .exhausted());
    // 0 额度 = 当日不承接
    EXPECT_TRUE(account_quota_status(0, temp.utf8(), {team1, team2}, now_ms).exhausted());
    // 非法配置回退默认 30M,不被负值放大
    EXPECT_EQ(account_quota_status(-5, temp.utf8(), {team1, team2}, now_ms).quota,
              kDefaultDailyQuotaTokens);
    // 重复团队 ID 去重,不重复求和
    EXPECT_EQ(account_quota_status(30'000'000, temp.utf8(), {team1, team1}, now_ms).used,
              10'000'000);
}

// 4.3 场景: 额度用尽 → 本机 host 会话置 paused(额度已用尽),别人的会话不动
TEST(QuotaService, ExhaustionPausesSelfHostedSessions) {
    TempDir temp;
    Clock clock;
    TeamService service(temp.utf8(), identity("guid-a"), [&] { return clock.now; });
    ASSERT_TRUE(service.apply_snapshot(team_with_member()));
    ASSERT_TRUE(service.start_session("team-1", RepoIdentity{"remote", "r"}, "s-1", "本机会话"));
    // 别的设备 host 的会话
    TeamMeta meta = *service.load_team("team-1");
    meta.sessions.push_back({"s-2", "远端会话", "guid-b", "running", "", "guid-b", 1, 1});
    ASSERT_TRUE(TeamStore(temp.utf8(), "team-1").save_meta(meta));

    const auto paused = service.pause_sessions_for_quota("team-1");
    ASSERT_EQ(paused.size(), 1u);
    EXPECT_EQ(paused[0], "s-1");

    const auto stored = service.load_team("team-1");
    ASSERT_TRUE(stored.has_value());
    const TeamSessionRef* s1 = find_session(*stored, "s-1");
    ASSERT_TRUE(s1);
    EXPECT_EQ(s1->state, "paused");
    EXPECT_EQ(s1->pause_reason, "额度已用尽");
    const TeamSessionRef* s2 = find_session(*stored, "s-2");
    ASSERT_TRUE(s2);
    EXPECT_EQ(s2->state, "running"); // 别人的 host 不受本机额度影响

    // 幂等:再调用不再返回,状态不再变
    EXPECT_TRUE(service.pause_sessions_for_quota("team-1").empty());
}

// 4.3 场景: 本机时间日切换后本机 host 的配额暂停恢复 running;其他暂停原因不动
TEST(QuotaService, QuotaPauseResumesNextDay) {
    TempDir temp;
    Clock clock;
    TeamService service(temp.utf8(), identity("guid-a"), [&] { return clock.now; });
    ASSERT_TRUE(service.apply_snapshot(team_with_member()));
    ASSERT_TRUE(service.start_session("team-1", RepoIdentity{"remote", "r"}, "s-1", "配额暂停"));
    ASSERT_FALSE(service.pause_sessions_for_quota("team-1").empty());
    // 另一个失联原因的暂停,不应被恢复
    TeamMeta meta = *service.load_team("team-1");
    meta.sessions.push_back({"s-2", "失联暂停", "guid-a", "paused", "失联", "guid-a", 1, 1});
    ASSERT_TRUE(TeamStore(temp.utf8(), "team-1").save_meta(meta));

    clock.now += 90'000; // 日切换由账本/调用方判定,这里只验证恢复逻辑
    const auto resumed = service.resume_quota_paused_sessions("team-1");
    ASSERT_EQ(resumed.size(), 1u);
    EXPECT_EQ(resumed[0], "s-1");

    const auto stored = service.load_team("team-1");
    ASSERT_TRUE(stored.has_value());
    EXPECT_EQ(find_session(*stored, "s-1")->state, "running");
    EXPECT_TRUE(find_session(*stored, "s-1")->pause_reason.empty());
    EXPECT_EQ(find_session(*stored, "s-2")->state, "paused"); // 失联原因保持
    EXPECT_EQ(find_session(*stored, "s-2")->pause_reason, "失联");
}

// 4.3 场景: 准入带额度门:用尽(含 0 额度)拒绝发起
TEST(TeamSessionBook, AdmissionRejectsWhenQuotaExhausted) {
    const TeamMeta meta = team_with_member();
    const RepoIdentity repo{"remote", "r"};

    DeviceQuotaStatus fine;
    fine.quota = kDefaultDailyQuotaTokens;
    fine.used = 1'000;
    fine.day = 1;
    EXPECT_EQ(check_session_admission(meta, repo, "s-1", &fine).ok, true);

    DeviceQuotaStatus exhausted;
    exhausted.quota = kDefaultDailyQuotaTokens;
    exhausted.used = exhausted.quota;
    EXPECT_EQ(check_session_admission(meta, repo, "s-1", &exhausted).error, "quota_exhausted");

    DeviceQuotaStatus zero_quota;
    zero_quota.quota = 0; // 0 = 当日不承接
    EXPECT_EQ(check_session_admission(meta, repo, "s-1", &zero_quota).error, "quota_exhausted");
}
