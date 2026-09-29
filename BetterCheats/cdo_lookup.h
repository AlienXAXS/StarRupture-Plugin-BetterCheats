#pragma once

#include "plugin_helpers.h"
#include "object_ref.h"

#include <cstdio>

// Finding a class default object (CDO) by name, shared by every feature
// module that tunes one: weapon and mining tool data assets
// (player_weapons.cpp, player_tools.cpp) and the ability/projectile CDOs
// whose fields are only reachable by reflected name.
//
// Game thread only: ObjectWalker and ObjectProperties both touch GObjects.
namespace BetterCheats
{
	// How long between lookups while a CDO or property hasn't resolved yet
	// (class package not loaded, ability not granted yet).
	// FindFirstObjectByName is a GObjects scan, too expensive to retry every
	// tick.
	constexpr float kCdoRetryInterval = 5.0f;

	// Counts `retryCooldown` down and says whether a lookup is due now,
	// restarting the countdown when it is. Each caller owns its own cooldown.
	inline bool CdoRetryDue(float& retryCooldown, float deltaSeconds)
	{
		retryCooldown -= deltaSeconds;
		if (retryCooldown > 0.0f)
			return false;
		retryCooldown = kCdoRetryInterval;
		return true;
	}

	// FindFirstObjectByName, or null while the walker isn't ready yet.
	// A GObjects scan: gate every call behind CdoRetryDue.
	inline void* FindObjectByName(const char* objectName)
	{
		IPluginHooks* hooks = GetHooks();
		IPluginObjectWalker* walker = hooks ? hooks->ObjectWalker : nullptr;
		if (!walker || !walker->IsReady())
			return nullptr;
		return walker->FindFirstObjectByName(objectName);
	}

	// A float or double UPROPERTY on a CDO, resolved by NAME through the
	// loader's IPluginObjectProperties rather than a cast through a
	// Dumper-7 struct -- offset-independent, so a class whose layout for
	// this client build is unverified can't be read or written at the wrong
	// bytes.
	struct CdoFieldHandle
	{
		ObjectRef<SDK::UObject> object;
		PluginPropertyHandle    property      = nullptr;
		bool                    ok            = false;
		bool                    loggedMiss    = false;
		float                   retryCooldown = 0.0f;
	};

	// Retries every kCdoRetryInterval seconds until "Default__<className>"
	// and its named property both resolve, then keeps the result for as
	// long as the CDO is still alive. A Blueprint class and its CDO are
	// freed when their package unloads, so the cached CDO is checked on
	// every use and looked up again once it's gone. Returns the live CDO,
	// or null. `logTag` prefixes the log lines ("Weapons", "Tools").
	inline SDK::UObject* ResolveCdoField(CdoFieldHandle& h, const char* className, const char* propertyName,
		float deltaSeconds, const char* logTag)
	{
		if (h.ok)
		{
			if (SDK::UObject* object = h.object.Get())
				return object;

			// Unloaded since it was resolved. Look it up again straight away.
			h.ok = false;
			h.retryCooldown = 0.0f;
		}

		if (!CdoRetryDue(h.retryCooldown, deltaSeconds))
			return nullptr;

		IPluginHooks* hooks = GetHooks();
		IPluginObjectWalker*     walker = hooks ? hooks->ObjectWalker     : nullptr;
		IPluginObjectProperties* props  = hooks ? hooks->ObjectProperties : nullptr;
		if (!walker || !props || !walker->IsReady() || !props->IsReady())
			return nullptr;   // not ready yet -- try again next cooldown

		char cdoName[160];
		snprintf(cdoName, sizeof(cdoName), "Default__%s", className);
		void* object = FindObjectByName(cdoName);
		if (!object)
		{
			if (!h.loggedMiss)
			{
				LOG_WARN("%s: CDO '%s' not found (yet) -- '%s' stays disabled until it resolves.",
					logTag, cdoName, propertyName);
				h.loggedMiss = true;
			}
			return nullptr;
		}

		PluginPropertyHandle property = props->FindPropertyByName(className, propertyName);
		if (!property || props->GetPropertyKind(property) != PluginPropertyKind::Float)
		{
			if (!h.loggedMiss)
			{
				LOG_WARN("%s: property '%s::%s' not found or not a float -- row stays disabled.",
					logTag, className, propertyName);
				h.loggedMiss = true;
			}
			return nullptr;
		}

		h.object.Set(static_cast<SDK::UObject*>(object));
		h.property = property;
		h.ok       = true;
		LOG_INFO("%s: resolved '%s::%s'.", logTag, className, propertyName);
		return static_cast<SDK::UObject*>(object);
	}
}
