// Tests for plugins/as_warnsystem/src/warn_core.hpp (engine-independent core of the ported
// "[AS] WarnSystem" module).
//
//   * warnsystem.ini/core.ini parsing against the real files of the live server
//     (ref/pisex/live-configs-admin_system; the KeyValues resolver is driven exactly like the
//     plugin does it: SetPluginName("admin_system") + the Metamod path spelling),
//   * the in-memory variants (custom category, notify channels, reasons block, clamping),
//   * the duration formatting of the menus and the store rules (count, active reason, pruning),
//   * the SQL layer: CREATE/INSERT/SELECT/UPDATE round trip against SQLite ":memory:" always
//     and against MySQL when MYSQL_HOST is set (test.sh provides MYSQL_HOST/MYSQL_PORT/
//     MYSQL_USER/MYSQL_PASSWORD/MYSQL_DATABASE). Rows created in MySQL are deleted at the end.

#include <plg/plugin.hpp>

#include <pisex/keyvalues.hpp>
#include <pisex/paths.hpp>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <functional>
#include <map>
#include <memory>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "warn_core.hpp"

// A real plugin gets these from PLUGIFY_PLUGIN; the test leaves them null (also the state the
// code has to survive: no plugify dirs, no logger). The path resolver is driven through
// pisex::SetPluginName/SetConfigDir instead.
namespace plg {
GetBaseDirFn GetBaseDir{nullptr};
GetExtensionsDirFn GetExtensionsDir{nullptr};
GetConfigsDirFn GetConfigsDir{nullptr};
GetDataDirFn GetDataDir{nullptr};
GetLogsDirFn GetLogsDir{nullptr};
GetCacheDirFn GetCacheDir{nullptr};
IsLoadedFn IsLoaded{nullptr};
LogFn Log{nullptr};
BeginZoneFn BeginZone{nullptr};
EndZoneFn EndZone{nullptr};
} // namespace plg

