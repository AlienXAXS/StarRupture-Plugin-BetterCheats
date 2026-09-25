#include "player_movement.h"
#include "plugin_helpers.h"
#include "plugin_config.h"
#include "cheat_menu.h"
#include "keybind_picker.h"
#include "game_context.h"
#include "session_config.h"
#include "ui_widgets.h"
#include "attribute_compose.h"
#include "cheat_math.h"
#include "player_lookup.h"
#include "game_thread.h"

#include "Chimera_classes.hpp"

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>

// No-clip is the classic UE ghost cheat, done entirely through the generated
// SDK — no AOB patterns, no detours:
//
//   * the capsule stops colliding, so nothing blocks the character
//   * the movement mode becomes MOVE_Flying, which skips gravity and the floor
//     checks that would otherwise drop the character out of the world
//   * bCheatFlying tells the movement component this flying is deliberate, so a
//     listen host does not correct it back
//
// Horizontal movement still comes from the game's own input; the game only
// steers on the horizontal plane, so Space / Left Ctrl are fed in here as an
// extra vertical axis.
//
// Everything that touches a UObject runs on the game thread (Tick).
// RenderImGui() runs on the render thread and only ever reads the snapshot.

namespace BetterCheats::Panels::Movement
{
	namespace
	{
		constexpr float kDefaultFlySpeedMultiplier = 3.0f;
		constexpr float kMinFlySpeedMultiplier     = 1.0f;
		constexpr float kMaxFlySpeedMultiplier     = 25.0f;

		// Used only if the movement component ships with no fly speed of its own —
		// UCharacterMovementComponent's own default, in cm/s.
		constexpr float kFallbackMaxFlySpeed = 600.0f;

		// ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingStretchProp
		constexpr int kTableFlags  = (1 << 6) | (1 << 9) | (3 << 13);

		// A value is applied only when it differs from the game default, so there is
		// no separate enable toggle to keep in sync -- resetting a row IS disabling
		// it. See cheat_math.h for kActiveEpsilon/DiffersFromDefault, shared with
		// player_weapons.cpp.

		// MaxEnergy.CurrentValue is an absolute stamina-pool size, not a multiplier,
		// so its composed result gets a generous sanity clamp instead of reusing
		// kRaws[kRawMaxEnergy]'s 0.10x-20.0x range -- that range bounds the slider
		// (the multiplier), not the pool size it produces.
		constexpr float kMaxEnergyFloor   = 1.0f;
		constexpr float kMaxEnergyCeiling = 1000000.0f;

		// Wanted state — written from the render thread and the keybind callback,
		// read on the game thread.
		std::atomic<bool>  g_noClipWanted{ false };
		std::atomic<float> g_flySpeedMultiplier{ kDefaultFlySpeedMultiplier };

		// Flight and wall-clipping are separate concerns. F9 toggles FLIGHT; this
		// decides whether the capsule also stops colliding while flying. Off gives you
		// flight that still bumps into the world, which is what you want for getting
		// around a base without falling through it.
		std::atomic<bool>  g_passThroughWalls{ true };

		// Applied state — game thread only. g_appliedTo is compared, never
		// dereferenced blind: the pawn is replaced on respawn, so state belonging to
		// a character that is no longer the local pawn is dropped rather than restored.
		bool                         g_active    = false;
		SDK::ACrCharacterPlayerBase* g_appliedTo = nullptr;

		SDK::ECollisionEnabled g_savedCollision      = SDK::ECollisionEnabled::QueryAndPhysics;
		float                  g_savedMaxFlySpeed    = kFallbackMaxFlySpeed;
		float                  g_savedFlyingBraking  = 0.0f;
		bool                   g_savedCheatFlying    = false;

		// Key state is polled rather than bound: the modloader exposes press and
		// release events, not "is held", and holding Space is exactly what
		// ascending needs. Only listen while this process owns the foreground
		// window, or the character climbs while the player is in another app.
		bool IsGameForeground()
		{
			HWND foreground = GetForegroundWindow();
			if (!foreground) return false;

			DWORD pid = 0;
			GetWindowThreadProcessId(foreground, &pid);
			return pid == GetCurrentProcessId();
		}

		bool IsKeyHeld(int virtualKey)
		{
			return (GetAsyncKeyState(virtualKey) & 0x8000) != 0;
		}

		void Enable(SDK::ACrCharacterPlayerBase* character)
		{
			SDK::UCharacterMovementComponent* move = character->CharacterMovement;
			SDK::UCapsuleComponent* capsule = character->CapsuleComponent;
			if (!move || !capsule)
				return;

			g_savedCollision     = capsule->GetCollisionEnabled();
			g_savedMaxFlySpeed   = move->MaxFlySpeed > 0.0f ? move->MaxFlySpeed : kFallbackMaxFlySpeed;
			g_savedFlyingBraking = move->BrakingDecelerationFlying;
			g_savedCheatFlying   = move->bCheatFlying != 0;

			if (g_passThroughWalls.load())
				capsule->SetCollisionEnabled(SDK::ECollisionEnabled::NoCollision);
			move->bCheatFlying = 1;
			move->SetMovementMode(SDK::EMovementMode::MOVE_Flying, 0);

			g_active    = true;
			g_appliedTo = character;

			LOG_INFO("Movement: no-clip enabled.");
		}

		void Disable(SDK::ACrCharacterPlayerBase* character)
		{
			g_active    = false;
			g_appliedTo = nullptr;

			SDK::UCharacterMovementComponent* move = character->CharacterMovement;
			SDK::UCapsuleComponent* capsule = character->CapsuleComponent;

			if (capsule)
				capsule->SetCollisionEnabled(g_savedCollision);

			if (move)
			{
				move->MaxFlySpeed               = g_savedMaxFlySpeed;
				move->BrakingDecelerationFlying = g_savedFlyingBraking;
				move->bCheatFlying              = g_savedCheatFlying ? 1 : 0;

				// Falling rather than walking: the character is usually in mid-air
				// when no-clip goes off, and the engine drops it into walking as
				// soon as it finds a floor.
				move->SetMovementMode(SDK::EMovementMode::MOVE_Falling, 0);
			}

			LOG_INFO("Movement: no-clip disabled.");
		}

		void MaintainNoClip(SDK::ACrCharacterPlayerBase* character)
		{
			SDK::UCharacterMovementComponent* move = character->CharacterMovement;
			if (!move)
				return;

			// Zipline, slide and dash all drive the movement mode themselves, so
			// take it back whenever something else has changed it.
			if (move->MovementMode != SDK::EMovementMode::MOVE_Flying)
				move->SetMovementMode(SDK::EMovementMode::MOVE_Flying, 0);

			// Honour a mid-flight change to the wall-clipping toggle rather than making
			// the player land and take off again.
			if (SDK::UCapsuleComponent* capsule = character->CapsuleComponent)
			{
				const bool wantGhost = g_passThroughWalls.load();
				const bool isGhost   = capsule->GetCollisionEnabled() == SDK::ECollisionEnabled::NoCollision;
				if (wantGhost && !isGhost)
					capsule->SetCollisionEnabled(SDK::ECollisionEnabled::NoCollision);
				else if (!wantGhost && isGhost)
					capsule->SetCollisionEnabled(g_savedCollision);
			}

			const float maxSpeed = g_savedMaxFlySpeed * g_flySpeedMultiplier.load();
			move->MaxFlySpeed = maxSpeed;

			// Flying braking ships at 0 on most components, which means coasting
			// forever once you let go. Brake hard instead so the character parks
			// where the player stopped steering.
			move->BrakingDecelerationFlying = maxSpeed * 4.0f;

			if (CheatMenu::IsOpen() || !IsGameForeground())
				return;

			float vertical = 0.0f;
			if (IsKeyHeld(VK_SPACE))                                 vertical += 1.0f;
			if (IsKeyHeld(VK_LCONTROL) || IsKeyHeld(VK_RCONTROL))    vertical -= 1.0f;

			if (vertical != 0.0f)
				character->AddMovementInput(SDK::FVector(0.0, 0.0, 1.0), vertical, true);
		}

		// Snapshot — populated on the game thread (Tick), read on the ImGui render
		// thread. Never touch SDK objects from RenderImGui().
		struct MovementSnapshot
		{
			bool  characterFound = false;
			bool  active         = false;
			float baseFlySpeed   = kFallbackMaxFlySpeed;
		};

		std::mutex       g_snapshotMutex;
		MovementSnapshot g_snapshot;

		void RefreshSnapshot(SDK::ACrCharacterPlayerBase* character)
		{
			MovementSnapshot snap;
			snap.characterFound = character != nullptr;
			snap.active         = g_active;
			snap.baseFlySpeed   = g_active ? g_savedMaxFlySpeed : kFallbackMaxFlySpeed;

			if (!g_active && character && character->CharacterMovement && character->CharacterMovement->MaxFlySpeed > 0.0f)
				snap.baseFlySpeed = character->CharacterMovement->MaxFlySpeed;

			std::lock_guard<std::mutex> lock(g_snapshotMutex);
			g_snapshot = snap;
		}

		// The combo we actually registered with. Config::GetNoClipKey() hands back
		// a shared static buffer, so it is read once here rather than from the
		// render thread every frame — and unregistering must use the same string.
		// Written by the rebind control on the render thread, read there and on
		// shutdown, hence the lock.
		std::mutex g_keyMutex;
		char       g_noClipKey[64] = "";

		void ReadNoClipKey(char* out, size_t outSize)
		{
			std::lock_guard<std::mutex> lock(g_keyMutex);
			snprintf(out, outSize, "%s", g_noClipKey);
		}

		void OnNoClipKeyPressed(EModKey /*key*/, EModKeyEvent /*event*/)
		{
			if (!GameContext::IsInChimeraMain() || !GameContext::AreCheatsAllowed())
				return;

			g_noClipWanted.store(!g_noClipWanted.load());
		}

		// Swaps the live registration and persists the new combo. The loader
		// matches an unregistration against the name the bind was created with, so
		// handing it the string we last registered is enough even if it has been
		// rebound from the loader's own config UI in the meantime.
		void ApplyNoClipKey(const char* combo)
		{
			IPluginSelf* self = GetSelf();
			if (!self || !self->hooks || !self->hooks->Input || !combo || !*combo)
				return;

			char previous[64];
			{
				std::lock_guard<std::mutex> lock(g_keyMutex);
				if (strcmp(g_noClipKey, combo) == 0)
					return;

				snprintf(previous, sizeof(previous), "%s", g_noClipKey);
				snprintf(g_noClipKey, sizeof(g_noClipKey), "%s", combo);
			}

			if (previous[0])
				self->hooks->Input->UnregisterKeybindByName(previous, EModKeyEvent::Pressed, &OnNoClipKeyPressed);

			self->hooks->Input->RegisterKeybindByName(combo, EModKeyEvent::Pressed, &OnNoClipKeyPressed);
			BetterCheatsConfig::Config::SetNoClipKey(combo);

			LOG_INFO("Movement: no-clip keybind is now %s.", combo);
		}

