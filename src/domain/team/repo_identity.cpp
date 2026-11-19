#include "team/repo_identity.hpp"

#include "utils/atomic_file.hpp"
#include "utils/text.hpp"
#include "utils/utf8_path.hpp"
#include "utils/uuid.hpp"
#include "worktree/worktree_manager.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

namespace acecode::team {
namespace {

// 去 scheme 与凭据,留下 host/path;SCP 形式(host:path)与 ssh://host/path 统一。
std::string strip_scheme_and_user(std::string_view url) {
    std::string text(url);
    const auto scheme_end = text.find("://");
    std::string remainder;
    if (scheme_end != std::string::npos) {
        const std::string scheme = text.substr(0, scheme_end);
        // 只保留可跨设备复用的网络协议;file 形式不是 remote。
        if (scheme != "http" && scheme != "https" && scheme != "ssh" &&
            scheme != "git")
            return {};
        remainder = text.substr(scheme_end + 3);
        // 去掉 user[:pass]@
        const auto at = remainder.find('@');
        const auto slash = remainder.find('/');
        if (at != std::string::npos && (slash == std::string::npos || at < slash))
            remainder = remainder.substr(at + 1);
        // 去掉显式端口(host:port/...)
        const auto host_end = remainder.find('/');
        if (host_end != std::string::npos) {
            const std::string host_port = remainder.substr(0, host_end);
            const auto colon = host_port.rfind(':');
            if (colon != std::string::npos) {
                std::string host = host_port.substr(0, colon);
                remainder = host + remainder.substr(host_end);
            }
        }
        return remainder;
    }
    // SCP 形式:user@host:org/repo.git
    const auto at = text.find('@');
    if (at != std::string::npos) text = text.substr(at + 1);
    const auto colon = text.find(':');
    if (colon == std::string::npos) return {}; // 无 scheme 无冒号 -> 本地路径
    if (text.find('/') != std::string::npos &&
        text.find('/') < colon)
        return {}; // /abs/path:xxx 之类,按本地路径处理
    return text.substr(0, colon) + "/" + text.substr(colon + 1);
}

} // namespace

std::string normalize_git_remote_url(std::string_view url) {
    std::string value = utils::trim_ascii_copy(std::string(url));
    if (value.empty()) return {};
    // scp 形式里 host 后是冒号;含 Windows 盘符(C:\...)的按本地路径处理。
    if (value.size() >= 2 && value[1] == ':') return {};
    std::string remainder = strip_scheme_and_user(value);
    if (remainder.empty()) return {};
    // 末尾斜杠与 .git 后缀
    while (!remainder.empty() && (remainder.back() == '/' || remainder.back() == '\\'))
        remainder.pop_back();
    const std::string suffix = ".git";
    if (remainder.size() > suffix.size() &&
        remainder.compare(remainder.size() - suffix.size(), suffix.size(), suffix) == 0)
        remainder.resize(remainder.size() - suffix.size());
    const auto slash = remainder.find('/');
    if (slash == std::string::npos || slash == 0 ||
        slash + 1 >= remainder.size())
        return {};
    std::string host = utils::ascii_lower(remainder.substr(0, slash));
    std::string path = remainder.substr(slash + 1);
    while (!path.empty() && path.front() == '/') path.erase(path.begin());
    if (host.empty() || path.empty()) return {};
    return host + "/" + path;
}

RepoIdentityResult resolve_repo_identity(const std::string& cwd) {
    RepoIdentityResult result;
    const std::string root = worktree::find_git_root(cwd);
    if (root.empty()) {
        result.error = "not_git_repo";
        return result;
    }

    auto remote_out = worktree::run_git({"remote", "get-url", "origin"}, root, 10000);
    std::string raw_remote;
    if (remote_out.ok()) {
        raw_remote = utils::trim_ascii_copy(remote_out.out);
    } else {
        auto list = worktree::run_git({"remote"}, root, 10000);
        if (list.ok()) {
            std::istringstream stream(list.out);
            std::string first;
            std::getline(stream, first);
            first = utils::trim_ascii_copy(first);
            if (!first.empty()) {
                auto url = worktree::run_git({"remote", "get-url", first}, root, 10000);
                if (url.ok()) raw_remote = utils::trim_ascii_copy(url.out);
            }
        }
    }
    const std::string normalized = normalize_git_remote_url(raw_remote);
    if (!normalized.empty()) {
        result.identity.kind = "remote";
        result.identity.value = normalized;
        return result;
    }

    // 无 remote:在 git common dir 放一个稳定标识(worktree 与主仓共享)。
    auto common = worktree::run_git({"rev-parse", "--git-common-dir"}, root, 10000);
    fs::path common_dir = path_from_utf8(root);
    if (common.ok()) {
        std::string common_rel = utils::trim_ascii_copy(common.out);
        if (!common_rel.empty()) {
            fs::path candidate = path_from_utf8(common_rel);
            common_dir = candidate.is_absolute() ? candidate : path_from_utf8(root) / candidate;
        }
    }
    const fs::path id_path = common_dir / "acecode-repo-id";
    std::error_code ec;
    std::string id;
    if (fs::exists(id_path, ec)) {
        std::ifstream input(id_path);
        std::ostringstream buffer;
        buffer << input.rdbuf();
        id = utils::trim_ascii_copy(buffer.str());
    }
    if (id.empty()) {
        id = generate_uuid();
        if (!atomic_write_file(path_to_utf8(id_path), id + "\n")) {
            result.error = "repo_id_write_failed";
            return result;
        }
    }
    result.identity.kind = "local-id";
    result.identity.value = id;
    return result;
}

bool team_matches_repo(const TeamMeta& meta, const RepoIdentity& identity) {
    if (identity.kind.empty() || identity.value.empty()) return false;
    return std::any_of(meta.repos.begin(), meta.repos.end(),
        [&](const RepoIdentity& candidate) {
            return candidate.kind == identity.kind && candidate.value == identity.value;
        });
}

} // namespace acecode::team
