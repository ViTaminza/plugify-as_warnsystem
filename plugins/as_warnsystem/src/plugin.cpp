// as_warnsystem — Plugify C++ port of the Pisex Metamod:Source module "[AS] WarnSystem"
// (as_warnsystem.cpp).
//
// The module gives admins a warning system: warnings with reasons and expiries, an automatic ban
// when the limit is reached, an offline warning list for recently disconnected players, a
// per-player "my warnings" menu and the warning administration menu inside the admin menu.
//
// Kept identical to the original:
//   * the warnsystem.ini keys, defaults and the reasons block (src/warn_core.cpp),
//   * the commands: "!warn|/warn" chat + "mm_warn" console (warn), "!warns|/warns" +
//     "mm_warns" (own warnings), including the original's parsing of steamid/reason/seconds,
//   * all menus (player list, actions, reason select/confirm, remove warning, my warnings,
//     detail, offline player/reason/confirm) with the same items, phrases and navigation,
//   * the same `as_warnsystem` database table and the same statements (INSERT/SELECT/UPDATE,
//     "removed" flag, removed_by_*), so Metamod and Plugify servers share the table,
//   * the admin API usage: HasPermission, IsAdmin/GetImmunityType/GetAdminImmunity (immunity
//     blocks), AddPlayerPunishment/AddOfflinePlayerPunishment, GetTranslation, the admin menu
//     registration (RegisterCategory/RegisterItem) and ShowAdminLastCategoryMenu.
//
// Ported to s2sdk/pisexcompat/plgdb (deliberate, documented differences):
//   * GetMySQLConnection() is gone: the module opens its own plgdb connection with the settings
//     of GetDatabaseConfig() (API.md §3.4) and binds its SQL parameters instead of the original's
//     Escape()+format strings. The table stays `as_warnsystem` (the original did not apply the
//     core.ini prefix to it either).
//   * the warning sound is played through s2sdk::ClientCommand (checked binding).
//   * OnClientAuthenticated is known not to fire reliably, so the warning
//     load of the original's HookOnClientAuthorized is backed by a once-per-second reconcile
//     (the same pattern as plugins/perm_connector): every connected, authenticated, non-bot slot
//     whose warnings have not been loaded for the current steamid gets them loaded once.
//   * RunDuration formatting, menu texts and phrases are unchanged; the only formatting change
//     is `%lu` where the original had `%lld` for a uint64_t (same decimal output).
//   * CreateTables() runs at plugin start (the module owns its connection now) and, like the
//     original, again from the admin core's OnCoreLoaded callback (idempotent).
//
// Nothing touches a client in OnPluginStart, no exception escapes into s2sdk, every s2sdk call
// goes through the guards of pisexcompat / the null-binding checks of s2sdk, and the plugin never
// returns Failed (that would stop every Plugify plugin on the server).

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <deque>
#include <exception>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <plg/plugin.hpp>
#include <plg/string.hpp>
#include <plugin_export.h>

#include <s2sdk/s2sdk.hpp>

#include <pisex/pisex.hpp>

#include <admin_api.hpp>

#include "warn_core.hpp"

