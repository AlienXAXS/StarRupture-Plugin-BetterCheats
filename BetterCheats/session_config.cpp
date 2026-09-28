#include "session_config.h"
#include "plugin_helpers.h"
#include "aob_patterns.h"

#include "Chimera_classes.hpp"
#include "Engine_classes.hpp"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <mutex>

namespace BetterCheats::SessionConfig
{
	namespace
	{
		using Clock = std::chrono::steady_clock;

		// Coalesces a burst of Set() calls (a dragged slider firing every
		// render frame, or every stackable item re-composing the same tick)
		// into one disk write per window instead of one per call -- see the
		// per-tick file I/O this replaced, audited in player_weapons.cpp/
		// player_inventory.cpp. Long enough to matter, short enough that a
		// crash right after an edit loses at most this much.
		constexpr std::chrono::milliseconds kCommitDebounce{ 1000 };

		std::mutex     g_mutex;
		std::string    g_configDir;
		std::string    g_sessionName;
		nlohmann::json g_data;
		bool           g_loaded = false;

		// Set() only ever marks this; Commit() is the only thing that clears
		// it, and the only thing that touches disk. Guarded by g_mutex, same
		// as g_data -- the UI/render thread stages edits, the game-thread
		// tick (and Shutdown/Reload, whichever thread they land on) flush
		// them, and both sides must see one consistent view.
		bool           g_dirty = false;
		Clock::time_point g_lastCommit{};

		// The plugin DLL's own directory — used as the base for the per-plugin
		// config folder. Resolved via the address of this function rather than
		// a stored DllMain HMODULE.
		std::string GetModuleDirectory()
		{
			HMODULE module = nullptr;
			GetModuleHandleExA(
				GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
				reinterpret_cast<LPCSTR>(&GetModuleDirectory), &module);

			char path[MAX_PATH] = {};
			GetModuleFileNameA(module, path, MAX_PATH);

			return std::filesystem::path(path).parent_path().string();
		}

		// Replaces filesystem-unsafe characters so a session name can be used
		// directly as a file name.
		std::string SanitizeFileName(const std::string& name)
		{
			std::string result = name;
			for (char& c : result)
			{
				switch (c)
				{
					case '<': case '>': case ':': case '"':
					case '/': case '\\': case '|': case '?': case '*':
						c = '_';
						break;
					default:
						break;
				}
			}
			return result.empty() ? "Default" : result;
		}

		std::string GetConfigPath()
		{
			return g_configDir + "\\" + SanitizeFileName(g_sessionName) + ".json";
		}

		// Must run on the game thread — touches UWorld/UObject state.
		std::string ResolveSessionName()
		{
			try
			{
				LOG_DEBUG("Attempting to resolve current session name");

				SDK::UWorld* world = SDK::UWorld::GetWorld();
				if (!world)
				{
					LOG_DEBUG("SessionConfig: ResolveSessionName - no world loaded.");
					return "";
				}

				SDK::UCrGameInstance* gameInstance = reinterpret_cast<SDK::UCrGameInstance*>(SDK::UGameplayStatics::GetGameInstance(world));
				if (!gameInstance)
				{
					LOG_DEBUG("SessionConfig: ResolveSessionName - no game instance.");
					return "";
				}

				// UCrGameInstance::ServerSessionName — not present in the generated SDK
				// (folded into the trailing padding bytes), but populated on load even
				// for single-player sessions. Read directly via its known offset.
				auto* serverSessionName = reinterpret_cast<SDK::FString*>(
					reinterpret_cast<std::uint8_t*>(gameInstance) + BetterCheats::AOB::kServerSessionNameOffset);

				std::string sessionName = serverSessionName->ToString();
				LOG_DEBUG("SessionConfig: ResolveSessionName resolved '%s'.", sessionName.c_str());
				return sessionName;
			}
			catch (const std::exception& e)
			{
				LOG_WARN("SessionConfig: ResolveSessionName threw: %s", e.what());
				return "";
			}
			catch (...)
			{
				LOG_WARN("SessionConfig: ResolveSessionName threw an unknown exception.");
				return "";
			}
		}

		// Splits a dot-separated path into an RFC 6901 JSON pointer.
		nlohmann::json::json_pointer ToJsonPointer(const std::string& path)
		{
			std::string pointer;
			pointer.reserve(path.size() + 1);

			for (char c : path)
				pointer += (c == '.') ? '/' : c;

			return nlohmann::json::json_pointer("/" + pointer);
		}
	}

	void Initialize(IPluginSelf* self)
	{
		const std::string pluginName = (self && self->name) ? self->name : "BetterCheats";
		const std::string moduleDir = GetModuleDirectory();
		g_configDir = moduleDir + "\\" + pluginName;

		LOG_DEBUG("SessionConfig: module directory '%s'.", moduleDir.c_str());
		LOG_DEBUG("SessionConfig: config directory '%s'.", g_configDir.c_str());

		std::error_code ec;
		std::filesystem::create_directories(g_configDir, ec);
		if (ec)
			LOG_WARN("SessionConfig: failed to create config directory '%s': %s", g_configDir.c_str(), ec.message().c_str());
		else
			LOG_DEBUG("SessionConfig: config directory ready '%s'.", g_configDir.c_str());
	}

