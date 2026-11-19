#include "team/team_invite.hpp"

#include "utils/base64.hpp"
#include "utils/host_validation.hpp"

#include <algorithm>
#include <array>
#include <stdexcept>
#include <nlohmann/json.hpp>

namespace acecode::team {
namespace {

bool url64(unsigned char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
        (c >= '0' && c <= '9') || c == '-' || c == '_';
}

std::uint32_t checksum(std::string_view bytes) {
    std::uint32_t crc = 0xffffffffu;
    for (unsigned char c : bytes) {
        crc ^= c;
        for (int bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

std::string encode64(std::string_view bytes) {
    auto result = base64_encode(bytes);
    std::replace(result.begin(), result.end(), '+', '-');
    std::replace(result.begin(), result.end(), '/', '_');
    while (!result.empty() && result.back() == '=') result.pop_back();
    return result;
}

int hex(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

std::string unescape(std::string_view text) {
    std::string out;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '%' && i + 2 < text.size() &&
            hex(text[i + 1]) >= 0 && hex(text[i + 2]) >= 0) {
            out.push_back(static_cast<char>((hex(text[i + 1]) << 4) | hex(text[i + 2])));
            i += 2;
        } else out.push_back(text[i]);
    }
    return out;
}

std::string validate(const TeamInviteLink& invite, std::int64_t now) {
    if (invite.team_id.empty() || invite.team_id.size() > 128 ||
        invite.team_name.empty() || invite.team_name.size() > 256 ||
        invite.inviter_name.empty() || invite.inviter_name.size() > 256 ||
        invite.inviter_guid.empty() || invite.inviter_guid.size() > 128 ||
        invite.addresses.empty() || invite.addresses.size() > 32 ||
        invite.port < 1 || invite.port > 65535 ||
        invite.token.empty() || invite.token.size() > 128 ||
        !std::all_of(invite.token.begin(), invite.token.end(), url64) ||
        invite.expires_at <= 0 || invite.expires_at > 9007199254740991LL)
        return "invalid_fields";
    // 创建起最多 15 分钟:签发时就把有效期钳死在 now + TTL。
    if (invite.expires_at > now + kInviteTtlSeconds) return "invalid_expiry";
    for (const auto& address : invite.addresses)
        if (!utils::is_valid_network_host(address)) return "invalid_address";
    return {};
}

} // namespace

std::string extract_team_invite_link(std::string_view text) {
    const auto decoded = unescape(text.substr(0, 8192));
    const auto start = decoded.find(kTeamInvitePrefix);
    if (start == std::string::npos) return {};
    auto end = start + kTeamInvitePrefix.size();
    while (end < decoded.size() && url64(static_cast<unsigned char>(decoded[end]))) ++end;
    return decoded.substr(start, end - start);
}

TeamInviteDecodeResult decode_team_invite_link(std::string_view text, std::int64_t now) {
    TeamInviteDecodeResult result;
    const auto link = extract_team_invite_link(text);
    if (link.empty()) { result.error = "not_team_invite"; return result; }
    if (link.size() > kMaxTeamInviteLength) { result.error = "too_long"; return result; }
    const auto encoded = link.substr(kTeamInvitePrefix.size());
    const auto raw = base64_decode(encoded);
    if (!raw || raw->size() < 5 || encode64(*raw) != encoded) {
        result.error = "checksum";
        return result;
    }
    const auto body = raw->substr(0, raw->size() - 4);
    std::uint32_t expected = 0;
    for (auto i = raw->size() - 4; i < raw->size(); ++i)
        expected = (expected << 8) | static_cast<unsigned char>((*raw)[i]);
    if (checksum(body) != expected) { result.error = "checksum"; return result; }
    try {
        const auto json = nlohmann::json::parse(body);
        if (!json.is_object() || !json.contains("v") || !json["v"].is_number_integer() ||
            !json.contains("tid") || !json["tid"].is_string() ||
            !json.contains("tn") || !json["tn"].is_string() ||
            !json.contains("in") || !json["in"].is_string() ||
            !json.contains("ig") || !json["ig"].is_string() ||
            !json.contains("a") || !json["a"].is_array() ||
            !json.contains("p") || !json["p"].is_number_integer() ||
            !json.contains("c") || !json["c"].is_string() ||
            !json.contains("e") || !json["e"].is_number_integer()) {
            result.error = "invalid_fields";
            return result;
        }
        if (json["v"] != 1) { result.error = "unsupported_version"; return result; }
        result.value = {json["tid"].get<std::string>(), json["tn"].get<std::string>(),
            json["in"].get<std::string>(), json["ig"].get<std::string>(),
            json["a"].get<std::vector<std::string>>(), json["p"].get<int>(),
            json["c"].get<std::string>(), json["e"].get<std::int64_t>()};
        result.error = validate(result.value, now);
        // 与 invite_usable 同口径:expires_at 时刻仍有效,超过才判过期。
        if (result.error.empty() && result.value.expires_at < now) result.error = "expired";
    } catch (const nlohmann::json::exception&) {
        result.error = "invalid_fields";
    }
    return result;
}

std::string encode_team_invite_link(const TeamInviteLink& invite, std::int64_t now) {
    const auto error = validate(invite, now);
    if (!error.empty()) throw std::invalid_argument(error);
    nlohmann::json json{{"v", 1}, {"tid", invite.team_id}, {"tn", invite.team_name},
        {"in", invite.inviter_name}, {"ig", invite.inviter_guid},
        {"a", invite.addresses}, {"p", invite.port}, {"c", invite.token},
        {"e", invite.expires_at}};
    auto bytes = json.dump();
    const auto crc = checksum(bytes);
    for (int shift = 24; shift >= 0; shift -= 8)
        bytes.push_back(static_cast<char>(crc >> shift));
    auto link = std::string(kTeamInvitePrefix) + encode64(bytes);
    if (link.size() > kMaxTeamInviteLength) throw std::invalid_argument("too_long");
    return link;
}

} // namespace acecode::team
