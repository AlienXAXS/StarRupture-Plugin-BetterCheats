#pragma once

#include "plugin_interface.h"

// Gates the developer-only cheat panel. Set by the "Client Debug" configuration
// (which defines _DEBUG); "Client Release" defines NDEBUG and leaves it off, so
// nothing under this macro reaches a shipped build.
#if defined(_DEBUG)
	#define BETTERCHEATS_DEV_BUILD 1
#else
	#define BETTERCHEATS_DEV_BUILD 0
#endif

// Forward declaration to access the global plugin self pointer
IPluginSelf* GetSelf();

// Convenience wrappers used by implementation files
inline IPluginHooks*   GetHooks()   { auto* s = GetSelf(); return s ? s->hooks   : nullptr; }
inline IPluginConfig*  GetConfig()  { auto* s = GetSelf(); return s ? s->config  : nullptr; }
inline IPluginScanner* GetScanner() { auto* s = GetSelf(); return s ? s->scanner : nullptr; }

// Convenience macros for logging
#define LOG_TRACE(format, ...) if (auto s = GetSelf()) s->logger->Trace(s, format, ##__VA_ARGS__)
#define LOG_DEBUG(format, ...) if (auto s = GetSelf()) s->logger->Debug(s, format, ##__VA_ARGS__)
#define LOG_INFO(format, ...)  if (auto s = GetSelf()) s->logger->Info (s, format, ##__VA_ARGS__)
#define LOG_WARN(format, ...)  if (auto s = GetSelf()) s->logger->Warn (s, format, ##__VA_ARGS__)
#define LOG_ERROR(format, ...) if (auto s = GetSelf()) s->logger->Error(s, format, ##__VA_ARGS__)
