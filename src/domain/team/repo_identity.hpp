#pragma once

// 仓库身份(spec capability: team-state-sync)。团队通过"同一 git 仓库"关联
// 到各队员设备上的本地项目:优先用规范化后的 git remote URL(跨设备天然一致),
// 没有 remote 的本地仓库退化为写进 git common dir 的唯一标识
// (.git/acecode-repo-id),这样同一台机器的多个检出共享同一个身份,而不同
// 机器上的本地仓库不会误匹配。

#include "team/team_meta.hpp"

#include <string>
#include <string_view>

namespace acecode::team {

// 规范化 git remote URL,输出 "<host>/<path>" 形式:
//   https://user:pass@GitHub.com/Org/Repo.git  -> github.com/Org/Repo
//   git@github.com:Org/Repo.git                -> github.com/Org/Repo
//   ssh://git@github.com:22/Org/Repo           -> github.com/Org/Repo
// 本地路径 / file:// / 空串返回空(不是可用于跨设备匹配的 remote)。
std::string normalize_git_remote_url(std::string_view url);

struct RepoIdentityResult {
    RepoIdentity identity;
    std::string error; // 空 = 成功;非空 = 不在 git 仓库/无法读取
    explicit operator bool() const { return error.empty(); }
};

// 解析 cwd 所属仓库的身份:
//   1) 有 remote(优先 origin,其次第一个 remote) -> {remote, 规范化 URL}
//   2) 无 remote -> 读取/生成 git common dir 下的 acecode-repo-id
//      -> {local-id, uuid}
// 生成的标识文件是原子写,内容为一行 uuid。
RepoIdentityResult resolve_repo_identity(const std::string& cwd);

// 显式提供的远程仓库 URL -> {remote, 规范化 URL}。本地路径 / file:// /
// 缺 host/path 等无法跨设备复用的写法返回错误 invalid_repo_url。
RepoIdentityResult repo_identity_from_url(const std::string& url);

// 团队是否关联了该仓库身份。
bool team_matches_repo(const TeamMeta& meta, const RepoIdentity& identity);

} // namespace acecode::team
