#include "panel_player.h"
#include "player_skills.h"

namespace BetterCheats::Panels
{
	void RenderPlayer_Self(IModLoaderImGui* imgui)
	{
		static bool  godMode      = false;
		static bool  infiniteStam = false;
		static float healthPct    = 1.0f;

		imgui->SeparatorText("Health & Status");
		imgui->Checkbox("God Mode", &godMode);
		imgui->Checkbox("Infinite Stamina", &infiniteStam);
		imgui->SliderFloat("Health %", &healthPct, 0.0f, 1.0f, "%.0f%%");
	}

	void RenderPlayer_Weapon(IModLoaderImGui* imgui)
	{
		static bool infiniteAmmo    = false;
		static bool noReload        = false;
		static bool instantCooldown = false;

		imgui->SeparatorText("Weapons");
		imgui->Checkbox("Infinite Ammo", &infiniteAmmo);
		imgui->Checkbox("No Reload", &noReload);
		imgui->Checkbox("Instant Ability Cooldown", &instantCooldown);
	}

	void RenderPlayer_Movement(IModLoaderImGui* imgui)
	{
		static bool  noClip     = false;
		static float speedMul   = 1.0f;

		imgui->SeparatorText("Movement");
		imgui->Checkbox("No-Clip", &noClip);
		imgui->SliderFloat("Speed Multiplier", &speedMul, 0.1f, 10.0f, "%.1fx");
	}

	void RenderPlayer_Teleport(IModLoaderImGui* imgui)
	{
		imgui->SeparatorText("Teleport");
		imgui->TextDisabled("No teleport options yet.");
	}

	void RenderPlayer_Building(IModLoaderImGui* imgui)
	{
		imgui->SeparatorText("Building");
		imgui->TextDisabled("No building options yet.");
	}

	void RenderPlayer_Skills(IModLoaderImGui* imgui)
	{
		Skills::RenderImGui(imgui);
	}
}
