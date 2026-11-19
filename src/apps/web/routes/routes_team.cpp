#include "web/server_impl.hpp"

#include "session/session_storage.hpp"
#include "session/session_usage_ledger.hpp"
#include "share/invite_link.hpp"
#include "team/team_invite.hpp"
#include "team/repo_identity.hpp"
#include "team_host/team_client.hpp"
#include "team_host/team_takeover.hpp"
#include "utils/machine_identity.hpp"
#include "utils/paths.hpp"
#include "utils/text.hpp"
#include "utils/url_encoding.hpp"
#include "worktree/worktree_manager.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <sstream>

namespace fs = std::filesystem;

namespace acecode::web {
using nlohmann::json;
namespace {

crow::response team_response(const crow::request& req, int status, std::string body = {}) {
    crow::response response(status, std::move(body));
    add_loopback_cors_headers(req, response);
    response.set_header("Content-Type", "application/json");
    response.set_header("Cache-Control", "no-store");
    return response;
}

crow::response team_error(const crow::request& req, int status, const std::string& code) {
    return team_response(req, status, json{{"error", code}}.dump());
}

// host 上本机会话是否正在生成(消息中继的 busy 判定)。
bool local_session_busy(SessionClient* client, const std::string& session_id) {
    if (!client) return false;
    for (const auto& info : client->list_sessions())
        if (info.id == session_id) return info.busy;
    return false;
}

std::string read_file_or_empty(const std::string& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input.is_open()) return {};
    std::ostringstream buffer;
    buffer << input.rdbuf();
    return buffer.str();
}

// 本机定位查找:在本机 projects/ 各工作区目录下找该会话的 meta。
// 找到返回 cwd 与(非 no_workspace 会话的)workspace_hash;找不到说明
// 该团队会话由其他设备托管,保持稀疏投影。
struct LocalSessionLocation {
    std::string cwd;
    std::string workspace_hash;
};
std::optional<LocalSessionLocation> find_local_session_location(
    const std::string& session_id) {
    if (session_id.empty()) return std::nullopt;
    std::error_code ec;
    const fs::path projects = fs::path(get_acecode_dir()) / "projects";
    if (!fs::exists(projects, ec)) return std::nullopt;
    for (const auto& entry : fs::directory_iterator(projects, ec)) {
        if (!entry.is_directory(ec)) continue;
        const fs::path meta_path = entry.path() / (session_id + ".meta.json");
        if (!fs::exists(meta_path, ec)) continue;
        const auto meta = SessionStorage::read_meta(path_to_utf8(meta_path));
        LocalSessionLocation location;
        location.cwd = meta.cwd;
        if (!meta.no_workspace && !meta.cwd.empty())
            location.workspace_hash = SessionStorage::compute_project_hash(meta.cwd);
        return location;
    }
    return std::nullopt;
}

// 团队会话引用公开视图 + 本机定位投影(本机存在会话数据时补 cwd/hash)。
nlohmann::json team_session_ref_with_local_info(const team::TeamSessionRef& ref) {
    auto j = web::team_session_ref_public_json(ref);
    if (const auto location = find_local_session_location(ref.session_id)) {
        const auto local = web::team_session_local_info_json(location->cwd,
                                                            location->workspace_hash);
        for (auto it = local.begin(); it != local.end(); ++it) j[it.key()] = it.value();
    }
    return j;
}

// 团队状态 JSON 的会话列表逐条合并本机定位(仅本机 UI 可见,见下)。
void merge_local_session_info_into_state(json& state) {
    auto sessions = state.find("sessions");
    if (sessions == state.end() || !sessions->is_array()) return;
    for (auto& ref : *sessions) {
        const std::string session_id = ref.value("session_id", std::string{});
        const auto location = find_local_session_location(session_id);
        if (!location) continue;
        const auto local = web::team_session_local_info_json(location->cwd,
                                                            location->workspace_hash);
        for (auto it = local.begin(); it != local.end(); ++it) ref[it.key()] = it.value();
    }
}

// 本机对团队的可达地址(LAN 地址 + 代理监听端口):见 Impl::team_self_address()。

} // namespace

// =====================================================================
// 团队运行时(add-decentralized-team)
// =====================================================================

void WebServer::Impl::initialize_team() {
    try {
        const auto directory = path_to_utf8(path_from_utf8(projects_dir()).parent_path());
        team_service = std::make_shared<team::TeamService>(
            directory,
            team::DeviceIdentity{deps.guid, utils::current_account_name(), std::string{}});
    } catch (const std::exception& error) {
        LOG_WARN(std::string("[team] init failed: ") + error.what());
    }
}

void WebServer::Impl::stop_team() {
    std::vector<crow::websocket::connection*> connections;
    std::vector<std::pair<std::string, std::uint64_t>> subscriptions;
    {
        std::lock_guard<std::mutex> lock(team_mu);
        for (auto& entry : team_connections) connections.push_back(entry.first);
        team_connections.clear();
        pending_team_connections.clear();
        for (auto& entry : team_session_subs) subscriptions.push_back(entry);
        team_session_subs.clear();
        team_relays.clear();
    }
    for (const auto& [session_id, sub] : subscriptions) {
        if (deps.session_client) deps.session_client->unsubscribe(session_id, sub);
    }
    for (auto* connection : connections) connection->close("server_stopping");
}

// 心跳式自地址维护:把自己的 LAN 地址与 last_seen 写进成员记录。
// 幂等(地址未变且 30 秒内已刷新时不写盘);别的设备靠它转发消息/接管取数。
std::string WebServer::Impl::team_self_address() {
    int port = 0;
    std::string bind_hint = "0.0.0.0";
    if (deps.remote_web_proxy) {
        const auto proxy = deps.remote_web_proxy->status();
        if (proxy.running) {
            port = proxy.port;
            if (proxy.ipv6) bind_hint.clear();
        }
    }
    if (port <= 0) port = effective_port;
    std::string address;
    try {
        const auto hosts = rank_remote_web_hosts(
            deps.remote_web_hosts ? deps.remote_web_hosts() : discover_remote_web_hosts(),
            {}, bind_hint,
            deps.remote_web_computer_name ? deps.remote_web_computer_name()
                                          : discover_remote_web_computer_name());
        if (!hosts.empty()) address = hosts.front();
    } catch (const std::exception&) {}
    if (address.empty()) address = "127.0.0.1";
    return address + ":" + std::to_string(port);
}

