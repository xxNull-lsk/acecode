#include "team_host/team_client.hpp"

#include "network/proxy_resolver.hpp"
#include "utils/host_validation.hpp"
#include "utils/url_encoding.hpp"

#include <cpr/cpr.h>
#include <memory>
#include <stdexcept>

namespace acecode::team {
namespace {

std::string quote_host(std::string host) {
    const auto zone = host.find('%');
    if (zone != std::string::npos) host.replace(zone, 1, "%25");
    if (host.find(':') != std::string::npos) host = "[" + host + "]";
    return host;
}

// 快照响应的上限:团队状态是成员+会话列表,远小于会话内容。
constexpr std::size_t kMaxResponseBytes = 32u * 1024u * 1024u;

} // namespace

std::string team_base_url(const std::string& address, int port) {
    if (!utils::is_valid_network_host(address) || port < 1 || port > 65535)
        throw std::invalid_argument("invalid_address");
    return "http://" + quote_host(address) + ":" + std::to_string(port);
}

TeamHttpResponse team_request(const std::string& address, int port,
                              const std::string& path,
                              const std::optional<std::string>& team_token,
                              const std::optional<nlohmann::json>& body) {
    TeamHttpResponse result;
    std::string url;
    try {
        url = team_base_url(address, port) + path;
    } catch (const std::invalid_argument&) {
        result.error = "invalid_address";
        return result;
    }
    auto proxy = network::proxy_options_for(url);
    if (proxy.resolved.url.empty()) proxy.proxies = cpr::Proxies{{"http", ""}, {"https", ""}};

    cpr::Session session;
    session.SetUrl(cpr::Url{url});
    cpr::Header header{{"Accept", "application/json"}};
    if (body) header.insert({"Content-Type", "application/json"});
    if (team_token && !team_token->empty()) header.insert({std::string(kTeamTokenHeader), *team_token});
    session.SetHeader(header);
    session.SetProxies(proxy.proxies);
    session.SetProxyAuth(proxy.auth);
    session.SetConnectTimeout(cpr::ConnectTimeout{3000});
    session.SetTimeout(cpr::Timeout{5000});
    session.SetRedirect(cpr::Redirect{false});

    auto received = std::make_shared<std::string>();
    session.SetWriteCallback(cpr::WriteCallback{[received](std::string_view bytes, intptr_t) {
        if (received->size() + bytes.size() > kMaxResponseBytes) return false;
        received->append(bytes);
        return true;
    }});
    if (body) session.SetBody(cpr::Body{body->dump()});
    const auto response = body ? session.Post() : session.Get();
    if (response.error.code != cpr::ErrorCode::OK) {
        result.error = response.error.message.empty() ? "transport_error" : response.error.message;
        return result;
    }
    result.status = static_cast<int>(response.status_code);
    result.body = std::move(*received);
    return result;
}

SnapshotFetcher make_network_join_fetcher(const DeviceIdentity& self) {
    return [self](const TeamInviteLink& invite, std::string* error) -> std::optional<TeamMeta> {
        nlohmann::json request{{"token", invite.token},
                               {"guid", self.guid},
                               {"account", self.account},
                               {"display_name", self.display_name}};
        std::string last_error = "connection_failed";
        for (const auto& address : invite.addresses) {
            if (!utils::is_valid_network_host(address)) continue;
            const auto response = team_request(address, invite.port, "/api/team/v1/join",
                                               std::nullopt, request);
            if (!response.error.empty()) {
                last_error = response.error;
                continue;
            }
            if (response.status != 200) {
                last_error = "join_rejected";
                continue;
            }
            try {
                const auto parsed = nlohmann::json::parse(response.body);
                if (!parsed.contains("snapshot") || !parsed["snapshot"].is_object()) {
                    last_error = "invalid_snapshot";
                    continue;
                }
                TeamMeta snapshot = team_meta_from_json(parsed["snapshot"]);
                if (snapshot.team_id != invite.team_id) {
                    last_error = "team_id_mismatch";
                    continue;
                }
                if (!find_active_member(snapshot, self.guid)) {
                    last_error = "self_not_in_team";
                    continue;
                }
                return snapshot;
            } catch (const nlohmann::json::exception&) {
                last_error = "invalid_snapshot_json";
                continue;
            }
        }
        if (error) *error = last_error;
        return std::nullopt;
    };
}

TeamStatePull pull_team_state(const std::string& address, int port,
                              const std::string& team_id,
                              const std::string& team_token) {
    TeamStatePull result;
    const std::string path = "/api/team/v1/state?team_id=" + utils::percent_encode_query_component(team_id);
    const auto response = team_request(address, port, path, team_token, std::nullopt);
    if (!response.error.empty()) {
        result.error = response.error;
        return result;
    }
    if (response.status != 200) {
        result.error = "state_rejected";
        return result;
    }
    try {
        const auto parsed = nlohmann::json::parse(response.body);
        if (!parsed.contains("snapshot") || !parsed["snapshot"].is_object()) {
            result.error = "invalid_snapshot";
            return result;
        }
        result.state = team_meta_from_json(parsed["snapshot"]);
        if (result.state.team_id != team_id) {
            result.error = "team_id_mismatch";
            return result;
        }
    } catch (const nlohmann::json::exception&) {
        result.error = "invalid_snapshot_json";
    }
    return result;
}

TeamOpResult sync_with_peer(TeamService& service, const std::string& address, int port,
                            const std::string& team_id, const std::string& team_token) {
    auto pulled = pull_team_state(address, port, team_id, team_token);
    if (!pulled) return TeamOpResult::fail(pulled.error);
    std::string merge_error;
    auto merged = service.merge_remote(team_id, pulled.state, &merge_error);
    if (!merged) return TeamOpResult::fail(merge_error.empty() ? "merge_failed" : merge_error);
    return TeamOpResult::success();
}

} // namespace acecode::team
