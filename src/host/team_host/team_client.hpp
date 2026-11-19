#pragma once

// 团队网络客户端(host 层):与队员 daemon 的团队路由通信。
//
// 协议(与 P6 的 routes_team 对齐):
//   POST /api/team/v1/join   body={token, guid, account, display_name}
//                            -> 200 {snapshot:<TeamMeta JSON>} / 错误 JSON
//   GET  /api/team/v1/state?team_id=<id>
//                            header: X-ACECode-Team-Token: <派生凭据>
//                            -> 200 {snapshot:<TeamMeta JSON>}
//
// 鉴权边界:团队凭据只经 X-ACECode-Team-Token 传递,绝不复用 daemon token;
// 逐地址尝试的顺序沿用 share/peer 的候选地址直连(私有网段不走系统代理)。

#include "team_host/team_service.hpp"
#include "team/team_invite.hpp"
#include "team/team_meta.hpp"

#include <optional>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>

namespace acecode::team {

struct TeamHttpResponse {
    int status = 0;
    std::string body;
    std::string error; // 传输层错误;非空时 status 无意义
};

// 组装 http://host:port;地址非法或端口越界抛 std::invalid_argument。
std::string team_base_url(const std::string& address, int port);

// 单次请求。team_token 非空时带 X-ACECode-Team-Token 头。
TeamHttpResponse team_request(const std::string& address, int port,
                              const std::string& path,
                              const std::optional<std::string>& team_token,
                              const std::optional<nlohmann::json>& body);

// 加入流程的网络 fetcher:按邀请码的候选地址逐个尝试 join。
// 校验响应快照的团队 ID 与成员身份(邀请者可能返回损坏数据)。
SnapshotFetcher make_network_join_fetcher(const DeviceIdentity& self);

// 从指定队员拉取全量团队状态(增量同步用),失败返回错误串。
struct TeamStatePull {
    TeamMeta state;
    std::string error;
    explicit operator bool() const { return error.empty(); }
};
TeamStatePull pull_team_state(const std::string& address, int port,
                              const std::string& team_id,
                              const std::string& team_token);

// 便捷封装:从某个在线队员拉取并合并进本地 TeamService。
TeamOpResult sync_with_peer(TeamService& service, const std::string& address, int port,
                            const std::string& team_id, const std::string& team_token);

} // namespace acecode::team
