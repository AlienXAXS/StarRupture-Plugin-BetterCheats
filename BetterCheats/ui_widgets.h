#pragma once

#include "plugin_interface.h"
#include "attribute_compose.h"
#include "preset_store.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstring>

// Small custom widgets. ResetButton draws a Material Icons glyph (the loader
// embeds MaterialIcons-Regular.ttf and loads glyphs on demand -- see
// MaterialIcons.md, "Plugins need no setup to use these") through the
// ImDrawList API rather than a real Button widget, so its color can react to
// hover/active within the same frame. A real Button widget can't: it draws its
// label with whatever Col_Text is on the style stack at submit time, and
// IsItemHovered() only becomes true *after* that submit, one frame too late to
// feed back into the color that was just drawn.
namespace BetterCheats::UI
{
	// A "replay" glyph button (U+E042, MaterialIcons.md), muted grey at
	// rest and white on hover/active. Returns true on click. `size` is the
	// square side in pixels; pass ImGui's frame height for a control that lines
	// up with a checkbox. `id` is this instance's ImGui identity -- as with any
	// widget sharing a label across instances (see the doc's ##id note), every
	// call site needs its own, which here also still doubles as the click
	// target's InvisibleButton id, same as before.
	inline bool ResetButton(IModLoaderImGui* imgui, const char* id, float size = 0.0f)
	{
		if (size <= 0.0f)
			size = imgui->GetFrameHeight ? imgui->GetFrameHeight() : 18.0f;

		float x = 0.0f, y = 0.0f;
		imgui->GetCursorScreenPos(&x, &y);

		const bool pressed = imgui->InvisibleButton(id, size, size);
		const bool hovered = imgui->IsItemHovered();
		const bool active  = imgui->IsItemActive();

		// ImGui packs colours as 0xAABBGGRR.
		const unsigned int col = (active || hovered) ? 0xFFFFFFFFu    // white while hovered/held
		                                              : 0xFFB0B0B0u;  // muted grey at rest

		PluginDrawList dl = imgui->GetWindowDrawList();
		if (!dl)
			return pressed;

		const char* glyph = "\xEE\x81\x82";   // U+E042 replay
		float textW = 0.0f, textH = 0.0f;
		imgui->CalcTextSize(glyph, &textW, &textH, false, -1.0f);
		imgui->DL_AddText(dl, x + (size - textW) * 0.5f, y + (size - textH) * 0.5f, col, glyph);

		return pressed;
	}

	// "Show live values" readout, shared by the Weapons and Movement panels.
	// `expected` is the value a row intends (the composed result while active, or
	// the game's own value while inactive); `game` is what the attribute/field
	// actually holds right now. They should always converge within a frame or two
	// of a change -- a lasting mismatch means something else keeps overriding or
	// ignoring the write.

	// 2 decimals normally; 3 once the magnitude drops under 0.1 (e.g. a near-zero
	// recoil multiplier), where 2 decimals would hide the difference that matters.
	inline void FormatLiveValue(char* out, size_t outSize, float value)
	{
		const float mag = std::fabs(value);
		snprintf(out, outSize, (mag > 0.0f && mag < 0.1f) ? "%.3f" : "%.2f", value);
	}

	// Match = within display precision: an absolute 0.005, or a relative 1e-4 for
	// values large enough that an absolute epsilon would be too tight (speeds etc.).
	inline bool LiveValuesMatch(float expected, float game)
	{
		const float diff = std::fabs(expected - game);
		if (diff <= 0.005f) return true;
		const float mag = (std::max)(std::fabs(expected), std::fabs(game));
		return mag > 0.0f && (diff / mag) <= 1e-4f;
	}

	// Same precision rule as FormatLiveValue, but always signed ("+0.15"/"-0.05")
	// -- used for the "Mods & buffs"/"Ours" tooltip lines, which read as deltas
	// rather than absolute values. Callers check LiveValuesMatch(value, 0.0f)
	// first and print "none" instead of calling this for a ~zero delta.
	inline void FormatSignedLiveValue(char* out, size_t outSize, float value)
	{
		const float mag = std::fabs(value);
		snprintf(out, outSize, (mag > 0.0f && mag < 0.1f) ? "%+.3f" : "%+.2f", value);
	}

