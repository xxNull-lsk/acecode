// 覆盖 src/domain/team/repo_identity.cpp。仓库身份是团队会话准入的判定依据:
// 归一化必须让同一远端的不同写法相等,本地路径不能误匹配,无 remote 的仓库
// 用 git common dir 里的稳定标识兜底。测试直接起临时 git 仓库。

#include <gtest/gtest.h>

#include "team/repo_identity.hpp"
#include "utils/utf8_path.hpp"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;
using acecode::team::normalize_git_remote_url;
using acecode::team::RepoIdentity;
using acecode::team::resolve_repo_identity;
using acecode::team::team_matches_repo;
using acecode::team::TeamMeta;

namespace {

struct TempDir {
    fs::path root;
    TempDir() {
        root = fs::temp_directory_path() /
               ("repo_identity_test_" +
                std::to_string(std::chrono::steady_clock::now()
                                   .time_since_epoch()
                                   .count()));
        fs::remove_all(root);
        fs::create_directories(root);
    }
    ~TempDir() { fs::remove_all(root); }
};

int run(const std::string& cmd, const fs::path& cwd) {
    const std::string full = "cd '" + acecode::path_to_utf8(cwd) + "' && " + cmd;
    return std::system(full.c_str());
}

} // namespace

// 场景: 同一远端的不同写法归一化到同一身份
TEST(RepoIdentity, NormalizesRemoteUrlForms) {
    const std::string expected = "github.com/Org/Repo";
    EXPECT_EQ(normalize_git_remote_url("https://github.com/Org/Repo.git"), expected);
    EXPECT_EQ(normalize_git_remote_url("https://user:pass@GitHub.com/Org/Repo.git"), expected);
    EXPECT_EQ(normalize_git_remote_url("git@github.com:Org/Repo.git"), expected);
    EXPECT_EQ(normalize_git_remote_url("ssh://git@github.com:22/Org/Repo.git"), expected);
    EXPECT_EQ(normalize_git_remote_url("https://github.com/Org/Repo/"), expected);
}

// 场景: 本地路径与非法 URL 不产生远端身份
TEST(RepoIdentity, LocalPathsAreNotRemotes) {
    EXPECT_EQ(normalize_git_remote_url(""), "");
    EXPECT_EQ(normalize_git_remote_url("/home/a/repo"), "");
    EXPECT_EQ(normalize_git_remote_url("file:///home/a/repo"), "");
    EXPECT_EQ(normalize_git_remote_url("C:\\work\\repo"), "");
    EXPECT_EQ(normalize_git_remote_url("https://github.com"), "");
}

// 场景: 非 git 目录解析失败
TEST(RepoIdentity, NonGitDirectoryFails) {
    TempDir temp;
    const auto result = resolve_repo_identity(acecode::path_to_utf8(temp.root));
    EXPECT_FALSE(result);
    EXPECT_EQ(result.error, "not_git_repo");
}

// 场景: 有 remote 的仓库解析为 remote 身份
TEST(RepoIdentity, RemoteRepoIdentity) {
    TempDir temp;
    ASSERT_EQ(run("git init -q .", temp.root), 0);
    ASSERT_EQ(run("git remote add origin git@github.com:Org/Repo.git", temp.root), 0);

    const auto result = resolve_repo_identity(acecode::path_to_utf8(temp.root));
    ASSERT_TRUE(result) << result.error;
    EXPECT_EQ(result.identity.kind, "remote");
    EXPECT_EQ(result.identity.value, "github.com/Org/Repo");
}

// 场景: 无 remote 的本地仓库生成稳定 local-id,再次解析得到同一个值
TEST(RepoIdentity, LocalRepoFallbackIdIsStable) {
    TempDir temp;
    ASSERT_EQ(run("git init -q .", temp.root), 0);

    const auto first = resolve_repo_identity(acecode::path_to_utf8(temp.root));
    ASSERT_TRUE(first) << first.error;
    EXPECT_EQ(first.identity.kind, "local-id");
    EXPECT_FALSE(first.identity.value.empty());

    const auto second = resolve_repo_identity(acecode::path_to_utf8(temp.root));
    ASSERT_TRUE(second) << second.error;
    EXPECT_EQ(second.identity.value, first.identity.value);
    EXPECT_TRUE(fs::exists(temp.root / ".git" / "acecode-repo-id"));
}

// 场景: 团队关联匹配按 kind+value 精确比较
TEST(RepoIdentity, TeamMatchesRepo) {
    TeamMeta meta;
    meta.repos.push_back({"remote", "github.com/Org/Repo"});

    EXPECT_TRUE(team_matches_repo(meta, RepoIdentity{"remote", "github.com/Org/Repo"}));
    EXPECT_FALSE(team_matches_repo(meta, RepoIdentity{"remote", "github.com/Org/Other"}));
    // local-id 与 remote 同名也不匹配
    EXPECT_FALSE(team_matches_repo(meta, RepoIdentity{"local-id", "github.com/Org/Repo"}));
    EXPECT_FALSE(team_matches_repo(meta, RepoIdentity{}));
}
