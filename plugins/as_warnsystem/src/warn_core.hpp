#pragma once
// Engine-independent core of the as_warnsystem plugin (no s2sdk, no plugify, no engine).
//
// Port of the Metamod module "[AS] WarnSystem" (as_warnsystem.cpp):
//   * the warnsystem.ini model (same keys, same defaults, same semantics as the original
//     LoadConfig()/LoadCorePunishServerID()-pair),
//   * the in-memory warning store and the "recently disconnected" bookkeeping rules,
//   * the duration formatting of the menus ("1 d 2 h" style, at most two units),
//   * the asynchronous database layer over plgdb (same `as_warnsystem` table and same
//     statements as the original, with bound parameters instead of string interpolation).
//
// Everything here is testable without an engine; the plugin (src/plugin.cpp) owns the s2sdk /
// admin_system glue. Threading: every method runs on the game thread; the plgdb callbacks are
// executed by WarnDatabase::Poll() (called from OnPluginUpdate).

#include <cstdint>
#include <ctime>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <plgdb/db.hpp>
#include <pisex/keyvalues.hpp>

namespace as_warnsystem {

// --- configuration --------------------------------------------------------------

// The original's NotifyChannel flags (notify_channels = chat|center).
enum NotifyChannel {
	NOTIFY_CHAT = 1 << 0,
	NOTIFY_CENTER = 1 << 1
};

// The original's WarnConfig: the member initialisers are the built-in defaults that are used when
// warnsystem.ini is missing or a key is absent.
struct WarnConfig {
	int iMaxWarnings = 3;
	int iBanDuration = 1440;
	std::string sBanReason = "Reached max warnings";
	int iExpireSeconds = 604800;
	std::vector<std::string> vReasons = {"Warning"};
	std::vector<int> vReasonExpire = {604800};
	bool bUseDB = true;
	bool bAllowDuplicateReason = true;
	int iServerID = 0;
	// offline_recent_limit (1..50): how many recently disconnected players the offline menus keep
	// (the original's g_MaxRecentDisconnects).
	int iRecentLimit = 10;
	std::string sCommand = "!warn";
	std::string sConsoleCommand = "warn";
	std::string sPlayerCommand = "!warns";
	std::string sPlayerConsoleCommand;
	std::string sWarnSound;
	int iNotifyChannels = NOTIFY_CHAT;
	std::string sFlag = "@admin/warn";
	bool bCustomCategory = false;
	std::string sCategory = "players";
};

// The original's Split()/ToLower() helpers (same behaviour, including "empty tokens are dropped").
std::vector<std::string> Split(const std::string& input, char delim);
std::string ToLower(const std::string& s);

// Parses the warnsystem.ini tree into cfg (keys, defaults and the custom-category rule of the
// original LoadConfig()). `iCoreServerID` is the punish_server_id taken from core.ini.
void ParseConfig(pisex::KeyValues& kv, WarnConfig& cfg, int iCoreServerID);

// Reads warnsystem.ini and parses it. Returns false when the file cannot be read (the caller
// logs and keeps the defaults, like the original).
bool LoadConfigFile(const std::string& path, WarnConfig& cfg, int iCoreServerID);

// The original's LoadCorePunishServerID(): punish_server_id of core.ini, -1 when the file or the
// key is missing.
int LoadCorePunishServerIDFile(const std::string& path);

// --- the "!warn" command line -----------------------------------------------------------------

// The original command handler strips every leading token that looks like one of the registered
// command names ("!warn", "/warn", "warn", "mm_warn", case-insensitive, any number of them) and
// then reads "<steamid64> [reason] [seconds]".
struct WarnCommandArgs {
	uint64_t targetSteam = 0;
	std::string reason;    // only when a full "<steamid> <reason> <seconds>" triple was given
	int expireSeconds = 0; // <= 0 in the input means "config default"
	bool hasReason = false;
};

// Returns false when there is no steamid in the line (the original opened the player menu then).
bool ParseWarnCommand(const std::string& sContent, const WarnConfig& cfg, WarnCommandArgs& out);

// --- warnings --------------------------------------------------------------------

// The original's WarnEntry.
struct WarnEntry {
	int iID = 0;
	std::string sReason;
	std::time_t tCreated = 0;
	std::time_t tExpire = 0;
	uint64_t iAdminSteam = 0;
	std::string sAdminName;
};

// The original's RecentDisconnect (the offline warning list).
struct RecentDisconnect {
	uint64_t iSteam = 0;
	std::string sName;
	std::time_t tDisconnected = 0;
};

std::string SteamToString(uint64_t id);

// Translation lookup of the plugin (IAdminApi::GetTranslation()).
using TranslateFn = std::function<const char*(const char*)>;

// The original's FormatDurationShort()/FormatExpireDuration(): days/hours/minutes/seconds
// (at most two units), "Menu_WarnExpireNever" for 0/negative.
std::string FormatDurationShort(int seconds, const TranslateFn& tr);
std::string FormatExpireDuration(int seconds, const TranslateFn& tr);

// The original's g_WarnStorage: warnings per steamid string ("the steamid as text").
class WarnStore {
public:
	// The original's g_WarnStorage[steam] (creates the empty vector on first use).
	std::vector<WarnEntry>& operator[](const std::string& steam) { return m_storage[steam]; }

