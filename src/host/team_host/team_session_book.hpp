#pragma once

// 团队会话簿(host 层,spec capability: team-session):
//   - 会话发起准入:项目仓库身份必须已关联团队,并发会话数 ≤ 5;
//   - SessionMeta 的团队标记(host/member 角色写入本地会话元数据);
//   - 队员消息中继:host 是唯一写者,生成期间消息进入等待队列不丢失,
//     回合结束后按 FIFO 投递;投递出去的 human 消息标注发送者;
//   - 会话副本增量同步:host 每次写后把 jsonl 尾部与 meta 复制到本机
//     team 目录下的最近副本,供失联后手动接管使用(不是可写副本)。
//
// 网络不在本层:队列的投递目标是注入的 Deliverer(daemon 路由侧用它调
// LocalSessionClient::send_input),副本同步只读写本机文件。

#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>

#include "session/session_storage.hpp"
#include "team/team_meta.hpp"
#include "team/team_store.hpp"
#include "team_host/quota_ledger.hpp"

#include <nlohmann/json.hpp>

namespace acecode::team {

// ---- 准入 ----

struct AdmissionCheck {
    bool ok = false;
    std::string error; // repo_not_linked / too_many_concurrent_sessions / invalid_session
};

// 团队会话发起前的纯校验(不含落盘);start_session 里会再走同一口径。
// quota 非空时同时校验额度:本机当日额度用尽(含 0 额度)拒绝承接。
AdmissionCheck check_session_admission(const TeamMeta& meta, const RepoIdentity& repo,
                                       const std::string& session_id,
                                       const DeviceQuotaStatus* quota = nullptr);

// SessionMeta 团队标记:发起设备 role=host,查看/投递设备 role=member。
// 普通(非团队)会话两个字段保持为空。
void mark_team_session_meta(SessionMeta& meta, const std::string& team_id,
                            const std::string& role);

// ---- 会话事件通知 ----

// 5.4:会话事件载荷(kind: created / paused / resumed / host_changed / ended /
// removed)。各设备收到后更新团队会话列表与会话视图:paused 带原因,host_changed
// 带新 host。handler 直接 dump 下发,不经任何二次组装。
nlohmann::json team_session_event_json(std::string_view kind, const TeamSessionRef& ref);

// ---- 队员消息中继 ----

struct TeamInboundMessage {
    std::string team_id;
    std::string session_id;
    std::string sender_guid;
    std::string sender_name;  // 展示名;为空时标注回退到 sender_guid
    std::string text;
    std::int64_t received_at_ms = 0;
};

enum class RelayOutcome {
    Delivered, // host 空闲,已直接投递(触发 AI 回合)
    Queued,    // host 忙/暂停,进入等待队列(容量满时拒绝并丢弃,调用方须重试)
    Rejected,  // 消息本身非法(空文本/空会话),不入队
};

// host 侧消息中继。Deliverer 返回 false 表示 host 忙或投递失败:消息留在队首,
// 保序不丢失,由回合结束后的 flush() 重试。paused(host 暂停承接)时一律入队。
class TeamMessageRelay {
public:
    using Deliverer = std::function<bool(const TeamInboundMessage& message)>;

    explicit TeamMessageRelay(Deliverer deliver, std::size_t capacity = 256);

    // host 侧收到队员消息。host_busy 表示该会话正在生成中。
    RelayOutcome receive(TeamInboundMessage message, bool host_busy);

    // 回合结束/host 空闲时按 FIFO 投递;遇到一次失败即停止(后面的消息继续等,
    // 不跳过,保证会话流顺序)。返回本轮成功投递的条数。
    std::size_t flush();

    std::size_t pending() const;
    void set_paused(bool paused) { paused_ = paused; }
    bool paused() const { return paused_; }

    // 消息标注发送者:进 LLM 与 UI 都用 "<发送者>: <文本>" 的形式,
    // 让 host 端的 AI 和用户都能看出这条输入来自哪位队员。
    static std::string labeled_text(const TeamInboundMessage& message);

private:
    mutable std::mutex mu_;
    Deliverer deliver_;
    std::size_t capacity_;
    bool paused_ = false;
    std::deque<TeamInboundMessage> queue_;
};

// ---- 会话副本增量同步 ----

struct ReplicaSyncResult {
    bool ok = false;
    std::int64_t appended_bytes = 0; // 本次新写入副本的字节数(jsonl 增量)
    bool meta_rewritten = false;
    std::string error;
    explicit operator bool() const { return error.empty(); }
};

// host 写后会话数据落到 <data>/team/<tid>/sessions/<sid>/ 的最近副本:
//   - jsonl 只追加源文件比副本多出的尾部(副本比源大 = 数据被重写,整体覆盖);
//   - meta 内容有变化才原子覆盖;
//   - head_commit 非空时写入 <sid>.head(接管时 git 对齐的源 HEAD)。
// 副本仅用于失联接管(P5),追加中断留下的未完成尾行由接管侧的
// load_messages_with_diagnostics 容错。
ReplicaSyncResult sync_session_replica(const TeamStore& store, const std::string& session_id,
                                       const std::string& jsonl_path,
                                       const std::string& meta_path,
                                       const std::string& head_commit = {});

} // namespace acecode::team
