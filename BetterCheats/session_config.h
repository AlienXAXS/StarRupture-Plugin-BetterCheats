#pragma once

#include "plugin_interface.h"
#include "json.hpp"

#include <string>

namespace BetterCheats::SessionConfig
{
	// Resolves the plugin's config folder (<Plugins>\<pluginName>\, created if
	// missing) — call once during PluginInit.
	void Initialize(IPluginSelf* self);
	void Shutdown();

	// Resolves the active save's session name from UCrSaveSubsystem and loads
	// its JSON config from <Plugins>\<pluginName>\<SessionName>.json. Must run
	// on the game thread (e.g. from OnExperienceLoadComplete). Force-commits
	// whatever the PREVIOUS session left dirty before switching, so a session
	// change never silently drops an edit that hadn't been written yet.
	// Returns false if no session is currently active.
	bool Reload();

	// True once Reload() has resolved a session and loaded (or created) its config.
	bool IsLoaded();

	std::string GetSessionName();

	// Reads the value at a dot-separated path (e.g. "playerAttributes.maxHealth").
	// Returns defaultValue if no session is loaded or the path doesn't exist.
	nlohmann::json Get(const std::string& path, const nlohmann::json& defaultValue);

	// Writes the value at a dot-separated path, in memory only. No-op if the
	// value already matches what's stored (nothing to commit), or if no
	// session is currently loaded. Touches no disk -- call Commit() (from the
	// engine tick) to actually persist. Safe from any thread; guarded by the
	// same lock Commit() uses.
	void Set(const std::string& path, const nlohmann::json& value);

	// Persists the in-memory config to disk if something staged by Set() is
	// still pending, atomically (temp file, then replace). Debounced so a
	// slider held down for a second doesn't turn into a write per frame --
	// call it every engine tick; it no-ops almost all of those calls (nothing
	// dirty, or the debounce window hasn't elapsed). Pass force=true to flush
	// right now regardless of the debounce -- used on shutdown and before a
	// session switch, where a delayed write would just be lost. Plain file
	// I/O only, no UObject access, so it's safe to call from the render
	// thread (PluginShutdown's RELOAD path runs there).
	void Commit(bool force = false);
}