	// "Our change" as the row's own compose formula would describe it -- not the
	// row's slider format string, since this always needs to read as a delta on
	// top of the unmodified value, e.g. "x1.50" even on a row whose slider box
	// shows "1.50x". Used only in RenderLiveValue's tooltip.
	inline void FormatChangeDesc(char* out, size_t outSize, ComposedAttribute::Mode mode, float amount)
	{
		switch (mode)
		{
		case ComposedAttribute::Mode::Multiply: snprintf(out, outSize, "x%.2f",     amount); break;
		case ComposedAttribute::Mode::Add:      snprintf(out, outSize, "%+.0f",    amount); break;
		case ComposedAttribute::Mode::Absolute: snprintf(out, outSize, "set to %.0f", amount); break;
		}
	}

	// Draws "<expected> = <game>" in blue on a match, "<expected> != <game>" in
	// red otherwise, plus a "+<tagWord>" note in accent orange when `hasBase` and
	// `base` (BaseValue, which we never write) differs from `unmodified` (the
	// game's own aggregate, LEMs/buffs/attachments included, with our own change
	// excluded) -- i.e. something external is contributing to this attribute.
	// Every row, tag or not, gets one hover tooltip over the whole group.
	//
	// `hasBase` alone (any row with a real base to read, not just Multiply
	// rows) is enough to print the "Mods & buffs" line -- unmodified - base,
	// i.e. whatever's contributing to this attribute besides us, whether or
	// not that delta is one this row's own compose formula could re-derive
	// (`ours` needs a Multiply row's baseValue * (amount - 1) scaling; "Mods
	// & buffs" needs nothing row-specific at all).
	//
	// `hasOurs` is true only for a Multiply row with a real base -- the one
	// case where `ours` is ALSO a meaningful, addable number, so that row's
	// tooltip spells out the full breakdown, in the same units the slider
	// shows, so it visibly adds up:
	//
	//   <baseLabel>: <v>                                   -- e.g. "Base (no attachments): 1.00"
	//   Mods & buffs: +<v> / none                          -- unmodified - base
	//   Ours: +<v> / none                                  -- baseValue * (amount - 1)
	//   Result: <expected> [(floor/clamp applied)]         -- noted when the sum above isn't what got written
	//   Live in game: <game>
	//
	// A hasBase row without hasOurs (Add/Absolute) still gets the Mods &
	// buffs line, just without an "Ours" to add to it:
	//
	//   <baseLabel>: <v>
	//   Mods & buffs: +<v> / none
	//   Our change: x1.50 / +15 / set to 2 / none     -- "none" while inactive
	//   Result: <expected>
	//   Live in game: <game>
	//
	// No real base at all (!hasBase) falls back to the oldest, shortest chain:
	//
	//   Unmodified: <v>
	//   Our change: x1.50 / +15 / set to 2 / none     -- "none" while inactive
	//   Result: <expected>
	//   Live in game: <game>
	//
	// `estimate` (a weapon/item not currently equipped/active, so there is no
	// live game read to compare against) swaps the blue/red match colour for
	// the theme's own disabled-text colour (GetStyleColorVec4, not a
	// hand-picked RGB -- see ImGuiCol_TextDisabled below) and drops the
	// trailing "Live in game" tooltip line for a one-line "estimated" note.
	// Every other line (Base/Mods & buffs/Ours/Result) is unchanged: callers
	// feed `base`/`unmodified`/`ours` from the last mods this row saw while
	// it WAS live, so the breakdown still adds up, just without a fresh game
	// read to confirm it against.
	//
	// ASCII "!=" and "->" rather than U+2260/U+2192 -- the loader's baked font
	// ranges (imgui_backend.cpp) stop at Latin-1/Cyrillic plus a CJK merge that
	// doesn't cover Mathematical Operators or Arrows either, so the real glyphs
	// would render as "?".
	inline void RenderLiveValue(IModLoaderImGui* imgui, float expected, float game,
	                             bool active, bool hasBase, float base, float unmodified,
	                             const char* changeDesc, const char* tagWord, const char* baseLabel,
	                             bool hasOurs = false, float ours = 0.0f, bool estimate = false)
	{
		char e[32], line[80];
		FormatLiveValue(e, sizeof(e), expected);

		const bool showTag = hasBase && !LiveValuesMatch(base, unmodified);

		imgui->BeginGroup();
		if (estimate)
		{
			// ImGuiCol_TextDisabled == 1 in every Dear ImGui version this loader
			// has shipped -- the enum's first two entries (Text, TextDisabled)
			// have never been reordered. Reusing the theme's own disabled-text
			// colour here (rather than a hand-picked grey) is what keeps this
			// "not live" tint matching whatever palette/theme is active.
			float dr = 0.0f, dg = 0.0f, db = 0.0f, da = 1.0f;
			imgui->GetStyleColorVec4(1, &dr, &dg, &db, &da);
			imgui->TextColored(dr, dg, db, da, e);
		}
		else
		{
			char g[32];
			FormatLiveValue(g, sizeof(g), game);
			const bool match = LiveValuesMatch(expected, game);
			snprintf(line, sizeof(line), "%s %s %s", e, match ? "=" : "!=", g);
			if (match) imgui->TextColored(0.310f, 0.639f, 0.878f, 1.0f, line);   // #4FA3E0
			else       imgui->TextColored(0.878f, 0.282f, 0.282f, 1.0f, line);   // #E04848
		}
		if (showTag)
		{
			char tag[16];
			snprintf(tag, sizeof(tag), "+%s", tagWord);
			imgui->SameLine(0.0f, 8.0f);
			imgui->TextColored(0.910f, 0.639f, 0.239f, 1.0f, tag);   // accent orange, ~#E8A33D
		}
		imgui->EndGroup();

		if (imgui->IsItemHovered())
		{
			char expectedStr[32], gameStr[32], tip[384];
			FormatLiveValue(expectedStr, sizeof(expectedStr), expected);
			FormatLiveValue(gameStr, sizeof(gameStr), game);

			int n = 0;
			if (hasBase)
			{
				char baseStr[32];
				FormatLiveValue(baseStr, sizeof(baseStr), base);
				n += snprintf(tip + n, sizeof(tip) - n, "%s: %s", baseLabel, baseStr);

				// Whatever's contributing to this attribute besides us -- true for
				// any row with a real base, not just the Multiply/hasOurs ones.
				// modsAndBuffs + ours + base == unmodified + ours -- the row's own
				// pre-floor/clamp sum (see attribute_compose.cpp's Compute) -- so a
				// mismatch against `expected` (post floor/clamp) is exactly the case
				// worth calling out once `ours` is in the picture (hasOurs below).
				const float modsAndBuffs = unmodified - base;
				char modsStr[32];
				if (LiveValuesMatch(modsAndBuffs, 0.0f)) snprintf(modsStr, sizeof(modsStr), "none");
				else                                     FormatSignedLiveValue(modsStr, sizeof(modsStr), modsAndBuffs);
				n += snprintf(tip + n, sizeof(tip) - n, "\nMods & buffs: %s", modsStr);
			}
			else
			{
				char unmodStr[32];
				FormatLiveValue(unmodStr, sizeof(unmodStr), unmodified);
				n += snprintf(tip + n, sizeof(tip) - n, "Unmodified: %s", unmodStr);
			}

			if (hasOurs)
			{
				char oursStr[32];
				if (LiveValuesMatch(ours, 0.0f)) snprintf(oursStr, sizeof(oursStr), "none");
				else                             FormatSignedLiveValue(oursStr, sizeof(oursStr), ours);
				n += snprintf(tip + n, sizeof(tip) - n, "\nOurs: %s", oursStr);

				const float rawSum = unmodified + ours;
				const bool  adjusted = !LiveValuesMatch(rawSum, expected);
				n += snprintf(tip + n, sizeof(tip) - n, "\nResult: %s%s", expectedStr,
					adjusted ? " (floor/clamp applied)" : "");
			}
			else
			{
				n += snprintf(tip + n, sizeof(tip) - n, "\nOur change: %s", active ? changeDesc : "none");
				n += snprintf(tip + n, sizeof(tip) - n, "\nResult: %s", expectedStr);
			}

			if (estimate)
				n += snprintf(tip + n, sizeof(tip) - n, "\nEstimated -- equip this weapon to see live values.");
			else
				n += snprintf(tip + n, sizeof(tip) - n, "\nLive in game: %s", gameStr);
			imgui->SetTooltip(tip);
		}
	}

