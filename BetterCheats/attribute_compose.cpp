#include "attribute_compose.h"
#include "session_config.h"

#include <algorithm>

namespace BetterCheats
{
	float ComposedAttribute::Compute(float gameValue, float baseValue, float amount, Mode mode, float minFinal, float maxFinal)
	{
		float final = 0.0f;
		switch (mode)
		{
		case Mode::Multiply:
			// One more mod stacked on the base, not a multiplier over whatever the
			// game already aggregated -- so a x2 slider with +15% attachments on top
			// lands on 2.15, not 2.30. Floored at 0 before the row's own min/max
			// clamp below: a reduction row (Recoil, Spread, costs...) can otherwise
			// go negative once the game's own mods have already cut gameValue below
			// baseValue and amount is small.
			final = gameValue + baseValue * (amount - 1.0f);
			final = (std::max)(final, 0.0f);
			break;
		case Mode::Add:      final = gameValue + amount; break;
		case Mode::Absolute: final = amount;             break;
		}
		return std::clamp(final, minFinal, maxFinal);
	}

	void ComposedAttribute::Apply(const void* owner, float& value, const float* baseValue, float amount,
	                               Mode mode, float minFinal, float maxFinal)
	{
		if (!m_active || m_owner != owner)
		{
			// First activation, or the owner changed under us -- the only
			// trustworthy source for "what the game has" is what it holds right now.
			m_game   = value;
			m_owner  = owner;
			m_active = true;
		}
		else if (value != m_written)
		{
			// Nothing here wrote this value, so the game re-aggregated (attachment or
			// LEM changed, weapon swapped). Recapture -- never from a value we wrote,
			// or a Multiply would compound on itself.
			m_game = value;
		}

		// No real base to read (see the header comment) -- compose onto m_game
		// itself, which reduces Compute's formula back to the classic
		// gameValue * amount.
		const float base  = baseValue ? *baseValue : m_game;
		const float final = Compute(m_game, base, amount, mode, minFinal, maxFinal);

		if (value != final)
			value = final;
		m_written = final;
	}

	void ComposedAttribute::Release(const void* owner, float& value)
	{
		if (m_active && m_owner == owner && value == m_written)
			value = m_game;
		m_active = false;
	}

	void ComposedAttribute::Forget()
	{
		m_active = false;
	}

	void RestoreIfStale(const std::string& keyPrefix, float& value)
	{
		if (!SessionConfig::Get(keyPrefix + ".stored", false))
			return;

		const float storedWritten = SessionConfig::Get(keyPrefix + ".written", 0.0f);
		if (value != storedWritten)
			return;   // not stale -- the game (or nothing) has touched it since

		value = SessionConfig::Get(keyPrefix + ".game", value);
	}

	void SaveComposeState(const std::string& keyPrefix, float game, float written)
	{
		SessionConfig::Set(keyPrefix + ".game", game);
		SessionConfig::Set(keyPrefix + ".written", written);
		SessionConfig::Set(keyPrefix + ".stored", true);
	}

	void ClearComposeState(const std::string& keyPrefix)
	{
		SessionConfig::Set(keyPrefix + ".stored", false);
	}

	ComposeStep ApplyComposedRow(ComposedAttribute& composed, const void* owner,
		float& value, const std::string& key, bool active, float amount,
		ComposedAttribute::Mode mode, float minValue, float maxValue)
	{
		const bool  wasActive     = composed.IsActive();
		const float previousWrite = composed.GetWritten();
		if (!wasActive)
			RestoreIfStale(key, value);
		const float gameBefore = value;

		if (active)
			// None of this helper's callers (weapon data-asset fields, grenade
			// data-asset/global fields, inventory stack size) sit behind a real
			// FGameplayAttributeData -- see the row-by-row audit in
			// player_weapons.cpp/player_inventory.cpp. nullptr composes Multiply
			// rows onto `value` itself, identical to the pre-fix behaviour.
			composed.Apply(owner, value, nullptr, amount, mode, minValue, maxValue);
		else
			composed.Release(owner, value);

		if (active && (!wasActive || gameBefore != previousWrite))
			SaveComposeState(key, composed.GetGame(), composed.GetWritten());
		else if (!active)
			ClearComposeState(key);

		const float expected = active ? (wasActive ? previousWrite : gameBefore) : gameBefore;
		return { gameBefore, expected };
	}
}