namespace {

int g_checks = 0;
int g_failures = 0;
std::string_view g_section = "?";

void Section(std::string_view name) {
	g_section = name;
	std::printf("-- %.*s\n", static_cast<int>(name.size()), name.data());
}

#define CHECK(expr)                                                                            \
	do {                                                                                       \
		++g_checks;                                                                            \
		if (!(expr)) {                                                                         \
			++g_failures;                                                                      \
			std::printf("FAIL [%.*s] %s:%d: %s\n", static_cast<int>(g_section.size()),         \
						g_section.data(), __FILE__, __LINE__, #expr);                          \
		}                                                                                      \
	} while (false)

#define CHECK_EQ_STR(actual, expected)                                                         \
	do {                                                                                       \
		++g_checks;                                                                            \
		const std::string a_ = (actual);                                                       \
		const std::string e_ = (expected);                                                     \
		if (a_ != e_) {                                                                        \
			++g_failures;                                                                      \
			std::printf("FAIL [%.*s] %s:%d: got \"%s\", expected \"%s\"\n",                    \
						static_cast<int>(g_section.size()), g_section.data(), __FILE__,       \
						__LINE__, a_.c_str(), e_.c_str());                                     \
		}                                                                                      \
	} while (false)

#define CHECK_EQ_INT(actual, expected)                                                         \
	do {                                                                                       \
		++g_checks;                                                                            \
		const long long a_ = static_cast<long long>(actual);                                   \
		const long long e_ = static_cast<long long>(expected);                                 \
		if (a_ != e_) {                                                                        \
			++g_failures;                                                                      \
			std::printf("FAIL [%.*s] %s:%d: got %lld, expected %lld\n",                        \
						static_cast<int>(g_section.size()), g_section.data(), __FILE__,       \
						__LINE__, a_, e_);                                                     \
		}                                                                                      \
	} while (false)

// The plugin's IAdminApi::GetTranslation(), with fixed unit texts.
const char* Translate(const char* key) {
	static const std::map<std::string, std::string, std::less<>> kPhrases = {
		{"Menu_WarnExpireNever", "never"},
		{"Unit_Day", "d"},
		{"Unit_Hour", "h"},
		{"Unit_Min", "m"},
		{"Unit_Sec", "s"},
	};
	const auto it = kPhrases.find(key);
	return it == kPhrases.end() ? key : it->second.c_str();
}

as_warnsystem::TranslateFn Translator() {
	return [](const char* key) { return Translate(key); };
}

// Polls the owner (plgdb::Database or WarnDatabase) until `done` or the timeout.
template <typename Owner>
bool WaitFor(Owner& owner, const std::function<bool()>& done, int timeoutMs = 10000) {
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
	for (;;) {
		owner.Poll();
		if (done()) {
			return true;
		}
		if (std::chrono::steady_clock::now() >= deadline) {
			owner.Poll();
			return done();
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(2));
	}
}

// --- configuration (the real live files) ------------------------------------------------

void TestRealConfig() {
	Section("config: live warnsystem.ini + core.ini");

	// Exactly the resolution the plugin uses (pisex::Init("admin_system") sets the name; the
	// path spellings are the Metamod ones of the original module).
	pisex::SetPluginName("admin_system");
	pisex::SetConfigDir(AS_WARNSYSTEM_TEST_DATA_DIR);

	const int iCoreServerID =
		as_warnsystem::LoadCorePunishServerIDFile("addons/configs/admin_system/core.ini");
	CHECK_EQ_INT(iCoreServerID, 1); // the live core.ini

	as_warnsystem::WarnConfig cfg;
	CHECK(as_warnsystem::LoadConfigFile("addons/configs/admin_system/warnsystem.ini", cfg, iCoreServerID));

	CHECK_EQ_INT(cfg.iMaxWarnings, 3);
	CHECK_EQ_INT(cfg.iBanDuration, 1440);
	CHECK_EQ_STR(cfg.sBanReason, "Достигнут лимит предупреждений");
	CHECK_EQ_INT(cfg.iExpireSeconds, 604800);
	CHECK_EQ_INT(cfg.iServerID, 1); // server_id -1 -> core.ini punish_server_id
	CHECK_EQ_STR(cfg.sCommand, "!warn|/warn");
	CHECK_EQ_STR(cfg.sConsoleCommand, "mm_warn");
	CHECK_EQ_STR(cfg.sPlayerCommand, "!warns|/warns");
	CHECK_EQ_STR(cfg.sPlayerConsoleCommand, "mm_warns");
	CHECK_EQ_STR(cfg.sFlag, "@admin/warn");
	CHECK(!cfg.bAllowDuplicateReason);
	CHECK_EQ_INT(cfg.iRecentLimit, 10);
	CHECK_EQ_STR(cfg.sWarnSound, "sounds/buttons/blip2.vsnd");
	CHECK_EQ_INT(cfg.iNotifyChannels, as_warnsystem::NOTIFY_CHAT | as_warnsystem::NOTIFY_CENTER);
	CHECK_EQ_STR(cfg.sCategory, "punishments");
	CHECK(!cfg.bCustomCategory); // "punishments" is a built-in category

	CHECK_EQ_INT(cfg.vReasons.size(), 3);
	CHECK_EQ_STR(cfg.vReasons[0], "Флуд / спам");
	CHECK_EQ_INT(cfg.vReasonExpire[0], 604800);
	CHECK_EQ_STR(cfg.vReasons[1], "Токсичное поведение");
	CHECK_EQ_INT(cfg.vReasonExpire[1], 259200);
	CHECK_EQ_STR(cfg.vReasons[2], "Гриферство");
	CHECK_EQ_INT(cfg.vReasonExpire[2], 86400);

	// Missing file: defaults, like the original ("using defaults").
	as_warnsystem::WarnConfig defaults;
	CHECK(!as_warnsystem::LoadConfigFile("addons/configs/admin_system/no_such_warnsystem.ini", defaults, 7));
	CHECK_EQ_INT(defaults.iMaxWarnings, 3);
	CHECK_EQ_INT(defaults.iBanDuration, 1440);
	CHECK_EQ_STR(defaults.sBanReason, "Reached max warnings");
	CHECK_EQ_INT(defaults.iExpireSeconds, 604800);
	CHECK_EQ_STR(defaults.sCommand, "!warn");
	CHECK_EQ_STR(defaults.sConsoleCommand, "warn");
	CHECK_EQ_STR(defaults.sPlayerCommand, "!warns");
	CHECK_EQ_STR(defaults.sFlag, "@admin/warn");
	CHECK(defaults.bAllowDuplicateReason);
	CHECK_EQ_STR(defaults.sCategory, "players");
	CHECK_EQ_INT(defaults.iNotifyChannels, as_warnsystem::NOTIFY_CHAT);
	CHECK_EQ_INT(defaults.vReasons.size(), 1);
	CHECK_EQ_STR(defaults.vReasons[0], "Warning");
	CHECK_EQ_INT(defaults.vReasonExpire[0], 604800);
	CHECK_EQ_INT(defaults.iServerID, 0); // no key, no core id used

	// A missing core.ini yields -1 (the original returned -1 as well).
	CHECK_EQ_INT(as_warnsystem::LoadCorePunishServerIDFile("addons/configs/admin_system/no_core.ini"), -1);
}

// --- configuration (in-memory variants) ---------------------------------------------------

void TestConfigVariants() {
	Section("config: custom category / notify / reasons");

	{
		pisex::KeyValues kv("warnsystem");
		CHECK(kv.LoadFromBuffer("\"warnsystem\" {"
								"  \"category\" \"mycat\""
								"  \"notify_channels\" \"center|CHAT\""
								"  \"offline_recent_limit\" \"99\""
								"  \"server_id\" \"-5\""
								"}"));
		as_warnsystem::WarnConfig cfg;
		as_warnsystem::ParseConfig(kv, cfg, 4);
		CHECK(cfg.bCustomCategory);
		CHECK_EQ_STR(cfg.sCategory, "mycat");
		CHECK_EQ_INT(cfg.iNotifyChannels, as_warnsystem::NOTIFY_CHAT | as_warnsystem::NOTIFY_CENTER);
		CHECK_EQ_INT(cfg.iRecentLimit, 50); // clamped to 50 like the original
		CHECK_EQ_INT(cfg.iServerID, -5);
	}

	{
		pisex::KeyValues kv("warnsystem");
		CHECK(kv.LoadFromBuffer("\"warnsystem\" {"
								"  \"notify_channels\" \"bogus\""
								"  \"offline_recent_limit\" \"0\""
								"  \"server_id\" \"-1\""
								"  \"category\" \"players\""
								"}"));
		as_warnsystem::WarnConfig cfg;
		as_warnsystem::ParseConfig(kv, cfg, 9);
		CHECK_EQ_INT(cfg.iNotifyChannels, as_warnsystem::NOTIFY_CHAT); // fallback
		CHECK_EQ_INT(cfg.iRecentLimit, 1);                             // clamped to 1
		CHECK_EQ_INT(cfg.iServerID, 9);                                // -1 -> core id
		CHECK(!cfg.bCustomCategory);                                   // built-in category
	}

	{
		pisex::KeyValues kv("warnsystem");
		CHECK(kv.LoadFromBuffer("\"warnsystem\" {"
								"  \"reasons\" {"
								"    \"1\" { \"text\" \"A\" \"expire_seconds\" \"10\" }"
								"    \"2\" { \"text\" \"B\" }"
								"    \"3\" { \"text\" \"\" \"expire_seconds\" \"5\" }"
								"  }"
								"}"));
		as_warnsystem::WarnConfig cfg; // iExpireSeconds default 604800
		as_warnsystem::ParseConfig(kv, cfg, -1);
		CHECK_EQ_INT(cfg.vReasons.size(), 2); // the empty text entry is skipped
		CHECK_EQ_STR(cfg.vReasons[0], "A");
		CHECK_EQ_INT(cfg.vReasonExpire[0], 10);
		CHECK_EQ_STR(cfg.vReasons[1], "B");
		CHECK_EQ_INT(cfg.vReasonExpire[1], 604800); // key absent -> iExpireSeconds default
		CHECK_EQ_INT(cfg.iServerID, -1);
	}
}

// --- duration formatting --------------------------------------------------------------------

void TestDurationFormatting() {
	Section("duration formatting");

	CHECK_EQ_STR(as_warnsystem::FormatDurationShort(0, Translator()), "never");
	CHECK_EQ_STR(as_warnsystem::FormatDurationShort(-5, Translator()), "never");
	CHECK_EQ_STR(as_warnsystem::FormatExpireDuration(0, Translator()), "never");
	CHECK_EQ_STR(as_warnsystem::FormatDurationShort(45, Translator()), "45 s");
	CHECK_EQ_STR(as_warnsystem::FormatDurationShort(60, Translator()), "1 m");
	CHECK_EQ_STR(as_warnsystem::FormatDurationShort(3661, Translator()), "1 h 1 m");
	CHECK_EQ_STR(as_warnsystem::FormatDurationShort(86400, Translator()), "1 d");
	CHECK_EQ_STR(as_warnsystem::FormatDurationShort(90061, Translator()), "1 d 1 h"); // max two units
	CHECK_EQ_STR(as_warnsystem::FormatDurationShort(604800, Translator()), "7 d");
	CHECK_EQ_STR(as_warnsystem::FormatExpireDuration(259200, Translator()), "3 d");
}

// --- the "!warn" command line ------------------------------------------------------------------

void TestWarnCommandParsing() {
	Section("!warn command parsing");

	as_warnsystem::WarnConfig cfg; // defaults: sCommand "!warn", sConsoleCommand "warn", expire 604800
	cfg.sCommand = "!warn|/warn";
	cfg.sConsoleCommand = "mm_warn";

	as_warnsystem::WarnCommandArgs out;

	// No steamid: the player menu (the original's startIdx >= args.size() / targetSteam == 0).
	CHECK(!as_warnsystem::ParseWarnCommand("!warn", cfg, out));
	CHECK(!as_warnsystem::ParseWarnCommand("!warn   ", cfg, out));
	CHECK(!as_warnsystem::ParseWarnCommand("", cfg, out));
	CHECK(!as_warnsystem::ParseWarnCommand("!warn abc", cfg, out));
	CHECK(!as_warnsystem::ParseWarnCommand("!warn 0", cfg, out));

	// Steamid only: open the reason menu.
	CHECK(as_warnsystem::ParseWarnCommand("!warn 76561198000000001", cfg, out));
	CHECK_EQ_INT(out.targetSteam, 76561198000000001ULL);
	CHECK(!out.hasReason);

	// The chat spelling, the console spelling and repeated command tokens are all stripped.
	out = {};
	CHECK(as_warnsystem::ParseWarnCommand("/warn 76561198000000001", cfg, out));
	CHECK_EQ_INT(out.targetSteam, 76561198000000001ULL);

	out = {};
	CHECK(as_warnsystem::ParseWarnCommand("mm_warn 76561198000000001", cfg, out));
	CHECK_EQ_INT(out.targetSteam, 76561198000000001ULL);

	out = {};
	CHECK(as_warnsystem::ParseWarnCommand("!warn !warn 76561198000000001 x 60", cfg, out));
	CHECK_EQ_INT(out.targetSteam, 76561198000000001ULL);
	CHECK(out.hasReason);
	CHECK_EQ_STR(out.reason, "x");
	CHECK_EQ_INT(out.expireSeconds, 60);

	// A steamid with the trigger in front (the original's normalizeToken).
	out = {};
	CHECK(as_warnsystem::ParseWarnCommand("!warn !76561198000000001", cfg, out));
	CHECK_EQ_INT(out.targetSteam, 76561198000000001ULL);

	// "steamid reason seconds" issues the warning with the given expiry.
	out = {};
	CHECK(as_warnsystem::ParseWarnCommand("mm_warn 76561198000000001 Flud 3600", cfg, out));
	CHECK_EQ_INT(out.targetSteam, 76561198000000001ULL);
	CHECK(out.hasReason);
	CHECK_EQ_STR(out.reason, "Flud");
	CHECK_EQ_INT(out.expireSeconds, 3600);

	// A missing / non-positive / non-numeric count falls back to expire_seconds (the original).
	out = {};
	CHECK(as_warnsystem::ParseWarnCommand("!warn 76561198000000001 reason abc", cfg, out));
	CHECK(out.hasReason);
	CHECK_EQ_INT(out.expireSeconds, cfg.iExpireSeconds);
	out = {};
	CHECK(as_warnsystem::ParseWarnCommand("!warn 76561198000000001 reason -5", cfg, out));
	CHECK_EQ_INT(out.expireSeconds, cfg.iExpireSeconds);
	out = {};
	CHECK(as_warnsystem::ParseWarnCommand("!warn 76561198000000001 some reason 3600", cfg, out));
	CHECK_EQ_STR(out.reason, "some");                      // only the first token is the reason
	CHECK_EQ_INT(out.expireSeconds, cfg.iExpireSeconds);   // atoi("reason") == 0
}

// --- store ------------------------------------------------------------------------------------

void TestStore() {
	Section("warning store");

	as_warnsystem::WarnStore store;
	const uint64_t steam = 76561198000000042ULL;
	const std::string key = as_warnsystem::SteamToString(steam);
	const std::time_t now = 1700000000;

	CHECK_EQ_INT(store.Count(steam), 0);
	CHECK(store.Find(key) == nullptr);

	as_warnsystem::WarnEntry a;
	a.iID = 11;
	a.sReason = "A";
	a.tExpire = 0; // never
	as_warnsystem::WarnEntry b;
	b.iID = 12;
	b.sReason = "A";
	b.tExpire = now - 5; // already expired
	as_warnsystem::WarnEntry c;
	c.iID = 13;
	c.sReason = "A";
	c.tExpire = now + 100;

	store[key].push_back(a);
	store[key].push_back(b);
	store[key].push_back(c);
	CHECK_EQ_INT(store.Count(steam), 3);

	// The newest active entry with that reason wins (b is skipped - expired).
	as_warnsystem::WarnEntry found;
	CHECK(store.FindActiveReason(steam, "A", now, found));
	CHECK_EQ_INT(found.iID, 13);
	CHECK(!store.FindActiveReason(steam, "B", now, found));

	store.PruneExpired(now);
	CHECK_EQ_INT(store.Count(steam), 2); // the expired one is gone, tExpire == 0 survives
	CHECK(store.FindActiveReason(steam, "A", now, found));
	CHECK_EQ_INT(found.iID, 13);

	// Replace() is the DB load path.
	std::vector<as_warnsystem::WarnEntry> loaded(1);
	loaded[0].iID = 99;
	store.Replace(key, std::move(loaded));
	CHECK_EQ_INT(store.Count(steam), 1);
	CHECK_EQ_INT((*store.Find(key))[0].iID, 99);

	store.PruneExpired(now);
	CHECK_EQ_INT(store.Count(steam), 1); // tExpire == 0 never expires
}

// --- database ------------------------------------------------------------------------------------

struct DbFixture {
	as_warnsystem::WarnDatabase db;
	std::vector<std::string> errors;
	std::vector<std::string> createdSteamIds;

	explicit DbFixture(std::unique_ptr<plgdb::Database> database)
		: db(std::move(database)) {}

	std::function<void(std::string_view)> OnError() {
		return [this](std::string_view message) { errors.push_back(std::string(message)); };
	}

	// INSERT + wait for the id; the id also proves the CREATE queued before it finished.
	uint64_t Insert(const std::string& steam, const std::string& name, const std::string& reason,
					std::time_t created, std::time_t expire, uint64_t adminSteam = 76561198000000001ULL,
					const std::string& adminName = "Admin") {
		as_warnsystem::WarnEntry entry;
		entry.sReason = reason;
		entry.tCreated = created;
		entry.tExpire = expire;
		entry.iAdminSteam = adminSteam;
		entry.sAdminName = adminName;

		uint64_t id = 0;
		db.InsertWarning(1, steam, name, entry, [&id](uint64_t value) { id = value; }, OnError());
		createdSteamIds.push_back(steam);
		if (!WaitFor(db, [&id] { return id != 0; })) {
			return 0;
		}
		return id;
	}

	std::vector<as_warnsystem::WarnEntry> Load(uint64_t steam) {
		std::vector<as_warnsystem::WarnEntry> result;
		bool done = false;
		db.LoadWarnings(steam, [&](std::vector<as_warnsystem::WarnEntry> entries) {
			result = std::move(entries);
			done = true;
		}, OnError());
		WaitFor(db, [&done] { return done; });
		return result;
	}
};

void TestDatabase(const plgdb::Config& config, const std::string& what, uint64_t steamBase, bool cleanup) {
	std::printf("== db: %s\n", what.c_str());

	std::unique_ptr<plgdb::Database> database = plgdb::Database::Create(config);
	CHECK(database != nullptr);
	if (!database) {
		return;
	}
	DbFixture fx(std::move(database));

	// The plugin calls CreateSchema at start and again from the core's OnCoreLoaded callback.
	fx.db.CreateSchema(fx.OnError());
	fx.db.CreateSchema(fx.OnError());

	const std::string steam = as_warnsystem::SteamToString(steamBase + 1);
	const std::time_t now = std::time(nullptr);

	// --- insert + load ---
	const uint64_t id1 = fx.Insert(steam, "Player 'quoted'; --", "Флуд / спам", now, 0);
	CHECK(id1 != 0);
	CHECK_EQ_INT(fx.errors.size(), 0);

	std::vector<as_warnsystem::WarnEntry> loaded = fx.Load(steamBase + 1);
	CHECK_EQ_INT(loaded.size(), 1);
	if (!loaded.empty()) {
		CHECK_EQ_INT(loaded[0].iID, id1);
		CHECK_EQ_STR(loaded[0].sReason, "Флуд / спам");
		CHECK_EQ_INT(loaded[0].tCreated, now);
		CHECK_EQ_INT(loaded[0].tExpire, 0);
		CHECK_EQ_INT(loaded[0].iAdminSteam, 76561198000000001ULL);
		CHECK_EQ_STR(loaded[0].sAdminName, "Admin");
	}

	// --- a second warning, then "remove the newest active one" (warnId 0) ---
	const uint64_t id2 = fx.Insert(steam, "Player", "B", now, now + 3600);
	CHECK(id2 != 0);
	CHECK(id1 != id2);
	fx.db.MarkRemoved(steam, 0, 76561198000000002ULL, "Remover", fx.OnError());
	loaded = fx.Load(steamBase + 1); // the SELECT is queued after the UPDATE
	CHECK_EQ_INT(loaded.size(), 1);
	if (!loaded.empty()) {
		CHECK_EQ_INT(loaded[0].iID, id1); // warning 2 is gone, warning 1 stays
	}

	// --- remove by id ---
	fx.db.MarkRemoved(steam, static_cast<int>(id1), 76561198000000002ULL, "Remover", fx.OnError());
	loaded = fx.Load(steamBase + 1);
	CHECK_EQ_INT(loaded.size(), 0);

	// --- expired rows are not returned (the original's expires_at > UNIX_TIMESTAMP()) ---
	const uint64_t id3 = fx.Insert(steam, "Player", "C", now - 7200, now - 3600);
	CHECK(id3 != 0);
	loaded = fx.Load(steamBase + 1);
	CHECK_EQ_INT(loaded.size(), 0);

	// The row is still in the table (only the SELECT filters it) and marked with the values.
	if (fx.db.Handle() != nullptr) {
		const plgdb::Result raw = fx.db.Handle()->QuerySync(
			"SELECT `removed`,`reason` FROM `as_warnsystem` WHERE `player_steamid`=? AND `id`=?",
			{steam, static_cast<int64_t>(id3)});
		CHECK(raw.Ok());
		CHECK_EQ_INT(raw.RowCount(), 1);
		if (raw.RowCount() == 1) {
			CHECK_EQ_INT(raw.GetInt(0, 0), 0);
			CHECK_EQ_STR(raw.GetString(0, 1), "C");
		}
	}

	// A warning of another player must not show up (the WHERE of the original).
	const uint64_t idOther = fx.Insert(as_warnsystem::SteamToString(steamBase + 2), "Other", "D", now, 0);
	CHECK(idOther != 0);
	loaded = fx.Load(steamBase + 3);
	CHECK_EQ_INT(loaded.size(), 0);

	// --- the legacy table of an older module version (without removed_by_*) is migrated ---
	// CreateSchema() checks the two columns (information_schema / PRAGMA) and ALTERs them in;
	// everything below is FIFO-ordered behind those statements.
	if (fx.db.Handle() != nullptr) {
		const bool mysql = fx.db.Driver() == plgdb::Driver::MySQL;
		const std::string legacyCreate = mysql
			? "CREATE TABLE `as_warnsystem` (`id` INT PRIMARY KEY AUTO_INCREMENT, `server_id` INT DEFAULT 0, "
			  "`player_steamid` VARCHAR(32), `player_name` VARCHAR(64), `admin_steamid` VARCHAR(32), "
			  "`admin_name` VARCHAR(64), `reason` VARCHAR(128), `created_at` INT UNSIGNED, `expires_at` INT UNSIGNED, "
			  "`removed` TINYINT(1) DEFAULT 0, `removed_at` INT UNSIGNED DEFAULT 0);"
			: "CREATE TABLE `as_warnsystem` (`id` INTEGER PRIMARY KEY AUTOINCREMENT, `server_id` INTEGER DEFAULT 0, "
			  "`player_steamid` TEXT, `player_name` TEXT, `admin_steamid` TEXT, `admin_name` TEXT, `reason` TEXT, "
			  "`created_at` INTEGER, `expires_at` INTEGER, `removed` INTEGER DEFAULT 0, `removed_at` INTEGER DEFAULT 0);";

		CHECK(fx.db.Handle()->QuerySync("DROP TABLE IF EXISTS `as_warnsystem`;", {}).Ok());
		CHECK(fx.db.Handle()->QuerySync(legacyCreate, {}).Ok());

		fx.db.CreateSchema(fx.OnError());

		// The column checks report through Poll() (game thread) and only then queue their ALTER;
		// poll until a SELECT of the migrated columns goes through.
		bool columnsReady = false;
		for (int attempt = 0; attempt < 2000 && !columnsReady; ++attempt) {
			fx.db.Poll();
			const plgdb::Result columns = fx.db.Handle()->QuerySync(
				"SELECT `removed_by_steamid`, `removed_by_name` FROM `as_warnsystem` LIMIT 1;", {});
			columnsReady = columns.Ok();
			if (!columnsReady) {
				std::this_thread::sleep_for(std::chrono::milliseconds(2));
			}
		}
		CHECK(columnsReady);

		const uint64_t idLegacy = fx.Insert(steam, "Legacy", "L", now, 0);
		CHECK(idLegacy != 0);
		fx.db.MarkRemoved(steam, static_cast<int>(idLegacy), 76561198000000002ULL, "Remover", fx.OnError());
		const plgdb::Result row = fx.db.Handle()->QuerySync(
			"SELECT `removed`,`removed_at`,`removed_by_steamid`,`removed_by_name` FROM `as_warnsystem` WHERE `id`=?;",
			{static_cast<int64_t>(idLegacy)});
		CHECK(row.Ok());
		CHECK_EQ_INT(row.RowCount(), 1);
		if (row.RowCount() == 1) {
			CHECK_EQ_INT(row.GetInt(0, 0), 1);
			CHECK(row.GetInt(0, 1) > 0);
			CHECK_EQ_STR(row.GetString(0, 2), "76561198000000002");
			CHECK_EQ_STR(row.GetString(0, 3), "Remover");
		}
	}

	CHECK_EQ_INT(fx.errors.size(), 0);
	for (const std::string& message : fx.errors) {
		std::printf("   db error: %s\n", message.c_str());
	}

	// --- cleanup (the shared MySQL database should not grow) ---
	if (cleanup && fx.db.Handle() != nullptr) {
		for (const std::string& s : fx.createdSteamIds) {
			fx.db.Handle()->QuerySync("DELETE FROM `as_warnsystem` WHERE `player_steamid`=?", {s});
		}
		fx.db.Handle()->QuerySync("DELETE FROM `as_warnsystem` WHERE `player_steamid`=?",
								  {as_warnsystem::SteamToString(steamBase + 3)});
	}
}

plgdb::Config SqliteConfig() {
	plgdb::Config config;
	config.driver = plgdb::Driver::SQLite;
	config.sqlitePath = ":memory:";
	return config;
}

bool MySqlConfig(plgdb::Config& config, std::string& error) {
	const char* host = std::getenv("MYSQL_HOST");
	if (!host || !*host) {
		return false;
	}
	config.driver = plgdb::Driver::MySQL;
	config.host = host;
	config.user = std::getenv("MYSQL_USER") ? std::getenv("MYSQL_USER") : "";
	config.password = std::getenv("MYSQL_PASSWORD") ? std::getenv("MYSQL_PASSWORD") : "";
	config.database = std::getenv("MYSQL_DATABASE") ? std::getenv("MYSQL_DATABASE") : "";
	const char* port = std::getenv("MYSQL_PORT");
	if (port && *port) {
		try {
			const int value = std::stoi(port);
			if (value < 1 || value > 65535) {
				error = "MYSQL_PORT out of range";
				return false;
			}
			config.port = static_cast<uint16_t>(value);
		} catch (const std::exception&) {
			error = "MYSQL_PORT is not a number";
			return false;
		}
	}
	return true;
}

} // namespace

int main() {
	std::printf("as_warnsystem_core_tests\n");

	std::random_device device;
	std::mt19937_64 rng(device());
	// A fresh block of steam ids per run so repeated runs (and the shared MySQL database) never
	// interfere; the rows of this run are deleted at the end.
	const uint64_t base = 76561199000000000ULL + (rng() % 100000ULL) * 10ULL;

	TestRealConfig();
	TestConfigVariants();
	TestDurationFormatting();
	TestWarnCommandParsing();
	TestStore();

	TestDatabase(SqliteConfig(), "sqlite (:memory:)", base, /*cleanup=*/false);

	plgdb::Config mysql;
	std::string error;
	if (MySqlConfig(mysql, error)) {
		std::printf("== mysql: %s:%u/%s\n", mysql.host.c_str(), mysql.port, mysql.database.c_str());
		TestDatabase(mysql, "mysql", base + 100000ULL, /*cleanup=*/true);
	} else if (!error.empty()) {
		std::printf("FATAL: %s\n", error.c_str());
		++g_failures;
	} else {
		std::printf("== mysql: skipped (MYSQL_HOST not set)\n");
	}

	std::printf("%d checks, %d failures\n", g_checks, g_failures);
	return g_failures == 0 ? 0 : 1;
}
