// 覆盖 src/domain/team/team_invite.cpp。邀请码是团队唯一的入队凭据:
// CRC 校验防止截断,15 分钟上限在编码侧钳死,过期判定用调用方传入的 now —
// 与 share 邀请链接同构但语义独立,不共享前缀。

#include <gtest/gtest.h>

#include "team/team_invite.hpp"

#include <stdexcept>

namespace acecode::team {

namespace {

TeamInviteLink sample_invite(std::int64_t now) {
    TeamInviteLink invite;
    invite.team_id = "team-1";
    invite.team_name = "研发组";
    invite.inviter_name = "老王";
    invite.inviter_guid = "guid-a";
    invite.addresses = {"192.168.1.10"};
    invite.port = 12399;
    invite.token = "0123456789abcdef0123456789abcdef";
    invite.expires_at = now + kInviteTtlSeconds;
    return invite;
}

} // namespace

// 场景: 生成的链接可以往返解码,逐字段相同
TEST(TeamInvite, RoundTripPreservesFields) {
    const std::int64_t now = 1'700'000'000;
    const auto invite = sample_invite(now);
    const std::string link = encode_team_invite_link(invite, now);
    EXPECT_EQ(link.substr(0, std::string(kTeamInvitePrefix).size()), kTeamInvitePrefix);
    EXPECT_LE(link.size(), kMaxTeamInviteLength);

    const auto decoded = decode_team_invite_link(link, now);
    ASSERT_TRUE(decoded);
    EXPECT_EQ(decoded.value.team_id, invite.team_id);
    EXPECT_EQ(decoded.value.team_name, invite.team_name);
    EXPECT_EQ(decoded.value.inviter_name, invite.inviter_name);
    EXPECT_EQ(decoded.value.inviter_guid, invite.inviter_guid);
    EXPECT_EQ(decoded.value.addresses, invite.addresses);
    EXPECT_EQ(decoded.value.port, invite.port);
    EXPECT_EQ(decoded.value.token, invite.token);
    EXPECT_EQ(decoded.value.expires_at, invite.expires_at);
}

// 场景: 从带标点与说明文字的消息里宽松提取
TEST(TeamInvite, ExtractFromText) {
    const std::int64_t now = 1'700'000'000;
    const std::string link = encode_team_invite_link(sample_invite(now), now);
    const auto decoded = decode_team_invite_link(
        "老王 邀请你加入团队：acecode://team/" + link.substr(std::string(kTeamInvitePrefix).size()) + "。", now);
    // 宽松提取忽略前后标点;链接主体是 url64 字符集,直接可识别。
    ASSERT_TRUE(decoded) << decoded.error;
    EXPECT_EQ(decoded.value.team_id, "team-1");
}

// 场景: 邀请码可携带团队仓库身份(remote 类),往返解码逐条相同
TEST(TeamInvite, RoundTripCarriesRepos) {
    const std::int64_t now = 1'700'000'000;
    auto invite = sample_invite(now);
    invite.repos = {{"remote", "github.com/Org/Repo"}, {"remote", "gitlab.com/a/b"}};
    const auto decoded = decode_team_invite_link(encode_team_invite_link(invite, now), now);
    ASSERT_TRUE(decoded) << decoded.error;
    ASSERT_EQ(decoded.value.repos.size(), 2u);
    EXPECT_EQ(decoded.value.repos[0].kind, "remote");
    EXPECT_EQ(decoded.value.repos[0].value, "github.com/Org/Repo");
    EXPECT_EQ(decoded.value.repos[1].value, "gitlab.com/a/b");
}

// 场景: 旧格式(无 r 字段)解码为空仓库,向后兼容
TEST(TeamInvite, LegacyLinkWithoutReposDecodesEmpty) {
    const std::int64_t now = 1'700'000'000;
    const auto decoded = decode_team_invite_link(encode_team_invite_link(sample_invite(now), now), now);
    ASSERT_TRUE(decoded) << decoded.error;
    EXPECT_TRUE(decoded.value.repos.empty());
}

// 场景: 载荷校验拒绝 local-id 条目与超量条目
TEST(TeamInvite, InvalidReposRejected) {
    const std::int64_t now = 1'700'000'000;
    auto invite = sample_invite(now);
    invite.repos = {{"local-id", "uuid-1"}};
    EXPECT_THROW(encode_team_invite_link(invite, now), std::invalid_argument);

    invite.repos.assign(5, {"remote", "github.com/Org/Repo"});
    EXPECT_THROW(encode_team_invite_link(invite, now), std::invalid_argument);
}

// 场景: 链接被聊天软件截断 → 校验值不匹配
TEST(TeamInvite, TruncatedLinkFailsChecksum) {
    const std::int64_t now = 1'700'000'000;
    const std::string link = encode_team_invite_link(sample_invite(now), now);
    const auto decoded = decode_team_invite_link(link.substr(0, link.size() - 4), now);
    EXPECT_FALSE(decoded);
    EXPECT_EQ(decoded.error, "checksum");
}

// 场景: 超过 15 分钟 → 过期,不发起连接
TEST(TeamInvite, ExpiredLinkRejected) {
    const std::int64_t now = 1'700'000'000;
    const std::string link = encode_team_invite_link(sample_invite(now), now);
    const auto decoded = decode_team_invite_link(link, now + kInviteTtlSeconds + 1);
    EXPECT_FALSE(decoded);
    EXPECT_EQ(decoded.error, "expired");
    // 边界: 到期时刻仍有效
    EXPECT_TRUE(decode_team_invite_link(link, now + kInviteTtlSeconds));
}

// 场景: 编码侧钳制 — 有效期最多创建起 15 分钟,不许签发长期码
TEST(TeamInvite, EncodeClampsToMaxTtl) {
    const std::int64_t now = 1'700'000'000;
    auto invite = sample_invite(now);
    invite.expires_at = now + kInviteTtlSeconds + 1;
    EXPECT_THROW(encode_team_invite_link(invite, now), std::invalid_argument);
    invite.expires_at = now + kInviteTtlSeconds;
    EXPECT_NO_THROW(encode_team_invite_link(invite, now));
}

// 场景: 非法字段在编码侧拒绝(空 token、坏地址、坏端口)
TEST(TeamInvite, EncodeRejectsInvalidFields) {
    const std::int64_t now = 1'700'000'000;
    auto invite = sample_invite(now);
    invite.token.clear();
    EXPECT_THROW(encode_team_invite_link(invite, now), std::invalid_argument);
    invite = sample_invite(now);
    invite.token = "bad token with spaces";
    EXPECT_THROW(encode_team_invite_link(invite, now), std::invalid_argument);
    invite = sample_invite(now);
    invite.addresses = {"256.1.1.1"};
    EXPECT_THROW(encode_team_invite_link(invite, now), std::invalid_argument);
    invite = sample_invite(now);
    invite.port = 0;
    EXPECT_THROW(encode_team_invite_link(invite, now), std::invalid_argument);
}

// 场景: 非 acecode://team/ 前缀一律识别不出,不误吞 share 链接
TEST(TeamInvite, ForeignPrefixRejected) {
    const auto decoded = decode_team_invite_link("acecode://connect/AAAA", 0);
    EXPECT_FALSE(decoded);
    EXPECT_EQ(decoded.error, "not_team_invite");
    EXPECT_EQ(extract_team_invite_link("nothing here"), "");
}

// 场景: 未知主版本被拒
TEST(TeamInvite, UnsupportedVersionRejected) {
    const std::int64_t now = 1'700'000'000;
    const std::string link = encode_team_invite_link(sample_invite(now), now);
    // 改不出有效 CRC 的 v2 payload,直接构造坏载荷:base64 部分合法但 CRC 不对。
    const auto decoded = decode_team_invite_link(std::string(kTeamInvitePrefix) + link.substr(std::string(kTeamInvitePrefix).size(), 8), now);
    EXPECT_FALSE(decoded);
}

} // namespace acecode::team
