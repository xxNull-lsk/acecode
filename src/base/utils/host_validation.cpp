#include "utils/host_validation.hpp"

#include <algorithm>
#include <array>
#include <string>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#endif

namespace acecode::utils {
namespace {

bool ascii_alnum(unsigned char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
        (c >= '0' && c <= '9');
}

} // namespace

bool is_valid_network_host(std::string_view address) {
    if (address.empty() || address.size() > 253) return false;
    std::string value(address);
    std::array<unsigned char, 16> parsed{};
    if (value.find(':') != std::string::npos) {
        const auto zone = value.find('%');
        if (zone != std::string::npos) {
            const auto suffix = value.substr(zone + 1);
            if (suffix.empty() || !std::all_of(suffix.begin(), suffix.end(),
                    [](unsigned char c) { return ascii_alnum(c) || c == '_' || c == '-'; }))
                return false;
            value.resize(zone);
        }
        return inet_pton(AF_INET6, value.c_str(), parsed.data()) == 1;
    }
    if (std::all_of(value.begin(), value.end(), [](unsigned char c) {
            return (c >= '0' && c <= '9') || c == '.';
        }))
        return inet_pton(AF_INET, value.c_str(), parsed.data()) == 1;
    std::size_t start = 0;
    while (start < value.size()) {
        auto end = value.find('.', start);
        if (end == std::string::npos) end = value.size();
        if (end == start || end - start > 63 || !ascii_alnum(value[start]) ||
            !ascii_alnum(value[end - 1]))
            return false;
        for (auto i = start; i < end; ++i)
            if (!ascii_alnum(value[i]) && value[i] != '-') return false;
        start = end + 1;
    }
    return value.back() != '.';
}

} // namespace acecode::utils
