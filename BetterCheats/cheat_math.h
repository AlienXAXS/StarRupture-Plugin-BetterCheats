#pragma once

#include <cmath>

// Small numeric helpers shared by every feature module that checks whether a
// slider/attribute has been touched, or clamps a value pulled out of an
// atomic (stale config, a bad edit, a torn read) before it reaches the game.
namespace BetterCheats
{
	// A value counts as "active" (touched, non-default) once it differs from
	// its resting value by more than this. One definition shared by every
	// "differs from default" check, so a row can't drift active/inactive
	// depending on which file's own copy of the epsilon it used.
	constexpr float kActiveEpsilon = 0.0001f;

	inline bool DiffersFromDefault(float value, float baseline)
	{
		return std::fabs(value - baseline) > kActiveEpsilon;
	}

	// NaN-guards `raw` back to `fallback`, then clamps to [minValue, maxValue].
	inline float SafeValue(float raw, float fallback, float minValue, float maxValue)
	{
		float v = raw;
		if (!(v == v))    v = fallback;   // NaN
		if (v < minValue) v = minValue;
		if (v > maxValue) v = maxValue;
		return v;
	}
}
