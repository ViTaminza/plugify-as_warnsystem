#include "warn_core.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <optional>
#include <utility>

namespace as_warnsystem {

// --- small helpers (the original's Split()/ToLower(), verbatim) -------------------

std::vector<std::string> Split(const std::string& input, char delim) {
	std::vector<std::string> out;
	std::string token;
	for (char c : input) {
		if (c == delim) {
			if (!token.empty()) out.push_back(token);
			token.clear();
		} else {
			token.push_back(c);
		}
	}
	if (!token.empty()) out.push_back(token);
	return out;
}

std::string ToLower(const std::string& s) {
	std::string out = s;
	std::transform(out.begin(), out.end(), out.begin(),
				   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
	return out;
}

std::string SteamToString(uint64_t id) {
	return std::to_string(id);
}

// --- configuration ------------------------------------------------------------------

namespace {

// The original LoadConfig() body (minus the KeyValues I/O), including the custom-category rule:
// a configured category is "custom" unless it is one of the two built-in ones.
void ApplyConfig(pisex::KeyValues& kv, WarnConfig& cfg, int iCoreServerID) {
	cfg.iMaxWarnings = kv.GetInt("max_warnings", cfg.iMaxWarnings);
	cfg.iBanDuration = kv.GetInt("ban_duration", cfg.iBanDuration);
	cfg.sBanReason = kv.GetString("ban_reason", cfg.sBanReason.c_str());
	cfg.iExpireSeconds = kv.GetInt("expire_seconds", cfg.iExpireSeconds);
	cfg.sCommand = kv.GetString("command", cfg.sCommand.c_str());
	cfg.sConsoleCommand = kv.GetString("console_command", cfg.sConsoleCommand.c_str());
	cfg.sPlayerCommand = kv.GetString("player_command", cfg.sPlayerCommand.c_str());
	cfg.sPlayerConsoleCommand = kv.GetString("player_console_command", cfg.sPlayerConsoleCommand.c_str());
	cfg.sWarnSound = kv.GetString("warn_sound", cfg.sWarnSound.c_str());
	cfg.sFlag = kv.GetString("flag", cfg.sFlag.c_str());
	cfg.bCustomCategory = kv.GetBool("custom_category", cfg.bCustomCategory);
	cfg.sCategory = kv.GetString("category", cfg.sCategory.c_str());

	int iRecentLimit = kv.GetInt("offline_recent_limit", cfg.iRecentLimit);
	if (iRecentLimit < 1) iRecentLimit = 1;
	if (iRecentLimit > 50) iRecentLimit = 50;
	cfg.iRecentLimit = iRecentLimit;

	const char* pszNotify = kv.GetString("notify_channels", "chat");
	const std::vector<std::string> vNotify = Split(pszNotify, '|');
	int iNotify = 0;
	for (const auto& token : vNotify) {
		const std::string sTok = ToLower(token);
		if (sTok == "chat") {
			iNotify |= NOTIFY_CHAT;
		} else if (sTok == "center") {
			iNotify |= NOTIFY_CENTER;
		}
	}
	if (iNotify == 0) iNotify = NOTIFY_CHAT;
	cfg.iNotifyChannels = iNotify;

	cfg.bAllowDuplicateReason = kv.GetBool("allow_duplicate_reason", cfg.bAllowDuplicateReason);

	if (!cfg.bCustomCategory) {
		if (cfg.sCategory != "players" && cfg.sCategory != "punishments") {
			cfg.bCustomCategory = true;
		}
	}

	const int iCfgServer = kv.GetInt("server_id", -1);
	if (iCfgServer == -1) {
		cfg.iServerID = iCoreServerID;
	} else {
		cfg.iServerID = iCfgServer;
	}

	if (pisex::KeyValues* pReasons = kv.FindKey("reasons")) {
		cfg.vReasons.clear();
		cfg.vReasonExpire.clear();
		// pisexcompat spells hl2sdk's FOR_EACH_TRUE_SUBKEY as FOR_EACH_SUBKEY (same semantics:
		// only children that are blocks).
		FOR_EACH_SUBKEY(pReasons, pSub) {
			const char* pszName = pSub->GetString("text", pSub->GetName());
			const int iExpire = pSub->GetInt("expire_seconds", cfg.iExpireSeconds);
			if (pszName && pszName[0]) {
				cfg.vReasons.push_back(pszName);
				cfg.vReasonExpire.push_back(iExpire);
			}
		}
	}

	if (cfg.vReasons.empty()) {
		cfg.vReasons.push_back("Warning");
		cfg.vReasonExpire.push_back(cfg.iExpireSeconds);
	}
}

} // namespace

void ParseConfig(pisex::KeyValues& kv, WarnConfig& cfg, int iCoreServerID) {
	ApplyConfig(kv, cfg, iCoreServerID);
}

bool LoadConfigFile(const std::string& path, WarnConfig& cfg, int iCoreServerID) {
	pisex::KeyValues kv("warnsystem");
	if (path.empty() || !kv.LoadFromFile(path)) {
		return false; // the caller logs and keeps the defaults, like the original
	}
	ApplyConfig(kv, cfg, iCoreServerID);
	return true;
}

int LoadCorePunishServerIDFile(const std::string& path) {
	pisex::KeyValues kv("admin_system");
	if (path.empty() || !kv.LoadFromFile(path)) {
		return -1; // the original returned -1 when core.ini was missing
	}
	return kv.GetInt("punish_server_id", -1);
}

// --- the "!warn" command line (the original handler's parsing, verbatim) -----------------------

bool ParseWarnCommand(const std::string& sContent, const WarnConfig& cfg, WarnCommandArgs& out) {
	std::string sInput = sContent;
	const std::size_t pos = sInput.find_first_not_of(' ');
	if (pos != std::string::npos) {
		sInput.erase(0, pos);
	} else {
		sInput.clear();
	}

	std::vector<std::string> vCmds = Split(cfg.sCommand, '|');
	{
		std::vector<std::string> vMore = Split(cfg.sConsoleCommand, '|');
		vCmds.insert(vCmds.end(), vMore.begin(), vMore.end());
	}
	std::vector<std::string> vCmdsNorm;
	for (auto c : vCmds) {
		if (!c.empty() && (c[0] == '!' || c[0] == '/')) c.erase(0, 1);
		vCmdsNorm.push_back(ToLower(c));
	}

	const std::vector<std::string> args = Split(sInput, ' ');

	const auto normalizeToken = [](std::string s) {
		if (!s.empty() && (s[0] == '!' || s[0] == '/')) s.erase(0, 1);
		return s;
	};

	std::size_t startIdx = 0;
	while (startIdx < args.size()) {
		const std::string candidate = ToLower(normalizeToken(args[startIdx]));
		bool isCmd = false;
		for (const auto& c : vCmdsNorm) {
			if (c == candidate) {
				isCmd = true;
				break;
			}
		}
		if (isCmd) {
			++startIdx;
			continue;
		}
		break;
	}

	if (startIdx >= args.size()) {
		return false;
	}

	out.targetSteam = std::strtoull(normalizeToken(args[startIdx]).c_str(), nullptr, 10);
	if (out.targetSteam == 0) {
		return false;
	}

	if (args.size() >= startIdx + 3) {
		out.reason = args[startIdx + 1];
		int iCustomExpire = std::atoi(args[startIdx + 2].c_str());
		if (iCustomExpire <= 0) iCustomExpire = cfg.iExpireSeconds;
		out.expireSeconds = iCustomExpire;
		out.hasReason = true;
	}
	return true;
}

// --- duration formatting ---------------------------------------------------------------

std::string FormatDurationShort(int seconds, const TranslateFn& tr) {
	if (seconds <= 0) {
		return tr("Menu_WarnExpireNever");
	}

	int s = seconds;
	const int days = s / 86400;
	s %= 86400;
	const int hours = s / 3600;
	s %= 3600;
	const int mins = s / 60;
	const int secs = s % 60;

	const bool useSecs = (days == 0 && hours == 0 && mins == 0);

	std::vector<std::string> parts;
	if (days) {
		parts.push_back(std::to_string(days) + " " + tr("Unit_Day"));
	}
	if (hours) {
		parts.push_back(std::to_string(hours) + " " + tr("Unit_Hour"));
	}
	if (mins) {
		parts.push_back(std::to_string(mins) + " " + tr("Unit_Min"));
	}
	if (useSecs) {
		parts.push_back(std::to_string(secs) + " " + tr("Unit_Sec"));
	}

	if (parts.size() > 2) {
		parts.resize(2);
	}

	std::string out;
	for (std::size_t i = 0; i < parts.size(); ++i) {
		if (i) out += ' ';
		out += parts[i];
	}
	return out;
}

std::string FormatExpireDuration(int seconds, const TranslateFn& tr) {
	if (seconds <= 0) {
		return tr("Menu_WarnExpireNever");
	}
	return FormatDurationShort(seconds, tr);
}

// --- store -------------------------------------------------------------------------------

std::vector<WarnEntry>* WarnStore::Find(const std::string& steam) {
	const auto it = m_storage.find(steam);
	return it == m_storage.end() ? nullptr : &it->second;
}

const std::vector<WarnEntry>* WarnStore::Find(const std::string& steam) const {
	const auto it = m_storage.find(steam);
	return it == m_storage.end() ? nullptr : &it->second;
}

void WarnStore::Replace(const std::string& steam, std::vector<WarnEntry> entries) {
	m_storage[steam] = std::move(entries);
}

int WarnStore::Count(uint64_t iSteamId) const {
	const std::vector<WarnEntry>* vec = Find(SteamToString(iSteamId));
	return vec == nullptr ? 0 : static_cast<int>(vec->size());
}

void WarnStore::PruneExpired(std::time_t now) {
	for (auto& pair : m_storage) {
		std::vector<WarnEntry>& vec = pair.second;
		vec.erase(std::remove_if(vec.begin(), vec.end(),
								 [now](const WarnEntry& w) { return w.tExpire > 0 && w.tExpire <= now; }),
				  vec.end());
	}
}

bool WarnStore::FindActiveReason(uint64_t iSteamId, const std::string& sReason, std::time_t now, WarnEntry& out) const {
	const std::vector<WarnEntry>* vec = Find(SteamToString(iSteamId));
	if (vec == nullptr) {
		return false;
	}
	for (auto rit = vec->rbegin(); rit != vec->rend(); ++rit) {
		const WarnEntry& w = *rit;
		if (w.sReason != sReason) continue;
		if (w.tExpire != 0 && w.tExpire <= now) continue;
		out = w;
		return true;
	}
	return false;
}

// --- database ---------------------------------------------------------------------------

namespace {

// The CREATE TABLE statement of the original (database.cpp-style), for the two drivers:
// only the types differ (AUTO_INCREMENT vs AUTOINCREMENT, TINYINT(1) vs INTEGER, ...). A table
// created by the Metamod variant is used as it is.
constexpr const char* kCreateTableMysql =
	"CREATE TABLE IF NOT EXISTS `as_warnsystem` ("
	"`id` INT PRIMARY KEY AUTO_INCREMENT,"
	"`server_id` INT DEFAULT 0,"
	"`player_steamid` VARCHAR(32),"
	"`player_name` VARCHAR(64),"
	"`admin_steamid` VARCHAR(32),"
	"`admin_name` VARCHAR(64),"
	"`reason` VARCHAR(128),"
	"`created_at` INT UNSIGNED,"
	"`expires_at` INT UNSIGNED,"
	"`removed` TINYINT(1) DEFAULT 0,"
	"`removed_at` INT UNSIGNED DEFAULT 0,"
	"`removed_by_steamid` VARCHAR(32) DEFAULT '',"
	"`removed_by_name` VARCHAR(64) DEFAULT ''"
	") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;";

constexpr const char* kCreateTableSqlite =
	"CREATE TABLE IF NOT EXISTS `as_warnsystem` ("
	"`id` INTEGER PRIMARY KEY AUTOINCREMENT,"
	"`server_id` INTEGER DEFAULT 0,"
	"`player_steamid` TEXT,"
	"`player_name` TEXT,"
	"`admin_steamid` TEXT,"
	"`admin_name` TEXT,"
	"`reason` TEXT,"
	"`created_at` INTEGER,"
	"`expires_at` INTEGER,"
	"`removed` INTEGER DEFAULT 0,"
	"`removed_at` INTEGER DEFAULT 0,"
	"`removed_by_steamid` TEXT DEFAULT '',"
	"`removed_by_name` TEXT DEFAULT ''"
	");";

} // namespace

WarnDatabase::WarnDatabase(std::unique_ptr<plgdb::Database> db) : m_db(std::move(db)) {}

WarnDatabase::~WarnDatabase() = default;

std::size_t WarnDatabase::Poll() {
	return m_db ? m_db->Poll() : 0;
}

void WarnDatabase::CreateSchema(const ErrorFn& onError) {
	if (!m_db) {
		return;
	}
	const bool mysql = m_db->GetDriver() == plgdb::Driver::MySQL;
	m_db->Query(mysql ? kCreateTableMysql : kCreateTableSqlite, {},
				[onError](const plgdb::Result& result) {
					if (!result.Ok() && onError) {
						onError("as_warnsystem: CREATE TABLE failed: " + result.Error());
					}
				});

	// The original's AddColumnIfMissing() calls (columns added by a later version of the module).
	AddColumnIfMissing("removed_by_steamid", "VARCHAR(32) DEFAULT ''", "removed_at", onError);
	AddColumnIfMissing("removed_by_name", "VARCHAR(64) DEFAULT ''", "removed_by_steamid", onError);
}

void WarnDatabase::AddColumnIfMissing(const std::string& column, const std::string& definition,
									  const std::string& afterColumn, const ErrorFn& onError) {
	if (!m_db || column.empty() || definition.empty()) {
		return;
	}
	const bool mysql = m_db->GetDriver() == plgdb::Driver::MySQL;
	// The column name is bound; the (fixed, not user-supplied) table name is literal.
	const std::string check = mysql
		? "SELECT 1 FROM information_schema.columns WHERE table_schema = DATABASE() AND table_name = 'as_warnsystem' AND column_name = ? LIMIT 1;"
		: "SELECT 1 FROM pragma_table_info('as_warnsystem') WHERE name = ? LIMIT 1;";

	plgdb::Database* db = m_db.get();
	db->Query(check, {column},
			  [db, mysql, column, definition, afterColumn, onError](const plgdb::Result& result) {
				  if (!result.Ok()) {
					  if (onError) onError("as_warnsystem: column check for " + column + " failed: " + result.Error());
					  return;
				  }
				  if (result.RowCount() != 0) {
					  return; // the column exists
				  }
				  std::string alter;
				  if (mysql) {
					  alter = "ALTER TABLE `as_warnsystem` ADD COLUMN `" + column + "` " + definition +
							  (afterColumn.empty() ? std::string() : " AFTER `" + afterColumn + "`") + ";";
				  } else {
					  alter = "ALTER TABLE `as_warnsystem` ADD COLUMN `" + column + "` " + definition + ";";
				  }
				  db->Query(alter, {}, [column, onError](const plgdb::Result& alterResult) {
					  if (!alterResult.Ok() && onError) {
						  onError("as_warnsystem: failed to add column " + column + ": " + alterResult.Error());
					  }
				  });
			  });
}

void WarnDatabase::InsertWarning(int iServerID, const std::string& steamId, const std::string& playerName,
								 const WarnEntry& entry, const std::function<void(uint64_t)>& onInsertId,
								 const ErrorFn& onError) {
	if (!m_db) {
		return;
	}
	// The original's INSERT (the table name is fixed in the original as well); the values are
	// bound instead of the original's Escape()+interpolation. admin_steamid is a VARCHAR in the
	// original schema and was written as a quoted number - it stays a string here.
	static constexpr const char* kInsert =
		"INSERT INTO `as_warnsystem` (`server_id`,`player_steamid`,`player_name`,`admin_steamid`,`admin_name`,"
		"`reason`,`created_at`,`expires_at`,`removed`,`removed_at`) VALUES (?,?,?,?,?,?,?,?,0,0);";

	plgdb::Params params;
	params.emplace_back(static_cast<int64_t>(iServerID));
	params.emplace_back(steamId);
	params.emplace_back(playerName);
	params.emplace_back(std::to_string(entry.iAdminSteam));
	params.emplace_back(entry.sAdminName);
	params.emplace_back(entry.sReason);
	params.emplace_back(static_cast<int64_t>(entry.tCreated));
	params.emplace_back(static_cast<int64_t>(entry.tExpire));

	m_db->Query(kInsert, std::move(params), [onInsertId, onError](const plgdb::Result& result) {
		if (!result.Ok()) {
			if (onError) onError("as_warnsystem: INSERT failed: " + result.Error());
			if (onInsertId) onInsertId(0);
			return;
		}
		if (onInsertId) onInsertId(result.InsertId());
	});
}

void WarnDatabase::MarkRemoved(const std::string& steamId, int warnId, uint64_t iAdminSteam,
							   const std::string& adminName, const ErrorFn& onError) {
	if (!m_db) {
		return;
	}
	const bool mysql = m_db->GetDriver() == plgdb::Driver::MySQL;

	// removed_at was UNIX_TIMESTAMP() in the original; the current unix time is bound instead so
	// both drivers work (a sub-second difference to the database clock on the Metamod variant).
	const int64_t now = static_cast<int64_t>(std::time(nullptr));
	plgdb::Params params;
	params.emplace_back(now);
	params.emplace_back(std::to_string(iAdminSteam));
	params.emplace_back(adminName);

	std::string sql;
	if (warnId > 0) {
		sql = "UPDATE `as_warnsystem` SET `removed`=1,`removed_at`=?,`removed_by_steamid`=?,`removed_by_name`=? WHERE `id`=?;";
		params.emplace_back(static_cast<int64_t>(warnId));
	} else if (mysql) {
		sql = "UPDATE `as_warnsystem` SET `removed`=1,`removed_at`=?,`removed_by_steamid`=?,`removed_by_name`=? "
			  "WHERE `player_steamid`=? AND `removed`=0 ORDER BY `id` DESC LIMIT 1;";
		params.emplace_back(steamId);
	} else {
		// SQLite only supports ORDER BY/LIMIT in UPDATE with a special build flag; the id of the
		// newest active row is selected instead (same row as the MySQL statement).
		sql = "UPDATE `as_warnsystem` SET `removed`=1,`removed_at`=?,`removed_by_steamid`=?,`removed_by_name`=? "
			  "WHERE `id`=(SELECT `id` FROM `as_warnsystem` WHERE `player_steamid`=? AND `removed`=0 ORDER BY `id` DESC LIMIT 1);";
		params.emplace_back(steamId);
	}

	m_db->Query(sql, std::move(params), [onError](const plgdb::Result& result) {
		if (!result.Ok() && onError) {
			onError("as_warnsystem: UPDATE (mark removed) failed: " + result.Error());
		}
	});
}

void WarnDatabase::LoadWarnings(uint64_t iSteamId, const std::function<void(std::vector<WarnEntry>)>& onLoaded,
								const ErrorFn& onError) {
	if (!m_db) {
		return;
	}
	// The original selected the row's columns by name and read them by field name; the order is
	// the same here.
	static constexpr const char* kSelect =
		"SELECT `id`,`reason`,`created_at`,`expires_at`,`admin_steamid`,`admin_name` FROM `as_warnsystem` "
		"WHERE `player_steamid`=? AND `removed`=0 AND (`expires_at`=0 OR `expires_at`>?);";

	const int64_t now = static_cast<int64_t>(std::time(nullptr));
	m_db->Query(kSelect, {std::to_string(iSteamId), now},
				[onLoaded, onError](const plgdb::Result& result) {
					if (!result.Ok()) {
						if (onError) onError("as_warnsystem: SELECT failed: " + result.Error());
						return;
					}
					std::vector<WarnEntry> vec;
					const std::optional<std::size_t> colId = result.ColumnIndex("id");
					const std::optional<std::size_t> colReason = result.ColumnIndex("reason");
					const std::optional<std::size_t> colCreated = result.ColumnIndex("created_at");
					const std::optional<std::size_t> colExpires = result.ColumnIndex("expires_at");
					const std::optional<std::size_t> colAdminSteam = result.ColumnIndex("admin_steamid");
					const std::optional<std::size_t> colAdminName = result.ColumnIndex("admin_name");
					for (std::size_t row = 0; row < result.RowCount(); ++row) {
						WarnEntry e;
						if (colId) e.iID = static_cast<int>(result.GetInt(row, *colId));
						if (colReason) e.sReason = result.GetString(row, *colReason);
						if (colCreated) e.tCreated = static_cast<std::time_t>(result.GetInt(row, *colCreated));
						if (colExpires) e.tExpire = static_cast<std::time_t>(result.GetInt(row, *colExpires));
						if (colAdminSteam) {
							// The column is a VARCHAR in the schema; a numeric value is rendered
							// the same way (std::to_string) as the original wrote it.
							e.iAdminSteam = static_cast<uint64_t>(result.GetInt(row, *colAdminSteam));
						}
						if (colAdminName) e.sAdminName = result.GetString(row, *colAdminName);
						vec.push_back(std::move(e));
					}
					if (onLoaded) onLoaded(std::move(vec));
				});
}

} // namespace as_warnsystem