	// Widest of `count` labels (fetched one at a time through `getLabel`, so
	// callers with labels sitting inside a struct array don't need to copy them
	// into a flat `const char**` first), plus one frame-height of padding per
	// nesting level -- shared by every "Show live values" column so its readout
	// starts at the same X on every row regardless of that row's own label
	// length. `extraLevels` covers a panel's own arrow/indent room on top of the
	// base row (Movement's disclosure-arrow rows want more headroom than
	// Weapons' flat list, which passes 0).
	template <typename GetLabel>
	inline float PrescanLabelWidth(IModLoaderImGui* imgui, int count, GetLabel getLabel, int extraLevels = 0)
	{
		float widest = 0.0f;
		for (int i = 0; i < count; ++i)
		{
			float w = 0.0f, h = 0.0f;
			imgui->CalcTextSize(getLabel(i), &w, &h, false, -1.0f);
			if (w > widest) widest = w;
		}
		return widest + imgui->GetFrameHeight() * (1.0f + static_cast<float>(extraLevels));
	}

	// ImGuiTableColumnFlags_WidthFixed -- pass as a column's own flags in
	// TableSetupColumn to give it a pixel width instead of a stretch weight,
	// even inside a table whose overall sizing policy is stretch-proportional
	// (mixing a fixed label/readout column with stretch value/reset columns is
	// standard ImGui table usage).
	constexpr int kColumnWidthFixed = 1 << 4;