		// Attribute overrides — FGameplayAttributeData on three attribute sets that are
		// direct members of ACrCharacterPlayerBase (no ability-system traversal):
		// MovementAttributes, MovementSpeedMultiplierAttributes and GemAttributes. Same
		// enable-by-differing-from-default model as player_weapons.cpp, just one flat
		// set of player values instead of per-weapon profiles.
		struct AttrDef
		{
			const char* label;
			const char* key;
			float       defaultValue;
			float       minValue;
			float       maxValue;
			float       step;
			const char* format;
			const char* tooltip;
		};

		// Grouped so the group's row range is a contiguous slice — see kAttrs below.
		enum AttrIndex : int
		{
			// "Speed" group
			kAttrMoveSpeed = 0, kAttrMoveSpeedMax, kAttrMoveSpeedMin, kAttrSprintSpeed,
			// "Jump & Dodge" group
			kAttrJumpHeight, kAttrDoubleJumpCost, kAttrDodgeCost,
			// "Stamina & Survival" group
			kAttrStaminaRegen, kAttrSlideStaminaRegen, kAttrFallDamage, kAttrZiplineSpeed,
			kAttrCount
		};

		constexpr int kSpeedGroupFirst   = kAttrMoveSpeed,      kSpeedGroupCount   = 4;
		constexpr int kJumpGroupFirst    = kAttrJumpHeight,     kJumpGroupCount    = 3;
		constexpr int kStaminaGroupFirst = kAttrStaminaRegen,   kStaminaGroupCount = 4;

		const AttrDef kAttrs[kAttrCount] = {
			{ "Move Speed",           "moveSpeed",         1.0f, 0.10f, 10.0f, 0.05f, "%.2fx",
			  "CurrentMovementSpeedMultiplier -- this is what walking/running actually reads.\n"
			  "Multiplies the base value. A movement LEM bonus still adds on top." },
			{ "Move Speed Cap",       "moveSpeedMax",      1.0f, 0.10f, 10.0f, 0.05f, "%.2fx",
			  "Ceiling Move Speed is clamped to -- set directly, not multiplied. Raise this\n"
			  "alongside Move Speed or a high override gets clamped back down." },
			{ "Move Speed Floor",     "moveSpeedMin",      1.0f, 0.00f,  5.0f, 0.05f, "%.2fx",
			  "Floor Move Speed is clamped to -- set directly, not multiplied. Raise this so\n"
			  "slows and debuffs can never drop you below it." },
			{ "Sprint Speed",         "sprintSpeed",       1.0f, 0.10f,  5.0f, 0.05f, "%.2fx",
			  "Multiplies the base value. Move Speed and any LEM bonus still add on top." },
			{ "Jump Height",          "jumpHeight",        1.0f, 0.10f,  8.0f, 0.05f, "%.2fx",
			  "Multiplies the base jump height/impulse. LEM bonuses still add on top." },
			{ "Double Jump Cost",     "doubleJumpCost",    1.0f, 0.00f,  3.0f, 0.05f, "%.2fx",
			  "Multiplies the base stamina cost for double jump. Any LEM discount (e.g.\n"
			  "Airjumper) still applies on top. 0 makes it free." },
			{ "Dodge Cost",           "dodgeCost",         1.0f, 0.00f,  3.0f, 0.05f, "%.2fx",
			  "Stamina cost multiplier for dodge/dash. 0 makes it free." },
			{ "Stamina Regen",        "staminaRegen",      1.0f, 0.10f, 10.0f, 0.05f, "%.2fx",
			  "Multiplies the base stamina regeneration rate. Any LEM bonus (e.g. Runner)\n"
			  "still adds on top." },
			{ "Slide Stamina Regen",  "slideStaminaRegen", 1.0f, 0.10f, 10.0f, 0.05f, "%.2fx",
			  "Multiplies the base regeneration rate while sliding. Any LEM bonus (e.g.\n"
			  "Slidegiver) still adds on top." },
			{ "Fall Damage",          "fallDamage",        1.0f, 0.00f,  3.0f, 0.05f, "%.2fx",
			  "Fall damage taken multiplier. 0 removes fall damage entirely." },
			{ "Zipline Speed",        "ziplineSpeed",      1.0f, 0.10f,  5.0f, 0.05f, "%.2fx",
			  "Travel speed multiplier while riding a zipline." },
		};

		// Apply() mode per row, aligned with AttrIndex. Multiply composes onto
		// whatever the ability system aggregated (LEM bonuses included); Absolute
		// replaces it outright, which only fits the cap/floor pair below -- those
		// attributes are the literal clamp bounds Move Speed is held between, not a
		// modifier layered on top of one.
		constexpr BetterCheats::ComposedAttribute::Mode kAttrModes[kAttrCount] = {
			BetterCheats::ComposedAttribute::Mode::Multiply,  // Move Speed
			BetterCheats::ComposedAttribute::Mode::Absolute,  // Move Speed Cap
			BetterCheats::ComposedAttribute::Mode::Absolute,  // Move Speed Floor
			BetterCheats::ComposedAttribute::Mode::Multiply,  // Sprint Speed
			BetterCheats::ComposedAttribute::Mode::Multiply,  // Jump Height
			BetterCheats::ComposedAttribute::Mode::Multiply,  // Double Jump Cost
			BetterCheats::ComposedAttribute::Mode::Multiply,  // Dodge Cost
			BetterCheats::ComposedAttribute::Mode::Multiply,  // Stamina Regen
			BetterCheats::ComposedAttribute::Mode::Multiply,  // Slide Stamina Regen
			BetterCheats::ComposedAttribute::Mode::Multiply,  // Fall Damage
			BetterCheats::ComposedAttribute::Mode::Multiply,  // Zipline Speed
		};

		// Written from RenderImGui (sliders, presets, reset) and read every tick from
		// Tick (ApplyAttributeOverrides) on the game thread.
		std::atomic<float> g_attrValues[kAttrCount];
		std::atomic<bool>  g_attrValuesInit{ false };

		void EnsureAttrDefaults()
		{
			if (g_attrValuesInit.exchange(true)) return;
			for (int a = 0; a < kAttrCount; ++a)
				g_attrValues[a].store(kAttrs[a].defaultValue);
		}

		bool IsAttrActive(int attr)
		{
			return BetterCheats::DiffersFromDefault(g_attrValues[attr].load(), kAttrs[attr].defaultValue);
		}

		std::string AttrConfigKey(const char* leaf)
		{
			return std::string("playerMovement.attr.") + leaf;
		}

		// The three attribute sets are different C++ classes, so a single
		// pointer-to-member array (as player_weapons.cpp uses for its one class)
		// can't span all of them without type erasure. Three small typed binding
		// tables instead -- still declarative, still a one-line add per attribute.
		using MoveAttr  = SDK::FGameplayAttributeData SDK::UCrMovementAttributeSet::*;
		using SpeedAttr = SDK::FGameplayAttributeData SDK::UCrMovementSpeedMultiplierAttributeSet::*;
		using GemAttr   = SDK::FGameplayAttributeData SDK::UCrGemAttributeSet::*;

		struct MoveBinding  { int attr; MoveAttr  member; };
		struct SpeedBinding { int attr; SpeedAttr member; };
		struct GemBinding   { int attr; GemAttr   member; };

		const MoveBinding kMoveBindings[] = {
			{ kAttrSprintSpeed, &SDK::UCrMovementAttributeSet::SprintSpeedMultiplier },
			{ kAttrJumpHeight,  &SDK::UCrMovementAttributeSet::JumpMultiplier },
		};

		const SpeedBinding kSpeedBindings[] = {
			{ kAttrMoveSpeed,    &SDK::UCrMovementSpeedMultiplierAttributeSet::CurrentMovementSpeedMultiplier },
			{ kAttrMoveSpeedMax, &SDK::UCrMovementSpeedMultiplierAttributeSet::MaxMovementSpeedMultiplier },
			{ kAttrMoveSpeedMin, &SDK::UCrMovementSpeedMultiplierAttributeSet::MinMovementSpeedMultiplier },
		};

		const GemBinding kGemBindings[] = {
			{ kAttrStaminaRegen,      &SDK::UCrGemAttributeSet::StaminaRegenMultiplier },
			{ kAttrSlideStaminaRegen, &SDK::UCrGemAttributeSet::SlideStaminaRegenMultiplier },
			{ kAttrDodgeCost,         &SDK::UCrGemAttributeSet::DodgeCostMultiplier },
			{ kAttrDoubleJumpCost,    &SDK::UCrGemAttributeSet::DoubleJumpCostMultiplier },
			{ kAttrFallDamage,        &SDK::UCrGemAttributeSet::FallDamageMultiplier },
			{ kAttrZiplineSpeed,      &SDK::UCrGemAttributeSet::ZiplineSpeedMultiplier },
		};

		// Same contract as SafeMultiplier, for the gameplay-attribute half.
		float SafeAttrValue(int attr)
		{
			return BetterCheats::SafeValue(g_attrValues[attr].load(), kAttrs[attr].defaultValue,
				kAttrs[attr].minValue, kAttrs[attr].maxValue);
		}

		// One slot per row in kAttrs, shared across whichever of the three attribute
		// sets actually owns it -- ComposedAttribute only cares about the owner
		// pointer passed to it each call. Max Energy is a raw row (see kRaws below)
		// but composes the same way, since UCrEnergyAttributeSet::MaxEnergy has no
		// reflected base-value setter either. Game-thread only.
		BetterCheats::ComposedAttribute g_composedAttrs[kAttrCount];
		BetterCheats::ComposedAttribute g_composedMaxEnergy;

		// Detects a respawn/world change and drops every composed slot before
		// anything below tries to Apply/Release against an attribute set that may
		// already be gone. Mirrors the g_appliedTo check MaintainNoClip/Tick already
		// use for the same reason.
		SDK::ACrCharacterPlayerBase* g_composedCharacter = nullptr;

		void ForgetComposedIfCharacterChanged(SDK::ACrCharacterPlayerBase* character)
		{
			if (character == g_composedCharacter) return;
			for (int a = 0; a < kAttrCount; ++a)
				g_composedAttrs[a].Forget();
			g_composedMaxEnergy.Forget();
			g_composedCharacter = character;
		}

		// "Show live values" toggle -- default on so a fresh install proves the
		// compose fix works without the owner having to find the setting.
		std::atomic<bool> g_showLiveValues{ true };