	void Shutdown()
	{
		// Force -- this is the last chance to write anything staged since the
		// last debounced commit. Plain file I/O + the in-memory g_data below,
		// no UObject access, so it's fine even when PluginShutdown is running
		// off the game thread (the loader's RELOAD path).
		Commit(true);

		std::lock_guard<std::mutex> lock(g_mutex);
		LOG_DEBUG("SessionConfig: shutting down — discarding session '%s'.", g_sessionName.c_str());
		g_data.clear();
		g_sessionName.clear();
		g_loaded  = false;
		g_dirty   = false;
	}

	bool Reload()
	{
		// Flush whatever the OUTGOING session left dirty before switching --
		// otherwise the g_data reset below silently drops an edit that was
		// still waiting out the debounce window.
		Commit(true);

		std::string sessionName = ResolveSessionName();
		if (sessionName.empty())
		{
			LOG_DEBUG("SessionConfig: no active session — config not loaded.");
			std::lock_guard<std::mutex> lock(g_mutex);
			g_loaded = false;
			return false;
		}

		std::lock_guard<std::mutex> lock(g_mutex);

		g_sessionName = std::move(sessionName);
		g_data  = nlohmann::json::object();
		g_dirty = false;

		const std::string configPath = GetConfigPath();
		LOG_DEBUG("SessionConfig: resolved session name '%s'.", g_sessionName.c_str());
		LOG_DEBUG("SessionConfig: reading config file '%s'.", configPath.c_str());

		// A leftover temp file from a Commit() that wrote but never got to
		// rename (killed mid-write) never affects this read -- it lives at a
		// different path -- but clear it out anyway so it doesn't sit there
		// forever; a failed remove here is harmless, the next Commit() just
		// overwrites it.
		std::error_code staleEc;
		std::filesystem::remove(configPath + ".tmp", staleEc);

		std::ifstream file(configPath);
		if (file.is_open())
		{
			try
			{
				file >> g_data;
				LOG_DEBUG("SessionConfig: parsed config file '%s'.", configPath.c_str());
			}
			catch (...)
			{
				LOG_WARN("SessionConfig: failed to parse '%s' — starting with an empty config.", configPath.c_str());
				g_data = nlohmann::json::object();
			}
		}
		else
		{
			LOG_DEBUG("SessionConfig: no existing config file at '%s' — starting with an empty config.", configPath.c_str());
		}

		g_loaded = true;
		LOG_INFO("SessionConfig: loaded session '%s' (%s).", g_sessionName.c_str(), configPath.c_str());
		return true;
	}

	bool IsLoaded()
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		return g_loaded;
	}

	std::string GetSessionName()
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		return g_sessionName;
	}

	nlohmann::json Get(const std::string& path, const nlohmann::json& defaultValue)
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		if (!g_loaded)
		{
			LOG_DEBUG("SessionConfig: Get('%s') — no session loaded, returning default.", path.c_str());
			return defaultValue;
		}

		const auto pointer = ToJsonPointer(path);
		if (!g_data.contains(pointer))
		{
			LOG_DEBUG("SessionConfig: Get('%s') — not found, returning default.", path.c_str());
			return defaultValue;
		}

		LOG_DEBUG("SessionConfig: Get('%s') — found.", path.c_str());
		return g_data.at(pointer);
	}

	void Set(const std::string& path, const nlohmann::json& value)
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		if (!g_loaded)
		{
			LOG_DEBUG("SessionConfig: Set('%s') — no session loaded, ignoring.", path.c_str());
			return;
		}

		const auto pointer = ToJsonPointer(path);
		if (g_data.contains(pointer) && g_data.at(pointer) == value)
		{
			// Same value already staged/committed -- nothing changed, so
			// nothing to write. A caller re-asserting a value it didn't
			// actually change (e.g. a per-tick compose row re-confirming its
			// own last write) must never mark the config dirty on its own.
			return;
		}

		g_data[pointer] = value;
		g_dirty = true;

		LOG_DEBUG("SessionConfig: staged '%s' = %s for the next commit.", path.c_str(), value.dump().c_str());
	}

	void Commit(bool force)
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		if (!g_loaded || !g_dirty)
			return;

		if (!force)
		{
			const Clock::time_point now = Clock::now();
			if (g_lastCommit != Clock::time_point{} && now - g_lastCommit < kCommitDebounce)
				return;   // still dirty -- the next tick (or a forced flush) will catch it
		}

		const std::string configPath = GetConfigPath();
		const std::string tempPath   = configPath + ".tmp";

		std::ofstream file(tempPath, std::ios::trunc);
		if (!file.is_open())
		{
			LOG_WARN("SessionConfig: Commit — failed to open temp file '%s'.", tempPath.c_str());
			return;   // stays dirty, tries again next time
		}

		file << g_data.dump(2);
		file.close();
		if (file.fail())
		{
			LOG_WARN("SessionConfig: Commit — failed writing temp file '%s'.", tempPath.c_str());
			return;
		}

		// Atomic on the same volume -- always true here, tempPath and
		// configPath share a directory. std::filesystem::rename replaces an
		// existing target per the standard, so no separate remove-then-
		// rename window where a crash could leave neither file behind.
		std::error_code ec;
		std::filesystem::rename(tempPath, configPath, ec);
		if (ec)
		{
			LOG_WARN("SessionConfig: Commit — replacing '%s' failed: %s", configPath.c_str(), ec.message().c_str());
			return;   // stays dirty, tries again next time; tempPath is left for the next attempt to overwrite
		}

		g_dirty      = false;
		g_lastCommit = Clock::now();
		LOG_DEBUG("SessionConfig: committed '%s'.", configPath.c_str());
	}
}