	// Column-0 (Attribute) width wide enough for the longest label AND the
	// live-values readout next to it -- "<label>   <expected> = <game> +tag" --
	// so BuildRow's readout can never clip against column 1. `labelReserve` is
	// PrescanLabelWidth's own result (where the readout starts); this adds room
	// for a representative worst-case readout plus the tag word (RenderLiveValue
	// keeps the tag to just the word -- see its own comment for why).
	inline float GetReadoutColumnWidth(IModLoaderImGui* imgui, float labelReserve)
	{
		float readoutW = 0.0f, tagW = 0.0f, h = 0.0f;
		imgui->CalcTextSize("-888.88 = -888.88", &readoutW, &h, false, -1.0f);
		imgui->CalcTextSize(" +mods", &tagW, &h, false, -1.0f);   // same width as " +buff"
		return labelReserve + readoutW + tagW + imgui->GetFrameHeight() * 0.5f;
	}

	// 240px at the loader's base font size (kBasePx = 15.0f in
	// imgui_backend.cpp). GetFontSize() is already FontScale-adjusted, so this
	// scales the cap the same way the loader scales everything else.
	inline float GetSliderWidthCap(IModLoaderImGui* imgui)
	{
		return 240.0f * (imgui->GetFontSize() / 15.0f);
	}

	// One full cheat-slider row: label (+ optional disclosure arrow/indent), the
	// "Show live values" readout (+ "+buff"/"+mods" tag when hasBase), a slider
	// joined to a typed number box (width capped per GetSliderWidthCap unless
	// sliderWidthCap overrides it), and the drawn reset button. Must run inside
	// an open 3-column table (Attribute / Value / reset), after the caller's own
	// TableNextRow and PushID -- ID scheme and persistence (SessionConfig::Set)
	// stay with the caller, since weapons and movement key theirs differently
	// and only the caller knows which config entry a row maps to.
	struct RowSpec
	{
		const char* label;
		const char* tooltip = nullptr;
		float*      value;
		float       minValue, maxValue, step;
		const char* format;
		float       resetValue;
		bool        active         = true;
		bool        disableSlider  = false;
		float       sliderWidthCap = 0.0f;   // 0 = GetSliderWidthCap(imgui)