		// One-shot confirmation logging: records in ModLoader.log that both the
		// Tick-side fill and the RenderImGui-side draw have run this session,
		// so a silently-broken live-values pipeline (one side running, the
		// other not) is diagnosable from the log alone instead of guesswork.
		std::atomic<bool> g_loggedTickFill{ false };
		std::atomic<bool> g_loggedRender{ false };

		// Per-row "expected = game" / "expected != game" readout, indexed like
		// kAttrs/g_composedAttrs. `game` is CurrentValue read fresh at the start of
		// this tick, before we touch it; `expected` is what we intend it to be (the
		// composed result while active, or `game` itself while inactive). Compared
		// pre-write so a lasting mismatch actually means something. `base` is
		// BaseValue (never written by us); `buffed` is the game's own aggregate with
		// LEMs/buffs folded in but before our composition -- ComposedAttribute::
		// GetGame() while active, `game` itself while inactive. `buffed` != `base`
		// is what the "+buff" tag reports. Refreshed from Tick, read from
		// RenderImGui. The raw-field equivalents (g_dbgRawGame/g_dbgRawExpected/
		// g_dbgRawBase/g_dbgRawBuffed, including the Max Energy row -- the one raw
		// row that's actually GAS-backed) live below kRaws.
		std::atomic<float> g_dbgAttrGame[kAttrCount];
		std::atomic<float> g_dbgAttrExpected[kAttrCount];
		std::atomic<float> g_dbgAttrBase[kAttrCount];
		std::atomic<float> g_dbgAttrBuffed[kAttrCount];

		// Movement-component block readout. -1 = not available yet.
		std::atomic<float> g_dbgMaxWalkSpeed  { -1.0f };
		std::atomic<float> g_dbgHorizSpeed    { -1.0f };
		std::atomic<float> g_dbgJumpZVelocity { -1.0f };
		std::atomic<float> g_dbgStaminaCurrent{ -1.0f };
		std::atomic<float> g_dbgStaminaMax    { -1.0f };

		// Applies one row and records its "expected = game" readout. Captures game
		// (fresh CurrentValue) and, when the row was already active, the previous
		// write BEFORE calling Apply -- comparing pre-write is what makes a lasting
		// mismatch mean something instead of trivially matching what we just wrote.
		template <typename Set>
		void ApplyAttrRow(Set* set, SDK::FGameplayAttributeData& attr, int attrIndex)
		{
			const bool  wasActive     = g_composedAttrs[attrIndex].IsActive();
			const float previousWrite = g_composedAttrs[attrIndex].GetWritten();
			const std::string key = std::string("playerMovement.compose.") + kAttrs[attrIndex].key;
			if (!wasActive)
				BetterCheats::RestoreIfStale(key, attr.CurrentValue);
			const float gameBefore = attr.CurrentValue;

			bool active = IsAttrActive(attrIndex);
			if (active)
				g_composedAttrs[attrIndex].Apply(set, attr.CurrentValue, &attr.BaseValue, SafeAttrValue(attrIndex),
					kAttrModes[attrIndex], kAttrs[attrIndex].minValue, kAttrs[attrIndex].maxValue);
			else
				g_composedAttrs[attrIndex].Release(set, attr.CurrentValue);

			// Recaptured (first activation, or the game re-aggregated) -- persist so
			// a botched hot-reload can tell this from a stale leftover next time.
			// Never every frame.
			if (active && (!wasActive || gameBefore != previousWrite))
				BetterCheats::SaveComposeState(key, g_composedAttrs[attrIndex].GetGame(), g_composedAttrs[attrIndex].GetWritten());
			else if (!active)
				BetterCheats::ClearComposeState(key);

			const float expected = active ? (wasActive ? previousWrite : gameBefore) : gameBefore;
			// "Buffed" (base + LEMs/buffs, ours excluded): GetGame() reflects what
			// Apply() just composed from -- valid once active. Inactive rows never
			// had that captured, but CurrentValue is already untouched by us, so it
			// already IS base + buffs.
			const float buffed = active ? g_composedAttrs[attrIndex].GetGame() : gameBefore;
			g_dbgAttrGame[attrIndex].store(gameBefore);
			g_dbgAttrExpected[attrIndex].store(expected);
			g_dbgAttrBase[attrIndex].store(attr.BaseValue);
			g_dbgAttrBuffed[attrIndex].store(buffed);
		}

		void ApplyAttributeOverrides(SDK::ACrCharacterPlayerBase* character)
		{
			try
			{
				if (SDK::UCrMovementAttributeSet* set = character->MovementAttributes)
					for (const MoveBinding& b : kMoveBindings)
						ApplyAttrRow(set, set->*b.member, b.attr);

				if (SDK::UCrMovementSpeedMultiplierAttributeSet* set = character->MovementSpeedMultiplierAttributes)
					for (const SpeedBinding& b : kSpeedBindings)
						ApplyAttrRow(set, set->*b.member, b.attr);

				if (SDK::UCrGemAttributeSet* set = character->GemAttributes)
					for (const GemBinding& b : kGemBindings)
						ApplyAttrRow(set, set->*b.member, b.attr);

				if (!g_loggedTickFill.exchange(true))
					LOG_INFO("Movement: live values first filled by Tick.");
			}
			catch (...) {}
		}

		// Raw movement fields: plain engine/config floats on the player's own movement
		// component and character actor (per-player instances, never a CDO or shared
		// singleton), as opposed to the gameplay-attribute multipliers above. Shipped
		// values aren't known from the headers, so each is captured once as a baseline
		// and the UI edits a multiplier of it; the baseline is persisted per save so a
		// hot-reload can't re-read an already-cheated value and call it stock.
		enum RawIndex : int
		{
			kRawNormalSpeed = 0, kRawSprintSpeed, kRawCrouchSpeed, kRawADSSpeed,
			kRawAccel, kRawSprintRamp, kRawMaxAccel, kRawGroundFriction, kRawBrakingWalk,

			kRawJumpZ, kRawGravity, kRawAirControl, kRawCoyote, kRawStepHeight, kRawAirFriction,

			kRawDashDistance, kRawDashDuration, kRawDashCooldown, kRawDashEnergy, kRawAirDashes,

			kRawSlideMaxSpeed, kRawSlideImpulse, kRawSlideGravity, kRawSlideFriction,

			kRawZipSpeed, kRawZipAccel,

			kRawMaxEnergy, kRawRegenDelay, kRawSprintDrain, kRawRegenRate,
			kRawJumpEnergy, kRawDoubleJumpEnergy,
			kRawCount
		};

		struct RawDef
		{
			const char* label;
			const char* key;
			float       minValue;
			float       maxValue;
			const char* tooltip;
		};

		const RawDef kRaws[kRawCount] = {
			{ "Walk",              "normalSpeed",   0.10f, 10.0f, "Base ground speed. The sprint state machine feeds this into the\nmovement component every frame, so this is the value worth changing\nrather than fighting MaxWalkSpeed directly." },
			{ "Sprint",            "sprintSpeed",   0.10f, 10.0f, "Speed while sprinting." },
			{ "Crouch",            "crouchSpeed",   0.10f, 10.0f, "Speed while crouched." },
			{ "Aiming",            "adsSpeed",      0.10f, 10.0f, "Speed while aiming down sights." },
			{ "Acceleration",      "accel",         0.10f, 20.0f, "How fast you reach top speed and how fast you stop." },
			{ "Sprint Ramp Time",  "sprintRamp",    0.00f,  5.0f, "Time spent accelerating into a sprint. 0 makes sprint instant." },
			{ "Max Acceleration",  "maxAccel",      0.10f, 20.0f, "Movement component acceleration ceiling." },
			{ "Ground Friction",   "groundFric",    0.00f,  5.0f, "Grip. Lower is slidier; 0 is ice." },
			{ "Braking (ground)",  "brakeWalk",     0.00f,  5.0f, "Deceleration when you stop walking. 0 means you coast." },

			{ "Jump Velocity",     "jumpZ",         0.10f, 10.0f, "Upward impulse of a jump." },
			{ "Gravity Scale",     "gravity",       0.00f,  5.0f, "Gravity applied to you only. 0 is weightless; below 1 is moon-like." },
			{ "Air Control",       "airControl",    0.00f, 20.0f, "How much steering you have mid-air." },
			{ "Coyote Time",       "coyote",        0.00f, 20.0f, "Grace period to still jump after walking off an edge." },
			{ "Step Height",       "stepHeight",    0.10f, 20.0f, "How tall a ledge you walk up without jumping." },
			{ "Air Friction",      "airFriction",   0.00f,  5.0f, "Lateral friction while falling." },

			{ "Dash Distance",     "dashDist",      0.10f, 10.0f, "How far a dash carries you." },
			{ "Dash Duration",     "dashTime",      0.10f,  5.0f, "How long the dash takes. Shorter with the same distance is faster." },
			{ "Dash Cooldown",     "dashCd",        0.00f,  3.0f, "Delay between dashes. 0 removes the cooldown." },
			{ "Dash Energy Cost",  "dashEnergy",    0.00f,  3.0f, "Energy a dash costs. 0 makes it free." },
			{ "Max Air Dashes",    "airDashes",     1.00f, 20.0f, "How many dashes you get before touching the ground again." },

			{ "Max Slide Speed",   "slideMax",      0.10f, 10.0f, "Top speed a slide can reach." },
			{ "Slide Impulse",     "slideImpulse",  0.10f, 10.0f, "Kick you get entering a slide." },
			{ "Slide Gravity",     "slideGravity",  0.00f,  5.0f, "Downhill pull while sliding." },
			{ "Slide Friction",    "slideFric",     0.00f,  5.0f, "Slide drag. 0 means a slide never scrubs speed." },

			{ "Base Speed",        "zipSpeed",      0.10f, 10.0f, "Travel speed on a zipline (your rider, not the zipline itself)." },
			{ "Ramp Up",           "zipAccel",      0.10f, 10.0f, "How fast you get up to zipline speed." },

			{ "Max Energy",        "maxEnergy",     0.10f, 20.0f, "Size of the stamina pool. Sprint, jump and dash all draw from it.\nMultiplies the base value. A LEM bonus (e.g. Endurancegiver) still\nadds on top." },
			{ "Regen Delay",       "regenDelay",    0.00f,  3.0f, "Pause before energy starts refilling. 0 regenerates immediately." },
			{ "Sprint Drain",      "sprintDrain",   0.00f,  3.0f, "Energy burned while sprinting. 0 makes sprinting free." },
			{ "Regen Rate",        "regenRate",     0.10f, 20.0f, "How fast energy refills." },
			{ "Jump Energy Cost",  "jumpEnergy",    0.00f,  3.0f, "Energy a jump costs. 0 makes it free." },
			{ "Double Jump Energy","dblJumpEnergy", 0.00f,  3.0f, "Energy a double jump costs. 0 makes it free." },
		};

