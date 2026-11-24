#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>

// Some building features now live in another plugin. While that plugin is
// running, BetterCheats leaves the feature to it: the detours pass through and
// the menu row is greyed out. The saved setting is never touched, so the
// feature comes back by itself when the other plugin goes away.
//
// DeferredTo() is the one place that decides. To defer another feature, add a
// row to kDeferTable.
namespace BetterCheats::Defer
{
	struct Row
	{
		const char* featureId;   // the id the feature is declared with in map_fields.h
		const char* plugin;      // the other plugin's name, as it reports it in PluginInfo
	};

	inline constexpr Row kDeferTable[] = {
		{ "build-anywhere",      "Best Builder" },
		{ "deconstruct-windows", "Best Builder" },
	};
	inline constexpr size_t kRows = sizeof(kDeferTable) / sizeof(kDeferTable[0]);

	// How often Tick() asks again.
	inline constexpr uint64_t kRefreshMs = 3000;

	namespace detail
	{
		inline std::atomic<bool>     g_running[kRows];
		inline std::atomic<uint64_t> g_lastAskMs{ 0 };
		inline std::atomic<bool>     g_asked{ false };
	}

	// Answers whether the named plugin is running. Supplied by the caller so
	// this header stays free of loader types.
	using IsRunningFn = bool (*)(const char* plugin);

	// Game thread only (call from the engine tick, never from a draw
	// callback). Asks the loader at most every kRefreshMs and caches the
	// answers in atomics that DeferredTo() reads from any thread.
	inline void Tick(uint64_t nowMs, IsRunningFn isRunning)
	{
		if (!isRunning)
			return;
		if (detail::g_asked.load() && nowMs - detail::g_lastAskMs.load() < kRefreshMs)
			return;

		detail::g_lastAskMs = nowMs;
		detail::g_asked = true;
		for (size_t i = 0; i < kRows; ++i)
			detail::g_running[i] = isRunning(kDeferTable[i].plugin);
	}

	// The plugin a feature is left to, or null when it has no row.
	inline const char* PluginFor(const char* featureId)
	{
		for (size_t i = 0; i < kRows; ++i)
		{
			if (std::strcmp(kDeferTable[i].featureId, featureId) == 0)
				return kDeferTable[i].plugin;
		}
		return nullptr;
	}

	// True while the feature is left to another plugin. Reads the cache only.
	inline bool DeferredTo(const char* featureId)
	{
		for (size_t i = 0; i < kRows; ++i)
		{
			if (std::strcmp(kDeferTable[i].featureId, featureId) == 0)
				return detail::g_running[i].load();
		}
		return false;
	}
}
