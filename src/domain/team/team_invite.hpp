#pragma once

// 团队邀请码编解码(spec capability: team-membership)。与 share 的
// acecode://connect/ 同构:base64url(JSON)+CRC32 尾部校验,链上不加密,
// 链接本身就是凭据。区别在三点:一次性 token、创建起最多 15 分钟过期、
// 以及载荷带邀请者设备 GUID(全量快照从邀请者拉)。

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace acecode::team {

// 邀请码 TTL:创建起最多 15 分钟(spec: 邀请码有效期)。编码侧据此钳制,
// 账本(team_store)据此判过期,单一出处。
inline constexpr std::int64_t kInviteTtlSeconds = 15 * 60;

inline constexpr std::size_t kMaxTeamInviteLength = 1024;
inline constexpr std::string_view kTeamInvitePrefix = "acecode://team/";

struct TeamInviteLink {
    std::string team_id;
    std::string team_name;
    std::string inviter_name;    // 邀请者显示名(展示用)
    std::string inviter_guid;    // 邀请者设备 GUID,加入流程回执与连接目标
    std::vector<std::string> addresses;
    int port = 0;
    std::string token;           // 一次性 token
    std::int64_t expires_at = 0; // Unix 秒,与 share 链接同口径
};

struct TeamInviteDecodeResult {
    TeamInviteLink value;
    std::string error;
    explicit operator bool() const { return error.empty(); }
};

// 从一段文字里宽松提取团队邀请码(忽略前后标点、末尾多余 /、百分号编码)。
std::string extract_team_invite_link(std::string_view text);

// 解码并校验:主版本、必填字段、CRC 校验值、过期(相对于 now)、长度上限。
// 任何失败都不发起网络请求。token 非空与 url64 字符集在字段校验里。
TeamInviteDecodeResult decode_team_invite_link(std::string_view text, std::int64_t now);

// 编码。now 用于强制"创建起最多 15 分钟":expires_at 不得超过 now + TTL。
// 非法载荷抛 std::invalid_argument。
std::string encode_team_invite_link(const TeamInviteLink& invite, std::int64_t now);

} // namespace acecode::team