		// Reset is queued rather than written from the UI: the panel renders on the
		// render thread and every SDK write in this module happens from Tick.
		std::atomic<bool> g_rawRestoreWanted[kRawCount] = {};

		// Written from RenderImGui (sliders, presets, reset), read every tick from
		// Tick (ApplyRawOverrides) on the game thread.
		std::atomic<float> g_rawValues[kRawCount];      // the multiplier the player edits
		std::atomic<float> g_rawBaseline[kRawCount];    // the game's shipped value, captured once
		std::atomic<bool>  g_rawValuesInit{ false };
		std::atomic<bool>  g_rawBaselineReady{ false };

		// "expected = game" readout for the raw rows, same contract as
		// g_dbgAttrGame/g_dbgAttrExpected above (see that comment). kRawMaxEnergy's
		// slot is filled from g_composedMaxEnergy instead of baseline*multiplier,
		// since that row composes rather than writing a captured baseline.
		// g_dbgRawBase/g_dbgRawBuffed are meaningful only for kRawMaxEnergy -- the
		// one raw row backed by a real GAS attribute with its own BaseValue; every
		// other raw row is a plain engine field with no base/buff split, so those
		// slots are left unused and never rendered.
		std::atomic<float> g_dbgRawGame[kRawCount];
		std::atomic<float> g_dbgRawExpected[kRawCount];
		std::atomic<float> g_dbgRawBase[kRawCount];
		std::atomic<float> g_dbgRawBuffed[kRawCount];

		void EnsureRawDefaults()
		{
			if (g_rawValuesInit.exchange(true)) return;
			for (int r = 0; r < kRawCount; ++r)
			{
				g_rawValues[r].store(1.0f);
				g_rawBaseline[r].store(0.0f);
			}
		}

		bool IsRawActive(int raw)
		{
			return g_rawBaselineReady.load() && g_rawValuesInit.load()
			    && BetterCheats::DiffersFromDefault(g_rawValues[raw].load(), 1.0f);
		}

		std::string RawConfigKey(const char* leaf)
		{
			return std::string("playerMovement.raw.") + leaf;
		}

		std::string RawBaselineKey(const char* leaf)
		{
			return std::string("playerMovement.baseline.") + leaf;
		}

		// Pointer-to-member tables. Two owners, both per-player instances.
		using CompF = float SDK::UCrCharacterMovementComponent::*;
		using CharF = float SDK::ACrCharacterPlayerBase::*;

		struct CompBind { int raw; CompF member; };
		struct CharBind { int raw; CharF member; };

		const CompBind kCompBinds[] = {
			{ kRawMaxAccel,       &SDK::UCrCharacterMovementComponent::MaxAcceleration },
			{ kRawGroundFriction, &SDK::UCrCharacterMovementComponent::GroundFriction },
			{ kRawBrakingWalk,    &SDK::UCrCharacterMovementComponent::BrakingDecelerationWalking },
			{ kRawJumpZ,          &SDK::UCrCharacterMovementComponent::JumpZVelocity },
			{ kRawGravity,        &SDK::UCrCharacterMovementComponent::GravityScale },
			{ kRawAirControl,     &SDK::UCrCharacterMovementComponent::AirControl },
			{ kRawStepHeight,     &SDK::UCrCharacterMovementComponent::MaxStepHeight },
			{ kRawAirFriction,    &SDK::UCrCharacterMovementComponent::FallingLateralFriction },
			{ kRawCoyote,         &SDK::UCrCharacterMovementComponent::CoyoteTime },
			{ kRawDashDistance,   &SDK::UCrCharacterMovementComponent::MaxDashDistance },
			{ kRawDashDuration,   &SDK::UCrCharacterMovementComponent::DashDuration },
			{ kRawDashCooldown,   &SDK::UCrCharacterMovementComponent::DashCooldownSeconds },
			{ kRawDashEnergy,     &SDK::UCrCharacterMovementComponent::DashEnergyCostBase },
			{ kRawSlideMaxSpeed,  &SDK::UCrCharacterMovementComponent::MaxSlideSpeed },
			{ kRawSlideImpulse,   &SDK::UCrCharacterMovementComponent::SlideEnterImpulse },
			{ kRawSlideGravity,   &SDK::UCrCharacterMovementComponent::SlideGravityForce },
			{ kRawSlideFriction,  &SDK::UCrCharacterMovementComponent::SlideFrictionFactor },
			{ kRawZipSpeed,       &SDK::UCrCharacterMovementComponent::DefaultZiplineSpeed },
			{ kRawZipAccel,       &SDK::UCrCharacterMovementComponent::DefaultZiplineAcceleration },
			{ kRawJumpEnergy,     &SDK::UCrCharacterMovementComponent::JumpEnergyCost },
		};
		constexpr int kCompBindCount = static_cast<int>(sizeof(kCompBinds) / sizeof(kCompBinds[0]));

		const CharBind kCharBinds[] = {
			{ kRawNormalSpeed,      &SDK::ACrCharacterPlayerBase::NormalMoveSpeed },
			{ kRawSprintSpeed,      &SDK::ACrCharacterPlayerBase::SprintMoveSpeed },
			{ kRawCrouchSpeed,      &SDK::ACrCharacterPlayerBase::CrouchedMoveSpeed },
			{ kRawADSSpeed,         &SDK::ACrCharacterPlayerBase::ADSMoveSpeed },
			{ kRawAccel,            &SDK::ACrCharacterPlayerBase::AccelerationAndDeceleration },
			{ kRawSprintRamp,       &SDK::ACrCharacterPlayerBase::DurationOfAccelerationToSprint },
			{ kRawSprintDrain,      &SDK::ACrCharacterPlayerBase::EnergyLoweringMultiplierDuringSprint },
			{ kRawRegenRate,        &SDK::ACrCharacterPlayerBase::NormalEnergyRestorationRate },
			{ kRawDoubleJumpEnergy, &SDK::ACrCharacterPlayerBase::DoubleJumpEnergyUsage },
		};
		constexpr int kCharBindCount = static_cast<int>(sizeof(kCharBinds) / sizeof(kCharBinds[0]));

		// Captures every field's shipped value exactly once, then persists it so a
		// hot-reload restores the baseline instead of re-reading an already-cheated
		// component. See the header comment -- this is the one thing that must not go
		// wrong, because a bad baseline makes reset permanently wrong.
		void CaptureRawBaseline(SDK::ACrCharacterPlayerBase* character)
		{
			if (g_rawBaselineReady.load()) return;

			SDK::UCrCharacterMovementComponent* move = character->CrCharacterMovementComponent;
			if (!move) return;

			for (int i = 0; i < kCompBindCount; ++i)
				g_rawBaseline[kCompBinds[i].raw].store(move->*kCompBinds[i].member);

			for (int i = 0; i < kCharBindCount; ++i)
				g_rawBaseline[kCharBinds[i].raw].store(character->*kCharBinds[i].member);

			g_rawBaseline[kRawAirDashes].store(static_cast<float>(move->MaxAirDashesToExecute));

			// Max Energy has no baseline of its own to capture -- it composes onto
			// the game's live value instead (see g_composedMaxEnergy).

			if (SDK::UCrEnergyLogicComponent* logic = character->EnergyLogicComponent)
				g_rawBaseline[kRawRegenDelay].store(logic->DelayBeforeEnergyRegeneration);

			g_rawBaselineReady.store(true);

			for (int r = 0; r < kRawCount; ++r)
				SessionConfig::Set(RawBaselineKey(kRaws[r].key), g_rawBaseline[r].load());
			SessionConfig::Set("playerMovement.baselineCaptured", true);

			LOG_INFO("Movement: captured stock baseline for %d raw fields.", kRawCount);
		}

		// Nothing reaches a game field without passing through here.
		//
		// A row declares its own safe range, and the movement-critical ones (the gait
		// speeds and acceleration) declare a MINIMUM above zero -- so no combination of
		// stale config, a corrupt value or a bad edit can multiply the player's speed by
		// zero and strand them. Rows where zero is a legitimate setting (gravity, dash
		// cooldown, sprint drain) declare a minimum of 0 and keep it.
		float SafeMultiplier(int raw)
		{
			return BetterCheats::SafeValue(g_rawValues[raw].load(), 1.0f, kRaws[raw].minValue, kRaws[raw].maxValue);
		}

