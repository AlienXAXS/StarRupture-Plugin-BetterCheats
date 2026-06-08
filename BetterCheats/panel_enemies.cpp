#include "panel_enemies.h"

namespace BetterCheats::Panels
{
	void RenderEnemies(IModLoaderImGui* imgui)
	{
		static bool disableAI      = false;
		static bool oneHitKill     = false;
		static bool frozenEnemies  = false;

		imgui->SeparatorText("Behaviour");
		imgui->Checkbox("Disable AI", &disableAI);
		imgui->Checkbox("Freeze Enemies", &frozenEnemies);

		imgui->Spacing();
		imgui->SeparatorText("Combat");
		imgui->Checkbox("One-Hit Kill", &oneHitKill);
	}
}