void WebServer::Impl::refresh_team_self_address(const std::string& team_id) {
    if (!team_service) return;
    // 有团队活动时先确保 LAN 代理监听在跑(与分享/对端共用监听;daemon 重启后
    // 代理子进程不在,必须在这里拉起,否则其他设备按成员地址连不上)。
    reconcile_share_listener();
    const auto meta = team_service->load_team(team_id);
    if (!meta) {
        LOG_WARN("[team] refresh: no meta " + team_id);
        return;
    }
    const team::TeamMember* self = team::find_member(*meta, team_service->self().guid);
    if (!self) {
        LOG_WARN("[team] refresh: self not member of " + team_id);
        return;
    }
    const std::string address = team_self_address();
    const std::int64_t now = team_service->now();
    if (self->address != address || now - self->last_seen > 30) {
        LOG_INFO("[team] refresh self address -> " + address);
        (void)team_service->touch_member(team_id, team_service->self().guid, address);
    }
}

bool WebServer::Impl::deliver_team_message(const std::string& session_id,
                                           const team::TeamInboundMessage& message) {
    if (!deps.session_client) return false;
    const std::string labeled = team::TeamMessageRelay::labeled_text(message);
    if (deps.session_client->send_input(session_id, labeled, labeled)) return true;
    // 会话不在 registry(daemon 重启过):先从磁盘恢复再投递。
    if (!deps.session_client->resume_session(session_id)) return false;
    return deps.session_client->send_input(session_id, labeled, labeled);
}

std::shared_ptr<team::TeamMessageRelay> WebServer::Impl::team_relay_for(
    const std::string& session_id) {
    std::lock_guard<std::mutex> lock(team_mu);
    auto& slot = team_relays[session_id];
    if (!slot) {
        slot = std::make_shared<team::TeamMessageRelay>(
            [this, session_id](const team::TeamInboundMessage& inbound) {
                return deliver_team_message(session_id, inbound);
            });
    }
    return slot;
}

std::optional<crow::response> WebServer::Impl::require_team_auth(
    const crow::request& req, const std::string& team_id) {
    if (!team_service) return team_error(req, 503, "team_unavailable");
    if (team_id.empty()) return team_error(req, 400, "invalid_team");
    const auto header = req.get_header_value("X-ACECode-Team-Token");
    if (header.empty() || !team_service->authorize(team_id, header))
        return team_error(req, 401, "invalid_team_credential");
    return std::nullopt;
}

void WebServer::Impl::broadcast_team_state(const std::string& team_id) {    if (!team_service) return;
    const auto meta = team_service->load_team(team_id);
    if (!meta) return;
    const team::QuotaLedger ledger(team_service->data_dir(), team_id);
    auto state = web::team_state_json(*meta, team_service->self().guid, ledger,
                                      team_service->now());
    // 本机定位投影只发给本机 UI(daemon token 连接):各设备本地路径不同,
    // 本机路径对其他设备没有意义,也不该随状态广播出去。
    json plain_state = state;
    merge_local_session_info_into_state(state);
    const std::string local_text =
        json{{"type", "team_state"}, {"team_id", team_id}, {"state", state}}.dump();
    const std::string plain_text =
        json{{"type", "team_state"}, {"team_id", team_id}, {"state", plain_state}}.dump();
    std::vector<crow::websocket::connection*> targets;
    {
        std::lock_guard<std::mutex> lock(team_mu);
        for (auto& [connection, connection_state] : team_connections)
            if (connection_state->team_id == team_id) targets.push_back(connection);
    }
    for (auto* connection : targets) {
        const auto it = team_connections.find(connection);
        // onaccept 把本机 UI 记为自身 GUID,队员连接记为 "remote-member"。
        const bool local_ui = it != team_connections.end() &&
            (it->second->device_guid.empty() ||
             it->second->device_guid == team_service->self().guid);
        connection->send_text(local_ui ? local_text : plain_text);
    }
}

void WebServer::Impl::broadcast_team_session_event(const std::string& team_id,
                                                   std::string_view kind,
                                                   const team::TeamSessionRef& ref) {
    const std::string text =
        json{{"type", "team_session_event"},
             {"team_id", team_id},
             {"event", team::team_session_event_json(kind, ref)}}.dump();
    std::vector<crow::websocket::connection*> targets;
    {
        std::lock_guard<std::mutex> lock(team_mu);
        for (auto& [connection, connection_state] : team_connections)
            if (connection_state->team_id == team_id) targets.push_back(connection);
    }
    for (auto* connection : targets) connection->send_text(text);
}