		void ApplyRawOverrides(SDK::ACrCharacterPlayerBase* character)
		{
			// g_rawValuesInit matters as much as the baseline: a zero-initialised
			// multiplier table reads as "every row active" and multiplies the player's
			// speed by 0.0, which looks exactly like the game freezing.
			if (!g_rawBaselineReady.load() || !g_rawValuesInit.load()) return;

			SDK::UCrCharacterMovementComponent* move = character->CrCharacterMovementComponent;
			if (!move) return;

			// Live-values block: read regardless of which rows are active, so the
			// panel always shows what the game currently has.
			g_dbgMaxWalkSpeed.store(move->MaxWalkSpeed);
			g_dbgJumpZVelocity.store(move->JumpZVelocity);
			{
				const SDK::FVector& vel = move->Velocity;
				g_dbgHorizSpeed.store(static_cast<float>(std::sqrt(vel.X * vel.X + vel.Y * vel.Y)));
			}

			// Compared before every write below: once a row settles, re-deriving the
			// same target value every tick is a write the game never asked for. Each
			// row also records "expected = game" for the live-values readout --
			// `want` while active (what we intend), the field's own pre-write value
			// while inactive (we intend nothing, so the game's own value IS expected).
			for (int i = 0; i < kCompBindCount; ++i)
			{
				const int r = kCompBinds[i].raw;
				float& field = move->*kCompBinds[i].member;
				const float gameBefore = field;
				const bool  active     = IsRawActive(r);
				const float want       = g_rawBaseline[r].load() * SafeMultiplier(r);
				if (active && field != want) field = want;
				g_dbgRawGame[r].store(gameBefore);
				g_dbgRawExpected[r].store(active ? want : gameBefore);
			}

			for (int i = 0; i < kCharBindCount; ++i)
			{
				const int r = kCharBinds[i].raw;
				float& field = character->*kCharBinds[i].member;
				const float gameBefore = field;
				const bool  active     = IsRawActive(r);
				const float want       = g_rawBaseline[r].load() * SafeMultiplier(r);
				if (active && field != want) field = want;
				g_dbgRawGame[r].store(gameBefore);
				g_dbgRawExpected[r].store(active ? want : gameBefore);
			}

			{
				const bool  active      = IsRawActive(kRawAirDashes);
				const float gameBefore  = static_cast<float>(move->MaxAirDashesToExecute);
				const float want        = g_rawBaseline[kRawAirDashes].load() * SafeMultiplier(kRawAirDashes);
				const int   wantInt     = static_cast<int>(want + 0.5f);
				if (active && move->MaxAirDashesToExecute != wantInt)
					move->MaxAirDashesToExecute = wantInt;
				g_dbgRawGame[kRawAirDashes].store(gameBefore);
				g_dbgRawExpected[kRawAirDashes].store(active ? want : gameBefore);
			}

			// Max Energy composes onto the live game value instead of a captured
			// baseline (see g_composedMaxEnergy) -- ComposedAttribute::Apply/Release
			// already compare before writing.
			if (SDK::UCrEnergyAttributeSet* energy = character->EnergyAttributes)
			{
				constexpr const char* kMaxEnergyKey = "playerMovement.compose.maxEnergy";
				const bool  wasActive     = g_composedMaxEnergy.IsActive();
				const float previousWrite = g_composedMaxEnergy.GetWritten();
				if (!wasActive)
					BetterCheats::RestoreIfStale(kMaxEnergyKey, energy->MaxEnergy.CurrentValue);
				const float gameBefore = energy->MaxEnergy.CurrentValue;
				const bool  active     = IsRawActive(kRawMaxEnergy);

				if (active)
					g_composedMaxEnergy.Apply(energy, energy->MaxEnergy.CurrentValue, &energy->MaxEnergy.BaseValue,
						SafeMultiplier(kRawMaxEnergy), BetterCheats::ComposedAttribute::Mode::Multiply,
						kMaxEnergyFloor, kMaxEnergyCeiling);
				else
					g_composedMaxEnergy.Release(energy, energy->MaxEnergy.CurrentValue);

				if (active && (!wasActive || gameBefore != previousWrite))
					BetterCheats::SaveComposeState(kMaxEnergyKey, g_composedMaxEnergy.GetGame(), g_composedMaxEnergy.GetWritten());
				else if (!active)
					BetterCheats::ClearComposeState(kMaxEnergyKey);

				const float expected = active ? (wasActive ? previousWrite : gameBefore) : gameBefore;
				const float buffed   = active ? g_composedMaxEnergy.GetGame() : gameBefore;
				g_dbgRawGame[kRawMaxEnergy].store(gameBefore);
				g_dbgRawExpected[kRawMaxEnergy].store(expected);
				g_dbgRawBase[kRawMaxEnergy].store(energy->MaxEnergy.BaseValue);
				g_dbgRawBuffed[kRawMaxEnergy].store(buffed);

				// Live-values block: stamina current/max, read here since EnergyAttributes
				// is already resolved.
				g_dbgStaminaCurrent.store(energy->CurrentEnergy.CurrentValue);
				g_dbgStaminaMax.store(energy->MaxEnergy.CurrentValue);
			}

			if (SDK::UCrEnergyLogicComponent* logic = character->EnergyLogicComponent)
			{
				const float gameBefore = logic->DelayBeforeEnergyRegeneration;
				const bool  active     = IsRawActive(kRawRegenDelay);
				const float want       = g_rawBaseline[kRawRegenDelay].load() * SafeMultiplier(kRawRegenDelay);
				if (active && logic->DelayBeforeEnergyRegeneration != want)
					logic->DelayBeforeEnergyRegeneration = want;
				g_dbgRawGame[kRawRegenDelay].store(gameBefore);
				g_dbgRawExpected[kRawRegenDelay].store(active ? want : gameBefore);
			}
		}

		// Reset writes the captured baseline back once, because leaving the multiplier
		// at 1.0 only stops us overwriting the field -- it does not undo the last value
		// we wrote into it.
		void RestoreRawField(SDK::ACrCharacterPlayerBase* character, int raw)
		{
			if (!character || !g_rawBaselineReady.load()) return;

			SDK::UCrCharacterMovementComponent* move = character->CrCharacterMovementComponent;
			if (!move) return;

			const float baseline = g_rawBaseline[raw].load();

			for (int i = 0; i < kCompBindCount; ++i)
				if (kCompBinds[i].raw == raw) { move->*kCompBinds[i].member = baseline; return; }

			for (int i = 0; i < kCharBindCount; ++i)
				if (kCharBinds[i].raw == raw) { character->*kCharBinds[i].member = baseline; return; }

			if (raw == kRawAirDashes)
				move->MaxAirDashesToExecute = static_cast<int>(baseline + 0.5f);
			else if (raw == kRawMaxEnergy)
			{
				// No baseline to write back -- hand CurrentValue to whatever the game
				// currently has (see g_composedMaxEnergy).
				if (SDK::UCrEnergyAttributeSet* energy = character->EnergyAttributes)
				{
					g_composedMaxEnergy.Release(energy, energy->MaxEnergy.CurrentValue);
					BetterCheats::ClearComposeState("playerMovement.compose.maxEnergy");
				}
			}
			else if (raw == kRawRegenDelay)
			{
				if (SDK::UCrEnergyLogicComponent* logic = character->EnergyLogicComponent)
					logic->DelayBeforeEnergyRegeneration = baseline;
			}
		}

		struct PresetVal { int attr; float value; };
		struct PresetRaw { int raw;  float value; };

		struct Preset
		{
			const char* label;
			const char* tooltip;
			const char* credit;   // original mod this is modelled on, or null
			PresetVal   vals[6];
			PresetRaw   raws[8];  // component/actor fields, same multiplier model
		};

		const Preset kPresets[] = {
			{ "My Player Enhancements",
			  "Speed, jump, dash and slide with faster stamina regen -- a moderate\n"
			  "all-round mobility boost.",
			  "Recreates the pre-Update-2 pak mod 'My Player Enhancements'",
			  { {kAttrMoveSpeed,1.25f},{kAttrSprintSpeed,1.25f},{kAttrJumpHeight,1.35f},
			    {kAttrDodgeCost,0.50f},{kAttrStaminaRegen,1.75f},{kAttrSlideStaminaRegen,1.75f} },
			  { {kRawNormalSpeed,1.25f},{kRawSprintSpeed,1.25f},{kRawJumpZ,1.35f},
			    {kRawDashCooldown,0.60f},{kRawSlideMaxSpeed,1.25f},{kRawRegenRate,1.75f},{-1,0.0f} } },

			{ "Sonic Mode",
			  "Very high move, sprint and zipline speed. Raises the speed cap too, so\n"
			  "the override actually sticks instead of getting clamped back down.",
			  nullptr,
			  { {kAttrMoveSpeed,5.00f},{kAttrMoveSpeedMax,5.00f},{kAttrSprintSpeed,5.00f},
			    {kAttrZiplineSpeed,3.00f},{-1,0.0f} },
			  { {kRawNormalSpeed,5.00f},{kRawSprintSpeed,5.00f},{kRawCrouchSpeed,3.00f},
			    {kRawAccel,3.00f},{kRawMaxAccel,3.00f},{kRawZipSpeed,3.00f},
			    {kRawSprintRamp,0.10f},{-1,0.0f} } },

			{ "Moon Hop",
			  "Low gravity for real -- long floaty jumps, full air control and no fall\n"
			  "damage, with near-free double jumps.",
			  nullptr,
			  { {kAttrFallDamage,0.00f},{kAttrJumpHeight,4.00f},{kAttrDoubleJumpCost,0.05f},{-1,0.0f} },
			  { {kRawGravity,0.20f},{kRawJumpZ,2.50f},{kRawAirControl,4.00f},
			    {kRawAirFriction,0.30f},{kRawDoubleJumpEnergy,0.05f},{-1,0.0f} } },

			{ "Endless Momentum",
			  "Near-free dodges and double jumps backed by near-instant stamina regen --\n"
			  "spam both without ever running dry.",
			  nullptr,
			  { {kAttrDodgeCost,0.05f},{kAttrDoubleJumpCost,0.05f},
			    {kAttrStaminaRegen,8.00f},{kAttrSlideStaminaRegen,8.00f},{-1,0.0f} },
			  { {kRawDashEnergy,0.05f},{kRawDashCooldown,0.00f},{kRawJumpEnergy,0.05f},
			    {kRawDoubleJumpEnergy,0.05f},{kRawRegenRate,8.00f},{kRawRegenDelay,0.00f},
			    {kRawSprintDrain,0.00f},{-1,0.0f} } },
		};
		constexpr int kPresetCount = static_cast<int>(sizeof(kPresets) / sizeof(kPresets[0]));

		void ResetAllAttrs()
		{
			for (int a = 0; a < kAttrCount; ++a)
			{
				g_attrValues[a].store(kAttrs[a].defaultValue);
				SessionConfig::Set(AttrConfigKey(kAttrs[a].key), kAttrs[a].defaultValue);
			}
			for (int r = 0; r < kRawCount; ++r)
			{
				g_rawValues[r].store(1.0f);
				SessionConfig::Set(RawConfigKey(kRaws[r].key), 1.0f);
				g_rawRestoreWanted[r].store(true);   // put the stock value back, not just stop writing
			}
		}

		// Changing the overall Move Speed carries the individual gait speeds with it.
		// They are what the game actually reads, so a master multiplier that left them
		// behind would look applied and do nothing. Each one stays editable afterwards.
		void CascadeMoveSpeed(float value)
		{
			static const int kGaits[] = { kRawNormalSpeed, kRawSprintSpeed, kRawCrouchSpeed, kRawADSSpeed };
			for (int g : kGaits)
			{
				float v = value;
				if (v < kRaws[g].minValue) v = kRaws[g].minValue;
				if (v > kRaws[g].maxValue) v = kRaws[g].maxValue;
				g_rawValues[g].store(v);
				SessionConfig::Set(RawConfigKey(kRaws[g].key), v);
			}
		}

		void ApplyPreset(const Preset& preset)
		{
			for (const PresetVal& v : preset.vals)
			{
				if (v.attr < 0) break;
				g_attrValues[v.attr].store(v.value);
				SessionConfig::Set(AttrConfigKey(kAttrs[v.attr].key), v.value);
			}
			for (const PresetRaw& r : preset.raws)
			{
				if (r.raw < 0) break;
				g_rawValues[r.raw].store(r.value);
				SessionConfig::Set(RawConfigKey(kRaws[r.raw].key), r.value);
			}
			LOG_INFO("Movement: applied preset '%s'.", preset.label);
		}

		// ---------------------------------------------------------------------
		// Saved presets (PresetStore-backed) -- the owner's own tweaks,
		// named and kept apart from the built-in kPresets above. One "Movement"
		// group covers every row, GAS and raw, in kAttrs/kRaws index order
		// (GetLiveMovementFields/ApplyMovementFields must stay in that same
		// order -- PresetStore::Load matches by key, not position, but both
		// sides building the same array shape keeps this simple).
		// ---------------------------------------------------------------------

		constexpr const char* kMovementPresetGroup = "Movement";
		constexpr int         kMovementFieldCount  = kAttrCount + kRawCount;

