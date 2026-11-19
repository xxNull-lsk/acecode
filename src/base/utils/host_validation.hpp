#pragma once

// 邀请/团队链接里的主机字段校验:IPv4、IPv6(可带 %zone)或不带点的 DNS 名,
// 拒绝 URL、路径与非法字符。邀请码与团队邀请码共用同一口径;放在 base 层
// 让 domain/share 与 domain/team 都能引用而不产生同层模块依赖。

#include <string_view>

namespace acecode::utils {

bool is_valid_network_host(std::string_view address);

} // namespace acecode::utils
