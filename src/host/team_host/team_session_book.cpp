#include "team_host/team_session_book.hpp"

#include "team/repo_identity.hpp"
#include "team/team_store.hpp"
#include "utils/atomic_file.hpp"
#include "utils/utf8_path.hpp"

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

namespace acecode::team {

AdmissionCheck check_session_admission(const TeamMeta& meta, const RepoIdentity& repo,
                                       const std::string& session_id,
                                       const DeviceQuotaStatus* quota) {
    AdmissionCheck result;
    if (session_id.empty()) {
        result.error = "invalid_session";
        return result;
    }
    if (!team_matches_repo(meta, repo)) {
        result.error = "repo_not_linked";
        return result;
    }
    if (quota && quota->exhausted()) {
        result.error = "quota_exhausted";
        return result;
    }
    if (active_session_count(meta) >= kMaxConcurrentTeamSessions) {
        result.error = "too_many_concurrent_sessions";
        return result;
    }
    result.ok = true;
    return result;
}

void mark_team_session_meta(SessionMeta& meta, const std::string& team_id,
                            const std::string& role) {
    meta.team_id = team_id;
    meta.team_role = role;
}

// 5.4:会话事件载荷。kind 由调用方按语义给(created/paused/resumed/
// host_changed/ended/removed);字段直接来自引用,不做二次推导。
nlohmann::json team_session_event_json(std::string_view kind, const TeamSessionRef& ref) {
    nlohmann::json j;
    j["kind"] = kind;
    j["session_id"] = ref.session_id;
    if (!ref.title.empty()) j["title"] = ref.title;
    if (!ref.host_guid.empty()) j["host_guid"] = ref.host_guid;
    if (!ref.state.empty()) j["state"] = ref.state;
    if (!ref.pause_reason.empty()) j["pause_reason"] = ref.pause_reason;
    if (ref.updated_at != 0) j["updated_at"] = ref.updated_at;
    return j;
}

std::string TeamMessageRelay::labeled_text(const TeamInboundMessage& message) {
    const std::string& name =
        message.sender_name.empty() ? message.sender_guid : message.sender_name;
    return name + ": " + message.text;
}

TeamMessageRelay::TeamMessageRelay(Deliverer deliver, std::size_t capacity)
    : deliver_(std::move(deliver)), capacity_(capacity) {}

RelayOutcome TeamMessageRelay::receive(TeamInboundMessage message, bool host_busy) {
    if (message.session_id.empty() || message.text.empty()) return RelayOutcome::Rejected;
    std::lock_guard<std::mutex> lock(mu_);
    // host 忙或暂停承接时一律排队;空闲才尝试直接投递。投递失败同样入队,
    // 由 flush() 重试 —— 消息绝不因为一次网络/会话状态波动而丢失。
    if (host_busy || paused_) {
        if (queue_.size() >= capacity_) return RelayOutcome::Queued;
        queue_.push_back(std::move(message));
        return RelayOutcome::Queued;
    }
    if (deliver_ && deliver_(message)) return RelayOutcome::Delivered;
    if (queue_.size() >= capacity_) return RelayOutcome::Queued;
    queue_.push_back(std::move(message));
    return RelayOutcome::Queued;
}

std::size_t TeamMessageRelay::flush() {
    std::size_t delivered = 0;
    std::lock_guard<std::mutex> lock(mu_);
    if (paused_) return 0;
    while (!queue_.empty()) {
        // 失败即停止:队首消息留在队列里,顺序不乱,不跳过。
        if (!deliver_ || !deliver_(queue_.front())) break;
        queue_.pop_front();
        ++delivered;
    }
    return delivered;
}

std::size_t TeamMessageRelay::pending() const {
    std::lock_guard<std::mutex> lock(mu_);
    return queue_.size();
}

ReplicaSyncResult sync_session_replica(const TeamStore& store, const std::string& session_id,
                                       const std::string& jsonl_path,
                                       const std::string& meta_path,
                                       const std::string& head_commit) {
    ReplicaSyncResult result;
    if (session_id.empty()) {
        result.error = "invalid_session";
        return result;
    }
    std::error_code ec;
    const fs::path replica_root = store.replica_dir(session_id);
    fs::create_directories(replica_root, ec);
    if (ec) {
        result.error = "replica_dir_create_failed";
        return result;
    }

    // jsonl 增量:副本已有 N 字节则只追加源文件 [N, size) 区间。
    const fs::path replica_jsonl = replica_root / (session_id + ".jsonl");
    std::ifstream source(jsonl_path, std::ios::binary);
    if (!source) {
        result.error = "source_unreadable";
        return result;
    }
    source.seekg(0, std::ios::end);
    const std::streamoff source_size = source.tellg();
    std::streamoff replica_size = 0;
    if (fs::exists(replica_jsonl, ec)) {
        replica_size = static_cast<std::streamoff>(fs::file_size(replica_jsonl, ec));
        if (ec) replica_size = 0;
    }
    const bool append_mode = replica_size > 0 && replica_size <= source_size;
    const std::ios::openmode mode =
        append_mode ? std::ios::binary | std::ios::in | std::ios::out
                    : std::ios::binary | std::ios::out | std::ios::trunc;
    std::fstream replica(replica_jsonl, mode);
    if (!replica) {
        result.error = "replica_write_failed";
        return result;
    }
    if (append_mode) replica.seekp(replica_size);
    // 增量追加从副本已有长度处读源;整体重写从 0 读源
    //(前面为了取长度 seek 过文件尾,必须先归位)。
    source.clear();
    source.seekg(append_mode ? replica_size : std::streamoff(0));
    std::array<char, 64 * 1024> buffer{};
    std::int64_t appended = 0;
    while (source.read(buffer.data(), static_cast<std::streamsize>(buffer.size())) ||
           source.gcount() > 0) {
        replica.write(buffer.data(), source.gcount());
        if (!replica) {
            result.error = "replica_write_failed";
            return result;
        }
        appended += source.gcount();
        if (source.eof()) break;
    }
    if (appended < 0) appended = 0;
    result.appended_bytes = appended;

    // meta:内容有变化才覆盖(原子替换,避免接管读到半份)。
    if (!meta_path.empty()) {
        std::ifstream source_meta(meta_path, std::ios::binary);
        std::ostringstream current;
        current << source_meta.rdbuf();
        const std::string content = current.str();
        const fs::path replica_meta = replica_root / (session_id + ".meta.json");
        bool same = false;
        if (fs::exists(replica_meta, ec)) {
            std::ifstream replica_in(replica_meta, std::ios::binary);
            std::ostringstream existing;
            existing << replica_in.rdbuf();
            same = existing.str() == content;
        }
        if (!same && atomic_write_file(path_to_utf8(replica_meta), content)) {
            result.meta_rewritten = true;
        }
    }

    // 源设备 HEAD:接管时 git 对齐的基线;内容变化才覆盖。
    if (!head_commit.empty()) {
        const fs::path head_path = replica_root / (session_id + ".head");
        bool head_same = false;
        if (fs::exists(head_path, ec)) {
            std::error_code ec2;
            head_same = fs::file_size(head_path, ec2) == head_commit.size();
        }
        if (!head_same &&
            !atomic_write_file(path_to_utf8(head_path), head_commit)) {
            result.error = "head_write_failed";
            return result;
        }
    }
    result.ok = true;
    return result;
}

} // namespace acecode::team