		// kAttrs[kAttrSprintSpeed].key and kRaws[kRawSprintSpeed].key are both
		// the bare string "sprintSpeed" (the GAS multiplier and the raw
		// movement-component speed happen to share a name) -- PresetStore
		// keys fields by string, so two Field entries with the same key would
		// make Load() resolve both to whichever one appears first in the
		// stored preset, silently overwriting the other on every load. Only
		// the preset-facing key needs to change here; config persistence
		// (AttrConfigKey/RawConfigKey below) still uses kRaws[r].key as-is.
		const char* MovementRawPresetKey(int r)
		{
			return (r == kRawSprintSpeed) ? "sprintSpeedRaw" : kRaws[r].key;
		}

		void GetLiveMovementFields(BetterCheats::PresetStore::Field* out)
		{
			for (int a = 0; a < kAttrCount; ++a)
				out[a] = { kAttrs[a].key, g_attrValues[a].load() };
			for (int r = 0; r < kRawCount; ++r)
				out[kAttrCount + r] = { MovementRawPresetKey(r), g_rawValues[r].load() };
		}

		void ApplyMovementFields(const BetterCheats::PresetStore::Field* fields, int count)
		{
			for (int a = 0; a < kAttrCount && a < count; ++a)
			{
				g_attrValues[a].store(fields[a].value);
				SessionConfig::Set(AttrConfigKey(kAttrs[a].key), fields[a].value);
			}
			for (int r = 0; r < kRawCount && (kAttrCount + r) < count; ++r)
			{
				g_rawValues[r].store(fields[kAttrCount + r].value);
				SessionConfig::Set(RawConfigKey(kRaws[r].key), fields[kAttrCount + r].value);
			}
			LOG_INFO("Movement: applied a saved preset.");
		}

		bool IsBuiltinMovementName(const char* name)
		{
			for (int i = 0; i < kPresetCount; ++i)
				if (strcmp(kPresets[i].label, name) == 0)
					return true;
			return false;
		}

		// True only if every field the preset touches matches AND every field
		// it doesn't touch is still at its own default -- a hand-tweaked mix
		// that happens to overlap one preset's values isn't that preset.
		bool MatchesBuiltinMovementPreset(const Preset& preset)
		{
			bool touchedAttr[kAttrCount] = {};
			bool touchedRaw[kRawCount]   = {};

			for (const PresetVal& v : preset.vals)
			{
				if (v.attr < 0) break;
				if (std::fabs(g_attrValues[v.attr].load() - v.value) > BetterCheats::kActiveEpsilon)
					return false;
				touchedAttr[v.attr] = true;
			}
			for (const PresetRaw& r : preset.raws)
			{
				if (r.raw < 0) break;
				if (std::fabs(g_rawValues[r.raw].load() - r.value) > BetterCheats::kActiveEpsilon)
					return false;
				touchedRaw[r.raw] = true;
			}
			for (int a = 0; a < kAttrCount; ++a)
				if (!touchedAttr[a] && BetterCheats::DiffersFromDefault(g_attrValues[a].load(), kAttrs[a].defaultValue))
					return false;
			for (int r = 0; r < kRawCount; ++r)
				if (!touchedRaw[r] && BetterCheats::DiffersFromDefault(g_rawValues[r].load(), 1.0f))
					return false;
			return true;
		}

		void ComputeMovementSuggestedBase(char* out, int cap)
		{
			for (int i = 0; i < kPresetCount; ++i)
			{
				if (MatchesBuiltinMovementPreset(kPresets[i]))
				{
					snprintf(out, cap, "%s Custom", kPresets[i].label);
					return;
				}
			}
			snprintf(out, cap, "Custom");
		}

		BetterCheats::UI::SavedPresetRowState s_movementPresetRow;

		// One row: label (dimmed at default, + optional disclosure arrow/indent) +
		// live-values readout + joined slider/box + drawn reset button -- see
		// ui_widgets.h's BuildRow, which does the actual drawing.

		void RenderAttrRow(IModLoaderImGui* imgui, int attr, float labelReserve, int depth = 0, bool* openFlag = nullptr)
		{
			const AttrDef& def   = kAttrs[attr];
			// ImGui writes through a plain float*, so the atomic is read into a local
			// and only stored back when the widget actually reports a change.
			float          value = g_attrValues[attr].load();
			const bool     active = IsAttrActive(attr);

			imgui->PushIDInt(attr);
			imgui->TableNextRow(0, 0.0f);

			char changeDesc[24];
			BetterCheats::UI::FormatChangeDesc(changeDesc, sizeof(changeDesc), kAttrModes[attr], value);

			BetterCheats::UI::RowSpec spec;
			spec.label        = def.label;
			spec.tooltip      = def.tooltip;
			spec.value        = &value;
			spec.minValue     = def.minValue;
			spec.maxValue     = def.maxValue;
			spec.step         = def.step;
			spec.format       = def.format;
			spec.resetValue   = def.defaultValue;
			spec.active       = active;
			spec.depth        = depth;
			spec.openFlag     = openFlag;
			spec.padForArrow  = true;
			spec.showLive     = g_showLiveValues.load();
			spec.expected     = g_dbgAttrExpected[attr].load();
			spec.game         = g_dbgAttrGame[attr].load();
			spec.composing    = active;
			spec.hasBase      = true;
			spec.base         = g_dbgAttrBase[attr].load();
			spec.unmodified   = g_dbgAttrBuffed[attr].load();
			spec.changeDesc   = changeDesc;
			spec.tagWord      = "buff";
			spec.baseLabel    = "Base (no LEMs/buffs)";
			spec.labelReserve = labelReserve;

			if (BetterCheats::UI::BuildRow(imgui, spec).changed)
			{
				g_attrValues[attr].store(value);
				SessionConfig::Set(AttrConfigKey(def.key), value);

				if (attr == kAttrMoveSpeed)
					CascadeMoveSpeed(value);
			}

			imgui->PopID();
		}

		void RenderRawRow(IModLoaderImGui* imgui, int raw, float labelReserve, int depth = 1, bool* openFlag = nullptr)
		{
			const RawDef& def   = kRaws[raw];
			// ImGui writes through a plain float*, so the atomic is read into a local
			// and only stored back when the widget actually reports a change.
			float         value = g_rawValues[raw].load();
			const bool    active = IsRawActive(raw);
			const bool    baselineReady = g_rawBaselineReady.load();

			imgui->PushIDInt(1000 + raw);
			imgui->TableNextRow(0, 0.0f);

			// Every raw row is a Multiply of the captured baseline except Max Energy,
			// whose own tag/tooltip is GAS-composed the same way as the kAttrs rows.
			char changeDesc[24];
			BetterCheats::UI::FormatChangeDesc(changeDesc, sizeof(changeDesc),
				BetterCheats::ComposedAttribute::Mode::Multiply, value);

			BetterCheats::UI::RowSpec spec;
			spec.label        = def.label;
			spec.tooltip      = def.tooltip;
			spec.value        = &value;
			spec.minValue     = def.minValue;
			spec.maxValue     = def.maxValue;
			spec.step         = 0.05f;
			spec.format       = "%.2fx";
			spec.resetValue   = 1.0f;
			spec.active       = active;
			spec.disableSlider = !baselineReady;
			spec.depth        = depth;
			spec.openFlag     = openFlag;
			spec.padForArrow  = true;
			spec.showLive     = g_showLiveValues.load();
			spec.expected     = g_dbgRawExpected[raw].load();
			spec.game         = g_dbgRawGame[raw].load();
			spec.composing    = active;
			// Max Energy is the one raw row backed by a real GAS attribute --
			// baseline-driven rows have no BaseValue, so `unmodified` is the
			// captured stock baseline and no Base line/tag ever applies to them.
			spec.hasBase      = (raw == kRawMaxEnergy);
			spec.base         = g_dbgRawBase[raw].load();
			spec.unmodified   = (raw == kRawMaxEnergy) ? g_dbgRawBuffed[raw].load() : g_rawBaseline[raw].load();
			spec.changeDesc   = changeDesc;
			spec.tagWord      = "buff";
			spec.baseLabel    = "Base (no LEMs/buffs)";
			spec.labelReserve = labelReserve;
			spec.resetTooltip = "Put the game's stock value back (turns this row off).";

			const BetterCheats::UI::RowResult result = BuildRow(imgui, spec);
			if (result.changed)
			{
				g_rawValues[raw].store(value);
				SessionConfig::Set(RawConfigKey(def.key), value);
				if (result.resetClicked)
					g_rawRestoreWanted[raw].store(true);
			}

			imgui->PopID();
		}

		// Tuning rows. Where a headline control has finer detail behind it, that
		// control carries a disclosure arrow and its advanced rows sit indented
		// underneath -- so Move Speed is one row until you ask it for the individual
		// gaits, friction and acceleration.
		enum RowKind : int { RowAttr = 0, RowRaw = 1 };

		enum OpenId : int
		{
			kOpenMoveSpeed = 0, kOpenAccel, kOpenJump, kOpenDoubleJump, kOpenDodge,
			kOpenStamina, kOpenSlide, kOpenZipline, kOpenCount
		};
		bool g_rowOpen[kOpenCount] = {};

		struct ChildRef { int kind; int index; };
		struct TuneRow  { int kind; int index; const ChildRef* kids; int kidCount; int openId; };
		struct TuneGroup{ const char* title; const char* tableId; const TuneRow* rows; int count; };

		// Children sit with the control they are bound to, not with the system that
		// happens to own the underlying field. An action's energy cost belongs with the
		// action (you tune a dash as one thing); only the shared pool lives under stamina.
		// Air control and air friction go with dash because that is what they change the
		// feel of in practice.
		const ChildRef kKidsMoveSpeed[] = {
			{ RowRaw, kRawNormalSpeed }, { RowRaw, kRawSprintSpeed },
			{ RowRaw, kRawCrouchSpeed }, { RowRaw, kRawADSSpeed },
		};
		const ChildRef kKidsAccel[] = {
			{ RowRaw, kRawMaxAccel }, { RowRaw, kRawSprintRamp },
			{ RowRaw, kRawGroundFriction }, { RowRaw, kRawBrakingWalk },
		};
		const ChildRef kKidsJump[] = {
			{ RowRaw, kRawJumpZ }, { RowRaw, kRawGravity },
			{ RowRaw, kRawCoyote }, { RowRaw, kRawStepHeight },
			{ RowRaw, kRawJumpEnergy },
		};
		const ChildRef kKidsDoubleJump[] = {
			{ RowRaw, kRawDoubleJumpEnergy },
		};
		const ChildRef kKidsDodge[] = {
			{ RowRaw, kRawDashDistance }, { RowRaw, kRawDashDuration },
			{ RowRaw, kRawDashCooldown }, { RowRaw, kRawDashEnergy },
			{ RowRaw, kRawAirDashes },
			{ RowRaw, kRawAirControl }, { RowRaw, kRawAirFriction },
		};
		const ChildRef kKidsStamina[] = {
			{ RowRaw, kRawRegenRate }, { RowRaw, kRawRegenDelay },
			{ RowRaw, kRawSprintDrain }, { RowRaw, kRawMaxEnergy },
		};
		const ChildRef kKidsSlide[] = {
			{ RowRaw, kRawSlideMaxSpeed }, { RowRaw, kRawSlideImpulse },
			{ RowRaw, kRawSlideGravity }, { RowRaw, kRawSlideFriction },
		};
		const ChildRef kKidsZipline[] = {
			{ RowRaw, kRawZipSpeed }, { RowRaw, kRawZipAccel },
		};