		// Disclosure arrow / indent (Movement only -- weapons leaves these at
		// their defaults, which draws a flat, unindented row).
		int         depth       = 0;
		bool*       openFlag    = nullptr;   // non-null draws a disclosure arrow
		bool        padForArrow = false;     // indent non-arrow rows to match sibling arrow rows

		// Live-or-estimated readout -- see RenderLiveValue for the full contract.
		bool        showLive    = false;
		bool        estimate    = false;     // no live game read available -- dim/disabled colour, no "Live in game" line
		float       expected = 0.0f, game = 0.0f;
		bool        composing   = false;     // are we actually overriding this row right now (tooltip's "Our change")
		bool        hasBase     = false;     // GAS rows only: enables the +tag and the tooltip's Base line
		float       base = 0.0f, unmodified = 0.0f;
		bool        hasOurs     = false;     // Multiply rows with a real base: full Base/Mods & buffs/Ours/Result breakdown
		float       ours        = 0.0f;      // baseValue * (amount - 1) -- see RenderLiveValue
		const char* changeDesc  = nullptr;
		const char* tagWord     = "buff";
		const char* baseLabel   = "Base (no LEMs/buffs)";
		float       labelReserve = 0.0f;

		const char* resetTooltip = "Reset to the game default (turns this row off).";
	};

	struct RowResult
	{
		bool changed      = false;   // slider, number box, or reset
		bool resetClicked = false;   // reset specifically -- some callers need to react only to this
	};

	inline RowResult BuildRow(IModLoaderImGui* imgui, const RowSpec& spec)
	{
		RowResult result;
		const float frameH = imgui->GetFrameHeight();

		imgui->TableSetColumnIndex(0);

		if (spec.depth > 0)
			imgui->Indent(frameH * static_cast<float>(spec.depth));

		if (spec.openFlag)
		{
			if (imgui->ArrowButton("##expand", *spec.openFlag ? 3 : 1))   // 3 = Down, 1 = Right
				*spec.openFlag = !*spec.openFlag;
			imgui->SameLine(0.0f, -1.0f);
		}
		else if (spec.padForArrow)
		{
			imgui->Indent(frameH);   // keep labels aligned with the arrowed rows
		}

		if (spec.active) imgui->Text(spec.label);
		else              imgui->TextDisabled(spec.label);
		if (spec.tooltip && imgui->IsItemHovered())
			imgui->SetTooltip(spec.tooltip);

		if (spec.showLive)
		{
			imgui->SameLine(spec.labelReserve, 0.0f);
			RenderLiveValue(imgui, spec.expected, spec.game, spec.composing, spec.hasBase,
				spec.base, spec.unmodified, spec.changeDesc, spec.tagWord, spec.baseLabel,
				spec.hasOurs, spec.ours, spec.estimate);
		}

		if (!spec.openFlag && spec.padForArrow)
			imgui->Unindent(frameH);
		if (spec.depth > 0)
			imgui->Unindent(frameH * static_cast<float>(spec.depth));

		// Slider for feel, typed box on the right for precision. Zero spacing
		// between them so they read as one joined control.
		imgui->TableSetColumnIndex(1);
		if (spec.disableSlider)
			imgui->BeginDisabled(true);

		float availX = 0.0f, availY = 0.0f;
		imgui->GetContentRegionAvail(&availX, &availY);

		// InputFloat draws [text][-][+]. Measure the widest value this row can
		// actually show rather than guessing a pixel count -- the loader's
		// FontScale is user-configurable, so anything hardcoded clips at some scale.
		char widest[32];
		snprintf(widest, sizeof(widest), spec.format, (spec.maxValue >= 100.0f) ? -888.0f : -88.88f);
		float textW = 0.0f, textH = 0.0f;
		imgui->CalcTextSize(widest, &textW, &textH, false, -1.0f);

		const float numBoxW = textW + (frameH * 2.0f) + (frameH * 0.9f); // text + 2 steppers + padding
		const float cap     = (spec.sliderWidthCap > 0.0f) ? spec.sliderWidthCap : GetSliderWidthCap(imgui);
		float sliderW = (availX > numBoxW + frameH * 2.0f) ? (availX - numBoxW) : (availX * 0.55f);
		if (sliderW > cap) sliderW = cap;

		bool changed = false;
		imgui->SetNextItemWidth(sliderW);
		if (imgui->SliderFloat("##slider", spec.value, spec.minValue, spec.maxValue, spec.format))
			changed = true;

		imgui->SameLine(0.0f, 0.0f);
		imgui->SetNextItemWidth(numBoxW);
		if (imgui->InputFloat("##num", spec.value, spec.step, spec.step * 10.0f, spec.format))
			changed = true;

		if (changed)
		{
			if (*spec.value < spec.minValue) *spec.value = spec.minValue;
			if (*spec.value > spec.maxValue) *spec.value = spec.maxValue;
			result.changed = true;
		}
		if (spec.disableSlider)
			imgui->EndDisabled();

		imgui->TableSetColumnIndex(2);
		if (ResetButton(imgui, "##reset"))
		{
			*spec.value = spec.resetValue;
			result.changed      = true;
			result.resetClicked = true;
		}
		if (imgui->IsItemHovered())
			imgui->SetTooltip(spec.resetTooltip);

		return result;
	}

