#pragma once

#include "Chimera_classes.hpp"

#include <string>

// Shared by player_weapons.cpp and player_movement.cpp: neither file can set a
// GAS attribute's BaseValue (no reflected setter -- see the weapon/movement
// audits), so a cheat slider has to observe whatever the ability system
// aggregated (weapon attachments, LEMs) and layer its own change on top of
// CurrentValue instead of stomping it. Operates on a plain `float&` rather
// than `FGameplayAttributeData&` -- CurrentValue is the only member either
// file ever touches -- so the same class also composes onto a plain UObject
// property (e.g. a weapon data asset's BaseMagazine.Value) that has no GAS
// attribute wrapping it at all.
namespace BetterCheats
{
	class ComposedAttribute
	{
	public:
		enum class Mode { Multiply, Add, Absolute };

		// Pure formula: what Apply() would write, given `gameValue` as the game's
		// own aggregate and `baseValue` as the attribute's un-modded BaseValue.
		// Multiply composes the slider onto BaseValue alone -- final = gameValue +
		// baseValue * (amount - 1) -- so it acts as one more mod stacked on the
		// base rather than multiplying attachment/LEM bonuses too; passing
		// `gameValue` itself as `baseValue` collapses this back to the classic
		// gameValue * amount (see Apply's `baseValue` parameter). Add/Absolute
		// ignore `baseValue`. Exposed so the "Show live values" readout can
		// compute the same "expected" figure Apply just used without duplicating
		// the switch.
		static float Compute(float gameValue, float baseValue, float amount, Mode mode, float minFinal, float maxFinal);

		// `owner` identifies the instance `value` lives on, so a respawn (or, for a
		// per-weapon-type field, a weapon swap the caller must detect itself -- see
		// player_weapons.cpp's g_magazineOwner) is told apart from the game
		// re-aggregating in place. `amount` is the slider/toggle value; Multiply and
		// Add treat it as relative to the game's own value, Absolute replaces it
		// outright.
		//
		// `baseValue` is the attribute's FGameplayAttributeData::BaseValue, read
		// fresh by the caller every call -- never a value this class wrote, since
		// it only ever writes CurrentValue (see the header comment). Pass nullptr
		// when the row has no real base to read (a plain CDO/UObject field with no
		// FGameplayAttributeData behind it, e.g. grenade charge cost or the
		// grenade-global fields) -- Multiply then composes onto `value` itself,
		// identical to the pre-fix gameValue * amount. Ignored for Add/Absolute.
		void Apply(const void* owner, float& value, const float* baseValue, float amount, Mode mode,
		           float minFinal, float maxFinal);

		// Hands the value back to the game if our last write is still sitting there
		// untouched. If the game already re-aggregated over it, that value is newer
		// than ours and is left alone.
		void Release(const void* owner, float& value);

		// Owner gone (respawn, world change) -- drop state, write nothing. `value`
		// may already be dangling, so Forget never touches it.
		void Forget();

		// Debug-readout accessors for the "Show live values" panels -- same
		// game-thread-only contract as Apply/Release. GetGame() is the last value
		// the game held before this composed onto it; GetWritten() is the last
		// value Apply actually wrote (compare against a fresh read to see whether
		// the game is still honouring it); IsActive() says whether either of those
		// means anything right now.
		float GetGame()    const { return m_game; }
		float GetWritten() const { return m_written; }
		bool  IsActive()   const { return m_active; }

	private:
		const void* m_owner   = nullptr;
		float       m_game    = 0.0f;   // last known game-aggregated value (attachments/LEMs included)
		float       m_written = 0.0f;   // last value we wrote, so a re-aggregation is detectable
		bool        m_active  = false;
	};

	// Survives a hot-reload that couldn't run Release() cleanly. The loader's own
	// RELOAD button runs PluginShutdown on the render thread, not the game
	// thread every other entry point assumes; if GetLocalCharacter() throws
	// there, Release() never runs and FreeLibrary proceeds anyway, leaving the
	// old DLL instance's last write sitting in `value` for the new instance to
	// mistake for a live game aggregate. These persist {game, written} to SessionConfig, keyed
	// by a caller-chosen path (e.g. "playerWeapons.compose.damage") -- one slot per
	// composed row, not per weapon/character, since the thing being restored is a
	// value on a shared instance (the attribute set, or a weapon type's data
	// asset), not session-specific state.

	// Call once per row, only when the row was inactive going into this tick
	// (right before ComposedAttribute::Apply() would treat `value` as ground
	// truth on first activation). If a stored pair exists and `value` still equals
	// the stored `written`, that's a stale leftover -- restores `value` to the
	// stored `game` in place so Apply() captures the pre-reload aggregate instead.
	void RestoreIfStale(const std::string& keyPrefix, float& value);

	// Call after Apply(), only when this tick actually recaptured (first
	// activation, or the game re-aggregated) -- never every frame.
	void SaveComposeState(const std::string& keyPrefix, float game, float written);

	// Call after Release() (or when persistence should stop tracking this slot).
	void ClearComposeState(const std::string& keyPrefix);

	// The capture/write/restore/persist dance shared by every composed row
	// that isn't a GAS attribute with its own extra bookkeeping (buffed/base
	// readouts, a One-Hit-Kill-style override) interleaved into it -- weapon
	// CDO fields (Magazine Size, Grenade Charge Cost in player_weapons.cpp)
	// and per-item-type fields (Inventory's stack size in
	// player_inventory.cpp) all follow this exact sequence: capture the game
	// value before touching it (recovering from a stale leftover write via
	// RestoreIfStale first), Apply or Release depending on `active`, persist
	// only on a genuine recapture, and hand back what the row's "expected"
	// readout should show. Kept as free functions rather than methods on
	// ComposedAttribute itself, since RestoreIfStale/SaveComposeState/
	// ClearComposeState are free functions with their own SessionConfig
	// concerns that ComposedAttribute doesn't otherwise know about.
	struct ComposeStep { float game; float expected; };

	ComposeStep ApplyComposedRow(ComposedAttribute& composed, const void* owner,
		float& value, const std::string& key, bool active, float amount,
		ComposedAttribute::Mode mode, float minValue, float maxValue);
}