		#define KIDS(a) a, static_cast<int>(sizeof(a) / sizeof(a[0]))

		const TuneRow kRowsSpeed[] = {
			{ RowAttr, kAttrMoveSpeed,    KIDS(kKidsMoveSpeed), kOpenMoveSpeed },
			{ RowRaw,  kRawAccel,         KIDS(kKidsAccel),     kOpenAccel },
			{ RowAttr, kAttrMoveSpeedMax, nullptr, 0, -1 },
			{ RowAttr, kAttrMoveSpeedMin, nullptr, 0, -1 },
			{ RowAttr, kAttrSprintSpeed,  nullptr, 0, -1 },
		};

		const TuneRow kRowsJump[] = {
			{ RowAttr, kAttrJumpHeight,     KIDS(kKidsJump),       kOpenJump },
			{ RowAttr, kAttrDoubleJumpCost, KIDS(kKidsDoubleJump), kOpenDoubleJump },
			{ RowAttr, kAttrDodgeCost,      KIDS(kKidsDodge),      kOpenDodge },
		};

		const TuneRow kRowsStamina[] = {
			{ RowAttr, kAttrStaminaRegen,      KIDS(kKidsStamina), kOpenStamina },
			{ RowAttr, kAttrSlideStaminaRegen, KIDS(kKidsSlide),   kOpenSlide },
			{ RowAttr, kAttrZiplineSpeed,      KIDS(kKidsZipline), kOpenZipline },
			{ RowAttr, kAttrFallDamage,        nullptr, 0, -1 },
		};

		const TuneGroup kTuneGroups[] = {
			{ "Speed",              "##tg_speed",   KIDS(kRowsSpeed) },
			{ "Jump & Dodge",       "##tg_jump",    KIDS(kRowsJump) },
			{ "Stamina & Survival", "##tg_stamina", KIDS(kRowsStamina) },
		};
		#undef KIDS
		constexpr int kTuneGroupCount = static_cast<int>(sizeof(kTuneGroups) / sizeof(kTuneGroups[0]));

		void RenderTuneGroups(IModLoaderImGui* imgui)
		{
			if (!g_rawBaselineReady.load())
				imgui->TextDisabled("Advanced rows stay greyed until the game's stock values are read - load a save.");

			// Widest label across both row kinds decides where the live-values
			// readout starts, so every row's readout lines up across every group.
			// +2 extra levels (on top of the base 1x): arrow button + one indent
			// step, the deepest a child row ever sits.
			const float labelReserve = (std::max)(
				BetterCheats::UI::PrescanLabelWidth(imgui, kAttrCount, [](int a) { return kAttrs[a].label; }, 2),
				BetterCheats::UI::PrescanLabelWidth(imgui, kRawCount,  [](int r) { return kRaws[r].label; },  2));
			const float readoutColumnWidth = BetterCheats::UI::GetReadoutColumnWidth(imgui, labelReserve);

			for (int g = 0; g < kTuneGroupCount; ++g)
			{
				const TuneGroup& grp = kTuneGroups[g];

				imgui->Spacing();
				imgui->SeparatorText(grp.title);
				imgui->Spacing();

				if (!imgui->BeginTable(grp.tableId, 3, kTableFlags))
					continue;

				// Fixed, not stretched -- see player_weapons.cpp's identical table
				// for why (must fit the readout, and the slider has slack to give
				// up now that it's capped).
				imgui->TableSetupColumn("Attribute", BetterCheats::UI::kColumnWidthFixed, readoutColumnWidth);
				imgui->TableSetupColumn("Value",     0, 0.54f);
				imgui->TableSetupColumn("",          0, 0.10f);

				for (int r = 0; r < grp.count; ++r)
				{
					const TuneRow& row = grp.rows[r];
					bool* openFlag = (row.openId >= 0) ? &g_rowOpen[row.openId] : nullptr;

					if (row.kind == RowAttr) RenderAttrRow(imgui, row.index, labelReserve, 0, openFlag);
					else                     RenderRawRow(imgui, row.index, labelReserve, 0, openFlag);

					if (!openFlag || !*openFlag)
						continue;

					for (int k = 0; k < row.kidCount; ++k)
					{
						if (row.kids[k].kind == RowAttr) RenderAttrRow(imgui, row.kids[k].index, labelReserve, 1, nullptr);
						else                             RenderRawRow(imgui, row.kids[k].index, labelReserve, 1, nullptr);
					}
				}

				imgui->EndTable();
			}
		}

	}

	void Initialize()
	{
		// Seed before the panel or the tick is registered. Left to the lazy callers, Tick
		// and RenderImGui can race the once-check and one of them reads zeroed values.
		EnsureAttrDefaults();
		EnsureRawDefaults();

		IPluginSelf* self = GetSelf();
		if (!self || !self->hooks || !self->hooks->Input)
			return;

		{
			std::lock_guard<std::mutex> lock(g_keyMutex);
			snprintf(g_noClipKey, sizeof(g_noClipKey), "%s", BetterCheatsConfig::Config::GetNoClipKey());
		}

		char combo[64];
		ReadNoClipKey(combo, sizeof(combo));

		self->hooks->Input->RegisterKeybindByName(combo, EModKeyEvent::Pressed, &OnNoClipKeyPressed);
	}

	void Shutdown()
	{
		IPluginSelf* self = GetSelf();
		if (self && self->hooks && self->hooks->Input)
		{
			char combo[64];
			ReadNoClipKey(combo, sizeof(combo));

			if (combo[0])
				self->hooks->Input->UnregisterKeybindByName(combo, EModKeyEvent::Pressed, &OnNoClipKeyPressed);
		}

		// The loader's own RELOAD button runs PluginShutdown from its D3D Present
		// hook, not the game thread Tick() recorded -- GetLocalCharacter() and
		// every UObject touch below intermittently crash there (see player_lookup.h).
		// Forget instead of Release: SessionConfig still has whatever RestoreIfStale
		// needs to undo a leftover write on the next activation, off-thread or not.
		if (!BetterCheats::IsGameThread())
		{
			for (int a = 0; a < kAttrCount; ++a)
				g_composedAttrs[a].Forget();
			g_composedMaxEnergy.Forget();
			g_active    = false;
			g_appliedTo = nullptr;
			g_noClipWanted.store(false);
			return;
		}

		// Leaving a character permanently non-colliding would outlive the plugin,
		// so put it back — but only if the pawn we changed is still the live one.
		try
		{
			SDK::ACrCharacterPlayerBase* character = GetLocalCharacter();

			if (g_active && character && character == g_appliedTo)
				Disable(character);

			// Same rule for the composed overrides: hand CurrentValue back only if
			// it's still the same character/attribute-set combination they were
			// captured against, otherwise just drop the state.
			if (character && character == g_composedCharacter)
			{
				if (SDK::UCrMovementAttributeSet* set = character->MovementAttributes)
					for (const MoveBinding& b : kMoveBindings)
					{
						g_composedAttrs[b.attr].Release(set, (set->*b.member).CurrentValue);
						BetterCheats::ClearComposeState(std::string("playerMovement.compose.") + kAttrs[b.attr].key);
					}

				if (SDK::UCrMovementSpeedMultiplierAttributeSet* set = character->MovementSpeedMultiplierAttributes)
					for (const SpeedBinding& b : kSpeedBindings)
					{
						g_composedAttrs[b.attr].Release(set, (set->*b.member).CurrentValue);
						BetterCheats::ClearComposeState(std::string("playerMovement.compose.") + kAttrs[b.attr].key);
					}

				if (SDK::UCrGemAttributeSet* set = character->GemAttributes)
					for (const GemBinding& b : kGemBindings)
					{
						g_composedAttrs[b.attr].Release(set, (set->*b.member).CurrentValue);
						BetterCheats::ClearComposeState(std::string("playerMovement.compose.") + kAttrs[b.attr].key);
					}

				if (SDK::UCrEnergyAttributeSet* energy = character->EnergyAttributes)
				{
					g_composedMaxEnergy.Release(energy, energy->MaxEnergy.CurrentValue);
					BetterCheats::ClearComposeState("playerMovement.compose.maxEnergy");
				}
			}
			else
			{
				for (int a = 0; a < kAttrCount; ++a)
					g_composedAttrs[a].Forget();
				g_composedMaxEnergy.Forget();
			}
		}
		catch (...) {}

		g_active    = false;
		g_appliedTo = nullptr;
		g_noClipWanted.store(false);
	}

	void Tick(float /*deltaSeconds*/)
	{
		BetterCheats::RecordGameThread();

		EnsureAttrDefaults();
		EnsureRawDefaults();

		try
		{
			SDK::ACrCharacterPlayerBase* character = GetLocalCharacter();

			// Respawn or a world change replaces the pawn; the one we altered is
			// gone, so forget it rather than writing through a freed pointer.
			if (g_appliedTo && g_appliedTo != character)
			{
				g_active    = false;
				g_appliedTo = nullptr;
			}

			// Same idea for the composed GAS/Max Energy overrides -- drop them before
			// anything below tries to Apply/Release against an attribute set that no
			// longer belongs to the current pawn.
			ForgetComposedIfCharacterChanged(character);

			if (!character)
			{
				RefreshSnapshot(nullptr);
				return;
			}

			const bool wanted = g_noClipWanted.load();
			if (wanted && !g_active)
				Enable(character);
			else if (!wanted && g_active)
				Disable(character);

			if (g_active)
				MaintainNoClip(character);

			// Independent of no-clip -- these are gameplay-effect re-evaluated
			// attributes, so an active override has to be re-asserted every tick.
			ApplyAttributeOverrides(character);

			// Raw component/actor fields. Baseline first, and only ever once: it is
			// what reset restores, so capturing it while an override was live would
			// bake a cheated value in as stock.
			CaptureRawBaseline(character);

			for (int r = 0; r < kRawCount; ++r)
				if (g_rawRestoreWanted[r].load())
				{
					RestoreRawField(character, r);
					g_rawRestoreWanted[r].store(false);
				}

			ApplyRawOverrides(character);

			RefreshSnapshot(character);
		}
		catch (...)
		{
			LOG_ERROR("Movement: exception while driving no-clip.");
		}
	}