// 会话事件转发:团队 WS 下发 + 用量记账 + 等待队列投递 + 副本同步(3.5)。
void WebServer::Impl::forward_team_session_event(const std::string& team_id,
                                                 const std::string& session_id,
                                                 const SessionEvent& event) {
    if (!team_service) return;
    const std::int64_t now_ms = usage_now_unix_ms();

    // 额度记账:Usage 事件按 UTC 日入账,记在 host(本机)头上。
    if (event.kind == SessionEventKind::Usage) {
        try {
            const auto& payload = event.payload;
            acecode::TokenUsage usage;
            usage.prompt_tokens = payload.value("prompt_tokens", 0);
            usage.completion_tokens = payload.value("completion_tokens", 0);
            usage.total_tokens = payload.value("total_tokens",
                usage.prompt_tokens + usage.completion_tokens);
            usage.cache_read_tokens = payload.value("cache_read_tokens", 0);
            usage.cache_write_tokens = payload.value("cache_write_tokens", 0);
            usage.reasoning_tokens = payload.value("reasoning_tokens", 0);
            usage.has_data = true;
            if (usage.total_tokens > 0) {
                team::QuotaLedger ledger(team_service->data_dir(), team_id);
                acecode::SessionMeta team_flag;
                team_flag.id = session_id;
                team_flag.team_id = team_id;
                std::string error;
                if (ledger.record(team_flag, usage, now_ms, &error)) {
                    (void)team_service->report_quota_usage(team_id, ledger.used_today(now_ms),
                                                           now_ms / team::kQuotaDayMs);
                }
            }
        } catch (const std::exception&) {}
    }

    // 队员消息中继:回合结束(busy=false)时投递等待队列。
    if (event.kind == SessionEventKind::BusyChanged) {
        std::shared_ptr<team::TeamMessageRelay> relay;
        {
            std::lock_guard<std::mutex> lock(team_mu);
            if (auto it = team_relays.find(session_id); it != team_relays.end()) relay = it->second;
        }
        if (relay && !event.payload.value("busy", true)) relay->flush();
    }

    // 副本增量同步(3.5):每条持久事件后追加 jsonl 尾部;回合结束补 meta+HEAD。
    if (event.kind == SessionEventKind::Message ||
        event.kind == SessionEventKind::TranscriptReplace ||
        event.kind == SessionEventKind::Done) {
        const std::string project_dir = SessionStorage::get_project_dir(deps.cwd);
        const std::string jsonl_path = project_dir + "/" + session_id + ".jsonl";
        const std::string meta_path = project_dir + "/" + session_id + ".meta.json";
        std::string head_commit;
        if (event.kind == SessionEventKind::Done) {
            const auto head = worktree::run_git({"rev-parse", "HEAD"}, deps.cwd, 10000);
            if (head.ok()) head_commit = utils::trim_ascii_copy(head.out);
        }
        const auto synced = team::sync_session_replica(
            team::TeamStore(team_service->data_dir(), team_id), session_id,
            jsonl_path, meta_path, head_commit);
        (void)synced;
    }

    nlohmann::json payload = {
        {"type", "session_event"},
        {"team_id", team_id},
        {"session_id", session_id},
        {"seq", event.seq},
        {"timestamp_ms", event.timestamp_ms},
        {"event_kind", to_string(event.kind)},
        {"data", event.payload},
    };
    const std::string text = payload.dump();
    std::vector<crow::websocket::connection*> targets;
    {
        std::lock_guard<std::mutex> lock(team_mu);
        for (auto& [connection, connection_state] : team_connections)
            if (connection_state->team_id == team_id) targets.push_back(connection);
    }
    for (auto* connection : targets) connection->send_text(text);
}

// 为本机 host 的团队会话建立事件订阅(幂等)。
void WebServer::Impl::ensure_team_session_subscriptions(const std::string& team_id) {
    if (!team_service || !deps.session_client) return;
    const auto meta = team_service->load_team(team_id);
    if (!meta) return;
    std::vector<std::string> to_subscribe;
    {
        std::lock_guard<std::mutex> lock(team_mu);
        for (const auto& ref : meta->sessions) {
            if (ref.state == team::kSessionStateEnded) continue;
            if (team_session_subs.count(ref.session_id)) continue;
            to_subscribe.push_back(ref.session_id);
        }
    }
    for (const auto& session_id : to_subscribe) {
        auto listener = [this, team_id, session_id](const SessionEvent& event) {
            forward_team_session_event(team_id, session_id, event);
        };
        const auto sub = deps.session_client->subscribe(session_id, listener);
        std::lock_guard<std::mutex> lock(team_mu);
        team_session_subs[session_id] = sub;
    }
}

// =====================================================================
// REST — 本机管理(require_auth: daemon token)
// =====================================================================