namespace {

using as_warnsystem::FormatDurationShort;
using as_warnsystem::FormatExpireDuration;
using as_warnsystem::RecentDisconnect;
using as_warnsystem::Split;
using as_warnsystem::SteamToString;
using as_warnsystem::WarnCommandArgs;
using as_warnsystem::WarnConfig;
using as_warnsystem::WarnDatabase;
using as_warnsystem::WarnEntry;
using as_warnsystem::WarnStore;

constexpr int kMaxSlots = 64;                     // engine player slots 0..63
constexpr float kPruneInterval = 5.0f;            // the original's 5 second PruneExpiredWarnings timer
constexpr auto kReconcileInterval = std::chrono::seconds(1);
constexpr const char* kLogTag = "[as_warnsystem]";

admin_system::AdminApi* g_pAdmin = nullptr;

WarnConfig g_Config;
WarnStore g_WarnStorage;
std::deque<RecentDisconnect> g_RecentDisconnects;
size_t g_MaxRecentDisconnects = 10;
std::unique_ptr<WarnDatabase> g_pDB;

// Per-slot steamid whose warnings have already been requested (the original loaded them once per
// authorization; this marker makes the once-per-second reconcile do the same).
std::array<uint64_t, kMaxSlots> g_SlotLoadedSteam{};
bool g_ReconcileEnabled = false;
std::chrono::milliseconds g_SinceReconcile{0};

// --- logging -----------------------------------------------------------------------------

void LogMessage(std::string_view message, plg::Severity severity) {
	if (plg::Log) {
		plg::Log(message, severity, plg::source_location::current());
	}
}

void LogInfo(std::string_view message) {
	LogMessage(message, plg::Severity::Info);
}

void LogWarning(std::string_view message) {
	LogMessage(message, plg::Severity::Warning);
}

void LogError(std::string_view message) {
	LogMessage(message, plg::Severity::Error);
}

void LogDatabaseError(std::string_view message) {
	LogError(message);
}

// --- message templates (the original's ChatMsg/ChatMsgAll/NotifyMsg/NotifyMsgAll) -------------

as_warnsystem::TranslateFn Translator() {
	return [](const char* key) { return g_pAdmin->GetTranslation(key); };
}

// Formats `format` with the arguments; the zero-argument case never calls printf with a
// non-literal format (the translations are the format strings - like the original).
template <typename... Args>
std::string FormatText(const char* format, Args... args) {
	if constexpr (sizeof...(Args) == 0) {
		return std::string(format);
	} else {
		return pisex::FormatString(format, args...);
	}
}

template <typename... Args>
void ChatMsg(int iSlot, const char* szKey, Args... args) {
	const char* szPrefix = g_pAdmin->GetTranslation("Chat_Prefix");
	const char* szBody = g_pAdmin->GetTranslation(szKey);
	const std::string text = FormatText(" %s%s", szPrefix, szBody);
	pisex::PrintToChat(iSlot, "%s", FormatText(text.c_str(), args...).c_str());
}

template <typename... Args>
void ChatMsgAll(const char* szKey, Args... args) {
	const char* szPrefix = g_pAdmin->GetTranslation("Chat_Prefix");
	const char* szBody = g_pAdmin->GetTranslation(szKey);
	const std::string text = FormatText(" %s%s", szPrefix, szBody);
	pisex::PrintToChatAll("%s", FormatText(text.c_str(), args...).c_str());
}

template <typename... Args>
void NotifyMsg(int iSlot, const char* szKey, Args... args) {
	const char* szBody = g_pAdmin->GetTranslation(szKey);
	if (g_Config.iNotifyChannels & as_warnsystem::NOTIFY_CHAT) {
		const char* szPrefix = g_pAdmin->GetTranslation("Chat_Prefix");
		const std::string text = FormatText(" %s%s", szPrefix, szBody);
		pisex::PrintToChat(iSlot, "%s", FormatText(text.c_str(), args...).c_str());
	}
	if (g_Config.iNotifyChannels & as_warnsystem::NOTIFY_CENTER) {
		pisex::PrintToCenter(iSlot, "%s", FormatText(szBody, args...).c_str());
	}
}

template <typename... Args>
void NotifyMsgAll(const char* szKey, Args... args) {
	const char* szBody = g_pAdmin->GetTranslation(szKey);
	if (g_Config.iNotifyChannels & as_warnsystem::NOTIFY_CHAT) {
		const char* szPrefix = g_pAdmin->GetTranslation("Chat_Prefix");
		const std::string text = FormatText(" %s%s", szPrefix, szBody);
		pisex::PrintToChatAll("%s", FormatText(text.c_str(), args...).c_str());
	}
	if (g_Config.iNotifyChannels & as_warnsystem::NOTIFY_CENTER) {
		pisex::PrintToCenterAll("%s", FormatText(szBody, args...).c_str());
	}
}

// --- immunity (the original's CanAdminPunish/CheckImmunityOrNotify) ---------------------------

bool CanAdminPunish(int iAdminSlot, int iTargetSlot) {
	if (iAdminSlot < 0 || iTargetSlot < 0) return true;
	const int immunityType = g_pAdmin->GetImmunityType();
	if (immunityType == 0) return true;
	if (!g_pAdmin->IsAdmin(iTargetSlot)) return true;
	const int adminImm = g_pAdmin->GetAdminImmunity(iAdminSlot);
	const int targetImm = g_pAdmin->GetAdminImmunity(iTargetSlot);

	switch (immunityType) {
	case 1:
		return adminImm >= targetImm;
	case 2:
		return adminImm > targetImm;
	case 3:
		return false;
	default:
		return true;
	}
}

bool CheckImmunityOrNotify(int iAdminSlot, int iTargetSlot) {
	if (CanAdminPunish(iAdminSlot, iTargetSlot)) return true;
	ChatMsg(iAdminSlot, "Chat_ImmunityBlocked");
	return false;
}

// --- expiring texts (the original's GetExistingReasonInfo) ------------------------------------

bool GetExistingReasonInfo(uint64_t iSteam, const std::string& sReason, std::string& outExpire) {
	const std::time_t now = std::time(nullptr);
	WarnEntry entry;
	if (!g_WarnStorage.FindActiveReason(iSteam, sReason, now, entry)) {
		return false;
	}
	if (entry.tExpire == 0) {
		outExpire = g_pAdmin->GetTranslation("Menu_WarnExpireNever");
	} else {
		int remaining = static_cast<int>(entry.tExpire - now);
		if (remaining < 0) remaining = 0;
		outExpire = FormatDurationShort(remaining, Translator());
	}
	return true;
}

bool HasActiveReason(uint64_t iSteam, const std::string& sReason) {
	std::string dummy;
	return GetExistingReasonInfo(iSteam, sReason, dummy);
}

// --- database (the original's CreateTables/SaveWarningToDB/MarkWarningRemovedInDB/
//     LoadWarningsFromDB on top of the module's own plgdb connection) --------------------------

void CreateTables() {
	if (!g_Config.bUseDB) return;
	if (!g_pDB || !g_pDB->Ready()) {
		LogError(std::string(kLogTag) + " no database connection - the as_warnsystem table was not created");
		return;
	}
	g_pDB->CreateSchema(&LogDatabaseError);
}

void SaveWarningToDB(const std::string& steam, const WarnEntry& entry, const char* szPlayerName) {
	if (!g_Config.bUseDB || !g_pDB || !g_pDB->Ready()) return;

	// The original captured the vector index of the entry it had just appended and filled the
	// database id in when the insert finished (a late result for a removed entry is dropped).
	const std::size_t idx = g_WarnStorage[steam].size() - 1;
	const std::string steamCopy = steam;
	g_pDB->InsertWarning(g_Config.iServerID, steam, szPlayerName ? szPlayerName : "", entry,
						 [steamCopy, idx](uint64_t id) {
							 std::vector<WarnEntry>* vec = g_WarnStorage.Find(steamCopy);
							 if (vec == nullptr || idx >= vec->size()) return;
							 (*vec)[idx].iID = static_cast<int>(id);
						 },
						 &LogDatabaseError);
}

void MarkWarningRemovedInDB(const std::string& steam, int warnId, uint64_t iAdminSteam, const char* szAdminName) {
	if (!g_Config.bUseDB || !g_pDB || !g_pDB->Ready()) return;
	g_pDB->MarkRemoved(steam, warnId, iAdminSteam, szAdminName ? szAdminName : "", &LogDatabaseError);
}

void LoadWarningsFromDB(uint64_t iSteamId) {
	if (!g_Config.bUseDB || !g_pDB || !g_pDB->Ready()) return;
	const std::string steam = SteamToString(iSteamId);
	g_pDB->LoadWarnings(
		iSteamId,
		[steam](std::vector<WarnEntry> entries) {
			// The original replaced the whole vector the same way.
			g_WarnStorage.Replace(steam, std::move(entries));
		},
		&LogDatabaseError);
}

// --- recent disconnects (the original's AddRecentDisconnect/FindRecentIndex/...) --------------

int FindRecentIndex(uint64_t iSteam) {
	for (size_t i = 0; i < g_RecentDisconnects.size(); ++i) {
		if (g_RecentDisconnects[i].iSteam == iSteam) return static_cast<int>(i);
	}
	return -1;
}

void UpdateRecentDisconnectName(uint64_t iSteam, const char* szName) {
	if (!szName || !szName[0]) return;
	for (auto& rec : g_RecentDisconnects) {
		if (rec.iSteam == iSteam) {
			rec.sName = szName;
			break;
		}
	}
}

void AddRecentDisconnect(uint64_t iSteam, const char* szName) {
	if (iSteam == 0) return;
	const char* szSafeName = (szName && szName[0]) ? szName : "Unknown";

	const int slot = pisex::g_pPlayers->FindPlayer(iSteam);
	if (slot != -1 && pisex::g_pPlayers->IsConnected(slot) && pisex::g_pPlayers->IsInGame(slot)) {
		return;
	}

	g_RecentDisconnects.erase(std::remove_if(g_RecentDisconnects.begin(), g_RecentDisconnects.end(),
											 [iSteam](const RecentDisconnect& rec) { return rec.iSteam == iSteam; }),
							  g_RecentDisconnects.end());

	RecentDisconnect rec;
	rec.iSteam = iSteam;
	rec.sName = szSafeName;
	rec.tDisconnected = std::time(nullptr);
	g_RecentDisconnects.push_front(rec);
	if (g_MaxRecentDisconnects > 0 && g_RecentDisconnects.size() > g_MaxRecentDisconnects) {
		g_RecentDisconnects.pop_back();
	}

	LoadWarningsFromDB(iSteam);
}

void HandlePlayerDisconnect(pisex::IGameEvent* pEvent) {
	if (!pEvent) return;
	if (pEvent->GetBool("bot", false)) return;

	uint64_t iSteam = pEvent->GetUInt64("xuid");
	if (iSteam == 0) iSteam = pEvent->GetUInt64("steamid");
	if (iSteam == 0) {
		const char* szSteamStr = pEvent->GetString("steamid");
		if (!szSteamStr || !szSteamStr[0]) szSteamStr = pEvent->GetString("networkid");
		if (szSteamStr && szSteamStr[0]) {
			iSteam = std::strtoull(szSteamStr, nullptr, 10);
		}
	}
	if (iSteam == 0) return;

	const char* szName = pEvent->GetString("name");
	const std::string nameCopy = (szName && szName[0]) ? szName : "Unknown";

	pisex::NextFrame([iSteam, nameCopy]() {
		const int slot = pisex::g_pPlayers->FindPlayer(iSteam);
		if (slot != -1 && pisex::g_pPlayers->IsConnected(slot) && pisex::g_pPlayers->IsInGame(slot)) {
			return;
		}
		AddRecentDisconnect(iSteam, nameCopy.c_str());
	});
}

// --- warning actions (the original's BuildWarnEntry/AddWarningInternal/AddWarning/...) ---------

WarnEntry BuildWarnEntry(int iAdmin, const std::string& sReason, int iReasonExpire) {
	const std::time_t now = std::time(nullptr);
	const int iExpireSeconds = iReasonExpire > 0 ? iReasonExpire : g_Config.iExpireSeconds;
	const std::time_t expire = iExpireSeconds > 0 ? now + iExpireSeconds : 0;

	WarnEntry entry;
	entry.sReason = sReason;
	entry.tCreated = now;
	entry.tExpire = expire;
	entry.iAdminSteam = pisex::g_pPlayers->GetSteamID64(iAdmin);
	entry.sAdminName = pisex::g_pPlayers->GetPlayerName(iAdmin);
	return entry;
}

void AddWarningInternal(const std::string& sSteam, const char* szPlayerName, const WarnEntry& entry, int iAdmin, int iTargetSlot) {
	std::vector<WarnEntry>& vec = g_WarnStorage[sSteam];
	vec.push_back(entry);

	const char* szTargetName = szPlayerName ? szPlayerName : "";

	if (iTargetSlot >= 0) {
		NotifyMsg(iTargetSlot, "Chat_WarnReceived", static_cast<int>(vec.size()), g_Config.iMaxWarnings, entry.sReason.c_str());
		NotifyMsg(iAdmin, "Chat_WarnIssued", szTargetName, static_cast<int>(vec.size()), g_Config.iMaxWarnings);

		// ClientCommand is g_pEngineServer->ClientCommand, which does not validate the slot: the
		// target is picked from a menu (or found by steamid earlier), so he may be gone by the
		// time the admin selects him. pisex::g_pPlayers->IsConnected is pisexcompat's "there is a
		// live client in this slot" guard (map lifecycle included), so only a slot that still
		// carries a client gets the sound.
		if (!g_Config.sWarnSound.empty() && __s2sdk_ClientCommand && pisex::g_pPlayers->IsConnected(iTargetSlot)) {
			// The original played the sound the same way, through the engine.
			s2sdk::ClientCommand(iTargetSlot, plg::string("play " + g_Config.sWarnSound));
		}
	} else {
		ChatMsg(iAdmin, "Chat_WarnIssuedOffline", szTargetName, static_cast<int>(vec.size()), g_Config.iMaxWarnings);
	}

	SaveWarningToDB(sSteam, entry, szTargetName);

	if (g_Config.iMaxWarnings > 0 && static_cast<int>(vec.size()) >= g_Config.iMaxWarnings) {
		if (iTargetSlot >= 0) {
			g_pAdmin->AddPlayerPunishment(iTargetSlot, admin_system::RT_BAN, g_Config.iBanDuration, g_Config.sBanReason.c_str(), iAdmin);
		} else {
			g_pAdmin->AddOfflinePlayerPunishment(sSteam.c_str(), szTargetName, admin_system::RT_BAN, g_Config.iBanDuration,
												 g_Config.sBanReason.c_str(), iAdmin);
		}
		NotifyMsgAll("Chat_WarnBan", szTargetName);
	}
}

// iTargetSteam is the steamid the target was resolved to when the menu (or the command line) was
// processed; the callers have verified that iTarget still carries that player.
void AddWarning(int iAdmin, int iTarget, uint64_t iTargetSteam, const std::string& sReason, int iReasonExpire) {
	if (!CheckImmunityOrNotify(iAdmin, iTarget)) return;
	if (!g_Config.bAllowDuplicateReason && HasActiveReason(iTargetSteam, sReason)) {
		ChatMsg(iAdmin, "Chat_WarnDuplicateBlocked");
		return;
	}
	const std::string sSteam = SteamToString(iTargetSteam);
	const WarnEntry entry = BuildWarnEntry(iAdmin, sReason, iReasonExpire);
	AddWarningInternal(sSteam, pisex::g_pPlayers->GetPlayerName(iTarget), entry, iAdmin, iTarget);
}

void AddOfflineWarning(int iAdmin, uint64_t iSteamTarget, const char* szPlayerName, const std::string& sReason, int iReasonExpire) {
	if (!g_Config.bAllowDuplicateReason && HasActiveReason(iSteamTarget, sReason)) {
		ChatMsg(iAdmin, "Chat_WarnDuplicateBlocked");
		return;
	}
	const std::string sSteam = SteamToString(iSteamTarget);
	const WarnEntry entry = BuildWarnEntry(iAdmin, sReason, iReasonExpire);
	AddWarningInternal(sSteam, szPlayerName, entry, iAdmin, -1);
}

void PruneExpiredWarnings() {
	if (g_Config.iExpireSeconds <= 0) return;
	g_WarnStorage.PruneExpired(std::time(nullptr));
}

// iTargetSteam is the steamid captured when the remove menu was opened; the caller has verified
// that iTarget still carries that player.
void RemoveWarningByIndex(int iAdmin, int iTarget, uint64_t iTargetSteam, int idx) {
	const std::string sSteam = SteamToString(iTargetSteam);
	std::vector<WarnEntry>* vec = g_WarnStorage.Find(sSteam);
	if (vec == nullptr || vec->empty()) {
		ChatMsg(iAdmin, "Chat_NoWarnings");
		return;
	}
	if (idx < 0 || idx >= static_cast<int>(vec->size())) {
		ChatMsg(iAdmin, "Chat_NoWarnings");
		return;
	}

	const int warnId = (*vec)[idx].iID;
	vec->erase(vec->begin() + idx);
	MarkWarningRemovedInDB(sSteam, warnId, pisex::g_pPlayers->GetSteamID64(iAdmin), pisex::g_pPlayers->GetPlayerName(iAdmin));
	NotifyMsg(iTarget, "Chat_WarnRemovedTarget", static_cast<int>(vec->size()));
	NotifyMsg(iAdmin, "Chat_WarnRemovedAdmin", pisex::g_pPlayers->GetPlayerName(iTarget));
}

// --- menus -------------------------------------------------------------------------------------

void ShowMyWarningsMenu(int iSlot);
void ShowRemoveWarningMenu(int iSlot, int iTarget);
void ShowOfflinePlayerMenu(int iSlot, bool bCommand);
void ShowOfflineReasonMenu(int iSlot, int iRecentIdx);
void ShowOfflineReasonConfirmMenu(int iSlot, int iRecentIdx, int idx);

void ShowMyWarningDetail(int iSlot, int idx) {
	const uint64_t iSteam = pisex::g_pPlayers->GetSteamID64(iSlot);
	const std::vector<WarnEntry>* vec = g_WarnStorage.Find(SteamToString(iSteam));
	if (vec == nullptr || idx < 0 || idx >= static_cast<int>(vec->size())) return;
	const WarnEntry& w = (*vec)[idx];

	pisex::Menu hMenu;
	const std::string szTitle = pisex::FormatString(g_pAdmin->GetTranslation("Menu_WarnDetail"), idx + 1);
	pisex::g_pMenus->SetTitleMenu(hMenu, szTitle.c_str());

	pisex::g_pMenus->AddItemMenu(hMenu, "reason", w.sReason.c_str(), pisex::ITEM_DISABLED);

	std::string sExpire;
	if (w.tExpire == 0) {
		sExpire = g_pAdmin->GetTranslation("Menu_WarnExpireNever");
	} else {
		const std::time_t now = std::time(nullptr);
		int remaining = static_cast<int>(w.tExpire - now);
		if (remaining < 0) remaining = 0;
		sExpire = FormatDurationShort(remaining, Translator());
	}
	const std::string szExpire = pisex::FormatString(g_pAdmin->GetTranslation("Menu_WarnExpires"), sExpire.c_str());
	pisex::g_pMenus->AddItemMenu(hMenu, "exp", szExpire.c_str(), pisex::ITEM_DISABLED);

	pisex::g_pMenus->SetBackMenu(hMenu, true);
	pisex::g_pMenus->SetExitMenu(hMenu, true);
	pisex::g_pMenus->SetCallback(hMenu, [idx](const char* /*szBack*/, const char* /*szFront*/, int iItem, int iSlot) {
		if (iItem == 7) {
			ShowMyWarningsMenu(iSlot);
		}
	});

	pisex::g_pMenus->DisplayPlayerMenu(hMenu, iSlot, true, true);
}

void ShowMyWarningsMenu(int iSlot) {
	const uint64_t iSteam = pisex::g_pPlayers->GetSteamID64(iSlot);
	const std::vector<WarnEntry>* vec = g_WarnStorage.Find(SteamToString(iSteam));
	if (vec == nullptr || vec->empty()) {
		pisex::Menu hMenu;
		const std::string szTitle = pisex::FormatString(g_pAdmin->GetTranslation("Menu_MyWarnings"), 0);
		pisex::g_pMenus->SetTitleMenu(hMenu, szTitle.c_str());
		pisex::g_pMenus->AddItemMenu(hMenu, "none", g_pAdmin->GetTranslation("Menu_NoWarningsSelf"), pisex::ITEM_DISABLED);
		pisex::g_pMenus->SetExitMenu(hMenu, true);
		pisex::g_pMenus->DisplayPlayerMenu(hMenu, iSlot, true, true);
		return;
	}

	pisex::Menu hMenu;
	const std::string szTitle =
		pisex::FormatString(g_pAdmin->GetTranslation("Menu_MyWarnings"), static_cast<int>(vec->size()));
	pisex::g_pMenus->SetTitleMenu(hMenu, szTitle.c_str());

	for (size_t i = 0; i < vec->size(); ++i) {
		const std::string szLine = pisex::FormatString("%zu) %s", i + 1, (*vec)[i].sReason.c_str());
		pisex::g_pMenus->AddItemMenu(hMenu, std::to_string(i).c_str(), szLine.c_str());
	}

	pisex::g_pMenus->SetExitMenu(hMenu, true);
	pisex::g_pMenus->SetCallback(hMenu, [](const char* szBack, const char* /*szFront*/, int iItem, int iSlot) {
		if (iItem < 7) {
			ShowMyWarningDetail(iSlot, std::atoi(szBack));
		}
	});
	pisex::g_pMenus->DisplayPlayerMenu(hMenu, iSlot, true, true);
}

std::string BuildWarningLabel(const WarnEntry& w);

void ShowActionsMenu(int iSlot, int iTarget);
void ShowReasonConfirmMenu(int iSlot, int iTarget, int idx);

void ShowRemoveWarningMenu(int iSlot, int iTarget) {
	const uint64_t iSteamTarget = pisex::g_pPlayers->GetSteamID64(iTarget);
	const std::string sSteam = SteamToString(iSteamTarget);
	const std::vector<WarnEntry>* vec = g_WarnStorage.Find(sSteam);
	pisex::Menu hMenu;

	const int iCount = vec == nullptr ? 0 : static_cast<int>(vec->size());
	const std::string szTitle = std::string(g_pAdmin->GetTranslation("Menu_RemoveSelect"));
	pisex::g_pMenus->SetTitleMenu(hMenu, szTitle.c_str());

	if (iCount == 0 || vec == nullptr) {
		pisex::g_pMenus->AddItemMenu(hMenu, "none", g_pAdmin->GetTranslation("Chat_NoWarnings"), pisex::ITEM_DISABLED);
	} else {
		for (size_t i = 0; i < vec->size(); ++i) {
			const std::string sLabel = BuildWarningLabel((*vec)[i]);
			pisex::g_pMenus->AddItemMenu(hMenu, std::to_string(i).c_str(), sLabel.c_str());
		}
	}

	pisex::g_pMenus->SetBackMenu(hMenu, true);
	pisex::g_pMenus->SetExitMenu(hMenu, true);
	pisex::g_pMenus->SetCallback(hMenu, [iTarget, iSteamTarget](const char* szBack, const char* /*szFront*/, int iItem, int iSlot) {
		// The menu lists the warnings of the player it was opened for: when the slot no longer
		// carries him, the click is dropped instead of hitting the new owner of the slot.
		if (pisex::g_pPlayers->FindPlayer(iSteamTarget) != iTarget) return;
		if (iItem < 7) {
			const int idx = std::atoi(szBack);
			RemoveWarningByIndex(iSlot, iTarget, iSteamTarget, idx);
			ShowRemoveWarningMenu(iSlot, iTarget);
		} else if (iItem == 7) {
			ShowActionsMenu(iSlot, iTarget);
		}
	});

	pisex::g_pMenus->DisplayPlayerMenu(hMenu, iSlot, true, true);
}

std::string BuildWarningLabel(const WarnEntry& w) {
	std::string sExpire;
	if (w.tExpire == 0) {
		sExpire = g_pAdmin->GetTranslation("Menu_WarnExpireNever");
	} else {
		const std::time_t now = std::time(nullptr);
		int remaining = static_cast<int>(w.tExpire - now);
		if (remaining < 0) remaining = 0;
		sExpire = FormatDurationShort(remaining, Translator());
	}
	return pisex::FormatString("%s (%s)", w.sReason.c_str(), sExpire.c_str());
}

void ShowReasonMenu(int iSlot, int iTarget) {
	pisex::Menu hMenu;
	// The player the reason menu was opened for (see ShowActionsMenu).
	const uint64_t iTargetSteam = pisex::g_pPlayers->GetSteamID64(iTarget);
	pisex::g_pMenus->SetTitleMenu(hMenu, g_pAdmin->GetTranslation("Menu_SelectReason"));
	for (size_t i = 0; i < g_Config.vReasons.size(); ++i) {
		pisex::g_pMenus->AddItemMenu(hMenu, std::to_string(i).c_str(), g_Config.vReasons[i].c_str());
	}
	pisex::g_pMenus->SetBackMenu(hMenu, true);
	pisex::g_pMenus->SetExitMenu(hMenu, true);
	pisex::g_pMenus->SetCallback(hMenu, [iTarget, iTargetSteam](const char* szBack, const char* /*szFront*/, int iItem, int iSlot) {
		if (pisex::g_pPlayers->FindPlayer(iTargetSteam) != iTarget) return;
		if (iItem < 7) {
			const int idx = std::atoi(szBack);
			if (idx >= 0 && idx < static_cast<int>(g_Config.vReasons.size())) {
				ShowReasonConfirmMenu(iSlot, iTarget, idx);
			}
		} else if (iItem == 7) {
			ShowActionsMenu(iSlot, iTarget);
		}
	});
	pisex::g_pMenus->DisplayPlayerMenu(hMenu, iSlot, true, true);
}

void ShowReasonConfirmMenu(int iSlot, int iTarget, int idx) {
	pisex::Menu hMenu;
	pisex::g_pMenus->SetTitleMenu(hMenu, g_pAdmin->GetTranslation("Menu_ConfirmReason"));

	// The player the confirm menu was opened for (see ShowReasonMenu); the warning is issued for
	// him and not for whoever may hold the slot at click time.
	const uint64_t iTargetSteam = pisex::g_pPlayers->GetSteamID64(iTarget);
	const int iExpire = idx < static_cast<int>(g_Config.vReasonExpire.size()) ? g_Config.vReasonExpire[idx] : g_Config.iExpireSeconds;
	const std::string sExpire = FormatExpireDuration(iExpire, Translator());
	const std::string szExpire = pisex::FormatString(g_pAdmin->GetTranslation("Menu_WarnExpires"), sExpire.c_str());

	pisex::g_pMenus->AddItemMenu(hMenu, "exp", szExpire.c_str(), pisex::ITEM_DISABLED);

	bool bDuplicate = false;
	std::string sDupExpire;
	if (GetExistingReasonInfo(iTargetSteam, g_Config.vReasons[idx], sDupExpire)) {
		bDuplicate = true;
		const std::string sDup = pisex::FormatString(g_pAdmin->GetTranslation("Menu_WarnDuplicate"), sDupExpire.c_str());
		pisex::g_pMenus->AddItemMenu(hMenu, "dup", sDup.c_str(), pisex::ITEM_DISABLED);
		if (!g_Config.bAllowDuplicateReason) {
			pisex::g_pMenus->AddItemMenu(hMenu, "block", g_pAdmin->GetTranslation("Menu_WarnDuplicateBlocked"), pisex::ITEM_DISABLED);
		}
	}

	if (!bDuplicate || g_Config.bAllowDuplicateReason) {
		pisex::g_pMenus->AddItemMenu(hMenu, "confirm", g_pAdmin->GetTranslation("Menu_IssueWarning"));
	}

	pisex::g_pMenus->SetBackMenu(hMenu, true);
	pisex::g_pMenus->SetExitMenu(hMenu, true);
	pisex::g_pMenus->SetCallback(hMenu, [iTarget, iTargetSteam, idx, iExpire](const char* szBack, const char* /*szFront*/, int iItem, int iSlot) {
		if (pisex::g_pPlayers->FindPlayer(iTargetSteam) != iTarget) return;
		if (iItem < 7 && std::strcmp(szBack, "confirm") == 0) {
			AddWarning(iSlot, iTarget, iTargetSteam, g_Config.vReasons[idx], iExpire);
			pisex::g_pMenus->ClosePlayerMenu(iSlot);
		} else if (iItem == 7) {
			ShowReasonMenu(iSlot, iTarget);
		}
	});

	pisex::g_pMenus->DisplayPlayerMenu(hMenu, iSlot, true, true);
}

void ShowOfflineReasonConfirmMenu(int iSlot, int iRecentIdx, int idx) {
	if (idx < 0 || idx >= static_cast<int>(g_Config.vReasons.size())) {
		ShowOfflineReasonMenu(iSlot, iRecentIdx);
		return;
	}
	if (iRecentIdx < 0 || iRecentIdx >= static_cast<int>(g_RecentDisconnects.size())) {
		ShowOfflinePlayerMenu(iSlot, false);
		return;
	}

	pisex::Menu hMenu;
	pisex::g_pMenus->SetTitleMenu(hMenu, g_pAdmin->GetTranslation("Menu_ConfirmReason"));

	const int iExpire = idx < static_cast<int>(g_Config.vReasonExpire.size()) ? g_Config.vReasonExpire[idx] : g_Config.iExpireSeconds;
	const std::string sExpire = FormatExpireDuration(iExpire, Translator());
	const std::string szExpire = pisex::FormatString(g_pAdmin->GetTranslation("Menu_WarnExpires"), sExpire.c_str());

	pisex::g_pMenus->AddItemMenu(hMenu, "exp", szExpire.c_str(), pisex::ITEM_DISABLED);

	bool bDuplicate = false;
	std::string sDupExpire;
	if (GetExistingReasonInfo(g_RecentDisconnects[iRecentIdx].iSteam, g_Config.vReasons[idx], sDupExpire)) {
		bDuplicate = true;
		const std::string sDup = pisex::FormatString(g_pAdmin->GetTranslation("Menu_WarnDuplicate"), sDupExpire.c_str());
		pisex::g_pMenus->AddItemMenu(hMenu, "dup", sDup.c_str(), pisex::ITEM_DISABLED);
		if (!g_Config.bAllowDuplicateReason) {
			pisex::g_pMenus->AddItemMenu(hMenu, "block", g_pAdmin->GetTranslation("Menu_WarnDuplicateBlocked"), pisex::ITEM_DISABLED);
		}
	}

	if (!bDuplicate || g_Config.bAllowDuplicateReason) {
		pisex::g_pMenus->AddItemMenu(hMenu, "confirm", g_pAdmin->GetTranslation("Menu_IssueWarning"));
	}

	pisex::g_pMenus->SetBackMenu(hMenu, true);
	pisex::g_pMenus->SetExitMenu(hMenu, true);
	pisex::g_pMenus->SetCallback(hMenu, [iRecentIdx, idx, iExpire](const char* szBack, const char* /*szFront*/, int iItem, int iSlot) {
		if (iItem < 7 && std::strcmp(szBack, "confirm") == 0) {
			if (iRecentIdx < 0 || iRecentIdx >= static_cast<int>(g_RecentDisconnects.size())) return;
			const RecentDisconnect& info = g_RecentDisconnects[iRecentIdx];
			AddOfflineWarning(iSlot, info.iSteam, info.sName.c_str(), g_Config.vReasons[idx], iExpire);
			pisex::g_pMenus->ClosePlayerMenu(iSlot);
		} else if (iItem == 7) {
			ShowOfflineReasonMenu(iSlot, iRecentIdx);
		}
	});

	pisex::g_pMenus->DisplayPlayerMenu(hMenu, iSlot, true, true);
}

void ShowOfflineReasonMenu(int iSlot, int iRecentIdx) {
	if (iRecentIdx < 0 || iRecentIdx >= static_cast<int>(g_RecentDisconnects.size())) {
		ShowOfflinePlayerMenu(iSlot, false);
		return;
	}

	const RecentDisconnect& info = g_RecentDisconnects[iRecentIdx];

	pisex::Menu hMenu;
	const std::string szTitle = pisex::FormatString(g_pAdmin->GetTranslation("Menu_WarningsFor"), info.sName.c_str(),
													g_WarnStorage.Count(info.iSteam), g_Config.iMaxWarnings);
	pisex::g_pMenus->SetTitleMenu(hMenu, szTitle.c_str());

	for (size_t i = 0; i < g_Config.vReasons.size(); ++i) {
		pisex::g_pMenus->AddItemMenu(hMenu, std::to_string(i).c_str(), g_Config.vReasons[i].c_str());
	}

	pisex::g_pMenus->SetBackMenu(hMenu, true);
	pisex::g_pMenus->SetExitMenu(hMenu, true);
	pisex::g_pMenus->SetCallback(hMenu, [iRecentIdx](const char* szBack, const char* /*szFront*/, int iItem, int iSlot) {
		if (iItem < 7) {
			const int idx = std::atoi(szBack);
			if (idx >= 0 && idx < static_cast<int>(g_Config.vReasons.size())) {
				ShowOfflineReasonConfirmMenu(iSlot, iRecentIdx, idx);
			}
		} else if (iItem == 7) {
			ShowOfflinePlayerMenu(iSlot, false);
		}
	});

	pisex::g_pMenus->DisplayPlayerMenu(hMenu, iSlot, true, true);
}

void ShowPlayerMenu(int iSlot, bool bCommand);

void ShowOfflinePlayerMenu(int iSlot, bool bCommand) {
	pisex::Menu hMenu;
	pisex::g_pMenus->SetTitleMenu(hMenu, g_pAdmin->GetTranslation("Menu_SelectOfflinePlayer"));

	std::vector<size_t> vAvailable;
	for (size_t i = 0; i < g_RecentDisconnects.size(); ++i) {
		const int slot = pisex::g_pPlayers->FindPlayer(g_RecentDisconnects[i].iSteam);
		if (slot != -1 && pisex::g_pPlayers->IsConnected(slot) && pisex::g_pPlayers->IsInGame(slot)) {
			continue;
		}
		vAvailable.push_back(i);
	}

	if (vAvailable.empty()) {
		pisex::g_pMenus->AddItemMenu(hMenu, "none", g_pAdmin->GetTranslation("Menu_NoOfflinePlayers"), pisex::ITEM_DISABLED);
	} else {
		for (size_t idx : vAvailable) {
			const RecentDisconnect& info = g_RecentDisconnects[idx];
			const int iCount = g_WarnStorage.Count(info.iSteam);
			const std::string szText = pisex::FormatString("%s (%i)", info.sName.c_str(), iCount);
			pisex::g_pMenus->AddItemMenu(hMenu, std::to_string(static_cast<int>(idx)).c_str(), szText.c_str());
		}
	}

	pisex::g_pMenus->SetBackMenu(hMenu, true);
	pisex::g_pMenus->SetExitMenu(hMenu, true);
	pisex::g_pMenus->SetCallback(hMenu, [bCommand](const char* szBack, const char* /*szFront*/, int iItem, int iSlot) {
		if (iItem < 7) {
			const int idx = std::atoi(szBack);
			if (idx >= 0 && idx < static_cast<int>(g_RecentDisconnects.size())) {
				ShowOfflineReasonMenu(iSlot, idx);
			}
		} else if (iItem == 7) {
			ShowPlayerMenu(iSlot, bCommand);
		}
	});

	pisex::g_pMenus->DisplayPlayerMenu(hMenu, iSlot, true, true);
}

void ShowActionsMenu(int iSlot, int iTarget) {
	pisex::Menu hMenu;
	// The menu is about the player shown in it, not about his slot: the steamid is captured here
	// and checked again before every action further down the menu chain.
	const uint64_t iTargetSteam = pisex::g_pPlayers->GetSteamID64(iTarget);
	const int iCount = g_WarnStorage.Count(iTargetSteam);
	const std::string szTitle = pisex::FormatString(g_pAdmin->GetTranslation("Menu_WarningsFor"),
													pisex::g_pPlayers->GetPlayerName(iTarget), iCount, g_Config.iMaxWarnings);
	pisex::g_pMenus->SetTitleMenu(hMenu, szTitle.c_str());

	pisex::g_pMenus->AddItemMenu(hMenu, "add", g_pAdmin->GetTranslation("Menu_IssueWarning"));
	pisex::g_pMenus->AddItemMenu(hMenu, "del", g_pAdmin->GetTranslation("Menu_RemoveWarning"),
								 iCount > 0 ? pisex::ITEM_DEFAULT : pisex::ITEM_DISABLED);
	pisex::g_pMenus->SetBackMenu(hMenu, true);
	pisex::g_pMenus->SetExitMenu(hMenu, true);
	pisex::g_pMenus->SetCallback(hMenu, [iTarget, iTargetSteam](const char* szBack, const char* /*szFront*/, int iItem, int iSlot) {
		if (pisex::g_pPlayers->FindPlayer(iTargetSteam) != iTarget) return;
		if (iItem < 7) {
			if (std::strcmp(szBack, "add") == 0) {
				ShowReasonMenu(iSlot, iTarget);
			} else if (std::strcmp(szBack, "del") == 0) {
				ShowRemoveWarningMenu(iSlot, iTarget);
			}
		} else if (iItem == 7) {
			ShowPlayerMenu(iSlot, false);
		}
	});

	pisex::g_pMenus->DisplayPlayerMenu(hMenu, iSlot, true, true);
}

void ShowPlayerMenu(int iSlot, bool bCommand) {
	pisex::Menu hMenu;
	pisex::g_pMenus->SetTitleMenu(hMenu, g_pAdmin->GetTranslation("Menu_SelectPlayer"));
	bool bAny = false;
	const bool bHasOffline = !g_RecentDisconnects.empty();
	for (int i = 0; i < kMaxSlots; ++i) {
		if (!pisex::g_pPlayers->IsConnected(i) || pisex::g_pPlayers->IsFakeClient(i) || !pisex::g_pPlayers->IsInGame(i)) continue;
		if (i == iSlot) continue;
		bAny = true;
		const int iCount = g_WarnStorage.Count(pisex::g_pPlayers->GetSteamID64(i));
		const std::string szText = pisex::FormatString("%s (%i)", pisex::g_pPlayers->GetPlayerName(i), iCount);
		// The item carries the steamid, not the slot: the menu stays open while the slot it was
		// built from can change hands, and the admin picked the player shown in the list.
		pisex::g_pMenus->AddItemMenu(hMenu, std::to_string(pisex::g_pPlayers->GetSteamID64(i)).c_str(), szText.c_str());
	}

	if (!bAny && !bHasOffline) {
		pisex::g_pMenus->AddItemMenu(hMenu, "noplayers", g_pAdmin->GetTranslation("Menu_NoPlayers"), pisex::ITEM_DISABLED);
	}

	if (bHasOffline) {
		pisex::g_pMenus->AddItemMenu(hMenu, "offline", g_pAdmin->GetTranslation("Menu_OfflineWarnings"));
	}

	if (!bCommand) pisex::g_pMenus->SetBackMenu(hMenu, true);
	pisex::g_pMenus->SetExitMenu(hMenu, true);
	pisex::g_pMenus->SetCallback(hMenu, [bCommand](const char* szBack, const char* /*szFront*/, int iItem, int iSlot) {
		if (iItem < 7) {
			if (std::strcmp(szBack, "offline") == 0) {
				ShowOfflinePlayerMenu(iSlot, bCommand);
			} else {
				const uint64_t iSteam = std::strtoull(szBack, nullptr, 10);
				const int iTarget = pisex::g_pPlayers->FindPlayer(iSteam);
				if (iTarget < 0) return; // the player left while the menu was open
				ShowActionsMenu(iSlot, iTarget);
			}
		} else if (iItem == 7 && !bCommand) {
			g_pAdmin->ShowAdminLastCategoryMenu(iSlot);
		}
	});

	pisex::g_pMenus->DisplayPlayerMenu(hMenu, iSlot, true, true);
}

// --- setup ---------------------------------------------------------------------------------------

void RegisterAdminMenu() {
	if (g_Config.bCustomCategory) {
		g_pAdmin->RegisterCategory(g_Config.sCategory.c_str(), "Warns", nullptr);
	} else if (g_Config.sCategory.empty()) {
		g_Config.sCategory = "players";
	}

	g_pAdmin->RegisterItem("warnsystem", g_pAdmin->GetTranslation("Menu_ItemWarnSystem"), g_Config.sCategory.c_str(),
						   g_Config.sFlag.c_str(), nullptr,
						   [](int iSlot, const char* /*szCategory*/, const char* /*szIdentity*/, const char* /*szItem*/) {
							   ShowPlayerMenu(iSlot, false);
						   });
}

void LoadConfig() {
	// The original's LoadCorePunishServerID() + LoadConfig(): core.ini decides the server id for
	// punishments when warnsystem.ini says -1.
	const int iCoreServerID = as_warnsystem::LoadCorePunishServerIDFile(
		pisex::ResolvePath("addons/configs/admin_system/core.ini"));

	const std::string path = pisex::ResolvePath("addons/configs/admin_system/warnsystem.ini");
	if (!as_warnsystem::LoadConfigFile(path, g_Config, iCoreServerID)) {
		pisex::g_pUtils->ErrorLog("%s Failed to load %s, using defaults", kLogTag,
								  path.empty() ? "addons/configs/admin_system/warnsystem.ini" : path.c_str());
		return;
	}
	g_MaxRecentDisconnects = static_cast<size_t>(g_Config.iRecentLimit);
	LogInfo(std::string(kLogTag) + " configuration loaded (" + std::to_string(g_Config.vReasons.size()) + " reasons, server id " +
			std::to_string(g_Config.iServerID) + ")");
}

void OpenDatabase() {
	const admin_system::DatabaseConfig cfg = g_pAdmin->GetDatabaseConfig();

	std::string sqlitePath;
	if (cfg.IsSqlite()) {
		std::string path = !cfg.host.empty() ? cfg.host : std::string("as_warnsystem.sqlite3");
		if (path != ":memory:" && !std::filesystem::path(path).is_absolute()) {
			const std::string dataDir = plg::GetDataDir ? std::string(plg::GetDataDir()) : std::string();
			if (!dataDir.empty()) {
				path = pisex::JoinPath(dataDir, path);
				std::error_code ec;
				const std::filesystem::path parent = std::filesystem::path(path).parent_path();
				if (!parent.empty()) {
					std::filesystem::create_directories(parent, ec);
				}
			}
		}
		sqlitePath = path;
	}

	const plgdb::Config dbConfig = admin_system::MakeDatabaseConfig(cfg, sqlitePath);
	std::unique_ptr<plgdb::Database> db = plgdb::Database::Create(dbConfig);
	if (!db) {
		LogWarning(std::string(kLogTag) + " database configuration is invalid - warnings will not be persisted");
		return;
	}
	g_pDB = std::make_unique<WarnDatabase>(std::move(db));
	LogInfo(std::string(kLogTag) + " database ready (" + cfg.driver + ")");
}

void HandleClientAuthorized(int iSlot, uint64_t iEventSteam) {
	if (iSlot < 0 || iSlot >= kMaxSlots) return;
	uint64_t iSteam = pisex::g_pPlayers->GetSteamID64(iSlot);
	if (iSteam == 0) iSteam = iEventSteam;
	// The event documents "Steam account id or 0 if not available"; a 32-bit account id is not a
	// SteamID64 and must not become the storage key - the reconcile picks the client up later.
	if (iSteam == 0 || iSteam < (1ull << 32)) return;
	if (g_SlotLoadedSteam[iSlot] == iSteam) return;

	g_SlotLoadedSteam[iSlot] = iSteam;
	LoadWarningsFromDB(iSteam);
	UpdateRecentDisconnectName(iSteam, pisex::g_pPlayers->GetPlayerName(iSlot));
}

// Once-per-second safety net for the fact that s2sdk's OnClientAuthenticated does not fire for
// every client (same approach as plugins/perm_connector): the original loaded the warnings in
// HookOnClientAuthorized, this makes sure every connected authorised player gets them once.
void ReconcileSlots() {
	for (int slot = 0; slot < kMaxSlots; ++slot) {
		if (!pisex::g_pPlayers->IsConnected(slot)) {
			g_SlotLoadedSteam[slot] = 0;
			continue;
		}
		if (pisex::g_pPlayers->IsFakeClient(slot) || !pisex::g_pPlayers->IsInGame(slot) ||
			!pisex::g_pPlayers->IsAuthenticated(slot)) {
			continue;
		}
		const uint64_t iSteam = pisex::g_pPlayers->GetSteamID64(slot);
		if (iSteam == 0 || g_SlotLoadedSteam[slot] == iSteam) continue;

		g_SlotLoadedSteam[slot] = iSteam;
		LoadWarningsFromDB(iSteam);
		UpdateRecentDisconnectName(iSteam, pisex::g_pPlayers->GetPlayerName(slot));
	}
}

void RegisterCommands() {
	std::vector<std::string> commands = Split(g_Config.sCommand, '|');
	if (commands.empty()) commands.push_back(g_Config.sCommand);

	std::vector<std::string> consoleCommands;
	if (!g_Config.sConsoleCommand.empty()) {
		consoleCommands = Split(g_Config.sConsoleCommand, '|');
		if (consoleCommands.empty()) consoleCommands.push_back(g_Config.sConsoleCommand);
	}

	pisex::g_pUtils->RegCommand(consoleCommands, commands, [](int iSlot, const char* szContent) {
		if (!g_pAdmin->HasPermission(iSlot, g_Config.sFlag.c_str())) return true;

		// The original's line parsing (command tokens + "<steamid64> [reason] [seconds]") lives in
		// the engine-free core so it is covered by the unit tests.
		as_warnsystem::WarnCommandArgs parsed;
		if (!as_warnsystem::ParseWarnCommand(szContent ? szContent : "", g_Config, parsed)) {
			ShowPlayerMenu(iSlot, true);
			return true;
		}
		const uint64_t targetSteam = parsed.targetSteam;

		const int targetSlot = pisex::g_pPlayers->FindPlayer(targetSteam);
		const bool targetOnline = (targetSlot != -1 && pisex::g_pPlayers->IsConnected(targetSlot) &&
								   pisex::g_pPlayers->IsInGame(targetSlot));

		if (targetSteam == pisex::g_pPlayers->GetSteamID64(iSlot)) {
			ChatMsg(iSlot, "Chat_SelfWarnNotAllowed");
			return true;
		}

		if (parsed.hasReason) {
			if (targetOnline) {
				if (!CheckImmunityOrNotify(iSlot, targetSlot)) return true;
				AddWarning(iSlot, targetSlot, targetSteam, parsed.reason, parsed.expireSeconds);
			} else {
				std::string sName = SteamToString(targetSteam);
				int idx = FindRecentIndex(targetSteam);
				if (idx == -1) {
					AddRecentDisconnect(targetSteam, sName.c_str());
					idx = FindRecentIndex(targetSteam);
				}
				if (idx != -1) sName = g_RecentDisconnects[idx].sName;
				AddOfflineWarning(iSlot, targetSteam, sName.c_str(), parsed.reason, parsed.expireSeconds);
			}
			return true;
		}

		// Only a steamid: open the reason menu for that player (online) or the offline entry.
		if (targetOnline) {
			if (!CheckImmunityOrNotify(iSlot, targetSlot)) return true;
			ShowReasonMenu(iSlot, targetSlot);
			return true;
		}

		int idx = FindRecentIndex(targetSteam);
		if (idx == -1) {
			const std::string sName = SteamToString(targetSteam);
			AddRecentDisconnect(targetSteam, sName.c_str());
			idx = FindRecentIndex(targetSteam);
		}
		if (idx != -1) {
			ShowOfflineReasonMenu(iSlot, idx);
		} else {
			ShowPlayerMenu(iSlot, true);
		}
		return true;
	});

	std::vector<std::string> playerCommands = Split(g_Config.sPlayerCommand, '|');
	if (playerCommands.empty()) playerCommands.push_back(g_Config.sPlayerCommand);

	std::vector<std::string> playerConsoleCommands;
	if (!g_Config.sPlayerConsoleCommand.empty()) {
		playerConsoleCommands = Split(g_Config.sPlayerConsoleCommand, '|');
		if (playerConsoleCommands.empty()) playerConsoleCommands.push_back(g_Config.sPlayerConsoleCommand);
	}

	pisex::g_pUtils->RegCommand(playerConsoleCommands, playerCommands, [](int iSlot, const char*) {
		if (iSlot < 0 || !pisex::g_pPlayers->IsConnected(iSlot)) return true;
		ShowMyWarningsMenu(iSlot);
		return true;
	});
}

void RegisterEventHooks() {
	// The original registered the same event through its utils.
	pisex::g_pUtils->HookEvent("player_disconnect",
							   [](const char* /*szName*/, pisex::IGameEvent* pEvent, bool /*bDontBroadcast*/) {
								   HandlePlayerDisconnect(pEvent);
							   });

	// The original loaded warnings from the same authorization hook.
	pisex::g_pPlayers->HookOnClientAuthorized([](int iSlot, uint64_t iSteam) { HandleClientAuthorized(iSlot, iSteam); });
}

class AsWarnSystemPlugin final : public plg::Plugin {
public:
	plg::PluginResult OnPluginStart() override {
		try {
			// The admin system family keeps its files in <plugify configs>/admin_system (fixed
			// layout); this also wires the s2sdk listeners of pisexcompat.
			pisex::Init("admin_system");
			g_pAdmin = &admin_system::AdminApi::Get();

			LoadConfig();
			OpenDatabase();

			// The original created its table from the admin core's OnCoreLoaded callback (its
			// connection only existed then). The module owns its connection now, so the schema is
			// created here and - idempotently, like the original - again when the core reports
			// that it finished loading.
			CreateTables();
			g_pAdmin->OnCoreLoaded([]() { CreateTables(); });

			RegisterAdminMenu();
			RegisterCommands();
			RegisterEventHooks();

			pisex::g_pUtils->CreateTimer(kPruneInterval, []() {
				PruneExpiredWarnings();
				return kPruneInterval;
			});

			g_ReconcileEnabled = true;
			LogInfo(std::string(kLogTag) + " ready");
		} catch (const std::exception& ex) {
			LogError(std::string(kLogTag) + " OnPluginStart failed: " + ex.what());
		} catch (...) {
			LogError(std::string(kLogTag) + " OnPluginStart failed with an unknown exception");
		}
		return {}; // never Failed
	}

