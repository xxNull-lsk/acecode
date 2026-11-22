#pragma once

// 手动会话接管(host 层,spec capability: team-session-handover)。
//
// 铁律:切换一律手动发起;失联/超额度/异常检测只置 paused 并通知,绝不自动
// 切换 host(见 TeamService::mark_lost_host_sessions / pause_sessions_for_quota)。
//
// 接管流程(在目标设备上执行,即本机成为新 host):
//   1) 资格:本机是成员、会话存在且未结束、本机当日额度未用尽;
//   2) 取数:源 host 在线 → 注入的 SourceDataFetcher 拉全量;失联 → 本机
//      最近副本(sessions/<sid>/);两路都拿到 jsonl+meta+源 HEAD;
//   3) git 对齐:git fetch + --ff-only pull 到源 HEAD;不自动 commit/push、
//      不改写未提交改动;任一步失败即中止(会话保持暂停);
//   4) 落数据 → 切 host(自己) → 状态恢复 running → 注入的 resume 回调恢复会话;
//   5) 广播由调用方负责(TeamService 已把变更写进 TeamMeta,经元数据同步生效)。

#include <functional>
#include <string>

#include "team_host/team_service.hpp"
#include "team_host/team_session_book.hpp"

namespace acecode::team {

// 从源设备或最近副本取得的会话数据。
struct SessionSourceData {
    std::string jsonl;        // 会话 jsonl 全量内容
    std::string meta;         // meta.json 全量内容
    std::string head_commit;  // 源设备会话所在仓库 HEAD(40/64 位 hex;空 = 未提供)
    std::string error;        // 拉取失败原因;非空时其余字段无效
    explicit operator bool() const { return error.empty(); }
};

// 源 host 在线时的取数实现(daemon 路由侧注入;测试用假实现)。
using SourceDataFetcher = std::function<SessionSourceData(const TeamMeta& meta,
                                                          const TeamSessionRef& ref)>;

// 数据落地后恢复会话(resume_session);返回 false = 恢复失败(接管失败)。
using SessionResumer = std::function<bool(const std::string& session_id)>;

// 目标设备(选择目标时)资格校验:成员 + 在线 + 当日额度有剩余。
// 远端额度用最后已知快照(member_quota_snapshot)判定。
struct TakeoverTargetCheck {
    bool ok = false;
    std::string error; // member_not_found / member_offline / quota_exhausted
};
TakeoverTargetCheck check_takeover_target(const TeamMeta& meta, const std::string& target_guid,
                                          std::int64_t now_seconds);

struct TakeoverOutcome {
    bool ok = false;
    std::string error;
    // member_not_found / session_not_found / session_ended / already_host /
    // quota_exhausted / data_unavailable / repo_align_failed / resume_failed /
    // write_failed
    TeamSessionRef ref; // 成功时为切换后的引用
    explicit operator bool() const { return ok; }
};

class TeamTakeover {
public:
    // fetch 仅在源 host 在线时被调用;resume 空则跳过恢复步骤(仍完成切换)。
    // account_quota_exhausted 非空时接管资格以账号级共享池为准(额度属于
    // 登录账号,账号下所有团队共享);空则退回成员记录口径(测试路径)。
    TeamTakeover(TeamService& service, SourceDataFetcher fetch, SessionResumer resume,
                 std::function<bool()> account_quota_exhausted = {});

    // 本机把 team_id/session_id 接管过来。cwd 为本机项目目录(git 对齐在它
    // 之下执行),project_dir 为会话文件目录(生产侧用
    // SessionStorage::get_project_dir(cwd);测试传 TempDir 隔离)。
    TakeoverOutcome take_over(const std::string& team_id, const std::string& session_id,
                              const std::string& cwd, const std::string& project_dir);

private:
    TeamService& service_;
    SourceDataFetcher fetch_;
    SessionResumer resume_;
    std::function<bool()> account_quota_exhausted_;
};

// git 对齐(5.3):fetch + --ff-only pull 到 head_commit。head 为空或已在该
// 提交上时为 no-op。--ff-only 保证不会自动产生 merge commit;落后场景直接
// 快进,分叉/冲突/网络失败返回错误串(空 = 成功),调用方中止接管。
std::string align_repo_to_head(const std::string& cwd, const std::string& head_commit);

} // namespace acecode::team