void WebServer::Impl::register_team() {
    // 创建团队(创建者即管理员):名称 + 工作目录 + 可选仓库 URL。
    // 工作目录是 git 仓库时从目录解析仓库身份;否则必须给 URL。
    // 身份解析在创建之前:失败则团队不创建,前端按错误码提示补救。
    CROW_ROUTE(app, "/api/team").methods(crow::HTTPMethod::POST)
    ([this](const crow::request& req) {
        if (auto denied = require_auth(req)) return std::move(*denied);
        if (!team_service) return team_error(req, 503, "team_unavailable");
        const auto body = json::parse(req.body, nullptr, false);
        if (const std::string invalid = web::validate_team_create_body(body); !invalid.empty())
            return team_error(req, 400, invalid);
        const std::string name = body["name"].get<std::string>();
        const std::string cwd = body["cwd"].get<std::string>();
        const std::string repo_url = body.contains("repo_url") && body["repo_url"].is_string()
            ? body["repo_url"].get<std::string>() : std::string{};
        const auto repo = repo_url.empty()
            ? team::resolve_repo_identity(cwd)
            : team::repo_identity_from_url(repo_url);
        if (!repo) {
            const std::string code = repo.error == "not_git_repo"
                ? "workdir_not_git_repo" : repo.error;
            return team_error(req, 400, code);
        }

        std::string error;
        const auto created = team_service->create_team(name, &error);
        if (!created) return team_error(req, 500, error.empty() ? "create_failed" : error);
        const auto registered = team_service->register_repo(created->team_id, repo.identity);
        if (!registered) return team_error(req, 500, registered.error);
        // 记住本机团队工作目录(每设备本地状态,不随元数据同步)。
        team::TeamLocalState local;
        local.work_dir = cwd;
        team_service->store_for(created->team_id).save_local_state(local);
        broadcast_team_state(created->team_id);
        const auto meta = team_service->load_team(created->team_id);
        return team_response(req, 200, web::team_meta_public_json(
            meta ? *meta : *created).dump());
    });

    // 团队列表。
    CROW_ROUTE(app, "/api/team").methods(crow::HTTPMethod::GET)
    ([this](const crow::request& req) {
        if (auto denied = require_auth(req)) return std::move(*denied);
        if (!team_service) return team_error(req, 503, "team_unavailable");
        for (const auto& meta : team_service->load_teams())
            refresh_team_self_address(meta.team_id);
        auto result = json::array();
        for (const auto& meta : team_service->load_teams()) {
            team::QuotaLedger ledger(team_service->data_dir(), meta.team_id);
            auto state = web::team_state_json(meta, team_service->self().guid, ledger,
                                              team_service->now());
            // 本机 UI 的 REST 响应:本机存在的会话补 cwd/workspace_hash,
            // 供侧边栏把团队会话当普通会话打开。
            merge_local_session_info_into_state(state);
            result.push_back(std::move(state));
        }
        return team_response(req, 200, json{{"teams", result}}.dump());
    });

    // 单个团队状态。
    CROW_ROUTE(app, "/api/team/<string>").methods(crow::HTTPMethod::GET)
    ([this](const crow::request& req, const std::string& team_id) {
        if (auto denied = require_auth(req)) return std::move(*denied);
        if (!team_service) return team_error(req, 503, "team_unavailable");
        refresh_team_self_address(team_id);
        const auto meta = team_service->load_team(team_id);
        if (!meta) return team_error(req, 404, "team_not_found");
        team::QuotaLedger ledger(team_service->data_dir(), team_id);
        auto state = web::team_state_json(*meta, team_service->self().guid, ledger,
                                          team_service->now());
        merge_local_session_info_into_state(state);
        return team_response(req, 200, state.dump());
    });

    // 签发一次性邀请码(仅管理员)。返回 acecode://team/ 链接文本。
    CROW_ROUTE(app, "/api/team/<string>/invite").methods(crow::HTTPMethod::POST)
    ([this](const crow::request& req, const std::string& team_id) {
        if (auto denied = require_auth(req)) return std::move(*denied);
        if (!team_service) return team_error(req, 503, "team_unavailable");
        const auto body = json::parse(req.body, nullptr, false);
        std::vector<std::string> addresses;
        if (body.is_object() && body.contains("addresses") && body["addresses"].is_array())
            for (const auto& address : body["addresses"])
                if (address.is_string()) addresses.push_back(address.get<std::string>());
        if (addresses.empty()) {
            addresses = rank_remote_web_hosts(
                deps.remote_web_hosts ? deps.remote_web_hosts() : discover_remote_web_hosts());
        }
        if (addresses.empty()) return team_error(req, 502, "no_lan_address");
        int port = effective_port;
        if (body.is_object() && body.contains("port") && body["port"].is_number_integer())
            port = body["port"].get<int>();
        if (port <= 0) return team_error(req, 502, "no_daemon_port");

        const auto issued_result = team_service->issue_invite(team_id, addresses, port);
        if (!issued_result) return team_error(req, issued_result.error == "not_admin" ? 403 : 500, issued_result.error);
        auto issued = issued_result;
        // 邀请码携带团队仓库身份(remote 类,最多 4 条):加入方在发起任何
        // 网络连接之前用它预检本机工作目录,匹配失败不烧邀请码。
        issued.invite.repos.clear();
        if (const auto meta = team_service->load_team(team_id)) {
            for (const auto& repo : meta->repos) {
                if (repo.kind != "remote" || repo.value.empty()) continue;
                if (issued.invite.repos.size() >= 4) break;
                issued.invite.repos.push_back(repo);
            }
        }
        try {
            const auto link = team::encode_team_invite_link(issued.invite, team_service->now());
            return team_response(req, 200, json{{"link", link},
                                                {"expires_at", issued.invite.expires_at}}.dump());
        } catch (const std::exception&) {
            return team_error(req, 500, "encode_failed");
        }
    });

    // 通过邀请链接加入(从邀请者拉全量快照)。必须指定本机工作目录:
    // 邀请码携带团队仓库身份(remote 类)时,先在本机解析工作目录身份并
    // 预检匹配 — 匹配失败不发起连接、不消费一次性邀请码。加入成功后
    // 登记本机仓库身份并记住工作目录(每设备本地状态)。
    CROW_ROUTE(app, "/api/team/join").methods(crow::HTTPMethod::POST)
    ([this](const crow::request& req) {
        if (auto denied = require_auth(req)) return std::move(*denied);
        if (!team_service) return team_error(req, 503, "team_unavailable");
        const auto body = json::parse(req.body, nullptr, false);
        if (const std::string invalid = web::validate_team_join_body(body); !invalid.empty())
            return team_error(req, 400, invalid);
        const auto invite = team::decode_team_invite_link(body["link"].get<std::string>(),
                                                          team_service->now());
        if (!invite) return team_error(req, 400, invite.error);
        const std::string cwd = body["cwd"].get<std::string>();

        // 预检(网络连接之前):fetch 快照本身就会在邀请者侧消费邀请码并
        // 登记成员,所以匹配必须前置,避免烧掉邀请码并留下幽灵成员。
        std::optional<team::RepoIdentity> local_repo;
        if (const auto resolved = team::resolve_repo_identity(cwd))
            local_repo = resolved.identity;
        if (!invite.value.repos.empty()) {
            if (!local_repo) return team_error(req, 400, "workdir_not_git_repo");
            const bool matched = std::any_of(invite.value.repos.begin(), invite.value.repos.end(),
                [&](const team::RepoIdentity& candidate) {
                    return candidate.kind == local_repo->kind &&
                           candidate.value == local_repo->value;
                });
            if (!matched) return team_error(req, 400, "repo_mismatch");
        }

        const auto fetcher = team::make_network_join_fetcher(team_service->self());
        const auto joined = team_service->join_team(invite.value, fetcher);
        if (!joined) return team_error(req, 502, joined.error);

        // 复核 + 登记。快照在邀请码签发后可能补过仓库(极小概率):有仓库
        // 却不匹配时删除本机刚落盘的数据再报错,不留半加入状态。
        const auto meta = team_service->load_team(invite.value.team_id);
        if (!meta) return team_error(req, 500, "team_not_found");
        if (!meta->repos.empty()) {
            if (!local_repo || !team::team_matches_repo(*meta, *local_repo)) {
                std::error_code ec;
                std::filesystem::remove_all(
                    team_service->store_for(invite.value.team_id).root_dir(), ec);
                return team_error(req, 400,
                    local_repo ? "repo_mismatch" : "workdir_not_git_repo");
            }
        } else if (local_repo) {
            (void)team_service->register_repo(invite.value.team_id, *local_repo);
        }
        team::TeamLocalState local;
        local.work_dir = cwd;
        team_service->store_for(invite.value.team_id).save_local_state(local);
        broadcast_team_state(invite.value.team_id);
        return team_response(req, 200, json{{"team_id", invite.value.team_id}}.dump());
    });

    // 角色管理(仅管理员)。
    CROW_ROUTE(app, "/api/team/<string>/role").methods(crow::HTTPMethod::POST)
    ([this](const crow::request& req, const std::string& team_id) {
        if (auto denied = require_auth(req)) return std::move(*denied);
        if (!team_service) return team_error(req, 503, "team_unavailable");
        const auto body = json::parse(req.body, nullptr, false);
        if (!body.is_object() || !body.contains("guid") || !body["guid"].is_string() ||
            !body.contains("role") || !body["role"].is_string())
            return team_error(req, 400, "invalid_body");
        const auto result = team_service->set_role(team_id, team_service->self().guid,
            body["guid"].get<std::string>(), body["role"].get<std::string>());
        if (!result) return team_error(req, result.error == "not_admin" ? 403 : 400, result.error);
        broadcast_team_state(team_id);
        return team_response(req, 200, "{\"ok\":true}");
    });

    // 每日额度设置(成员改自己的;0 = 当日不承接)。即时保存,无保存按钮。
    CROW_ROUTE(app, "/api/team/<string>/quota").methods(crow::HTTPMethod::POST)
    ([this](const crow::request& req, const std::string& team_id) {
        if (auto denied = require_auth(req)) return std::move(*denied);
        if (!team_service) return team_error(req, 503, "team_unavailable");
        const auto body = json::parse(req.body, nullptr, false);
        if (!body.is_object() || !body.contains("daily_token_quota") ||
            !body["daily_token_quota"].is_number_integer())
            return team_error(req, 400, "invalid_body");
        const auto result = team_service->set_daily_quota(team_id, team_service->self().guid,
            body["daily_token_quota"].get<std::int64_t>());
        if (!result) return team_error(req, 400, result.error);
        broadcast_team_state(team_id);
        return team_response(req, 200, "{\"ok\":true}");
    });

    // 从指定在线队员同步团队状态(拉取合并 + 推送本机合并)。
    CROW_ROUTE(app, "/api/team/<string>/sync").methods(crow::HTTPMethod::POST)
    ([this](const crow::request& req, const std::string& team_id) {
        if (auto denied = require_auth(req)) return std::move(*denied);
        if (!team_service) return team_error(req, 503, "team_unavailable");
        const auto meta = team_service->load_team(team_id);
        if (!meta) return team_error(req, 404, "team_not_found");
        const auto body = json::parse(req.body, nullptr, false);
        if (!body.is_object() || !body.contains("address") || !body["address"].is_string() ||
            !body.contains("port") || !body["port"].is_number_integer())
            return team_error(req, 400, "invalid_body");
        const std::string address = body["address"].get<std::string>();
        const int port = body["port"].get<int>();
        const auto credential = team::derive_team_token(meta->invite_seed, team_id);
        if (credential.empty()) return team_error(req, 500, "credential_unavailable");

        auto pulled = team::pull_team_state(address, port, team_id, credential);
        if (!pulled) return team_error(req, 502, pulled.error);
        std::string error;
        const auto merged = team_service->merge_remote(team_id, pulled.state, &error);
        if (!merged) return team_error(req, 502, error.empty() ? "sync_failed" : error);
        // 推送本机合并结果(对端按 LWW 合并)
        (void)team::team_request(address, port, "/api/team/v1/state/push?team_id=" +
            utils::percent_encode_query_component(team_id), credential,
            json{{"snapshot", team::team_meta_to_json(*merged)}});
        broadcast_team_state(team_id);
        return team_response(req, 200, "{\"ok\":true}");
    });

    // 关联项目:成员设备把自己的仓库身份登记进团队(repo_identities 并集,
    // 经元数据同步到各设备)。发起会话的前置条件。
    CROW_ROUTE(app, "/api/team/<string>/repos").methods(crow::HTTPMethod::POST)
    ([this](const crow::request& req, const std::string& team_id) {
        if (auto denied = require_auth(req)) return std::move(*denied);
        if (!team_service) return team_error(req, 503, "team_unavailable");
        const auto body = json::parse(req.body, nullptr, false);
        const std::string cwd = body.is_object() && body["cwd"].is_string() &&
            !body["cwd"].get<std::string>().empty() ? body["cwd"].get<std::string>() : deps.cwd;
        const auto repo = team::resolve_repo_identity(cwd);
        if (!repo) return team_error(req, 400, repo.error);
        const auto result = team_service->register_repo(team_id, repo.identity);
        if (!result) return team_error(req, 500, result.error);
        broadcast_team_state(team_id);
        return team_response(req, 200, json{{"kind", repo.identity.kind},
                                            {"value", repo.identity.value}}.dump());
    });

    // 发起团队会话:创建本地会话 → 标记团队 meta → 登记 session_refs(host)。
    CROW_ROUTE(app, "/api/team/<string>/session").methods(crow::HTTPMethod::POST)
    ([this](const crow::request& req, const std::string& team_id) {
        if (auto denied = require_auth(req)) return std::move(*denied);
        if (!team_service || !deps.session_client) return team_error(req, 503, "team_unavailable");
        const auto body = json::parse(req.body, nullptr, false);
        if (const std::string invalid = web::validate_team_session_start_body(body); !invalid.empty())
            return team_error(req, 400, invalid);
        // cwd 缺省链:显式 cwd → 本机团队工作目录 → daemon 工作目录。
        const auto local_state = team_service->store_for(team_id).load_local_state();
        const std::string cwd = web::resolve_team_session_cwd(body, local_state.work_dir,
                                                              deps.cwd);
        const std::string title = body.contains("title") && body["title"].is_string()
            ? body["title"].get<std::string>() : std::string{};

        // 仓库身份必须已关联团队
        const auto repo = team::resolve_repo_identity(cwd);
        if (!repo) return team_error(req, 400, repo.error);

        SessionOptions opts;
        opts.cwd = cwd;
        const std::string session_id = deps.session_client->create_session(opts);
        if (session_id.empty()) return team_error(req, 500, "session_create_failed");

        // SessionMeta 打团队标记(稀疏字段,host 角色)
        const std::string meta_path =
            SessionStorage::get_project_dir(cwd) + "/" + session_id + ".meta.json";
        acecode::SessionMeta session_meta = SessionStorage::read_meta(meta_path);
        session_meta.id = session_id;
        session_meta.cwd = cwd;
        team::mark_team_session_meta(session_meta, team_id, "host");
        if (!SessionStorage::write_meta(meta_path, session_meta)) {
            return team_error(req, 500, "meta_write_failed");
        }

        const auto started = team_service->start_session(team_id, repo.identity, session_id, title);
        if (!started) return team_error(req, 400, started.error);
        ensure_team_session_subscriptions(team_id);
        broadcast_team_state(team_id);
        broadcast_team_session_event(team_id, "created", started.ref);
        return team_response(req, 200,
            team_session_ref_with_local_info(started.ref).dump());
    });

    // 结束/暂停团队会话。
    CROW_ROUTE(app, "/api/team/<string>/session/<string>/state").methods(crow::HTTPMethod::POST)
    ([this](const crow::request& req, const std::string& team_id, const std::string& session_id) {
        if (auto denied = require_auth(req)) return std::move(*denied);
        if (!team_service) return team_error(req, 503, "team_unavailable");
        const auto body = json::parse(req.body, nullptr, false);
        if (!body.is_object() || !body.contains("state") || !body["state"].is_string())
            return team_error(req, 400, "invalid_body");
        const std::string pause_reason = body.contains("pause_reason") &&
            body["pause_reason"].is_string() ? body["pause_reason"].get<std::string>()
                                             : std::string{};
        const auto result = team_service->set_session_state(team_id, session_id,
            body["state"].get<std::string>(), pause_reason);
        if (!result) return team_error(req, 400, result.error);
        const auto meta = team_service->load_team(team_id);
        if (meta) {
            if (const auto* ref = team::find_session(*meta, session_id)) {
                broadcast_team_session_event(team_id,
                    ref->state == team::kSessionStateEnded ? "ended" : "paused", *ref);
            }
        }
        return team_response(req, 200, "{\"ok\":true}");
    });

    // 手动接管(本机成为新 host)。
    CROW_ROUTE(app, "/api/team/<string>/session/<string>/takeover").methods(crow::HTTPMethod::POST)
    ([this](const crow::request& req, const std::string& team_id, const std::string& session_id) {
        if (auto denied = require_auth(req)) return std::move(*denied);
        if (!team_service || !deps.session_client) return team_error(req, 503, "team_unavailable");
        const auto body = json::parse(req.body, nullptr, false);
        if (body.is_object() && body.contains("cwd") && !body["cwd"].is_string())
            return team_error(req, 400, "invalid_cwd");
        // cwd 缺省 = daemon 工作目录。
        const std::string cwd = body.is_object() && body["cwd"].is_string() &&
            !body["cwd"].get<std::string>().empty() ? body["cwd"].get<std::string>() : deps.cwd;

        // 在线源设备取数:按当前 host 成员记录的地址直连其 daemon。
        auto fetch = [this, team_id](const team::TeamMeta& meta,
                                     const team::TeamSessionRef& ref) -> team::SessionSourceData {
            const team::TeamMember* host = team::find_member(meta, ref.host_guid);
            std::string address = host ? host->address : std::string{};
            int port = 0;
            if (!address.empty()) {
                const auto colon = address.rfind(':');
                if (colon != std::string::npos) {
                    port = std::atoi(address.c_str() + colon + 1);
                    address.resize(colon);
                }
            }
            const auto meta_now = team_service->load_team(team_id);
            const auto credential = meta_now
                ? team::derive_team_token(meta_now->invite_seed, team_id) : std::string{};
            team::SessionSourceData data;
            if (credential.empty() || port <= 0 || address.empty()) {
                data.error = "no_source_address";
                return data;
            }
            const auto response = team::team_request(address, port,
                "/api/team/v1/session/data?team_id=" +
                    utils::percent_encode_query_component(team_id) + "&session_id=" +
                    utils::percent_encode_query_component(ref.session_id),
                credential, std::nullopt);
            if (response.error.empty() && response.status == 200) {
                try {
                    const auto parsed = json::parse(response.body);
                    data.jsonl = parsed.value("jsonl", std::string{});
                    data.meta = parsed.value("meta", std::string{});
                    data.head_commit = parsed.value("head_commit", std::string{});
                    return data;
                } catch (const json::exception&) {}
            }
            data.error = "source_unavailable";
            return data;
        };

        team::TeamTakeover takeover(*team_service, std::move(fetch),
            [this](const std::string& sid) {
                return deps.session_client->resume_session(sid);
            });
        const auto outcome = takeover.take_over(team_id, session_id, cwd,
            SessionStorage::get_project_dir(cwd));
        if (!outcome) return team_error(req, 409, outcome.error);
        ensure_team_session_subscriptions(team_id);
        broadcast_team_state(team_id);
        broadcast_team_session_event(team_id, "host_changed", outcome.ref);
        return team_response(req, 200,
            team_session_ref_with_local_info(outcome.ref).dump());
    });

    // 本机 UI 给团队会话发消息(daemon token):转发到当前 host 设备。
    // 本机就是 host 时直接走中继;否则按 host 成员记录的地址带团队凭据转发。
    CROW_ROUTE(app, "/api/team/<string>/session/<string>/message").methods(crow::HTTPMethod::POST)
    ([this](const crow::request& req, const std::string& team_id, const std::string& session_id) {
        if (auto denied = require_auth(req)) return std::move(*denied);
        if (!team_service || !deps.session_client) return team_error(req, 503, "team_unavailable");
        const auto body = json::parse(req.body, nullptr, false);
        if (!body.is_object() || !body.contains("text") || !body["text"].is_string() ||
            body["text"].get<std::string>().empty())
            return team_error(req, 400, "invalid_body");
        const std::string text = body["text"].get<std::string>();

        const auto meta = team_service->load_team(team_id);
        const auto* ref = meta ? team::find_session(*meta, session_id) : nullptr;
        if (!ref) return team_error(req, 404, "session_not_found");
        if (ref->state != team::kSessionStateRunning)
            return team_error(req, 409, "session_not_running");

        // 本机就是 host:直接投递(与队员路由同一条中继路径)。
        if (ref->host_guid == team_service->self().guid) {
            team::TeamInboundMessage message;
            message.team_id = team_id;
            message.session_id = session_id;
            message.sender_guid = team_service->self().guid;
            message.sender_name = utils::current_account_name();
            message.text = text;
            const auto relay = team_relay_for(session_id);
            const auto outcome =
                relay->receive(std::move(message), local_session_busy(deps.session_client, session_id));
            if (outcome == team::RelayOutcome::Rejected) return team_error(req, 400, "invalid_message");
            return team_response(req, outcome == team::RelayOutcome::Delivered ? 200 : 202,
                json{{"status", outcome == team::RelayOutcome::Delivered ? "delivered" : "queued"}}.dump());
        }

        // 转发到 host 设备(队员视角,消息标注本机账户名)。
        refresh_team_self_address(team_id);
        const team::TeamMember* host = team::find_member(*meta, ref->host_guid);
        std::string address = host ? host->address : std::string{};
        int port = 0;
        if (!address.empty()) {
            const auto colon = address.rfind(':');
            if (colon != std::string::npos) {
                port = std::atoi(address.c_str() + colon + 1);
                address.resize(colon);
            }
        }
        const auto credential = team::derive_team_token(meta->invite_seed, team_id);
        if (address.empty() || port <= 0 || credential.empty())
            return team_error(req, 502, "host_unreachable");
        const auto response = team::team_request(address, port,
            "/api/team/v1/session/message?team_id=" +
                utils::percent_encode_query_component(team_id),
            credential,
            json{{"session_id", session_id},
                 {"text", text},
                 {"sender_guid", team_service->self().guid},
                 {"sender_name", utils::current_account_name()}});
        if (!response.error.empty() || response.status != 200)
            return team_error(req, response.status == 202 ? 202 : 502,
                response.error.empty() ? "host_rejected" : response.error);
        return team_response(req, 200, "{\"status\":\"delivered\"}");
    });

    register_team_member_routes();
}