	plg::PluginResult OnPluginUpdate(std::chrono::milliseconds dt) override {
		try {
			if (g_pDB) {
				g_pDB->Poll(); // runs completed database callbacks on the game thread
			}
			pisex::Poll(); // timers, next-frame queue, menu requests, map readiness

			g_SinceReconcile += dt;
			if (g_ReconcileEnabled && g_SinceReconcile >= kReconcileInterval) {
				g_SinceReconcile = std::chrono::milliseconds(0);
				ReconcileSlots();
			}
		} catch (const std::exception& ex) {
			LogError(std::string(kLogTag) + " update failed: " + ex.what());
		} catch (...) {
			LogError(std::string(kLogTag) + " update failed with an unknown exception");
		}
		return {};
	}

	plg::PluginResult OnPluginEnd() override {
		try {
			g_ReconcileEnabled = false;
			// The core keeps a pointer to our OnCoreLoaded trampoline - drop it before the library
			// goes away. pisex::Shutdown() removes commands, events, chat listeners and timers.
			if (g_pAdmin) {
				g_pAdmin->OnCoreLoadedUnregister();
			}
			pisex::Shutdown();
			g_pDB.reset(); // flushes the pending statements and joins the plgdb worker
		} catch (const std::exception& ex) {
			LogError(std::string(kLogTag) + " OnPluginEnd failed: " + ex.what());
		} catch (...) {
			LogError(std::string(kLogTag) + " OnPluginEnd failed with an unknown exception");
		}
		return {};
	}
};

AsWarnSystemPlugin g_asWarnSystemPlugin;

} // namespace

PLUGIFY_PLUGIN(PLUGIN_API, &g_asWarnSystemPlugin)