	// ---------------------------------------------------------------------
	// Saved presets row -- a dropdown of a PresetStore group's saved
	// presets plus Save/Rename/Delete, alongside (never replacing) a panel's
	// own built-in preset buttons. Save never prompts: it computes a
	// suggested name itself and selects the result, staying one click.
	//
	// Ported from BetterDrone's RenderSavedPresetsRow (drone_ui.cpp), which
	// used plain C function pointers for its four callbacks -- generalized
	// here to a template taking arbitrary callables, since a caller like the
	// Weapons tab needs to capture per-instance context (which weapon
	// type's own built-in preset list applies) that a bare function pointer
	// can't carry.
	// ---------------------------------------------------------------------

	// Persists across frames per preset group: which saved preset is
	// selected, plus in-progress rename/delete/error UI state. One instance
	// per group the caller manages (Movement's own, one per weapon tab).
	struct SavedPresetRowState
	{
		char selected[BetterCheats::PresetStore::kMaxNameLen] = {};
		bool renaming = false;
		char renameBuf[BetterCheats::PresetStore::kMaxNameLen] = {};
		char errorMsg[96] = {};
	};

	template <typename GetLiveFields, typename ApplyFields, typename IsBuiltinName, typename ComputeSuggestedBase>
	inline void RenderSavedPresetsRow(IModLoaderImGui* imgui, const char* idScope, const char* group,
		BetterCheats::PresetStore::Field* fields, int fieldCount,
		GetLiveFields getLive, ApplyFields apply, IsBuiltinName isBuiltin, ComputeSuggestedBase computeSuggest,
		SavedPresetRowState& state)
	{
		imgui->PushIDStr(idScope);

		char names[16][BetterCheats::PresetStore::kMaxNameLen];
		const int count = BetterCheats::PresetStore::ListNames(group, names, 16);

		bool selectedStillValid = false;
		for (int i = 0; i < count; ++i)
			if (strcmp(names[i], state.selected) == 0)
				selectedStillValid = true;
		if (!selectedStillValid)
			state.selected[0] = '\0';

		imgui->AlignTextToFramePadding();
		imgui->Text("Saved Presets");
		imgui->SameLine(0.0f, -1.0f);

		imgui->SetNextItemWidth(220.0f);
		const char* preview = state.selected[0] ? state.selected : "(none saved)";
		if (imgui->BeginCombo("##saved", preview))
		{
			for (int i = 0; i < count; ++i)
			{
				const bool isSelected = (strcmp(names[i], state.selected) == 0);
				if (imgui->Selectable(names[i], isSelected))
				{
					snprintf(state.selected, sizeof(state.selected), "%s", names[i]);
					getLive(fields); // seed so a key missing from this preset stays unchanged
					if (BetterCheats::PresetStore::Load(group, state.selected, fields, fieldCount))
						apply(fields, fieldCount);
				}
			}
			imgui->EndCombo();
		}

		imgui->SameLine(0.0f, -1.0f);
		if (imgui->SmallButton("Save"))
		{
			char base[BetterCheats::PresetStore::kMaxNameLen];
			computeSuggest(base, sizeof(base));
			char suggested[BetterCheats::PresetStore::kMaxNameLen];
			BetterCheats::PresetStore::SuggestName(group, base, suggested, sizeof(suggested));

			getLive(fields);
			if (BetterCheats::PresetStore::Save(group, suggested, fields, fieldCount))
			{
				snprintf(state.selected, sizeof(state.selected), "%s", suggested);
				state.errorMsg[0] = '\0';
			}
			else
			{
				snprintf(state.errorMsg, sizeof(state.errorMsg), "Could not save -- presets file unavailable.");
			}
		}

		const bool hasSelection = state.selected[0] != '\0';

		imgui->SameLine(0.0f, -1.0f);
		imgui->BeginDisabled(!hasSelection);
		if (imgui->SmallButton("Rename"))
		{
			state.renaming = true;
			snprintf(state.renameBuf, sizeof(state.renameBuf), "%s", state.selected);
			state.errorMsg[0] = '\0';
		}
		imgui->EndDisabled();

		char deletePopupId[80];
		snprintf(deletePopupId, sizeof(deletePopupId), "Delete preset?##%s", group);

		imgui->SameLine(0.0f, -1.0f);
		imgui->BeginDisabled(!hasSelection);
		if (imgui->SmallButton("Delete"))
			imgui->OpenPopup(deletePopupId, 0);
		imgui->EndDisabled();

		if (imgui->BeginPopupModal(deletePopupId, nullptr, 0))
		{
			imgui->Text("Delete this saved preset?");
			imgui->TextDisabled(state.selected);
			imgui->Spacing();
			if (imgui->SmallButton("Delete##confirm"))
			{
				BetterCheats::PresetStore::Delete(group, state.selected);
				state.selected[0] = '\0';
				imgui->CloseCurrentPopup();
			}
			imgui->SameLine(0.0f, -1.0f);
			if (imgui->SmallButton("Cancel##delete"))
				imgui->CloseCurrentPopup();
			imgui->EndPopup();
		}

		if (state.renaming)
		{
			imgui->SetNextItemWidth(200.0f);
			imgui->InputText("##rename", state.renameBuf, sizeof(state.renameBuf));

			imgui->SameLine(0.0f, -1.0f);
			if (imgui->SmallButton("OK##rename"))
			{
				if (isBuiltin(state.renameBuf))
				{
					snprintf(state.errorMsg, sizeof(state.errorMsg), "\"%s\" is a built-in preset name.", state.renameBuf);
				}
				else if (BetterCheats::PresetStore::Rename(group, state.selected, state.renameBuf))
				{
					snprintf(state.selected, sizeof(state.selected), "%s", state.renameBuf);
					state.renaming = false;
					state.errorMsg[0] = '\0';
				}
				else
				{
					snprintf(state.errorMsg, sizeof(state.errorMsg), "\"%s\" is already used.", state.renameBuf);
				}
			}

			imgui->SameLine(0.0f, -1.0f);
			if (imgui->SmallButton("Cancel##rename"))
			{
				state.renaming = false;
				state.errorMsg[0] = '\0';
			}
		}

		if (state.errorMsg[0])
			imgui->TextColored(1.0f, 0.4f, 0.4f, 1.0f, state.errorMsg);

		imgui->PopID();
	}
}