// =====================================================================
// REST/WS — 队员互访(X-ACECode-Team-Token;绝不走 daemon require_auth)
// =====================================================================

void WebServer::Impl::register_team_member_routes() {
    // 加入(邀请者侧):验证并消费一次性邀请码,返回全量快照。
    CROW_ROUTE(app, "/api/team/v1/join").methods(crow::HTTPMethod::POST)
    ([this](const crow::request& req) {
        if (!team_service) return team_error(req, 503, "team_unavailable");
        const auto body = json::parse(req.body, nullptr, false);
        if (!body.is_object() || !body.contains("token") || !body["token"].is_string() ||
            !body.contains("guid") || !body["guid"].is_string())
            return team_error(req, 400, "invalid_body");
        const std::string token = body["token"].get<std::string>();
        if (token.empty()) return team_error(req, 400, "invalid_token");
        // token 必须对应某团队账本里的未消费邀请码(先按 token 找团队)。
        std::string team_id;
        for (const auto& meta : team_service->load_teams()) {
            if (team::TeamStore(team_service->data_dir(), meta.team_id).find_invite(token)) {
                team_id = meta.team_id;
                break;
            }
        }
        if (team_id.empty()) return team_error(req, 403, "invite_invalid");
        team::DeviceIdentity joiner{body["guid"].get<std::string>(),
            body.value("account", std::string{}), body.value("display_name", std::string{})};
        const auto accepted = team_service->accept_join(team_id, token, joiner);
        if (!accepted) return team_error(req, 403, accepted.error);
        return team_response(req, 200,
            json{{"snapshot", team::team_meta_to_json(accepted.snapshot)}}.dump());
    });

    // 状态拉取(队员间同步)。
    CROW_ROUTE(app, "/api/team/v1/state").methods(crow::HTTPMethod::GET)
    ([this](const crow::request& req) {
        const auto team_id = req.url_params.get("team_id");
        if (!team_id) return team_error(req, 400, "invalid_team");
        if (auto denied = require_team_auth(req, team_id)) return std::move(*denied);
        const auto meta = team_service->load_team(team_id);
        if (!meta) return team_error(req, 404, "team_not_found");
        return team_response(req, 200,
            json{{"snapshot", team::team_meta_to_json(*meta)}}.dump());
    });

    // 状态推送(对端同步路径的回推)。
    CROW_ROUTE(app, "/api/team/v1/state/push").methods(crow::HTTPMethod::POST)
    ([this](const crow::request& req) {
        const auto team_id = req.url_params.get("team_id");
        if (!team_id) return team_error(req, 400, "invalid_team");
        if (auto denied = require_team_auth(req, team_id)) return std::move(*denied);
        const auto body = json::parse(req.body, nullptr, false);
        if (!body.is_object() || !body.contains("snapshot") || !body["snapshot"].is_object())
            return team_error(req, 400, "invalid_snapshot");
        std::string error;
        const auto merged = team_service->merge_remote(team_id,
            team::team_meta_from_json(body["snapshot"]), &error);
        if (!merged) return team_error(req, 500, error.empty() ? "merge_failed" : error);
        broadcast_team_state(team_id);
        return team_response(req, 200, "{\"ok\":true}");
    });

    // 队员消息转发:host 唯一写者,追加 human 消息并触发 AI 回合;
    // host 忙时进等待队列,回合结束按 FIFO 投递。
    CROW_ROUTE(app, "/api/team/v1/session/message").methods(crow::HTTPMethod::POST)
    ([this](const crow::request& req) {
        const auto team_id = req.url_params.get("team_id");
        if (!team_id) return team_error(req, 400, "invalid_team");
        if (auto denied = require_team_auth(req, team_id)) return std::move(*denied);
        if (!deps.session_client) return team_error(req, 503, "team_unavailable");
        const auto body = json::parse(req.body, nullptr, false);
        if (!body.is_object() || !body.contains("session_id") ||
            !body["session_id"].is_string() || !body.contains("text") ||
            !body["text"].is_string())
            return team_error(req, 400, "invalid_body");
        const std::string session_id = body["session_id"].get<std::string>();
        const std::string text = body["text"].get<std::string>();
        if (text.empty()) return team_error(req, 400, "empty_message");

        const auto meta = team_service->load_team(team_id);
        const auto* ref = meta ? team::find_session(*meta, session_id) : nullptr;
        if (!ref) return team_error(req, 404, "session_not_found");
        if (ref->host_guid != team_service->self().guid)
            return team_error(req, 409, "not_host");
        if (ref->state != team::kSessionStateRunning)
            return team_error(req, 409, "session_not_running");

        team::TeamInboundMessage message;
        message.team_id = team_id;
        message.session_id = session_id;
        message.sender_guid = body.value("sender_guid", std::string{});
        message.sender_name = body.value("sender_name", std::string{});
        message.text = text;
        message.received_at_ms = now_unix_ms();

        const auto relay = team_relay_for(session_id);
        const bool busy = local_session_busy(deps.session_client, session_id);
        const auto outcome = relay->receive(std::move(message), busy);
        if (outcome == team::RelayOutcome::Rejected)
            return team_error(req, 400, "invalid_message");
        if (outcome == team::RelayOutcome::Delivered)
            return team_response(req, 200, json{{"status", "delivered"}}.dump());
        return team_response(req, 202, json{{"status", "queued"}}.dump());
    });

    // 会话数据(接管取数):host 把 jsonl/meta/HEAD 发给接管方。
    CROW_ROUTE(app, "/api/team/v1/session/data").methods(crow::HTTPMethod::GET)
    ([this](const crow::request& req) {
        const auto team_id = req.url_params.get("team_id");
        const auto session_id = req.url_params.get("session_id");
        if (!team_id || !session_id) return team_error(req, 400, "invalid_request");
        if (auto denied = require_team_auth(req, team_id)) return std::move(*denied);
        const auto meta = team_service->load_team(team_id);
        const auto* ref = meta ? team::find_session(*meta, session_id) : nullptr;
        if (!ref) return team_error(req, 404, "session_not_found");
        if (ref->host_guid != team_service->self().guid)
            return team_error(req, 409, "not_host");

        const std::string project_dir = SessionStorage::get_project_dir(deps.cwd);
        const std::string jsonl =
            read_file_or_empty(project_dir + "/" + session_id + ".jsonl");
        const std::string meta_text =
            read_file_or_empty(project_dir + "/" + session_id + ".meta.json");
        if (jsonl.empty() && meta_text.empty())
            return team_error(req, 404, "data_not_found");

        std::string head_commit;
        const auto head = worktree::run_git({"rev-parse", "HEAD"}, deps.cwd, 10000);
        if (head.ok()) head_commit = utils::trim_ascii_copy(head.out);

        return team_response(req, 200, json{{"jsonl", jsonl},
                                            {"meta", meta_text},
                                            {"head_commit", head_commit}}.dump());
    });

    // 团队 WS:daemon token = 本机 UI;团队凭据 = 队员只读投影(3.3)。
    CROW_WEBSOCKET_ROUTE(app, "/ws/team/<string>")
    .onaccept([this](const crow::request& req, void** userdata) {
        LOG_INFO("[team] ws onaccept entered");
        if (shutdown_requested.load() || !team_service) return false;
        const std::string team_id = req.url.substr(std::string("/ws/team/").size());
        if (team_id.empty() || team_id.find('/') != std::string::npos) return false;
        std::string query_token;
        if (const auto value = req.url_params.get("token")) query_token = value;
        const auto team_header = req.get_header_value("X-ACECode-Team-Token");

        auto state = std::make_shared<TeamConnectionState>();
        state->team_id = team_id;
        if (!query_token.empty() &&
            auth_result_for_request(req, query_token, query_token) == AuthResult::Allowed) {
            state->device_guid = team_service->self().guid; // 本机 UI
            LOG_INFO("[team] ws auth ok via daemon token");
        } else if (!team_header.empty() && team_service->authorize(team_id, team_header)) {
            state->device_guid = "remote-member"; // 队员只读投影,绝不产生写入
            LOG_INFO("[team] ws auth ok via team credential");
        } else {
            LOG_INFO("[team] ws auth rejected");
            return false;
        }
        *userdata = state.get();
        const std::string device = state->device_guid;
        std::lock_guard<std::mutex> lock(team_mu);
        pending_team_connections.emplace(state.get(), std::move(state));
        LOG_INFO("[team] ws accepted team=" + team_id + " device=" + device);
        return true;
    })
    .onopen([this](crow::websocket::connection& connection) {
        std::string team_id;
        {
            std::lock_guard<std::mutex> lock(team_mu);
            auto it = pending_team_connections.find(connection.userdata());
            if (it == pending_team_connections.end()) {
                LOG_WARN("[team] ws open without pending state");
                return;
            }
            auto state = std::move(it->second);
            pending_team_connections.erase(it);
            team_id = state->team_id;
            team_connections.emplace(&connection, std::move(state));
        }
        LOG_INFO("[team] ws open team=" + team_id);
        // 初始快照 + 本机 host 会话的实时事件订阅(3.3)。
        broadcast_team_state(team_id);
        ensure_team_session_subscriptions(team_id);
    })
    .onmessage([this](crow::websocket::connection& connection, const std::string& data, bool binary) {
        // 本机 UI / 队员只读投影都不接受客户端指令;保留 ping 语义即可。
        if (binary || data.size() > 65536) {
            connection.close("invalid_message");
            return;
        }
        auto body = json::parse(data, nullptr, false);
        if (body.is_object() && body.value("type", std::string{}) == "ping")
            connection.send_text(R"({"type":"pong"})");
    })
    .onclose([this](crow::websocket::connection& connection, const std::string&,
                    std::uint16_t) {
        std::lock_guard<std::mutex> lock(team_mu);
        team_connections.erase(&connection);
        pending_team_connections.erase(&connection);
    });
}

} // namespace acecode::web
