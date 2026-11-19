#pragma once

// 本机身份:系统用户名、电脑名与账户名(团队功能的成员账户来源)。
//
// 账户名 = 登录名 + 电脑名,是给人看的稳定标识;真正的成员身份主键是设备
// GUID(见 host/peers 与 team),账户名重复不会造成成员混淆。两个字段都从
// 环境变量读取,与反馈上传(feedback_upload)的历史口径保持一致。

#include <string>

namespace acecode::utils {

// 电脑名:Windows 优先 COMPUTERNAME,其余平台优先 HOSTNAME。
std::string current_computer_name();

// 登录名:Windows 优先 USERNAME,其余平台优先 USER。
std::string current_login_name();

// 账户名:<登录名>@<电脑名>;任一字段为空时退化为另一个字段,两者都空时为空。
std::string current_account_name();

} // namespace acecode::utils
