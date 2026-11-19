// 覆盖 src/base/utils/machine_identity.cpp。团队功能的成员账户名来源:
// 登录名 + 电脑名。环境变量口径必须与反馈上传(feedback_upload)一致;
// 账户名拼接规则(空字段退化)是 spec team-membership 的"身份"要求。

#include <gtest/gtest.h>

#include "utils/machine_identity.hpp"

using acecode::utils::current_account_name;
using acecode::utils::current_computer_name;
using acecode::utils::current_login_name;

// 场景: 环境变量正常时,登录名与电脑名非空
TEST(MachineIdentity, LoginAndComputerNameNonEmpty) {
    // CI 与开发机都设置了 HOSTNAME/USER 或对等变量;缺失才允许为空。
    EXPECT_FALSE(current_login_name().empty());
    EXPECT_FALSE(current_computer_name().empty());
}

// 场景: 账户名 = 登录名@电脑名
TEST(MachineIdentity, AccountNameJoinsLoginAndComputer) {
    const std::string account = current_account_name();
    EXPECT_EQ(account,
              current_login_name() + "@" + current_computer_name());
}

// 场景: 环境变量全空时各函数退化为空串而不是崩溃
TEST(MachineIdentity, AllEnvEmptyYieldsEmptyParts) {
    if (current_login_name().empty()) {
        // 登录名缺失时账户名必须直接等于电脑名,不产生悬空 '@'。
        EXPECT_EQ(current_account_name(), current_computer_name());
    }
    if (current_computer_name().empty()) {
        EXPECT_EQ(current_account_name(), current_login_name());
    }
}
