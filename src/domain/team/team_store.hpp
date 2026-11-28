#pragma once

// 团队本机存储(spec capability: team-state-sync):一个团队一个目录
// <data_dir>/team/<team_id>/,内含:
//   team.json          — TeamMeta(last-writer-wins 的落盘快照)
//   local.json         — 本机团队状态(工作目录等;不随元数据同步,各设备路径不同)
//   invite_ledger.json — 本机签发的一次性邀请码记账(15 分钟 TTL + 单次消费)
//   sessions/<sid>/    — 会话接管时的数据副本落点
//
// 目录定位只接收 data_dir(生产方传 resolve_data_dir(),测试传 TempDir),
// 本模块不感知 paths 解析。损坏的 JSON 按"读不到"处理并返回错误串 —
// 对邀请码账本这是 fail-closed:读不出来就当作无效凭据,不能放开。

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "team/team_invite.hpp"
#include "team/team_meta.hpp"

namespace acecode::team {

struct InviteLedgerRecord {
    std::string token;        // 一次性 token,凭据主体
    std::int64_t issued_at = 0;
    std::int64_t expires_at = 0;   // 签发时间 + 15 分钟
    // 消费状态:consumed_at 为 0 表示未消费;>0 后与过期共同构成双重失效。
    std::int64_t consumed_at = 0;
    std::string consumed_by;  // 消费方设备 GUID
};

struct InviteLedgerLoad {
    // has_file=false 且 error 非空 → 文件损坏,fail-closed。
    bool has_file = false;
    std::string error;
    std::vector<InviteLedgerRecord> records;
};

// 本机团队状态:各设备本地路径不同,绝不进 TeamMeta 同步链路。
struct TeamLocalState {
    std::string work_dir;         // 本机团队工作目录;空 = 未设置
    std::int64_t archived_at = 0; // 退出后的归档时间(秒);0 = 活跃
};

class TeamStore {
public:
    TeamStore(std::string data_dir, std::string team_id);

    // team.json:原子替换写;load 对文件缺失返回 nullopt,对损坏 JSON 也返回
    // nullopt 但不抛异常(同步层靠 updated_at 较新方覆盖,旧快照可再拉)。
    bool save_meta(const TeamMeta& meta) const;
    std::optional<TeamMeta> load_meta() const;

    // local.json:本机团队状态。损坏 JSON 按"未设置"处理(只影响展示与
    // 缺省目录,不涉及凭据,无需 fail-closed);缺失文件同样返回空状态。
    bool save_local_state(const TeamLocalState& state) const;
    TeamLocalState load_local_state() const;

    // 邀请码账本:整档原子替换写;查找按 token 精确匹配。
    InviteLedgerLoad load_invites() const;
    bool save_invites(const std::vector<InviteLedgerRecord>& records) const;
    std::optional<InviteLedgerRecord> find_invite(const std::string& token) const;
    // 单次消费:幂等保护(已消费返回 false),写入消费时间与消费方。
    bool consume_invite(const std::string& token, const std::string& consumer_guid,
                        std::int64_t now) const;

    // 会话副本目录:sessions/<session_id>/;仅创建目录并返回路径,写内容是
    // 接管流程(P5)的职责。
    std::filesystem::path replica_dir(const std::string& session_id) const;
    std::filesystem::path root_dir() const { return root_; }

private:
    std::filesystem::path root_;
    std::filesystem::path local_path_;
    std::filesystem::path invites_path_;
};

// 邀请码双重失效判定(过期与已消费,先到为准)。now 由调用方传入。
// TTL 常量 kInviteTtlSeconds 定义在 team_invite.hpp。
inline bool invite_usable(const InviteLedgerRecord& record, std::int64_t now) {
    return now <= record.expires_at && record.consumed_at == 0;
}

} // namespace acecode::team
