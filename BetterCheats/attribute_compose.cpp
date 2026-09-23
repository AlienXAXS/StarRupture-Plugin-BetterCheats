#include "attribute_compose.h"
#include "session_config.h"

#include <algorithm>

namespace BetterCheats
{
	float ComposedAttribute::Compute(float gameValue, float amount, Mode mode, float minFinal, float maxFinal)
	{
		float final = 0.0f;
		switch (mode)
		{
		case Mode::Multiply: final = gameValue * amount; break;
		case Mode::Add:      final = gameValue + amount; break;
		case Mode::Absolute: final = amount;             break;
		}
		return std::clamp(final, minFinal, maxFinal);
	}

	void ComposedAttribute::Apply(const void* owner, float& value, float amount,
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

		const float final = Compute(m_game, amount, mode, minFinal, maxFinal);

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
			composed.Apply(owner, value, amount, mode, minValue, maxValue);
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
