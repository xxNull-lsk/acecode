#include "team/team_store.hpp"

#include "utils/atomic_file.hpp"
#include "utils/utf8_path.hpp"

#include <nlohmann/json.hpp>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

namespace acecode::team {
namespace {

using nlohmann::json;

json invite_to_json(const InviteLedgerRecord& record) {
    json j;
    j["token"] = record.token;
    if (record.issued_at) j["issued_at"] = record.issued_at;
    if (record.expires_at) j["expires_at"] = record.expires_at;
    if (record.consumed_at) j["consumed_at"] = record.consumed_at;
    if (!record.consumed_by.empty()) j["consumed_by"] = record.consumed_by;
    return j;
}

// token 是账本的主键,缺失或为空都按损坏处理 — fail-closed。
bool invite_from_json(const json& j, InviteLedgerRecord& out) {
    if (!j.is_object()) return false;
    auto token = j.find("token");
    if (token == j.end() || !token->is_string() || token->get<std::string>().empty())
        return false;
    out.token = token->get<std::string>();
    out.issued_at = j.value("issued_at", std::int64_t{0});
    out.expires_at = j.value("expires_at", std::int64_t{0});
    out.consumed_at = j.value("consumed_at", std::int64_t{0});
    out.consumed_by = j.value("consumed_by", std::string{});
    return true;
}

std::string read_file(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input.is_open()) return {};
    std::ostringstream buffer;
    buffer << input.rdbuf();
    return buffer.str();
}

} // namespace

TeamStore::TeamStore(std::string data_dir, std::string team_id)
    : root_(path_from_utf8(data_dir) / "team" / path_from_utf8(team_id)),
      invites_path_(root_ / "invite_ledger.json") {
}

bool TeamStore::save_meta(const TeamMeta& meta) const {
    std::error_code ec;
    fs::create_directories(root_, ec);
    if (ec) return false;
    const std::string content = team_meta_to_json(meta).dump();
    return atomic_write_file(path_to_utf8(root_ / "team.json"), content);
}

std::optional<TeamMeta> TeamStore::load_meta() const {
    const fs::path meta_path = root_ / "team.json";
    std::error_code ec;
    if (!fs::exists(meta_path, ec)) return std::nullopt;
    const std::string content = read_file(meta_path);
    if (content.empty()) return std::nullopt;
    json parsed;
    try {
        parsed = json::parse(content);
    } catch (...) {
        return std::nullopt;
    }
    if (!parsed.is_object()) return std::nullopt;
    return team_meta_from_json(parsed);
}

InviteLedgerLoad TeamStore::load_invites() const {
    InviteLedgerLoad result;
    std::error_code ec;
    if (!fs::exists(invites_path_, ec)) return result;
    result.has_file = true;
    const std::string content = read_file(invites_path_);
    if (content.empty()) {
        result.error = "empty_invite_ledger";
        return result;
    }
    json parsed;
    try {
        parsed = json::parse(content);
    } catch (...) {
        result.error = "invalid_invite_ledger_json";
        return result;
    }
    if (!parsed.is_array()) {
        result.error = "invalid_invite_ledger_shape";
        return result;
    }
    for (const auto& entry : parsed) {
        InviteLedgerRecord record;
        if (!invite_from_json(entry, record)) {
            result.error = "invalid_invite_record";
            result.records.clear();
            return result;
        }
        result.records.push_back(std::move(record));
    }
    return result;
}

bool TeamStore::save_invites(const std::vector<InviteLedgerRecord>& records) const {
    std::error_code ec;
    fs::create_directories(root_, ec);
    if (ec) return false;
    json array = json::array();
    for (const auto& record : records) array.push_back(invite_to_json(record));
    return atomic_write_file(path_to_utf8(invites_path_), array.dump());
}

std::optional<InviteLedgerRecord> TeamStore::find_invite(const std::string& token) const {
    if (token.empty()) return std::nullopt;
    const InviteLedgerLoad loaded = load_invites();
    if (!loaded.error.empty()) return std::nullopt;
    for (const auto& record : loaded.records) {
        if (record.token == token) return record;
    }
    return std::nullopt;
}

bool TeamStore::consume_invite(const std::string& token, const std::string& consumer_guid,
                               std::int64_t now) const {
    if (token.empty()) return false;
    const InviteLedgerLoad loaded = load_invites();
    if (!loaded.error.empty()) return false;
    std::vector<InviteLedgerRecord> records = loaded.records;
    for (auto& record : records) {
        if (record.token != token) continue;
        if (!invite_usable(record, now)) return false; // 已过期或已消费,幂等拒绝
        record.consumed_at = now;
        record.consumed_by = consumer_guid;
        return save_invites(records);
    }
    return false;
}

fs::path TeamStore::replica_dir(const std::string& session_id) const {
    const fs::path dir = root_ / "sessions" / path_from_utf8(session_id);
    std::error_code ec;
    fs::create_directories(dir, ec);
    return dir;
}

} // namespace acecode::team