	// nullptr when the steamid is unknown (the original's find()/end() pair).
	std::vector<WarnEntry>* Find(const std::string& steam);
	const std::vector<WarnEntry>* Find(const std::string& steam) const;

	// The original's LoadWarningsFromDB: replaces the whole stored vector.
	void Replace(const std::string& steam, std::vector<WarnEntry> entries);

	// The original's GetWarningCount().
	int Count(uint64_t iSteamId) const;

	// The original's PruneExpiredWarnings(): drops entries with tExpire > 0 && tExpire <= now.
	void PruneExpired(std::time_t now);

	// The original's GetExistingReasonInfo(): the newest entry with that reason that is not
	// expired (tExpire == 0 means "never expires"). Returns false when there is none.
	bool FindActiveReason(uint64_t iSteamId, const std::string& sReason, std::time_t now, WarnEntry& out) const;

	const std::map<std::string, std::vector<WarnEntry>>& All() const { return m_storage; }

private:
	std::map<std::string, std::vector<WarnEntry>> m_storage;
};

// --- database ----------------------------------------------------------------------
//
// Async layer over plgdb with the original's statements. `as_warnsystem` is a fixed table name in
// the original (the core.ini prefix is not applied to it) - kept so Metamod and Plugify servers
// share the table.
class WarnDatabase {
public:
	// Frees the DB (and joins its worker) when the plugin ends.
	~WarnDatabase();

	explicit WarnDatabase(std::unique_ptr<plgdb::Database> db);

	WarnDatabase(const WarnDatabase&) = delete;
	WarnDatabase& operator=(const WarnDatabase&) = delete;

	bool Ready() const { return m_db != nullptr; }
	plgdb::Driver Driver() const { return m_db ? m_db->GetDriver() : plgdb::Driver::SQLite; }
	// Direct access for tests/diagnostics (e.g. QuerySync verification); the plugin uses the
	// typed methods above.
	plgdb::Database* Handle() const { return m_db.get(); }

	using ErrorFn = std::function<void(std::string_view)>;

	// The original's CreateTables(): CREATE TABLE IF NOT EXISTS `as_warnsystem` plus the two
	// removed_by_* columns added to tables created by older versions. Asynchronous and
	// FIFO-ordered before any insert queued afterwards.
	void CreateSchema(const ErrorFn& onError);

	// The original's SaveWarningToDB(): INSERT of one warning; onInsertId receives the new row id
	// (0 when the statement failed).
	void InsertWarning(int iServerID, const std::string& steamId, const std::string& playerName, const WarnEntry& entry,
					   const std::function<void(uint64_t)>& onInsertId, const ErrorFn& onError);

	// The original's MarkWarningRemovedInDB(): warnId > 0 updates that row, 0 updates the newest
	// row still active for the steamid.
	void MarkRemoved(const std::string& steamId, int warnId, uint64_t iAdminSteam, const std::string& adminName,
					 const ErrorFn& onError);

	// The original's LoadWarningsFromDB(): the active (unremoved, unexpired) warnings of a
	// steamid. onLoaded is only called for a successful query (the original left the in-memory
	// storage untouched when the query object was null, i.e. on error).
	void LoadWarnings(uint64_t iSteamId, const std::function<void(std::vector<WarnEntry>)>& onLoaded, const ErrorFn& onError);

	// Runs the completed plgdb callbacks on the calling (game) thread. Returns how many ran.
	std::size_t Poll();

private:
	void AddColumnIfMissing(const std::string& column, const std::string& definition, const std::string& afterColumn,
							const ErrorFn& onError);

	std::unique_ptr<plgdb::Database> m_db;
};

} // namespace as_warnsystem
