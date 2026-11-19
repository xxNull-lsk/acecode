#include "utils/machine_identity.hpp"

#include "utils/encoding.hpp"
#include "utils/text.hpp"

#include <initializer_list>

#ifdef _WIN32
#include <winsock2.h>
#else
#include <unistd.h>
#endif

namespace acecode::utils {
namespace {

std::string trim_env(const char* name) {
    return trim_ascii_copy(getenv_utf8(name));
}

std::string first_non_empty_env(std::initializer_list<const char*> names) {
    for (const char* name : names) {
        std::string value = trim_env(name);
        if (!value.empty()) return value;
    }
    return {};
}

// 环境变量缺失(如非 bash 登录的 zsh)时的兜底:直接问操作系统主机名。
std::string os_hostname() {
    char buffer[256] = {};
    if (::gethostname(buffer, sizeof(buffer)) != 0) return {};
    return trim_ascii_copy(std::string(buffer));
}

} // namespace

std::string current_computer_name() {
#ifdef _WIN32
    std::string value = first_non_empty_env({"COMPUTERNAME", "HOSTNAME"});
#else
    std::string value = first_non_empty_env({"HOSTNAME", "COMPUTERNAME"});
#endif
    if (value.empty()) value = os_hostname();
    return value;
}

std::string current_login_name() {
#ifdef _WIN32
    return first_non_empty_env({"USERNAME", "USER", "LOGNAME"});
#else
    return first_non_empty_env({"USER", "LOGNAME", "USERNAME"});
#endif
}

std::string current_account_name() {
    const std::string login = current_login_name();
    const std::string computer = current_computer_name();
    if (login.empty()) return computer;
    if (computer.empty()) return login;
    return login + "@" + computer;
}

} // namespace acecode::utils