	void ApplySavedConfig()
	{
		if (!SessionConfig::IsLoaded())
			return;

		float speed = SessionConfig::Get("playerMovement.flySpeedMultiplier", kDefaultFlySpeedMultiplier);
		if (speed < kMinFlySpeedMultiplier) speed = kMinFlySpeedMultiplier;
		if (speed > kMaxFlySpeedMultiplier) speed = kMaxFlySpeedMultiplier;

		g_flySpeedMultiplier.store(speed);
		g_passThroughWalls.store(SessionConfig::Get("playerMovement.passThroughWalls", true));
		g_showLiveValues.store(SessionConfig::Get("playerMovement.showLiveValues", true));

		EnsureAttrDefaults();
		EnsureRawDefaults();
		for (int a = 0; a < kAttrCount; ++a)
		{
			float value = SessionConfig::Get(AttrConfigKey(kAttrs[a].key), kAttrs[a].defaultValue);
			if (value < kAttrs[a].minValue) value = kAttrs[a].minValue;
			if (value > kAttrs[a].maxValue) value = kAttrs[a].maxValue;
			g_attrValues[a].store(value);
		}

		// Raw multipliers, plus the stock baseline they multiply. Restoring a persisted
		// baseline is what stops a plugin hot-reload re-reading an already-cheated
		// component and calling those values "stock".
		EnsureRawDefaults();
		const bool haveBaseline = SessionConfig::Get("playerMovement.baselineCaptured", false);
		for (int r = 0; r < kRawCount; ++r)
		{
			float value = SessionConfig::Get(RawConfigKey(kRaws[r].key), 1.0f);
			if (value < kRaws[r].minValue) value = kRaws[r].minValue;
			if (value > kRaws[r].maxValue) value = kRaws[r].maxValue;
			g_rawValues[r].store(value);

			g_rawBaseline[r].store(SessionConfig::Get(RawBaselineKey(kRaws[r].key), 0.0f));
		}
		g_rawBaselineReady.store(haveBaseline);

		LOG_INFO("Movement: applied saved config for session '%s' (baseline %s).",
			SessionConfig::GetSessionName().c_str(), haveBaseline ? "restored" : "not yet captured");
	}

	void RenderImGui(IModLoaderImGui* imgui)
	{
		if (!g_loggedRender.exchange(true))
			LOG_INFO("Movement: live values readout rendering.");

		EnsureAttrDefaults();
		EnsureRawDefaults();

		MovementSnapshot snap;
		{
			std::lock_guard<std::mutex> lock(g_snapshotMutex);
			snap = g_snapshot;
		}

		// First control in the tab, unconditional -- not gated behind
		// snap.characterFound below, so it's visible even before a save loads.
		bool showLiveValues = g_showLiveValues.load();
		if (imgui->Checkbox("Show live values", &showLiveValues))
		{
			g_showLiveValues.store(showLiveValues);
			SessionConfig::Set("playerMovement.showLiveValues", showLiveValues);
		}
		if (imgui->IsItemHovered())
			imgui->SetTooltip("Shows the game's own numbers next to each slider, so a change is\n"
			                  "obvious instead of a guess.");

		imgui->TextDisabled("Multipliers apply to your base stats. LEMs and game buffs still add on top.");

		imgui->SeparatorText("Fly / No-Clip Movement");

		if (!snap.characterFound)
		{
			imgui->TextDisabled("Player character not found -- load into a game session.");
			return;
		}

		char currentKey[64];
		ReadNoClipKey(currentKey, sizeof(currentKey));

		bool  noClip = g_noClipWanted.load();
		bool  ghost  = g_passThroughWalls.load();
		float speed  = g_flySpeedMultiplier.load();

		if (imgui->BeginTable("##noclip_table", 2, kTableFlags))
		{
			imgui->TableSetupColumn("Option", 0,            0.45f);
			imgui->TableSetupColumn("Value",  0,            0.55f);

			// Row 1: Fly Mode toggle + hotkey picker
			imgui->TableNextRow(0, 0.0f);
			imgui->TableSetColumnIndex(0);
			imgui->Text("Fly Mode Toggle:");
			if (imgui->IsItemHovered())
				imgui->SetTooltip("Toggle vertical flight mode. Ascend with Space, descend with Left Ctrl.");

			imgui->TableSetColumnIndex(1);
			if (imgui->Checkbox("Active##noclip", &noClip))
				g_noClipWanted.store(noClip);

			imgui->SameLine(0.0f, 12.0f);
			imgui->Text("Hotkey:");
			imgui->SameLine(0.0f, 6.0f);
			char pickedKey[64];
			if (Keybind::RenderPicker(imgui, "noclip_key_picker", currentKey, pickedKey, sizeof(pickedKey)))
				ApplyNoClipKey(pickedKey);

			imgui->SameLine(0.0f, 6.0f);
			if (imgui->SmallButton("Reset##key"))
			{
				Keybind::CancelCapture();
				ApplyNoClipKey(BetterCheatsConfig::kDefaultNoClipKey);
			}

			// Row 2: Pass through walls (clipping)
			imgui->TableNextRow(0, 0.0f);
			imgui->TableSetColumnIndex(0);
			imgui->Text("Pass Through Walls (Clipping)");
			if (imgui->IsItemHovered())
				imgui->SetTooltip("On: Disable capsule collision to fly through terrain & walls.\n"
				                  "Off: Retain collision so you can fly around without clipping through floors.");

			imgui->TableSetColumnIndex(1);
			if (imgui->Checkbox("Enabled##passwalls", &ghost))
			{
				g_passThroughWalls.store(ghost);
				SessionConfig::Set("playerMovement.passThroughWalls", ghost);
			}

			// Row 3: Fly Speed
			imgui->TableNextRow(0, 0.0f);
			imgui->TableSetColumnIndex(0);
			imgui->Text("Fly Speed Multiplier");
			imgui->TableSetColumnIndex(1);
			imgui->SetNextItemWidth(BetterCheats::UI::GetSliderWidthCap(imgui));
			if (imgui->SliderFloat("##fly_speed", &speed, kMinFlySpeedMultiplier, kMaxFlySpeedMultiplier, "%.1fx"))
			{
				g_flySpeedMultiplier.store(speed);
				SessionConfig::Set("playerMovement.flySpeedMultiplier", speed);
			}

			imgui->EndTable();
		}

		imgui->Spacing();

		// Flashing Warning Banner when clipping is enabled
		if (ghost)
		{
			ULONGLONG ticks = GetTickCount64();
			bool flash = ((ticks / 350) % 2) == 0;
			imgui->Spacing();
			if (flash)
			{
				imgui->TextColored(1.0f, 0.25f, 0.25f, 1.0f, "[!] WARNING: CLIPPING / PASS THROUGH WALLS IS ACTIVE!");
				imgui->TextColored(1.0f, 0.85f, 0.2f, 1.0f, "Disabling Fly mode while below ground geometry will cause you to fall through the world!");
			}
			else
			{
				imgui->TextColored(1.0f, 0.65f, 0.0f, 1.0f, "[!] WARNING: CLIPPING / PASS THROUGH WALLS IS ACTIVE!");
				imgui->TextColored(0.95f, 0.95f, 0.4f, 1.0f, "Disabling Fly mode while below ground geometry will cause you to fall through the world!");
			}
		}
		else
		{
			imgui->Spacing();
			imgui->TextDisabled("Collision enabled: Safe from falling through terrain when Fly is toggled off.");
		}

		imgui->Spacing();
		char buffer[220];
		float topCms = snap.baseFlySpeed * speed;
		float topKmh = topCms * 0.036f;
		float topMph = topCms * 0.0223693629f;
		snprintf(buffer, sizeof(buffer), "Top speed: %.1f km/h  (%.1f mph | %.0f cm/s)", topKmh, topMph, topCms);
		imgui->TextDisabled(buffer);

		imgui->Spacing();
		imgui->TextWrapped("Look and move as usual to fly horizontally; hold Space to rise and Left Ctrl to descend. Vertical input is ignored while this menu is open.");

		// ---- attribute presets -------------------------------------------------
		imgui->Spacing();
		imgui->SeparatorText("Presets");
		imgui->TextDisabled("Presets:");
		for (int i = 0; i < kPresetCount; ++i)
		{
			const Preset& preset = kPresets[i];

			imgui->SameLine(0.0f, -1.0f);
			if (imgui->SmallButton(preset.label))
				ApplyPreset(preset);
			if (imgui->IsItemHovered())
			{
				char tip[512];
				snprintf(tip, sizeof(tip), "%s%s%s",
					preset.tooltip,
					preset.credit ? "\n\n" : "",
					preset.credit ? preset.credit : "");
				imgui->SetTooltip(tip);
			}
		}

		imgui->Spacing();
		{
			BetterCheats::PresetStore::Field fields[kMovementFieldCount];
			BetterCheats::UI::RenderSavedPresetsRow(imgui, "movement_saved_presets", kMovementPresetGroup,
				fields, kMovementFieldCount,
				&GetLiveMovementFields, &ApplyMovementFields,
				&IsBuiltinMovementName, &ComputeMovementSuggestedBase, s_movementPresetRow);
		}

		// ---- tuning rows --------------------------------------------------------
		imgui->Spacing();
		imgui->TextDisabled("A value differing from the default is applied. Reset a row to turn it off.");
		imgui->TextDisabled("Rows with an arrow open up the finer controls behind them.");

		if (g_showLiveValues.load())
		{
			char walk[32], horiz[32], jumpZ[32], staCur[32], staMax[32];
			BetterCheats::UI::FormatLiveValue(walk,   sizeof(walk),   g_dbgMaxWalkSpeed.load());
			BetterCheats::UI::FormatLiveValue(horiz,  sizeof(horiz),  g_dbgHorizSpeed.load());
			BetterCheats::UI::FormatLiveValue(jumpZ,  sizeof(jumpZ),  g_dbgJumpZVelocity.load());
			BetterCheats::UI::FormatLiveValue(staCur, sizeof(staCur), g_dbgStaminaCurrent.load());
			BetterCheats::UI::FormatLiveValue(staMax, sizeof(staMax), g_dbgStaminaMax.load());
			char line[220];
			snprintf(line, sizeof(line),
				"  live: MaxWalkSpeed %s cm/s   horiz speed %s cm/s   JumpZVelocity %s   stamina %s / %s",
				walk, horiz, jumpZ, staCur, staMax);
			imgui->TextDisabled(line);
		}

		RenderTuneGroups(imgui);

		imgui->Spacing();
		if (imgui->SmallButton("Reset all movement attributes"))
			ResetAllAttrs();

		imgui->Spacing();
		imgui->Separator();
		imgui->TextDisabled("'My Player Enhancements' preset modelled on the pak mod of the same name that Update 2 broke.");
	}
}
