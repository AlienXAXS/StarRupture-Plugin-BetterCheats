#include "player_tools.h"
#include "plugin_helpers.h"
#include "aob_patterns.h"

namespace BetterCheats::Panels::Tools
{
	namespace
	{
		// ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingStretchProp
		constexpr int kToolsTableFlags = (1 << 6) | (1 << 9) | (3 << 13);
		// ImGuiTableColumnFlags_WidthFixed
		constexpr int kColumnWidthFixed = 1 << 4;

		using GetMiningDamageFn = float(__fastcall*)(void* self, bool isHittingWeakSpot);

		GetMiningDamageFn g_original = nullptr;
		HookHandle        g_hook     = nullptr;
		bool              g_overload = false;

		float __fastcall Detour_GetMiningDamage(void* self, bool isHittingWeakSpot)
		{
			if (g_overload)
				return 10000.0f;

			return g_original(self, isHittingWeakSpot);
		}
	}

	void Initialize()
	{
		IPluginScanner* scanner = GetScanner();
		if (!scanner)
		{
			LOG_WARN("Tools: scanner unavailable, mining laser hook skipped");
			return;
		}

		uintptr_t addr = scanner->FindPatternInMainModule(AOB::GetMiningDamage);
		if (!addr)
		{
			LOG_WARN("Tools: GetMiningDamage pattern not found");
			return;
		}

		IPluginHookUtils* hooks = GetHooks() ? GetHooks()->Hooks : nullptr;
		if (!hooks)
		{
			LOG_WARN("Tools: hook utils unavailable");
			return;
		}

		g_hook = hooks->Install(addr, reinterpret_cast<void*>(&Detour_GetMiningDamage),
		                        reinterpret_cast<void**>(&g_original));

		if (!g_hook)
			LOG_WARN("Tools: failed to install GetMiningDamage hook");
		else
			LOG_INFO("Tools: GetMiningDamage hook installed");
	}

	void Shutdown()
	{
		IPluginHookUtils* hooks = GetHooks() ? GetHooks()->Hooks : nullptr;
		if (hooks && g_hook)
		{
			hooks->Remove(g_hook);
			g_hook     = nullptr;
			g_original = nullptr;
		}
	}

	void RenderImGui(IModLoaderImGui* imgui)
	{
		imgui->SeparatorText("Mining");

		if (imgui->BeginTable("##mining_table", 2, kToolsTableFlags))
		{
			imgui->TableSetupColumn("Option", kColumnWidthFixed, 420.0f);
			imgui->TableSetupColumn("Enabled", 0, 0.0f);

			imgui->TableNextRow(0, 0.0f);

			imgui->TableSetColumnIndex(0);
			imgui->Text("Overload Handheld Mining Laser");

			imgui->TableSetColumnIndex(1);
			imgui->Checkbox("##overload_mining", &g_overload);

			imgui->EndTable();
		}
	}
}
