#include "player_weapons.h"
#include "plugin_helpers.h"
#include "session_config.h"
#include "aob_resolver.h"
#include "ui_widgets.h"
#include "attribute_compose.h"
#include "cheat_math.h"
#include "player_lookup.h"
#include "game_thread.h"

#include "Chimera_classes.hpp"
#include "Chimera_parameters.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace BetterCheats::Panels::Weapons
{
	namespace
	{
		// ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingStretchProp
		constexpr int kWeaponsTableFlags = (1 << 6) | (1 << 9) | (3 << 13);

		// Damage multiplier used by "One Hit Kill". Mirrors the flat 10000 the mining
		// laser hook uses in player_tools.cpp.
		constexpr float kOneHitKillDamage = 10000.0f;

		// A value is applied only when it differs from the game default, so there is no
		// separate enable toggle to keep in sync -- resetting a row IS disabling it.
		// See cheat_math.h for kActiveEpsilon/DiffersFromDefault, shared with Movement.

		// FGameplayAttributeData on UCrWeaponAttributeSet, reached via
		// ACrCharacterPlayerBase::WeaponAttributes. The set belongs to the character and
		// applies to whatever is equipped, so per-weapon profiles work by writing the
		// equipped weapon's profile every tick.
		using WeaponAttr = SDK::FGameplayAttributeData SDK::UCrWeaponAttributeSet::*;

		struct AttrDef
		{
			const char* label;
			const char* key;
			WeaponAttr  member;
			float       defaultValue;
			float       minValue;
			float       maxValue;
			float       step;
			const char* format;
			const char* tooltip;
		};

		enum AttrIndex : int
		{
			kAttrDamage = 0, kAttrFireRate, kAttrReload, kAttrMagazine, kAttrFalloff,
			kAttrRecoil, kAttrSpread, kAttrSway, kAttrADS, kAttrPierce, kAttrCount
		};

		const AttrDef kAttrs[kAttrCount] = {
			{ "Damage",             "damage",   &SDK::UCrWeaponAttributeSet::DamageModMultiplier,
			  1.0f, 0.10f,  25.0f, 0.05f, "%.2fx", "Multiplies outgoing weapon damage, attachment bonuses included." },
			{ "Fire Rate",          "fireRate", &SDK::UCrWeaponAttributeSet::FireRateModMultiplier,
			  1.0f, 0.10f,   5.0f, 0.05f, "%.2fx", "Multiplies rounds per second, attachment bonuses included." },
			{ "Reload Speed",       "reload",   &SDK::UCrWeaponAttributeSet::ReloadSpeedModMultiplier,
			  1.0f, 0.10f,   5.0f, 0.05f, "%.2fx", "Higher is faster. Multiplies on top of any attachment bonus." },
			// .member unused: Tick() special-cases this row onto the weapon data
			// asset's BaseMagazine.Value instead (g_composedMagazine) -- the clip-size
			// getter never reads MaxMagAmmoModOffset at all. Kept here only so every
			// row still has a valid member pointer for this table's shape.
			{ "Magazine Size",      "magazine", &SDK::UCrWeaponAttributeSet::MaxMagAmmoModOffset,
			  0.0f, -100.0f, 999.0f, 1.0f, "%.0f", "Flat OFFSET added to this weapon's magazine (its real base\ncapacity, not an attachment-style modifier). Negative shrinks it." },
			{ "Damage Falloff",     "falloff",  &SDK::UCrWeaponAttributeSet::DamageFallOffModMultiplier,
			  1.0f, 0.10f,   5.0f, 0.05f, "%.2fx", "Effective range before damage drops off. Multiplies on top of\nany attachment bonus." },
			{ "Recoil",             "recoil",   &SDK::UCrWeaponAttributeSet::RecoilModMultiplier,
			  1.0f, 0.00f,   3.0f, 0.05f, "%.2fx", "0 removes recoil entirely. Multiplies on top of any attachment bonus." },
			{ "Spread",             "spread",   &SDK::UCrWeaponAttributeSet::SpreadModMultiplier,
			  1.0f, 0.00f,   3.0f, 0.05f, "%.2fx", "0 is perfect accuracy. Multiplies on top of any attachment bonus." },
			{ "Sway",               "sway",     &SDK::UCrWeaponAttributeSet::SwayModMultiplier,
			  1.0f, 0.00f,   3.0f, 0.05f, "%.2fx", "0 removes weapon sway. Multiplies on top of any attachment bonus." },
			{ "ADS Speed",          "ads",      &SDK::UCrWeaponAttributeSet::ADS_TransitionSpeedModMultiplier,
			  1.0f, 0.10f,   5.0f, 0.05f, "%.2fx", "Aim-down-sights transition speed. Multiplies on top of any\nattachment bonus." },
			{ "Enemies Hit / Shot", "pierce",   &SDK::UCrWeaponAttributeSet::PossibleEnemiesHitPerTrace,
			  1.0f, 1.00f,  10.0f, 1.0f,  "%.0f",  "Projectile piercing -- how many enemies one shot passes through." },
		};

		// Apply() mode per row, aligned with AttrIndex. Multiply/Add compose onto
		// whatever the ability system aggregated (attachments included); Absolute
		// replaces it outright, which only fits Enemies Hit/Shot -- that field is a
		// literal pierce count, not a modifier layered on top of one.
		constexpr BetterCheats::ComposedAttribute::Mode kAttrModes[kAttrCount] = {
			BetterCheats::ComposedAttribute::Mode::Multiply,  // Damage (One Hit Kill overrides in Tick())
			BetterCheats::ComposedAttribute::Mode::Multiply,  // Fire Rate
			BetterCheats::ComposedAttribute::Mode::Multiply,  // Reload Speed
			BetterCheats::ComposedAttribute::Mode::Add,       // Magazine Size -- the attribute itself is an offset
			BetterCheats::ComposedAttribute::Mode::Multiply,  // Damage Falloff
			BetterCheats::ComposedAttribute::Mode::Multiply,  // Recoil
			BetterCheats::ComposedAttribute::Mode::Multiply,  // Spread
			BetterCheats::ComposedAttribute::Mode::Multiply,  // Sway
			BetterCheats::ComposedAttribute::Mode::Multiply,  // ADS Speed
			BetterCheats::ComposedAttribute::Mode::Absolute,  // Enemies Hit/Shot -- a literal count
		};

		// `match` is a lowercase substring tested against the weapon's asset name; ""
		// offers the preset on every weapon. Entries end at attr < 0.
		struct PresetVal { int attr; float value; };

		struct Preset
		{
			const char* match;    // lowercase substring of the asset name; "" = every weapon
			const char* label;
			const char* tooltip;
			const char* credit;   // original mod this is modelled on, or null
			PresetVal   vals[8];
		};

		const Preset kPresets[] = {
			{ "", "Better Weapons",
			  "Improved damage, rate of fire, magazine and range.",
			  "Modelled on 'Better Weapons' by axbhub",
			  { {kAttrDamage,1.50f},{kAttrFireRate,1.25f},{kAttrReload,1.50f},
			    {kAttrMagazine,15.0f},{kAttrFalloff,1.50f},{kAttrPierce,2.0f},{-1,0} } },

			{ "pistol", "Hand Cannon",
			  "Turns the pistol into a hand cannon: huge damage, slow heavy shots,\n"
			  "strong recoil and a smaller magazine.",
			  "Inspired by SwiftstepsKR's Hand Cannon mods",
			  { {kAttrDamage,4.00f},{kAttrFireRate,0.45f},{kAttrRecoil,1.80f},
			    {kAttrMagazine,-4.0f},{kAttrFalloff,1.50f},{kAttrPierce,3.0f},{-1,0} } },

			{ "", "Piercing",
			  "Shots pass through multiple enemies.",
			  "Modelled on SwiftstepsKR's piercing weapon mods",
			  { {kAttrPierce,3.0f},{-1,0} } },

			{ "", "Laser Beam",
			  "No recoil, no spread, no sway. Everything else left alone.",
			  nullptr,
			  { {kAttrRecoil,0.0f},{kAttrSpread,0.0f},{kAttrSway,0.0f},{-1,0} } },

			{ "", "Bullet Hose",
			  "Maximum rate of fire with a deep magazine and fast reload.",
			  nullptr,
			  { {kAttrFireRate,3.00f},{kAttrMagazine,100.0f},{kAttrReload,2.50f},
			    {kAttrSpread,0.60f},{-1,0} } },
		};
		constexpr int kPresetCount = static_cast<int>(sizeof(kPresets) / sizeof(kPresets[0]));

		// Grenade rows. UCrGrenadeWeaponItemDataBase extends
		// UCrWeaponItemDataBase, so a grenade shows up through the exact same
		// LastEquippedWeaponData/profile-discovery path as every gun -- this is
		// what used to render the standard 10-row kAttrs table (Damage, Fire
		// Rate, Recoil...) for a grenade, none of which mean anything for a
		// thrown charge. These three rows replace that table for any profile
		// whose Profile::isGrenade is set. Max/Min Charge are GAS attributes on
		// a DIFFERENT attribute set (character->GrenadeChargeAttributes, not
		// WeaponAttributes), so they need their own member-pointer type rather
		// than reusing WeaponAttr.
		using GrenadeAttr = SDK::FGameplayAttributeData SDK::UCrGrenadeChargeAttributeSet::*;

		struct GrenadeRowDef
		{
			const char* label;
			const char* key;
			GrenadeAttr member;   // unused for Charge Cost -- see the comment on that row below
			float       defaultValue, minValue, maxValue, step;
			const char* format;
			const char* tooltip;
		};

		enum GrenadeRowIndex : int { kGrenadeCost = 0, kGrenadeMax, kGrenadeMin, kGrenadeRowCount };

		const GrenadeRowDef kGrenadeRows[kGrenadeRowCount] = {
			// .member unused: Charge Cost composes onto the equipped grenade
			// TYPE's own data asset field (GrenadeThrowCostOfGrenadeCharge on
			// UCrGrenadeWeaponItemDataBase, Chimera_classes.hpp:17303), not a GAS
			// attribute on this set -- same per-type-CDO shape as Magazine Size's
			// BaseMagazine, just a plain float instead of FScalableFloat.Value.
			// See Tick()'s grenade block. Kept here only so this table's shape
			// matches kAttrs' own "every row has a member pointer" convention.
			{ "Charge Cost", "grenadeCost", nullptr,
			  1.0f, 0.00f, 2.00f, 0.05f, "%.2fx",
			  "Multiplies how many charges one throw costs. 0.00x throws for free." },
			{ "Max Charges", "grenadeMax", &SDK::UCrGrenadeChargeAttributeSet::MaxGrenadeCharge,
			  0.0f, 0.0f, 20.0f, 1.0f, "%.0f",
			  "Overrides how many charges you can bank, set directly rather than multiplied." },
			{ "Min Charge",  "grenadeMin", &SDK::UCrGrenadeChargeAttributeSet::MinGrenadeCharge,
			  0.0f, 0.0f, 5.0f, 1.0f, "%.0f",
			  "Overrides the floor your charge count can't drop below, set directly." },
		};

		constexpr BetterCheats::ComposedAttribute::Mode kGrenadeModes[kGrenadeRowCount] = {
			BetterCheats::ComposedAttribute::Mode::Multiply,  // Charge Cost
			BetterCheats::ComposedAttribute::Mode::Absolute,  // Max Charges
			BetterCheats::ComposedAttribute::Mode::Absolute,  // Min Charge
		};

		// Fuse / Blast Radius / Throw Force. These live on classes Dumper-7
		// only typed under the game's Server SDK dump (BP_GrenadeProjectile_
		// classes.hpp, GA_ThrowGrenade_classes.hpp), whose struct layout for
		// THIS client build is unverified and may not match. Resolved by NAME
		// via the loader's IPluginObjectProperties instead of a cast through
		// that Server-SDK C++ struct -- offset-independent, so a layout
		// mismatch can't silently read/write the wrong bytes.
		//
		// Unlike Charge Cost/Max/Min Charge above, each of these three lives on
		// exactly ONE shared CDO for the whole game (one projectile class, one
		// throw ability) -- not per grenade type -- so they're global controls
		// applied every tick regardless of what's currently equipped, shown
		// identically on every grenade tab rather than stored per-Profile.
		struct GrenadeGlobalDef
		{
			const char* label;
			const char* key;            // SessionConfig leaf + compose-state key segment
			const char* className;      // CDO's class, "Default__<className>" is its object name
			const char* propertyName;   // resolved via IPluginObjectProperties::FindPropertyByName
			float       defaultValue, minValue, maxValue, step;   // slider range, multiplier space
			const char* format;
			const char* tooltip;
		};

		enum GrenadeGlobalIndex : int { kGrenadeFuse = 0, kGrenadeRadius, kGrenadeThrowForce, kGrenadeGlobalCount };

		const GrenadeGlobalDef kGrenadeGlobals[kGrenadeGlobalCount] = {
			{ "Fuse Time",    "grenadeFuse",   "BP_GrenadeProjectile_C", "TimeToExplode",
			  1.0f, 0.05f, 5.00f, 0.05f, "%.2fx",
			  "Multiplies time-to-explode. Lower pops sooner. One shared projectile\nclass, so this affects every grenade type." },
			{ "Blast Radius", "grenadeRadius", "BP_GrenadeProjectile_C", "DamageRadius",
			  1.0f, 0.10f, 5.00f, 0.05f, "%.2fx",
			  "Multiplies the explosion's damage radius. One shared projectile class,\nso this affects every grenade type." },
			{ "Throw Force",  "grenadeThrow",  "GA_ThrowGrenade_C",      "ProjectileSpeed",
			  1.0f, 0.10f, 5.00f, 0.05f, "%.2fx",
			  "Multiplies how fast a thrown grenade launches. One shared throw\nability, so this affects every grenade type." },
		};

		// Generic final-value safety clamp (not the slider's own range above,
		// which bounds the multiplier the user picks) -- the real baseline
		// magnitudes for these three fields are unverified without a live
		// in-game read, so this just guards against zero/negative/runaway
		// results rather than encoding a game-specific number.
		constexpr float kGrenadeGlobalFinalMin = 0.001f;
		constexpr float kGrenadeGlobalFinalMax = 1000000.0f;

		// How long between resolution retries while a CDO/property hasn't
		// resolved yet (class package not loaded, ability not granted yet) --
		// WalkAllObjectsInto-family calls are a GObjects scan, too expensive to
		// retry every tick. Once resolved it's cached forever (CDOs don't
		// disappear), so this only ever costs anything before the player's
		// first grenade.
		constexpr float kGrenadeGlobalRetryInterval = 5.0f;

		struct GrenadeFieldHandle
		{
			void*                 object        = nullptr;
			PluginPropertyHandle  property      = nullptr;
			bool                  ok            = false;
			bool                  loggedMiss    = false;
			float                 retryCooldown = 0.0f;
		};

		GrenadeFieldHandle              g_grenadeGlobalHandle[kGrenadeGlobalCount];
		BetterCheats::ComposedAttribute g_composedGrenadeGlobal[kGrenadeGlobalCount];
		std::atomic<float>              g_grenadeGlobalValue[kGrenadeGlobalCount] = { 1.0f, 1.0f, 1.0f };

		// Debug readout, parallel to g_dbgGrenadeGame/Expected -- these three
		// aren't Profile-scoped (see above), so they get their own tiny arrays
		// rather than a slot in that one.
		std::atomic<float> g_dbgGrenadeGlobalGame[kGrenadeGlobalCount];
		std::atomic<float> g_dbgGrenadeGlobalExpected[kGrenadeGlobalCount];

		// Retries every kGrenadeGlobalRetryInterval seconds until the CDO and
		// its named property both resolve, then caches the result forever.
		// Game thread only (ObjectWalker/ObjectProperties both touch GObjects).
		bool ResolveGrenadeField(GrenadeFieldHandle& h, const char* className, const char* propertyName, float deltaSeconds)
		{
			if (h.ok) return true;

			h.retryCooldown -= deltaSeconds;
			if (h.retryCooldown > 0.0f) return false;
			h.retryCooldown = kGrenadeGlobalRetryInterval;

			IPluginHooks* hooks = GetHooks();
			IPluginObjectWalker*     walker = hooks ? hooks->ObjectWalker     : nullptr;
			IPluginObjectProperties* props  = hooks ? hooks->ObjectProperties : nullptr;
			if (!walker || !props || !walker->IsReady() || !props->IsReady())
				return false;   // not ready yet -- try again next cooldown

			char cdoName[160];
			snprintf(cdoName, sizeof(cdoName), "Default__%s", className);
			void* object = walker->FindFirstObjectByName(cdoName);
			if (!object)
			{
				if (!h.loggedMiss)
				{
					LOG_WARN("Weapons: grenade CDO '%s' not found (yet) -- '%s' stays disabled until it resolves.",
						cdoName, propertyName);
					h.loggedMiss = true;
				}
				return false;
			}

			PluginPropertyHandle property = props->FindPropertyByName(className, propertyName);
			if (!property || props->GetPropertyKind(property) != PluginPropertyKind::Float)
			{
				if (!h.loggedMiss)
				{
					LOG_WARN("Weapons: property '%s::%s' not found or not a float -- row stays disabled.",
						className, propertyName);
					h.loggedMiss = true;
				}
				return false;
			}

			h.object   = object;
			h.property = property;
			h.ok       = true;
			LOG_INFO("Weapons: resolved grenade field '%s::%s'.", className, propertyName);
			return true;
		}

		// Profiles are DISCOVERED, not hardcoded. Weapon data assets live in the paks
		// (no I_*DataItem_C classes exist in the SDK dump), so the only truthful source
		// of the roster is what the player actually equips.
		struct Profile
		{
			std::string key;        // sanitized, used as the config path segment
			std::string display;    // prettified for the tab label
			std::string raw;        // exact asset name, shown for diagnosis
			float       values[kAttrCount];
			bool        oneHitKill       = false;
			bool        infiniteMagazine = false;

			// Grenade-only fields, meaningful only when isGrenade is true --
			// discovered the same way as every other weapon, just tagged so
			// RenderImGui can swap in the grenade-specific layout.
			bool        isGrenade        = false;
			float       grenadeValues[kGrenadeRowCount];
			bool        infiniteCharges  = false;

			// Saved-presets UI state -- selected/renaming/error, one
			// instance per weapon type so switching tabs doesn't leak one
			// weapon's in-progress rename into another's.
			BetterCheats::UI::SavedPresetRowState presetRow;
		};

		// g_profiles is written from Tick (discovery, ApplySavedConfig) and iterated
		// with live Profile& references from RenderImGui on the render thread -- a
		// push_back from one side while the other holds a reference is a
		// use-after-free on reallocation. g_profilesMutex guards every access to the
		// container; callers of FindOrCreateProfile/EnsureBuiltInProfiles/
		// PersistKnownWeapons below must already hold it.
		std::mutex            g_profilesMutex;
		std::vector<Profile>  g_profiles;
		std::atomic<bool>     g_allInfiniteMagazine{ false };

		// Live readout, refreshed from Tick so the panel can show what the game actually
		// reports rather than what we hoped it would. Read from RenderImGui.
		std::atomic<float>    g_dbgMag       { -1.0f };
		std::atomic<float>    g_dbgMagMax    { -1.0f };
		std::atomic<float>    g_dbgReserve   { -1.0f };
		std::atomic<float>    g_dbgReserveMax{ -1.0f };

		// "Show live values" toggle -- default on so a fresh install proves the
		// compose fix works without the owner having to find the setting.
		std::atomic<bool> g_showLiveValues{ true };

		// One-shot confirmation logging: records in ModLoader.log that both the
		// Tick-side fill and the RenderImGui-side draw have run this session,
		// so a silently-broken live-values pipeline is diagnosable from the
		// log alone rather than from a report of "nothing happens".
		std::atomic<bool> g_loggedTickFill{ false };
		std::atomic<bool> g_loggedRender{ false };

		// The equipped weapon's real base stats, read from its data asset CDO
		// (LastEquippedWeaponData) -- the inherited Au-layer fields on
		// UCrWeaponAttributeSet (WeaponDamage, FireRate, ...) that a previous build
		// showed here are never written by the game at all (confirmed permanently
		// zero), a dead parallel layer, not a bug in how they were read.
		// -1 = not available yet, matching
		// g_dbgMag above. g_dbgWeaponBaseMagazine is the CAPTURED ORIGINAL (via
		// g_composedMagazine), not a live read, since Magazine Size writes through
		// this same field -- a live read while active would show our own offset.
		std::atomic<float> g_dbgWeaponBaseDamage       { -1.0f };
		std::atomic<float> g_dbgWeaponRoundsPerMinute  { -1.0f };
		std::atomic<float> g_dbgWeaponBaseMagazine     { -1.0f };
		std::atomic<float> g_dbgWeaponBaseRange        { -1.0f };

		// Per-row "expected = game" / "expected != game" readout, indexed like
		// kAttrs/g_composed. `game` is CurrentValue read fresh at the start of this
		// tick, before we touch it; `expected` is what we intend it to be (the
		// composed result while active, or `game` itself while inactive). Compared
		// pre-write so a lasting mismatch actually means something. `base` is
		// BaseValue (never written by us); `buffed` is the game's own aggregate
		// with attachments folded in but before our composition -- ComposedAttribute
		// ::GetGame() while active, `game` itself while inactive. `buffed` != `base`
		// is what the "+mods" tag reports. Refreshed from Tick, read from RenderImGui.
		std::atomic<float> g_dbgAttrGame[kAttrCount];
		std::atomic<float> g_dbgAttrExpected[kAttrCount];
		std::atomic<float> g_dbgAttrBase[kAttrCount];
		std::atomic<float> g_dbgAttrBuffed[kAttrCount];

		// Grenade row readouts, parallel to g_dbgAttrGame/g_dbgAttrExpected
		// above but indexed by GrenadeRowIndex instead of AttrIndex -- these three
		// rows aren't part of the per-gun kAttrs table (Charge Cost lives on a
		// different CDO type; Max/Min Charge live on a different attribute set),
		// so they get their own small parallel arrays instead of a slot in that
		// one. `unmod` is the same "captured original while active, live read
		// while not" value g_dbgWeaponBaseMagazine already computes for Magazine
		// Size, feeding RenderLiveValue's always-on "Unmodified" tooltip line.
		// Only ever populated while a grenade profile is the one equipped -- see
		// Tick()'s grenade block.
		std::atomic<float> g_dbgGrenadeGame[kGrenadeRowCount];
		std::atomic<float> g_dbgGrenadeExpected[kGrenadeRowCount];
		std::atomic<float> g_dbgGrenadeUnmod[kGrenadeRowCount];

		// Reserve ammo is an inventory item, not an attribute, so an empty inventory
		// blocks reload whatever is written to attributes. Tops up only on an observed
		// decrease: a stale read can miss a top-up, never repeat one.
		std::atomic<bool>  g_autoRestock     { false };
		std::atomic<bool>  g_restockHidden   { true };    // prefer the hidden inventory: no visible slot
		std::atomic<float> g_restockMags     { 3.0f };    // size of the one-off seed, in magazines
		std::atomic<int>   g_restockFailures { 0 };       // consecutive adds that did not raise the count
		std::atomic<int>   g_restockTotal    { 0 };       // rounds replaced this session, for the readout

		// Edge-detection state. Reset whenever the ammo type changes, because a count of
		// pistol rounds says nothing about rifle rounds.
		std::atomic<SDK::UAuItemDataBase*> g_restockItem  { nullptr };
		std::atomic<int>                   g_restockLast  { -1 };
		std::atomic<bool>                  g_restockSeeded{ false };

		constexpr int   kRestockMaxFailures     = 3;
		constexpr int   kRestockSessionCap      = 20000;

		using AddNewItemFn = SDK::TArray<SDK::FAuAddedItem>*(__fastcall*)(
			SDK::UAuItemsComponent* self, SDK::TArray<SDK::FAuAddedItem>* result,
			const SDK::UAuItemDataBase* newItem, uint32_t amount);

		AddNewItemFn ResolveAddNewItem()
		{
			static AddNewItemFn fn = nullptr;
			static bool tried = false;
			if (tried) return fn;
			tried = true;

			const uintptr_t address = AOB::Resolved().AddNewItem;
			if (!address)
			{
				LOG_WARN("Weapons: auto-restock unavailable -- AddNewItem pattern unresolved.");
				return nullptr;
			}
			fn = reinterpret_cast<AddNewItemFn>(address);
			return fn;
		}

		// The ammo item is the CDO of the class the weapon declares it needs.
		SDK::UAuItemDataBase* ResolveAmmoItem(SDK::UCrWeaponItemDataBase* weaponData)
		{
			if (!weaponData) return nullptr;

			SDK::UClass* itemClass = weaponData->RequiredItem;
			if (!itemClass || !itemClass->ClassDefaultObject) return nullptr;

			SDK::UClass* base = SDK::UAuItemDataBase::StaticClass();
			if (!base || !itemClass->ClassDefaultObject->IsA(base)) return nullptr;

			return static_cast<SDK::UAuItemDataBase*>(itemClass->ClassDefaultObject);
		}

		void RestockEquippedAmmo(SDK::ACrCharacterPlayerBase* character,
		                         SDK::UCrWeaponComponent* ws, float maxMag)
		{
			if (g_restockFailures.load() >= kRestockMaxFailures) return;
			if (g_restockTotal.load()    >= kRestockSessionCap)  return;

			SDK::UAuItemDataBase* ammo = ResolveAmmoItem(ws->LastEquippedWeaponData);
			if (!ammo) return;   // tools and anything with no declared ammo type

			AddNewItemFn addNewItem = ResolveAddNewItem();
			if (!addNewItem) return;

			// The hidden inventory is tried first: if reloads can draw from it, the
			// reserve is real but costs no visible slot.
			const bool restockHidden = g_restockHidden.load();
			SDK::UCrInventoryComponent* target = restockHidden
				? character->HiddenInventoryComponent
				: character->InventoryComponent;
			if (!target) target = character->InventoryComponent;
			if (!target) return;

			const int have = character->GetItemCount(ammo);

			// Switching weapons switches ammo type. Re-baseline rather than comparing
			// rifle rounds against a pistol count.
			bool seeded = g_restockSeeded.load();
			int  last   = g_restockLast.load();
			if (ammo != g_restockItem.load())
			{
				g_restockItem.store(ammo);
				last = have;
				g_restockLast.store(last);
				seeded = false;
				g_restockSeeded.store(false);
			}

			int amount = 0;

			if (!seeded)
			{
				// One-off seed, so switching this on with empty pockets gives you
				// something to reload from. After this, only consumption drives it.
				const int want = static_cast<int>(maxMag * g_restockMags.load() + 0.5f);
				amount = (have < want) ? (want - have) : 0;
				g_restockSeeded.store(true);
			}
			else if (have < last)
			{
				// THE EVENT: ammo actually left the inventory. Replace exactly that
				// much -- never a target, never a guess.
				amount = last - have;
			}

			if (amount <= 0)
			{
				g_restockLast.store(have);   // count rose (picked up, crafted) -- just follow it
				return;
			}

			SDK::TArray<SDK::FAuAddedItem> added{};
			addNewItem(reinterpret_cast<SDK::UAuItemsComponent*>(target), &added,
				ammo, static_cast<uint32_t>(amount));

			int gained = 0;
			for (int32_t i = 0; i < added.Num(); ++i)
				gained += added[i].Amount;

			// Backstop for what the delta rule cannot see: if adds are not landing at
			// all, stop rather than trying harder. A feature that quietly does nothing
			// beats one that fills every slot you own.
			if (gained <= 0)
			{
				if (g_restockFailures.fetch_add(1) + 1 >= kRestockMaxFailures)
				{
					g_autoRestock.store(false);
					SessionConfig::Set("playerWeapons.autoRestock", false);
					LOG_WARN("Weapons: auto-restock disabled itself -- %d adds of '%s' returned "
						"nothing (target %s inventory).",
						kRestockMaxFailures, ammo->GetName().c_str(),
						restockHidden ? "hidden" : "visible");
				}
				g_restockLast.store(have);
				return;
			}

			g_restockFailures.store(0);
			g_restockTotal.fetch_add(gained);

			// Re-read rather than assuming have+gained. If the count lags a frame the
			// worst case is a missed top-up next frame, not a repeated one.
			g_restockLast.store(character->GetItemCount(ammo));
		}

		// Written from Tick (weapon detection); read from RenderImGui for tab focus.
		std::atomic<int> g_activeProfile{ -1 };   // index into g_profiles, -1 = nothing equipped
		bool             g_configApplied = false; // set once ApplySavedConfig has run; game-thread only

		// One slot per weapon attribute, not per profile: character->WeaponAttributes
		// is a single set shared across whatever weapon is equipped (see the weapon
		// attachment audit), so switching weapons must not reset the composed state.
		// Game-thread only.
		BetterCheats::ComposedAttribute g_composed[kAttrCount];
		SDK::UCrWeaponAttributeSet*     g_composedOwner = nullptr;

		// Magazine Size composes separately from the other nine rows: the clip-size
		// getter reads LastEquippedWeaponData->BaseMagazine.Value directly (a plain
		// FScalableFloat.Value on the weapon TYPE's data asset CDO), not the GAS
		// MaxMagAmmoModOffset this row used to write. A magazine size can't
		// sensibly go to zero or negative, or run away unbounded if something
		// stacks badly, so it gets its own sanity clamp instead of the row's
		// own -100..999 slider range (that range bounds the OFFSET the slider
		// picks, not the result).
		constexpr float kMagazineFloor   = 1.0f;
		constexpr float kMagazineCeiling = 9999.0f;
		BetterCheats::ComposedAttribute g_composedMagazine;
		SDK::UCrWeaponItemDataBase*     g_magazineOwner = nullptr;

		// Charge Cost: same per-weapon-TYPE-CDO shape as Magazine Size above, just
		// a plain float member instead of FScalableFloat.Value.
		BetterCheats::ComposedAttribute    g_composedGrenadeCost;
		SDK::UCrGrenadeWeaponItemDataBase* g_grenadeCostOwner = nullptr;

		// Max/Min Charge: GAS attributes on character->GrenadeChargeAttributes, ONE
		// shared instance regardless of grenade type -- same "shared instance,
		// respawn-only Forget" shape as g_composed[]/g_composedOwner above, not the
		// per-type owner-swap-on-Release shape Magazine/Charge Cost need.
		BetterCheats::ComposedAttribute    g_composedGrenade[2];   // indexed by g - kGrenadeMax, see Tick()
		SDK::UCrGrenadeChargeAttributeSet* g_grenadeAttrOwner = nullptr;

		// Forgets every composed slot the moment the attribute set instance changes
		// (respawn, world change) -- before anything below tries to Apply/Release
		// against what may already be gone. Also drops the magazine and grenade-
		// cost slots: unlike `weapons`, their owners are per-weapon-TYPE data
		// assets, not the character, but a respawn is exactly the kind of "can't
		// trust what we think we own any more" event that should forget them too
		// rather than try to Release() against a data asset we can no longer be
		// sure is still valid.
		void ForgetComposedIfOwnerChanged(SDK::UCrWeaponAttributeSet* current)
		{
			if (current == g_composedOwner) return;
			for (int a = 0; a < kAttrCount; ++a)
				g_composed[a].Forget();
			g_composedOwner = current;

			g_composedMagazine.Forget();
			g_magazineOwner = nullptr;

			g_composedGrenadeCost.Forget();
			g_grenadeCostOwner = nullptr;

			g_composedGrenade[0].Forget();
			g_composedGrenade[1].Forget();
			g_grenadeAttrOwner = nullptr;
		}

		// Unlike `weapons` (one shared instance regardless of what's equipped),
		// LastEquippedWeaponData is a different CDO per weapon TYPE -- switching
		// weapons must restore the PREVIOUS one's magazine before adopting the new
		// one's, or the old weapon stays offset forever (ComposedAttribute's own
		// owner-change handling only ever recaptures on the new owner, it never
		// writes back to an owner it's leaving).
		void ReleaseMagazineIfOwnerChanged(SDK::UCrWeaponItemDataBase* current)
		{
			if (current == g_magazineOwner) return;
			if (g_magazineOwner)
			{
				g_composedMagazine.Release(g_magazineOwner, g_magazineOwner->BaseMagazine.Value);
				BetterCheats::ClearComposeState("playerWeapons.compose.magazine");
			}
			g_magazineOwner = current;
		}

		// Same reasoning as ReleaseMagazineIfOwnerChanged: Charge Cost lives on the
		// grenade TYPE's own CDO, not the character, so switching away from a
		// grenade (to a gun, a tool, or a different grenade type) must restore the
		// one being left before adopting the next.
		void ReleaseGrenadeCostIfOwnerChanged(SDK::UCrGrenadeWeaponItemDataBase* current)
		{
			if (current == g_grenadeCostOwner) return;
			if (g_grenadeCostOwner)
			{
				g_composedGrenadeCost.Release(g_grenadeCostOwner, g_grenadeCostOwner->GrenadeThrowCostOfGrenadeCharge);
				BetterCheats::ClearComposeState("playerWeapons.compose.grenadeCost");
			}
			g_grenadeCostOwner = current;
		}

		// Max/Min Charge share ONE instance across every grenade type (like
		// WeaponAttributes does for guns), so a weapon SWITCH must Release, not
		// Forget -- handled per-row in Tick() via `active`. Forget only belongs
		// here, on a genuine instance change (respawn, world change).
		void ForgetGrenadeAttrsIfOwnerChanged(SDK::UCrGrenadeChargeAttributeSet* current)
		{
			if (current == g_grenadeAttrOwner) return;
			g_composedGrenade[0].Forget();
			g_composedGrenade[1].Forget();
			g_grenadeAttrOwner = current;
		}

		bool IsActive(int attr, float value)
		{
			return BetterCheats::DiffersFromDefault(value, kAttrs[attr].defaultValue);
		}

		// ComposeStep/ApplyComposedRow: the capture/write/restore/persist dance
		// every composed row in this file (except the ten kAttrs rows, whose
		// own buffed/base/One-Hit-Kill bookkeeping interleaves too tightly to
		// factor out cleanly) shares -- now a BetterCheats:: helper in
		// attribute_compose.h, found here via unqualified enclosing-namespace
		// lookup, same as ComposedAttribute itself.

		std::string ToLower(const std::string& in)
		{
			std::string out = in;
			for (char& c : out)
				if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
			return out;
		}

		// The mining and building tools are equipped through the same UCrWeaponComponent,
		// so they surface here too -- but they belong to the Tools and Building panels,
		// and half this attribute set (recoil, spread, sway, ADS) is meaningless on a
		// drill. Keep them out of this panel entirely.
		bool IsWeaponAsset(const std::string& raw)
		{
			const std::string lowered = ToLower(raw);
			return lowered.find("tool") == std::string::npos;
		}

		// The game's weapon roster, so every tab is present from the start and a weapon
		// can be tuned without first going and equipping it. Discovery still runs, so
		// anything not listed here still gets its own tab.
		const char* kBuiltInWeapons[] = {
			"Default__I_PistolDataItem_C",
			"Default__I_RifleDataItem_C",
			"Default__I_ShotgunDataItem_C",
			"Default__I_MachineGunDataItem_C",
		};
		constexpr int kBuiltInCount = static_cast<int>(sizeof(kBuiltInWeapons) / sizeof(kBuiltInWeapons[0]));

		// "Default__I_RifleDataItem_C" -> "Rifle";  "...MachineGunDataItem_C" -> "Machine Gun"
		std::string Prettify(const std::string& raw)
		{
			std::string s = raw;
			auto stripPrefix = [&](const char* p) {
				const size_t n = std::string(p).size();
				if (s.size() > n && s.compare(0, n, p) == 0) s.erase(0, n);
			};
			auto stripSuffix = [&](const char* p) {
				const size_t n = std::string(p).size();
				if (s.size() > n && s.compare(s.size() - n, n, p) == 0) s.erase(s.size() - n);
			};
			stripPrefix("Default__");
			stripPrefix("I_");
			stripPrefix("BP_");
			stripSuffix("_C");
			stripSuffix("DataItem");
			stripSuffix("Data");
			stripSuffix("Item");

			// split CamelCase into words
			std::string out;
			for (size_t i = 0; i < s.size(); ++i)
			{
				if (i > 0 && s[i] >= 'A' && s[i] <= 'Z' && !(s[i-1] >= 'A' && s[i-1] <= 'Z'))
					out += ' ';
				out += s[i];
			}
			return out.empty() ? raw : out;
		}

		std::string Sanitize(const std::string& raw)
		{
			std::string out;
			for (char c : ToLower(raw))
				if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) out += c;
			return out.empty() ? "unknown" : out;
		}

		std::string ConfigKey(const Profile& p, const char* attrKey, const char* leaf)
		{
			return std::string("playerWeapons.w.") + p.key + "." + attrKey + "." + leaf;
		}

		void ResetProfileValues(Profile& p)
		{
			for (int a = 0; a < kAttrCount; ++a)
				p.values[a] = kAttrs[a].defaultValue;
			for (int g = 0; g < kGrenadeRowCount; ++g)
				p.grenadeValues[g] = kGrenadeRows[g].defaultValue;
		}

		void LoadProfileFromConfig(Profile& p)
		{
			if (!SessionConfig::IsLoaded())
				return;
			for (int a = 0; a < kAttrCount; ++a)
				p.values[a] = SessionConfig::Get(ConfigKey(p, kAttrs[a].key, "value"), p.values[a]);
			p.oneHitKill       = SessionConfig::Get(ConfigKey(p, "oneHitKill", "enabled"), false);
			p.infiniteMagazine = SessionConfig::Get(ConfigKey(p, "infiniteMagazine", "enabled"), false);

			for (int g = 0; g < kGrenadeRowCount; ++g)
				p.grenadeValues[g] = SessionConfig::Get(ConfigKey(p, kGrenadeRows[g].key, "value"), p.grenadeValues[g]);
			p.infiniteCharges = SessionConfig::Get(ConfigKey(p, "infiniteCharges", "enabled"), false);
		}

		// Caller must hold g_profilesMutex.
		void PersistKnownWeapons()
		{
			if (!SessionConfig::IsLoaded())
				return;
			nlohmann::json arr = nlohmann::json::array();
			for (const Profile& p : g_profiles)
				arr.push_back({ {"key", p.key}, {"display", p.display}, {"raw", p.raw}, {"isGrenade", p.isGrenade} });
			SessionConfig::Set("playerWeapons.known", arr);
		}

		int FindOrCreateProfile(const std::string& rawName, bool isGrenade);

		// Make sure every known weapon has a tab, whether or not it has been equipped.
		// Caller must hold g_profilesMutex.
		void EnsureBuiltInProfiles()
		{
			static bool s_done = false;
			if (s_done) return;
			s_done = true;
			for (int i = 0; i < kBuiltInCount; ++i)
				FindOrCreateProfile(kBuiltInWeapons[i], false);
		}

		// Caller must hold g_profilesMutex.
		int FindOrCreateProfile(const std::string& rawName, bool isGrenade)
		{
			const std::string key = Sanitize(rawName);
			for (size_t i = 0; i < g_profiles.size(); ++i)
				if (g_profiles[i].key == key)
					return static_cast<int>(i);

			Profile p;
			p.key       = key;
			p.raw       = rawName;
			p.display   = Prettify(rawName);
			p.isGrenade = isGrenade;
			ResetProfileValues(p);
			g_profiles.push_back(p);
			LoadProfileFromConfig(g_profiles.back());
			PersistKnownWeapons();

			LOG_INFO("Weapons: discovered weapon '%s' -> tab '%s'%s", rawName.c_str(), p.display.c_str(),
				isGrenade ? " (grenade)" : "");
			return static_cast<int>(g_profiles.size()) - 1;
		}

		// LastEquippedWeaponData (used below to pick the active profile) only becomes a
		// grenade once one has actually been thrown/charged, so a grenade that has only
		// ever been equipped never gets a tab. GetGrenadeItemData reports whatever grenade
		// is currently equipped regardless of charge state -- game-thread only, like every
		// other UObject/ProcessEvent call here.
		SDK::UCrGrenadeWeaponItemDataBase* GetEquippedGrenadeItemData(SDK::UCrWeaponComponent* weaponSystem)
		{
			if (!weaponSystem) return nullptr;

			static SDK::UFunction* func = nullptr;
			if (!func)
				func = SDK::UCrWeaponComponent::StaticClass()->GetFunction("CrWeaponComponent", "GetGrenadeItemData");
			if (!func)
			{
				LOG_WARN("Weapons: could not resolve UCrWeaponComponent::GetGrenadeItemData.");
				return nullptr;
			}

			SDK::Params::CrWeaponComponent_GetGrenadeItemData parms{};
			const auto flags = func->FunctionFlags;
			func->FunctionFlags |= 0x400;
			weaponSystem->ProcessEvent(func, &parms);
			func->FunctionFlags = flags;

			return parms.ReturnValue ? parms.OutGrenadeData : nullptr;
		}

	}

	void Tick(float deltaSeconds)
	{
		BetterCheats::RecordGameThread();

		// ApplySavedConfig() is driven by save load, which a hot-reloaded plugin has
		// already missed. plugin.cpp does call OnExperienceLoadComplete() on reload, but
		// from PluginInit's thread -- and SessionConfig::Reload() resolves the save name
		// through UCrSaveSubsystem, which needs the game thread. It returns false there,
		// so the config silently never loads and NOTHING persists. Retry from the engine
		// tick, which is the game thread.
		if (!g_configApplied && SessionConfig::IsLoaded())
			ApplySavedConfig();

		{
			std::lock_guard<std::mutex> lock(g_profilesMutex);
			EnsureBuiltInProfiles();
		}

		SDK::ACrCharacterPlayerBase* character = GetLocalCharacter();
		if (!character || !character->WeaponSystem)
			return;

		// Tab presence for whatever grenade is currently equipped, independent of the
		// active-profile selection below -- see GetEquippedGrenadeItemData.
		if (SDK::UCrGrenadeWeaponItemDataBase* grenadeData = GetEquippedGrenadeItemData(character->WeaponSystem))
		{
			std::lock_guard<std::mutex> lock(g_profilesMutex);
			FindOrCreateProfile(grenadeData->GetName(), true);
		}

		if (SDK::UCrWeaponItemDataBase* data = character->WeaponSystem->LastEquippedWeaponData)
		{
			const std::string raw = data->GetName();

			// Holding a tool leaves no active weapon profile, so nothing is applied.
			if (IsWeaponAsset(raw))
			{
				const bool isGrenade = data->IsA(SDK::UCrGrenadeWeaponItemDataBase::StaticClass());
				std::lock_guard<std::mutex> lock(g_profilesMutex);
				g_activeProfile.store(FindOrCreateProfile(raw, isGrenade));
			}
			else
			{
				g_activeProfile.store(-1);
			}
		}

		// Copy the active profile out under the lock instead of keeping a reference
		// into g_profiles across the SDK calls below -- a newly discovered weapon can
		// push_back and reallocate the vector from another Tick call. This also
		// used to `return` here whenever no weapon profile was active (holding a
		// tool, nothing equipped), which silently killed the live-values readout
		// below entirely -- unlike Movement, whose
		// equivalent update has no such gate and always runs. Fall back to an
		// all-default profile instead: IsActive() reads false for every row, so
		// the compose loop below Releases everything (correct -- nothing should be
		// overridden with no weapon out) while still updating every debug atomic.
		Profile profile;
		bool haveActiveProfile = false;
		{
			std::lock_guard<std::mutex> lock(g_profilesMutex);
			const int active = g_activeProfile.load();
			if (active >= 0 && active < static_cast<int>(g_profiles.size()))
			{
				profile = g_profiles[active];
				haveActiveProfile = true;
			}
		}
		if (!haveActiveProfile)
			ResetProfileValues(profile);   // oneHitKill/infiniteMagazine already default false

		try
		{
			SDK::UCrWeaponAttributeSet* weapons = character->WeaponAttributes;
			ForgetComposedIfOwnerChanged(weapons);

			if (weapons)
			{
				for (int a = 0; a < kAttrCount; ++a)
				{
					if (a == kAttrMagazine)
						continue;   // composes onto the weapon data asset instead -- see below

					SDK::FGameplayAttributeData& attr = weapons->*kAttrs[a].member;
					const std::string key = std::string("playerWeapons.compose.") + kAttrs[a].key;

					// Captured before we touch anything: "game" for the readout, and
					// (when this row was already active going into this tick) the
					// basis for "expected" -- what our last write should still read
					// back as if nothing has overridden it since.
					const bool  wasActive     = g_composed[a].IsActive();
					const float previousWrite = g_composed[a].GetWritten();
					if (!wasActive)
						BetterCheats::RestoreIfStale(key, attr.CurrentValue);
					const float gameBefore = attr.CurrentValue;

					bool active = true;
					if (a == kAttrDamage && profile.oneHitKill)
					{
						// Absolute wins outright; Release() hands Damage back to the
						// game's own aggregate once this turns off.
						g_composed[a].Apply(weapons, attr.CurrentValue, kOneHitKillDamage,
							BetterCheats::ComposedAttribute::Mode::Absolute,
							kOneHitKillDamage, kOneHitKillDamage);
					}
					else if (IsActive(a, profile.values[a]))
					{
						g_composed[a].Apply(weapons, attr.CurrentValue, profile.values[a], kAttrModes[a],
							kAttrs[a].minValue, kAttrs[a].maxValue);
					}
					else
					{
						active = false;
						g_composed[a].Release(weapons, attr.CurrentValue);
					}

					// Recaptured (first activation, or the game re-aggregated) --
					// persist so a botched hot-reload can tell this from a stale
					// leftover next time. Never every frame.
					if (active && (!wasActive || gameBefore != previousWrite))
						BetterCheats::SaveComposeState(key, g_composed[a].GetGame(), g_composed[a].GetWritten());
					else if (!active)
						BetterCheats::ClearComposeState(key);

					// On the very first activation frame there is no previous write to
					// compare against yet -- expected is trivially "game" until next tick.
					const float expected = active ? (wasActive ? previousWrite : gameBefore) : gameBefore;
					// "Buffed" (base + attachments, ours excluded): GetGame() reflects
					// what Apply() just composed from -- valid once active. Inactive rows
					// never had that captured, but CurrentValue is already untouched by
					// us, so it already IS base + attachments.
					const float buffed = active ? g_composed[a].GetGame() : gameBefore;
					g_dbgAttrGame[a].store(gameBefore);
					g_dbgAttrExpected[a].store(expected);
					g_dbgAttrBase[a].store(attr.BaseValue);
					g_dbgAttrBuffed[a].store(buffed);
				}

				if (!g_loggedTickFill.exchange(true))
					LOG_INFO("Weapons: live values first filled by Tick (weapon '%s').", profile.display.c_str());
			}

			// Magazine Size: composes (Add) onto LastEquippedWeaponData->BaseMagazine
			// .Value, the weapon TYPE's own data asset field the clip-size getter
			// actually reads -- not a GAS attribute, so it's outside the
			// `weapons`-gated loop above and keyed by the data asset, not the character.
			SDK::UCrWeaponItemDataBase* equippedData = character->WeaponSystem->LastEquippedWeaponData;
			ReleaseMagazineIfOwnerChanged(equippedData);

			if (equippedData)
			{
				const bool active = IsActive(kAttrMagazine, profile.values[kAttrMagazine]);
				const ComposeStep step = ApplyComposedRow(g_composedMagazine, equippedData,
					equippedData->BaseMagazine.Value, "playerWeapons.compose.magazine",
					active, profile.values[kAttrMagazine], BetterCheats::ComposedAttribute::Mode::Add,
					kMagazineFloor, kMagazineCeiling);

				g_dbgAttrGame[kAttrMagazine].store(step.game);
				g_dbgAttrExpected[kAttrMagazine].store(step.expected);

				// Weapon base stats -- real CDO fields the game itself uses, never
				// written by us except BaseMagazine, so show the ORIGINAL we captured
				// rather than a live read that would show our own offset while active.
				g_dbgWeaponBaseDamage.store(equippedData->BaseDamage.Value);
				g_dbgWeaponRoundsPerMinute.store(equippedData->RoundsPerMinute.Value);
				g_dbgWeaponBaseRange.store(equippedData->BaseRange.Value);
				g_dbgWeaponBaseMagazine.store(active ? g_composedMagazine.GetGame() : step.game);
			}

			// Grenade rows. Charge Cost lives on the equipped grenade
			// TYPE's own data asset (UCrGrenadeWeaponItemDataBase extends
			// UCrWeaponItemDataBase, so LastEquippedWeaponData covers it too) --
			// same per-type-CDO shape as BaseMagazine above. Max/Min Charge are GAS
			// attributes on character->GrenadeChargeAttributes, ONE shared instance
			// regardless of grenade type -- same "shared instance, active profile
			// only" convention as WeaponAttributes above. All three, and Infinite
			// Charges below, apply only while a grenade profile is the one
			// currently equipped, matching every other per-weapon row in this
			// file: switching to a gun or tool releases them exactly like
			// switching weapons releases Damage/Fire Rate/etc.
			SDK::UCrGrenadeWeaponItemDataBase* grenadeData =
				(equippedData && equippedData->IsA(SDK::UCrGrenadeWeaponItemDataBase::StaticClass()))
					? static_cast<SDK::UCrGrenadeWeaponItemDataBase*>(equippedData)
					: nullptr;
			ReleaseGrenadeCostIfOwnerChanged(grenadeData);

			if (grenadeData)
			{
				const bool active = BetterCheats::DiffersFromDefault(
					profile.grenadeValues[kGrenadeCost], kGrenadeRows[kGrenadeCost].defaultValue);
				const ComposeStep step = ApplyComposedRow(g_composedGrenadeCost, grenadeData,
					grenadeData->GrenadeThrowCostOfGrenadeCharge, "playerWeapons.compose.grenadeCost",
					active, profile.grenadeValues[kGrenadeCost], kGrenadeModes[kGrenadeCost],
					kGrenadeRows[kGrenadeCost].minValue, kGrenadeRows[kGrenadeCost].maxValue);

				g_dbgGrenadeGame[kGrenadeCost].store(step.game);
				g_dbgGrenadeExpected[kGrenadeCost].store(step.expected);
				g_dbgGrenadeUnmod[kGrenadeCost].store(active ? g_composedGrenadeCost.GetGame() : step.game);
			}

			SDK::UCrGrenadeChargeAttributeSet* grenadeAttrs = character->GrenadeChargeAttributes;
			ForgetGrenadeAttrsIfOwnerChanged(grenadeAttrs);

			if (grenadeAttrs)
			{
				for (int g = kGrenadeMax; g <= kGrenadeMin; ++g)
				{
					const GrenadeRowDef& def = kGrenadeRows[g];
					float& value = (grenadeAttrs->*def.member).CurrentValue;
					const bool active = grenadeData && BetterCheats::DiffersFromDefault(profile.grenadeValues[g], def.defaultValue);
					BetterCheats::ComposedAttribute& composed = g_composedGrenade[g - kGrenadeMax];
					const std::string key = std::string("playerWeapons.compose.") + def.key;

					const ComposeStep step = ApplyComposedRow(composed, grenadeAttrs, value, key,
						active, profile.grenadeValues[g], kGrenadeModes[g], def.minValue, def.maxValue);

					g_dbgGrenadeGame[g].store(step.game);
					g_dbgGrenadeExpected[g].store(step.expected);
					g_dbgGrenadeUnmod[g].store(active ? composed.GetGame() : step.game);
				}

				// Infinite Charges: tops CurrentGrenadeCharge up to Max when it
				// drops, exactly like Infinite magazine tops SetEquippedWeaponCurrentAmmo
				// below -- never an unconditional write every tick (which would
				// fight a regen-style LEM/buff), only a top-up on the edge where
				// it's short of whatever Max is currently honouring (the game's
				// own, or our Max Charges override just above).
				if (grenadeData && profile.infiniteCharges)
				{
					const float maxCharge = grenadeAttrs->MaxGrenadeCharge.CurrentValue;
					const float curCharge = grenadeAttrs->CurrentGrenadeCharge.CurrentValue;
					if (maxCharge > 0.0f && curCharge < maxCharge)
						grenadeAttrs->CurrentGrenadeCharge.CurrentValue = maxCharge;
				}
			}

			// Fuse / Blast Radius / Throw Force: global, not gated by whether a
			// grenade is currently equipped -- see the GrenadeGlobalDef comment
			// above for why.
			{
				IPluginHooks* hooks = GetHooks();
				IPluginObjectProperties* props = hooks ? hooks->ObjectProperties : nullptr;

				for (int g = 0; g < kGrenadeGlobalCount; ++g)
				{
					const GrenadeGlobalDef& def = kGrenadeGlobals[g];
					if (!props || !ResolveGrenadeField(g_grenadeGlobalHandle[g], def.className, def.propertyName, deltaSeconds))
						continue;

					GrenadeFieldHandle& h = g_grenadeGlobalHandle[g];
					double raw = 0.0;
					if (!props->GetFloatProperty(h.object, h.property, &raw))
						continue;

					float valueF = static_cast<float>(raw);
					const float sliderValue = g_grenadeGlobalValue[g].load();
					const bool  active = BetterCheats::DiffersFromDefault(sliderValue, def.defaultValue);
					const std::string key = std::string("playerWeapons.compose.") + def.key;

					const ComposeStep step = ApplyComposedRow(g_composedGrenadeGlobal[g], h.object, valueF, key,
						active, sliderValue, BetterCheats::ComposedAttribute::Mode::Multiply,
						kGrenadeGlobalFinalMin, kGrenadeGlobalFinalMax);

					g_dbgGrenadeGlobalGame[g].store(step.game);
					g_dbgGrenadeGlobalExpected[g].store(step.expected);

					if (raw != static_cast<double>(valueF))
						props->SetFloatProperty(h.object, h.property, static_cast<double>(valueF));
				}
			}

			// Ammo (the reserve pool) is Net/RepNotify and reverts on the next replication
			// tick if written directly; the unreplicated Cr multiplier is what sticks.
			// Holding the magazine at full is what survives: it never empties, so it never
			// reloads, so the reserve is never drawn from.
			SDK::UCrWeaponComponent* ws = character->WeaponSystem;

			const float maxMag = ws->GetEquippedWeaponMaxMagazineAmmo();
			const float curMag = ws->GetEquippedWeaponCurrentAmmo();
			g_dbgMag.store(curMag);
			g_dbgMagMax.store(maxMag);
			g_dbgReserve.store(ws->GetEquippedWeaponAmmoInInventory());
			g_dbgReserveMax.store(ws->GetEquippedWeaponMaxAmmo());

			if (g_allInfiniteMagazine.load() || profile.infiniteMagazine)
			{
				if (maxMag > 0.0f && curMag < maxMag)
					ws->SetEquippedWeaponCurrentAmmo(maxMag);
			}

			// Auto-restock. Called every frame, but it only WRITES on the edge where
			// the ammo count has actually dropped -- see the note above.
			if (g_autoRestock.load() && maxMag > 0.0f)
				RestockEquippedAmmo(character, ws, maxMag);

		}
		catch (...) {}
	}

	void Shutdown()
	{
		// The loader's own RELOAD button runs PluginShutdown from its D3D Present
		// hook, not the game thread Tick() recorded -- GetLocalCharacter() and
		// every UObject touch below intermittently crash there (see player_lookup.h).
		// Forget instead of Release: SessionConfig still has whatever RestoreIfStale
		// needs to undo a leftover write on the next activation, off-thread or not.
		if (!BetterCheats::IsGameThread())
		{
			for (int a = 0; a < kAttrCount; ++a)
				g_composed[a].Forget();
			g_composedMagazine.Forget();
			g_composedGrenadeCost.Forget();
			g_composedGrenade[0].Forget();
			g_composedGrenade[1].Forget();
			for (int g = 0; g < kGrenadeGlobalCount; ++g)
				g_composedGrenadeGlobal[g].Forget();
			return;
		}

		// Same character the composed state was captured against -- safe to hand
		// CurrentValue back. A different or null character means the attribute set
		// this state refers to is already gone; ForgetComposedIfOwnerChanged would
		// just no-op the writes below anyway, but skip the SDK call entirely.
		SDK::ACrCharacterPlayerBase* character = GetLocalCharacter();
		SDK::UCrWeaponAttributeSet* weapons = character ? character->WeaponAttributes : nullptr;
		SDK::UCrGrenadeChargeAttributeSet* grenadeAttrs = character ? character->GrenadeChargeAttributes : nullptr;

		try
		{
			if (weapons && weapons == g_composedOwner)
			{
				for (int a = 0; a < kAttrCount; ++a)
				{
					if (a == kAttrMagazine) continue;
					g_composed[a].Release(weapons, (weapons->*kAttrs[a].member).CurrentValue);
					BetterCheats::ClearComposeState(std::string("playerWeapons.compose.") + kAttrs[a].key);
				}
			}
			else
			{
				for (int a = 0; a < kAttrCount; ++a)
					g_composed[a].Forget();
			}

			if (g_magazineOwner)
			{
				g_composedMagazine.Release(g_magazineOwner, g_magazineOwner->BaseMagazine.Value);
				BetterCheats::ClearComposeState("playerWeapons.compose.magazine");
			}
			else
			{
				g_composedMagazine.Forget();
			}

			if (g_grenadeCostOwner)
			{
				g_composedGrenadeCost.Release(g_grenadeCostOwner, g_grenadeCostOwner->GrenadeThrowCostOfGrenadeCharge);
				BetterCheats::ClearComposeState("playerWeapons.compose.grenadeCost");
			}
			else
			{
				g_composedGrenadeCost.Forget();
			}

			if (grenadeAttrs && grenadeAttrs == g_grenadeAttrOwner)
			{
				g_composedGrenade[0].Release(grenadeAttrs, grenadeAttrs->MaxGrenadeCharge.CurrentValue);
				BetterCheats::ClearComposeState("playerWeapons.compose.grenadeMax");
				g_composedGrenade[1].Release(grenadeAttrs, grenadeAttrs->MinGrenadeCharge.CurrentValue);
				BetterCheats::ClearComposeState("playerWeapons.compose.grenadeMin");
			}
			else
			{
				g_composedGrenade[0].Forget();
				g_composedGrenade[1].Forget();
			}

			// Fuse/Blast Radius/Throw Force: global CDOs, not tied to the local
			// character at all -- resolved once, valid regardless of who (or
			// whether anyone) is currently possessed, so no owner-swap check.
			IPluginHooks* hooks = GetHooks();
			IPluginObjectProperties* props = hooks ? hooks->ObjectProperties : nullptr;
			for (int g = 0; g < kGrenadeGlobalCount; ++g)
			{
				GrenadeFieldHandle& h = g_grenadeGlobalHandle[g];
				if (props && h.ok)
				{
					double raw = 0.0;
					if (props->GetFloatProperty(h.object, h.property, &raw))
					{
						float valueF = static_cast<float>(raw);
						g_composedGrenadeGlobal[g].Release(h.object, valueF);
						if (raw != static_cast<double>(valueF))
							props->SetFloatProperty(h.object, h.property, static_cast<double>(valueF));
					}
					BetterCheats::ClearComposeState(std::string("playerWeapons.compose.") + kGrenadeGlobals[g].key);
				}
				else
				{
					g_composedGrenadeGlobal[g].Forget();
				}
			}
		}
		catch (...) {}
	}

	void ApplySavedConfig()
	{
		if (!SessionConfig::IsLoaded())
			return;

		size_t profileCount = 0;
		{
			std::lock_guard<std::mutex> lock(g_profilesMutex);

			// Restore previously discovered weapons so their tabs exist before the
			// player re-equips them.
			g_profiles.clear();
			const nlohmann::json known = SessionConfig::Get("playerWeapons.known", nlohmann::json::array());
			if (known.is_array())
			{
				bool dropped = false;
				for (const auto& e : known)
				{
					Profile p;
					p.key       = e.value("key", std::string());
					p.display   = e.value("display", std::string());
					p.raw       = e.value("raw", std::string());
					p.isGrenade = e.value("isGrenade", false);
					if (p.key.empty()) continue;

					// Drop tools recorded by an earlier build before they were filtered out.
					if (!p.raw.empty() && !IsWeaponAsset(p.raw)) { dropped = true; continue; }

					if (p.display.empty()) p.display = p.key;
					ResetProfileValues(p);
					g_profiles.push_back(p);
					LoadProfileFromConfig(g_profiles.back());
				}
				if (dropped)
					PersistKnownWeapons();
			}

			// ApplySavedConfig() rebuilds the list from config, which may predate the
			// built-in roster -- re-seed so every weapon still has a tab.
			for (int i = 0; i < kBuiltInCount; ++i)
				FindOrCreateProfile(kBuiltInWeapons[i], false);

			profileCount = g_profiles.size();
		}

		g_allInfiniteMagazine.store(SessionConfig::Get("playerWeapons.allInfiniteMagazine", false));
		g_autoRestock.store(SessionConfig::Get("playerWeapons.autoRestock", false));
		g_restockHidden.store(SessionConfig::Get("playerWeapons.restockHidden", true));
		g_restockMags.store(SessionConfig::Get("playerWeapons.restockMags", 3.0f));
		g_showLiveValues.store(SessionConfig::Get("playerWeapons.showLiveValues", true));

		for (int g = 0; g < kGrenadeGlobalCount; ++g)
			g_grenadeGlobalValue[g].store(SessionConfig::Get(
				std::string("playerWeapons.") + kGrenadeGlobals[g].key + ".value", kGrenadeGlobals[g].defaultValue));

		g_configApplied = true;

		LOG_INFO("Weapons: applied saved config for session '%s' (%zu known weapons).",
			SessionConfig::GetSessionName().c_str(), profileCount);
	}

	void RenderImGui(IModLoaderImGui* imgui)
	{
		if (!g_loggedRender.exchange(true))
			LOG_INFO("Weapons: live values readout rendering (%d rows).", kAttrCount);

		// First control in the tab, unconditional -- burying this below a
		// disclaimer line made it easy to miss entirely.
		bool showLiveValues = g_showLiveValues.load();
		if (imgui->Checkbox("Show live values", &showLiveValues))
		{
			g_showLiveValues.store(showLiveValues);
			SessionConfig::Set("playerWeapons.showLiveValues", showLiveValues);
		}
		if (imgui->IsItemHovered())
			imgui->SetTooltip("Shows the game's own numbers next to each slider, so a change is\n"
			                  "obvious instead of a guess.");
		imgui->SameLine(0.0f, 12.0f);
		imgui->TextDisabled("Shows the weapon in your hand. Equip one to see its stats.");

		imgui->TextDisabled("Multipliers apply on top of the weapon's base stats and any attachments.");

		{
			std::lock_guard<std::mutex> lock(g_profilesMutex);
			EnsureBuiltInProfiles();
		}

		imgui->SeparatorText("Ammo");

		bool allInfiniteMagazine = g_allInfiniteMagazine.load();
		if (imgui->Checkbox("Infinite clip - ALL weapons", &allInfiniteMagazine))
		{
			g_allInfiniteMagazine.store(allInfiniteMagazine);
			SessionConfig::Set("playerWeapons.allInfiniteMagazine", allInfiniteMagazine);
		}
		if (imgui->IsItemHovered())
			imgui->SetTooltip("Holds the clip at full. It never empties, so it never reloads -- and\n"
			                  "because it never reloads, your reserve is never drawn from either.\n\n"
			                  "Per-weapon equivalents are on each weapon's tab below.");

		bool autoRestock = g_autoRestock.load();
		if (imgui->Checkbox("Keep ammo stocked", &autoRestock))
		{
			g_autoRestock.store(autoRestock);
			SessionConfig::Set("playerWeapons.autoRestock", autoRestock);
			g_restockFailures.store(0);
			g_restockItem.store(nullptr);   // re-baseline and re-seed on next tick
			g_restockLast.store(-1);
			g_restockSeeded.store(false);
		}
		if (imgui->IsItemHovered())
			imgui->SetTooltip("Tops the equipped weapon's own ammo back up so you never run dry.\n"
			                  "The clip drains and you reload normally -- there is just always\n"
			                  "something to reload from.\n\n"
			                  "It follows whatever you are holding, using the ammo type the weapon\n"
			                  "itself declares, and only ever adds the shortfall.");

		if (autoRestock)
		{
			imgui->Indent(imgui->GetFrameHeight());

			bool restockHidden = g_restockHidden.load();
			if (imgui->Checkbox("Keep it out of my inventory", &restockHidden))
			{
				g_restockHidden.store(restockHidden);
				SessionConfig::Set("playerWeapons.restockHidden", restockHidden);
			}
			if (imgui->IsItemHovered())
				imgui->SetTooltip("Stocks the hidden inventory instead of your visible one, so it costs\n"
				                  "no slot. If reloads cannot draw from there, untick this and it will\n"
				                  "use your normal inventory instead.");

			float availX = 0.0f, availY = 0.0f;
			imgui->GetContentRegionAvail(&availX, &availY);
			float textW = 0.0f, textH = 0.0f;
			imgui->CalcTextSize("-88.88", &textW, &textH, false, -1.0f);
			const float frameH = imgui->GetFrameHeight();

			imgui->SetNextItemWidth(textW + (frameH * 2.0f) + (frameH * 0.9f));
			float restockMags = g_restockMags.load();
			if (imgui->InputFloat("Clips to keep in reserve", &restockMags, 1.0f, 5.0f, "%.0f"))
			{
				if (restockMags < 1.0f)   restockMags = 1.0f;
				if (restockMags > 100.0f) restockMags = 100.0f;
				g_restockMags.store(restockMags);
				SessionConfig::Set("playerWeapons.restockMags", restockMags);
			}

			imgui->Unindent(imgui->GetFrameHeight());
		}

		const float dbgMagMax = g_dbgMagMax.load();
		if (dbgMagMax >= 0.0f)
		{
			char line[220];
			snprintf(line, sizeof(line), "  clip %.0f / %.0f      reserve %.0f / %.0f%s",
				g_dbgMag.load(), dbgMagMax, g_dbgReserve.load(), g_dbgReserveMax.load(),
				g_restockTotal.load() > 0 ? "      [restocking]" : "");
			imgui->TextDisabled(line);
		}

		// The equipped weapon's real base stats, from its data asset -- not the
		// inherited Au-layer attribute fields a previous build showed here, which
		// the game never writes at all (confirmed permanently zero). Magazine
		// shows the CAPTURED ORIGINAL, so it reads the same whether or not
		// Magazine Size is active.
		if (g_showLiveValues.load() && dbgMagMax >= 0.0f)
		{
			char dmg[32], rpm[32], mag[32], range[32];
			BetterCheats::UI::FormatLiveValue(dmg,   sizeof(dmg),   g_dbgWeaponBaseDamage.load());
			BetterCheats::UI::FormatLiveValue(rpm,   sizeof(rpm),   g_dbgWeaponRoundsPerMinute.load());
			BetterCheats::UI::FormatLiveValue(mag,   sizeof(mag),   g_dbgWeaponBaseMagazine.load());
			BetterCheats::UI::FormatLiveValue(range, sizeof(range), g_dbgWeaponBaseRange.load());
			char line[220];
			snprintf(line, sizeof(line),
				"  weapon base: dmg %s  rounds/min %s  mag %s  range %s",
				dmg, rpm, mag, range);
			imgui->TextDisabled(line);
		}

		imgui->Spacing();
		imgui->SeparatorText("Per-Weapon");
		imgui->TextDisabled("A value differing from the default is applied. Reset a row to turn it off.");
		imgui->Spacing();

		// Widest attribute label decides where the live-values readout starts, so
		// every row's readout lines up regardless of that row's own label length.
		const float labelReserve = BetterCheats::UI::PrescanLabelWidth(imgui, kAttrCount,
			[](int a) { return kAttrs[a].label; });

		bool profilesEmpty = false;
		{
			std::lock_guard<std::mutex> lock(g_profilesMutex);
			profilesEmpty = g_profiles.empty();
		}
		if (profilesEmpty)
		{
			imgui->TextDisabled("No weapons seen yet - equip one and its tab will appear here.");
			return;
		}

		if (!imgui->BeginTabBar("##weapon_tabs", 0))
			return;

		static int s_lastFocused = -1;
		const int activeProfile = g_activeProfile.load();
		const bool focusChanged = (activeProfile != s_lastFocused);
		s_lastFocused = activeProfile;

		// Tabs are discovered in equip order, which is arbitrary. Present them in the
		// order the game itself progresses through them, with tools trailing.
		static const char* kTabOrder[] = { "pistol", "rifle", "shotgun", "machine" };
		constexpr int kTabOrderCount = static_cast<int>(sizeof(kTabOrder) / sizeof(kTabOrder[0]));

		// g_profiles is only ever touched under this lock, held for the whole tab loop
		// below because every Profile& taken from it stays live throughout.
		{
		std::lock_guard<std::mutex> profilesLock(g_profilesMutex);

		std::vector<int> order(g_profiles.size());
		for (size_t i = 0; i < order.size(); ++i) order[i] = static_cast<int>(i);
		std::sort(order.begin(), order.end(), [&](int a, int b)
		{
			auto rank = [](const std::string& raw) {
				const std::string lowered = ToLower(raw);
				for (int i = 0; i < kTabOrderCount; ++i)
					if (lowered.find(kTabOrder[i]) != std::string::npos) return i;
				return 100;   // anything unlisted (tools) sorts last
			};
			const int ra = rank(g_profiles[a].raw);
			const int rb = rank(g_profiles[b].raw);
			if (ra != rb) return ra < rb;
			return g_profiles[a].display < g_profiles[b].display;
		});

		for (int oi = 0; oi < static_cast<int>(order.size()); ++oi)
		{
			const int p = order[oi];
			Profile& profile = g_profiles[p];
			const bool isEquipped = (p == activeProfile);

			// "###key" keeps the ImGui ID stable while the visible label changes.
			char label[96];
			snprintf(label, sizeof(label), "%s%s###%s",
				profile.display.c_str(), isEquipped ? " *" : "", profile.key.c_str());

			const int tabFlags = (focusChanged && isEquipped) ? (1 << 1) : 0; // SetSelected
			if (!imgui->BeginTabItem(label, nullptr, tabFlags))
				continue;

			imgui->PushIDInt(p);

			if (profile.isGrenade)
			{
				// Grenade-specific layout replaces the standard weapon one entirely --
				// none of Damage/Fire Rate/Recoil/Spread/etc. mean anything for a
				// thrown charge, and neither do the gun presets above (they index
				// into kAttrs by attribute, all meaningless here).
				imgui->TextDisabled("Damage isn't changeable yet -- its GameplayEffect modifier setup\n"
				                    "isn't confirmed safe to write blind. Fuse, blast radius and throw\n"
				                    "force are below.");

				// Saved presets sit directly under the built-ins (none exist for
				// grenades) and above every control, same position in every group.
				imgui->Spacing();
				{
					constexpr int kGrenadePresetFieldCount = kGrenadeRowCount + 1; // + infiniteCharges
					BetterCheats::PresetStore::Field presetFields[kGrenadePresetFieldCount];
					const std::string presetGroup = std::string("Weapon:") + profile.key;

					auto getLive = [&profile](BetterCheats::PresetStore::Field* out)
					{
						for (int g = 0; g < kGrenadeRowCount; ++g)
							out[g] = { kGrenadeRows[g].key, profile.grenadeValues[g] };
						out[kGrenadeRowCount] = { "infiniteCharges", profile.infiniteCharges ? 1.0f : 0.0f };
					};
					auto applyFields = [&profile](const BetterCheats::PresetStore::Field* f, int count)
					{
						for (int g = 0; g < kGrenadeRowCount && g < count; ++g)
						{
							profile.grenadeValues[g] = f[g].value;
							SessionConfig::Set(ConfigKey(profile, kGrenadeRows[g].key, "value"), f[g].value);
						}
						if (count > kGrenadeRowCount)
						{
							profile.infiniteCharges = f[kGrenadeRowCount].value != 0.0f;
							SessionConfig::Set(ConfigKey(profile, "infiniteCharges", "enabled"), profile.infiniteCharges);
						}
					};
					// The grenade tab has no built-in presets to guard against.
					auto isBuiltin      = [](const char*) { return false; };
					auto computeSuggest = [](char* out, int cap) { snprintf(out, cap, "Custom"); };

					BetterCheats::UI::RenderSavedPresetsRow(imgui, "grenade_saved_presets", presetGroup.c_str(),
						presetFields, kGrenadePresetFieldCount,
						getLive, applyFields, isBuiltin, computeSuggest, profile.presetRow);
				}
				imgui->Spacing();

				if (imgui->Checkbox("Infinite charges", &profile.infiniteCharges))
					SessionConfig::Set(ConfigKey(profile, "infiniteCharges", "enabled"), profile.infiniteCharges);
				if (imgui->IsItemHovered())
					imgui->SetTooltip("Tops your charge count back up to the max the moment it drops\n"
					                  "below it -- using whatever the game (or your own Max Charges\n"
					                  "override below) currently honours as the max. Same top-up\n"
					                  "'Infinite magazine' uses for ammo, not a permanent override.");

				imgui->Spacing();

				if (imgui->BeginTable("##grenade_attr_table", 3, kWeaponsTableFlags))
				{
					const float grenadeLabelReserve = BetterCheats::UI::PrescanLabelWidth(imgui, kGrenadeRowCount,
						[](int g) { return kGrenadeRows[g].label; });

					imgui->TableSetupColumn("Attribute", BetterCheats::UI::kColumnWidthFixed,
						BetterCheats::UI::GetReadoutColumnWidth(imgui, grenadeLabelReserve));
					imgui->TableSetupColumn("Value",     0, 0.54f);
					imgui->TableSetupColumn("",          0, 0.10f);

					for (int g = 0; g < kGrenadeRowCount; ++g)
					{
						const GrenadeRowDef& def   = kGrenadeRows[g];
						float&                value = profile.grenadeValues[g];
						const bool active = BetterCheats::DiffersFromDefault(value, def.defaultValue);

						imgui->PushIDInt(g);
						imgui->TableNextRow(0, 0.0f);

						const bool showLive = g_showLiveValues.load();
						char changeDesc[24];
						if (showLive)
							BetterCheats::UI::FormatChangeDesc(changeDesc, sizeof(changeDesc), kGrenadeModes[g], value);

						BetterCheats::UI::RowSpec spec;
						spec.label         = def.label;
						spec.tooltip       = def.tooltip;
						spec.value         = &value;
						spec.minValue      = def.minValue;
						spec.maxValue      = def.maxValue;
						spec.step          = def.step;
						spec.format        = def.format;
						spec.resetValue    = def.defaultValue;
						spec.active        = active;
						spec.showLive      = showLive;
						spec.expected      = g_dbgGrenadeExpected[g].load();
						spec.game          = g_dbgGrenadeGame[g].load();
						spec.composing     = active;
						spec.hasBase       = false;   // no BaseValue split for these -- see cost/max/min comments in Tick()
						spec.unmodified    = g_dbgGrenadeUnmod[g].load();
						spec.changeDesc    = changeDesc;
						spec.labelReserve  = grenadeLabelReserve;

						if (BetterCheats::UI::BuildRow(imgui, spec).changed)
							SessionConfig::Set(ConfigKey(profile, def.key, "value"), value);

						imgui->PopID();
					}

					imgui->EndTable();
				}

				imgui->Spacing();
				if (imgui->SmallButton("Reset this weapon to stock"))
				{
					profile.infiniteCharges = false;
					SessionConfig::Set(ConfigKey(profile, "infiniteCharges", "enabled"), false);
					for (int g = 0; g < kGrenadeRowCount; ++g)
					{
						profile.grenadeValues[g] = kGrenadeRows[g].defaultValue;
						SessionConfig::Set(ConfigKey(profile, kGrenadeRows[g].key, "value"), kGrenadeRows[g].defaultValue);
					}
				}
				imgui->SameLine(0.0f, -1.0f);
				imgui->TextDisabled(profile.raw.c_str());

				// Fuse / Blast Radius / Throw Force: global, not per-weapon-type --
				// shown (and editable) identically on every grenade tab, so "Reset
				// this weapon to stock" above deliberately leaves these alone; each
				// row resets itself with its own reset button.
				imgui->Spacing();
				imgui->SeparatorText("Blast (applies to every grenade type)");

				if (imgui->BeginTable("##grenade_global_table", 3, kWeaponsTableFlags))
				{
					const float globalLabelReserve = BetterCheats::UI::PrescanLabelWidth(imgui, kGrenadeGlobalCount,
						[](int g) { return kGrenadeGlobals[g].label; });

					imgui->TableSetupColumn("Attribute", BetterCheats::UI::kColumnWidthFixed,
						BetterCheats::UI::GetReadoutColumnWidth(imgui, globalLabelReserve));
					imgui->TableSetupColumn("Value",     0, 0.54f);
					imgui->TableSetupColumn("",          0, 0.10f);

					for (int g = 0; g < kGrenadeGlobalCount; ++g)
					{
						const GrenadeGlobalDef& def = kGrenadeGlobals[g];
						const bool resolved = g_grenadeGlobalHandle[g].ok;

						float value = g_grenadeGlobalValue[g].load();
						const bool active = resolved && BetterCheats::DiffersFromDefault(value, def.defaultValue);

						imgui->PushIDInt(1000 + g);   // offset clear of the kGrenadeRowCount PushIDInt(g) block above
						imgui->TableNextRow(0, 0.0f);

						const bool showLive = g_showLiveValues.load();
						char changeDesc[24];
						if (showLive)
							BetterCheats::UI::FormatChangeDesc(changeDesc, sizeof(changeDesc),
								BetterCheats::ComposedAttribute::Mode::Multiply, value);

						BetterCheats::UI::RowSpec spec;
						spec.label         = def.label;
						spec.tooltip       = resolved ? def.tooltip
							: "Not resolved yet -- the grenade projectile/throw ability class hasn't\n"
							  "loaded in this session. Usually resolves within a few seconds of\n"
							  "your first grenade throw; check ModLoader.log if it never does.";
						spec.value         = &value;
						spec.minValue      = def.minValue;
						spec.maxValue      = def.maxValue;
						spec.step          = def.step;
						spec.format        = def.format;
						spec.resetValue    = def.defaultValue;
						spec.active        = active;
						spec.disableSlider = !resolved;
						spec.showLive      = showLive && resolved;
						spec.expected      = g_dbgGrenadeGlobalExpected[g].load();
						spec.game          = g_dbgGrenadeGlobalGame[g].load();
						spec.composing     = active;
						spec.hasBase       = false;
						spec.unmodified    = active ? g_composedGrenadeGlobal[g].GetGame() : g_dbgGrenadeGlobalGame[g].load();
						spec.changeDesc    = changeDesc;
						spec.labelReserve  = globalLabelReserve;

						if (BetterCheats::UI::BuildRow(imgui, spec).changed)
						{
							g_grenadeGlobalValue[g].store(value);
							SessionConfig::Set(std::string("playerWeapons.") + def.key + ".value", value);
						}

						imgui->PopID();
					}

					imgui->EndTable();
				}
			}
			else
			{
			// A preset writes only into THIS weapon's profile, so every weapon carries its
			// own independently. `match` gates presets that only make sense on one weapon.
			const std::string loweredRaw = ToLower(profile.raw);
			imgui->TextDisabled("Presets:");
			bool anyPreset = false;
			for (int i = 0; i < kPresetCount; ++i)
			{
				const Preset& preset = kPresets[i];
				if (preset.match && *preset.match && loweredRaw.find(preset.match) == std::string::npos)
					continue;

				imgui->SameLine(0.0f, -1.0f);
				anyPreset = true;

				if (imgui->SmallButton(preset.label))
				{
					// A preset is a starting point, not a mode: clear the weapon first so
					// leftovers from a previous preset don't silently survive underneath.
					for (int a = 0; a < kAttrCount; ++a)
					{
						profile.values[a] = kAttrs[a].defaultValue;
						SessionConfig::Set(ConfigKey(profile, kAttrs[a].key, "value"), kAttrs[a].defaultValue);
					}
					for (const PresetVal& v : preset.vals)
					{
						if (v.attr < 0) break;
						profile.values[v.attr] = v.value;
						SessionConfig::Set(ConfigKey(profile, kAttrs[v.attr].key, "value"), v.value);
					}
					LOG_INFO("Weapons: applied preset '%s' to '%s'", preset.label, profile.display.c_str());
				}
				if (imgui->IsItemHovered())
				{
					char tip[384];
					snprintf(tip, sizeof(tip), "%s\n\nApplies to %s only.%s%s",
						preset.tooltip, profile.display.c_str(),
						preset.credit ? "\n\n" : "",
						preset.credit ? preset.credit : "");
					imgui->SetTooltip(tip);
				}
			}
			if (!anyPreset) { imgui->SameLine(0.0f, -1.0f); imgui->TextDisabled("(none for this weapon)"); }

			// Saved presets sit directly under the built-ins and above every
			// control, same position in every group.
			imgui->Spacing();
			{
				constexpr int kWeaponPresetFieldCount = kAttrCount + 2; // + oneHitKill + infiniteMagazine
				BetterCheats::PresetStore::Field presetFields[kWeaponPresetFieldCount];
				const std::string presetGroup = std::string("Weapon:") + profile.key;

				auto getLive = [&profile](BetterCheats::PresetStore::Field* out)
				{
					for (int a = 0; a < kAttrCount; ++a)
						out[a] = { kAttrs[a].key, profile.values[a] };
					out[kAttrCount]     = { "oneHitKill",       profile.oneHitKill       ? 1.0f : 0.0f };
					out[kAttrCount + 1] = { "infiniteMagazine", profile.infiniteMagazine ? 1.0f : 0.0f };
				};
				auto applyFields = [&profile](const BetterCheats::PresetStore::Field* f, int count)
				{
					for (int a = 0; a < kAttrCount && a < count; ++a)
					{
						profile.values[a] = f[a].value;
						SessionConfig::Set(ConfigKey(profile, kAttrs[a].key, "value"), f[a].value);
					}
					if (count > kAttrCount)
					{
						profile.oneHitKill = f[kAttrCount].value != 0.0f;
						SessionConfig::Set(ConfigKey(profile, "oneHitKill", "enabled"), profile.oneHitKill);
					}
					if (count > kAttrCount + 1)
					{
						profile.infiniteMagazine = f[kAttrCount + 1].value != 0.0f;
						SessionConfig::Set(ConfigKey(profile, "infiniteMagazine", "enabled"), profile.infiniteMagazine);
					}
				};
				// Only the built-ins that actually apply to THIS weapon (same
				// `match` filtering the preset buttons above already use) --
				// a name free on other weapons but taken here should still
				// be refused, and vice versa.
				auto isBuiltin = [&loweredRaw](const char* name)
				{
					for (int i = 0; i < kPresetCount; ++i)
					{
						const Preset& preset = kPresets[i];
						if (preset.match && *preset.match && loweredRaw.find(preset.match) == std::string::npos)
							continue;
						if (strcmp(preset.label, name) == 0)
							return true;
					}
					return false;
				};
				// "<built-in> Custom" only when every applicable-preset field
				// matches AND every other row is still at its own default --
				// a hand-tweaked mix that happens to overlap one preset's
				// values isn't that preset.
				auto computeSuggest = [&profile, &loweredRaw](char* out, int cap)
				{
					for (int i = 0; i < kPresetCount; ++i)
					{
						const Preset& preset = kPresets[i];
						if (preset.match && *preset.match && loweredRaw.find(preset.match) == std::string::npos)
							continue;

						bool touched[kAttrCount] = {};
						bool matches = true;
						for (const PresetVal& v : preset.vals)
						{
							if (v.attr < 0) break;
							if (std::fabs(profile.values[v.attr] - v.value) > BetterCheats::kActiveEpsilon)
							{
								matches = false;
								break;
							}
							touched[v.attr] = true;
						}
						if (matches)
						{
							for (int a = 0; a < kAttrCount && matches; ++a)
								if (!touched[a] && BetterCheats::DiffersFromDefault(profile.values[a], kAttrs[a].defaultValue))
									matches = false;
						}
						if (matches && (profile.oneHitKill || profile.infiniteMagazine))
							matches = false;   // presets never set these -- a match must leave them off too

						if (matches)
						{
							snprintf(out, cap, "%s Custom", preset.label);
							return;
						}
					}
					snprintf(out, cap, "Custom");
				};

				BetterCheats::UI::RenderSavedPresetsRow(imgui, "weapon_saved_presets", presetGroup.c_str(),
					presetFields, kWeaponPresetFieldCount,
					getLive, applyFields, isBuiltin, computeSuggest, profile.presetRow);
			}
			imgui->Spacing();

			// ---- per-weapon toggles --------------------------------------------
			if (imgui->Checkbox("One Hit Kill", &profile.oneHitKill))
				SessionConfig::Set(ConfigKey(profile, "oneHitKill", "enabled"), profile.oneHitKill);
			if (imgui->IsItemHovered())
				imgui->SetTooltip("Overrides Damage with a flat 10000x while enabled. Turning it off\n"
				                  "hands Damage back to the game (attachment bonuses included).");

			imgui->SameLine(0.0f, -1.0f);
			if (imgui->Checkbox("Infinite magazine", &profile.infiniteMagazine))
				SessionConfig::Set(ConfigKey(profile, "infiniteMagazine", "enabled"), profile.infiniteMagazine);

			imgui->Spacing();

			// ---- attributes ----------------------------------------------------
			if (imgui->BeginTable("##weapon_attr_table", 3, kWeaponsTableFlags))
			{
				// Fixed, not stretched: must fit the label AND the live-values
				// readout next to it (see GetReadoutColumnWidth) or the readout
				// clips against the slider column. The other two stay stretched --
				// the slider is capped anyway (GetSliderWidthCap), so they have
				// slack to give up.
				imgui->TableSetupColumn("Attribute", BetterCheats::UI::kColumnWidthFixed,
					BetterCheats::UI::GetReadoutColumnWidth(imgui, labelReserve));
				imgui->TableSetupColumn("Value",     0, 0.54f);
				imgui->TableSetupColumn("",          0, 0.10f);

				for (int a = 0; a < kAttrCount; ++a)
				{
					const AttrDef& def   = kAttrs[a];
					float&         value = profile.values[a];

					const bool ownedByOneHitKill = (a == kAttrDamage && profile.oneHitKill);

					imgui->PushIDInt(a);
					imgui->TableNextRow(0, 0.0f);

					const bool showLive = g_showLiveValues.load();
					char changeDesc[24];
					if (showLive)
						BetterCheats::UI::FormatChangeDesc(changeDesc, sizeof(changeDesc),
							ownedByOneHitKill ? BetterCheats::ComposedAttribute::Mode::Absolute : kAttrModes[a],
							ownedByOneHitKill ? kOneHitKillDamage : value);

					BetterCheats::UI::RowSpec spec;
					spec.label         = def.label;
					spec.tooltip       = def.tooltip;
					spec.value         = &value;
					spec.minValue      = def.minValue;
					spec.maxValue      = def.maxValue;
					spec.step          = def.step;
					spec.format        = def.format;
					spec.resetValue    = def.defaultValue;
					spec.active        = IsActive(a, value) && !ownedByOneHitKill;
					spec.disableSlider = ownedByOneHitKill;
					spec.showLive      = showLive;
					spec.expected      = g_dbgAttrExpected[a].load();
					spec.game          = g_dbgAttrGame[a].load();
					spec.composing     = IsActive(a, value) || ownedByOneHitKill;
					// Magazine composes onto the weapon data asset now, not a GAS
					// attribute -- no BaseValue split, so no tag; "Unmodified" is the
					// captured original (g_dbgWeaponBaseMagazine, same number the base-
					// stats block above shows).
					spec.hasBase       = (a != kAttrMagazine);
					spec.base          = g_dbgAttrBase[a].load();
					spec.unmodified    = (a == kAttrMagazine) ? g_dbgWeaponBaseMagazine.load() : g_dbgAttrBuffed[a].load();
					spec.changeDesc    = changeDesc;
					spec.tagWord       = "mods";
					spec.baseLabel     = "Base (no attachments)";
					spec.labelReserve  = labelReserve;

					if (BetterCheats::UI::BuildRow(imgui, spec).changed)
						SessionConfig::Set(ConfigKey(profile, def.key, "value"), value);

					imgui->PopID();
				}

				imgui->EndTable();
			}

			imgui->Spacing();
			if (imgui->SmallButton("Reset this weapon to stock"))
			{
				profile.oneHitKill       = false;
				profile.infiniteMagazine = false;
				SessionConfig::Set(ConfigKey(profile, "oneHitKill", "enabled"), false);
				SessionConfig::Set(ConfigKey(profile, "infiniteMagazine", "enabled"), false);
				for (int a = 0; a < kAttrCount; ++a)
				{
					profile.values[a] = kAttrs[a].defaultValue;
					SessionConfig::Set(ConfigKey(profile, kAttrs[a].key, "value"), kAttrs[a].defaultValue);
				}
			}
			imgui->SameLine(0.0f, -1.0f);
			imgui->TextDisabled(profile.raw.c_str());
			} // else (!profile.isGrenade)

			imgui->PopID();
			imgui->EndTabItem();
		}

		} // profilesLock

		imgui->EndTabBar();

		imgui->Spacing();
		imgui->Separator();
		imgui->TextDisabled("Presets modelled on the pak mods that Update 2 broke -");
		imgui->TextDisabled("thanks to axbhub (Better Weapons) and SwiftstepsKR (piercing weapons).");
	}
}
