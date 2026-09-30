#include "item_registry.h"
#include "plugin_helpers.h"

#include "AssetRegistry_parameters.hpp"

#include <string>

namespace BetterCheats::ItemRegistry
{
	namespace
	{
		// Raw CoreUObject object/package lookup and loading functions -- these
		// are not UFunctions, so ProcessEvent can't reach them. The Kismet
		// soft-object-path / LoadAsset_Blocking route (FStreamableManager::
		// LoadSynchronous) reliably returns null for these off-disk Blueprint
		// packages in this build (verified: a well-formed soft path like
		// "/Game/Chimera/Items/I_AquaBar.I_AquaBar" still resolves to nullptr),
		// so resolution instead mirrors UCrUW_CheatItemsTab::
		// DebugGatherAllItems's own force-load sequence: FindPackage/LoadPackage
		// + StaticFindObject by the generated "<AssetName>_C" class name. The
		// modloader AOB-resolves these addresses at startup and exposes them
		// via IPluginEngineEvents.
		using StaticFindObjectByNameFn = SDK::UObject*  (__fastcall*)(SDK::UClass*, SDK::UObject*, const wchar_t*, bool);
		using FindPackageFn            = SDK::UPackage* (__fastcall*)(SDK::UObject*, const wchar_t*);
		using PackageFullyLoadFn       = void           (__fastcall*)(SDK::UPackage*);
		using LoadPackageFn            = SDK::UPackage* (__fastcall*)(SDK::UPackage*, const wchar_t*, uint32_t, void*, const void*);

		StaticFindObjectByNameFn g_staticFindObjectByName = nullptr;
		FindPackageFn            g_findPackage             = nullptr;
		PackageFullyLoadFn       g_packageFullyLoad        = nullptr;
		LoadPackageFn            g_loadPackage             = nullptr;
		bool                     g_engineFnsResolveTried   = false;

		bool ResolveEngineLookupFunctions()
		{
			if (g_engineFnsResolveTried)
				return g_staticFindObjectByName && g_findPackage && g_packageFullyLoad && g_loadPackage;

			g_engineFnsResolveTried = true;

			IPluginHooks* hooks = GetHooks();
			IPluginEngineEvents* engine = hooks ? hooks->Engine : nullptr;
			if (!engine)
			{
				LOG_WARN("ItemRegistry: engine events interface unavailable -- cannot resolve object/package lookup functions.");
				return false;
			}

			if (uintptr_t address = engine->GetStaticFindObjectByNameAddress())
				g_staticFindObjectByName = reinterpret_cast<StaticFindObjectByNameFn>(address);
			if (uintptr_t address = engine->GetFindPackageAddress())
				g_findPackage = reinterpret_cast<FindPackageFn>(address);
			if (uintptr_t address = engine->GetPackageFullyLoadAddress())
				g_packageFullyLoad = reinterpret_cast<PackageFullyLoadFn>(address);
			if (uintptr_t address = engine->GetLoadPackageAddress())
				g_loadPackage = reinterpret_cast<LoadPackageFn>(address);

			if (!g_staticFindObjectByName || !g_findPackage || !g_packageFullyLoad || !g_loadPackage)
			{
				LOG_WARN("ItemRegistry: failed to resolve object/package lookup functions "
					"(StaticFindObject=%p FindPackage=%p FullyLoad=%p LoadPackage=%p) -- item list will be incomplete.",
					reinterpret_cast<void*>(g_staticFindObjectByName), reinterpret_cast<void*>(g_findPackage),
					reinterpret_cast<void*>(g_packageFullyLoad), reinterpret_cast<void*>(g_loadPackage));
				return false;
			}

			return true;
		}
	}

	SDK::FTopLevelAssetPath ItemBlueprintClassPath()
	{
		SDK::FString packagePath(L"/Script/AuItems");
		SDK::FString className(L"AuItemBlueprint");
		return SDK::UKismetSystemLibrary::MakeTopLevelAssetPath(packagePath, className);
	}

	SDK::IAssetRegistry* GetAssetRegistry()
	{
		SDK::UAssetRegistryHelpers* cdo = SDK::UAssetRegistryHelpers::GetDefaultObj();
		if (!cdo) return nullptr;

		static SDK::UFunction* func = nullptr;
		if (!func)
			func = SDK::UAssetRegistryHelpers::StaticClass()->GetFunction("AssetRegistryHelpers", "GetAssetRegistry");
		if (!func)
		{
			LOG_WARN("ItemRegistry: could not resolve UAssetRegistryHelpers::GetAssetRegistry.");
			return nullptr;
		}

		SDK::Params::AssetRegistryHelpers_GetAssetRegistry parms{};
		const auto flags = func->FunctionFlags;
		func->FunctionFlags |= 0x400;
		cdo->ProcessEvent(func, &parms);
		func->FunctionFlags = flags;

		SDK::UObject* registryObject = parms.ReturnValue.GetObjectRef();
		return registryObject ? reinterpret_cast<SDK::IAssetRegistry*>(registryObject) : nullptr;
	}

	bool GetAssetsByClass(SDK::IAssetRegistry* registry, const SDK::FTopLevelAssetPath& classPath,
		SDK::TArray<SDK::FAssetData>& outAssets)
	{
		SDK::UObject* registryObject = registry ? registry->AsUObject() : nullptr;
		if (!registryObject) return false;

		static SDK::UFunction* func = nullptr;
		if (!func)
			func = SDK::IAssetRegistry::StaticClass()->GetFunction("AssetRegistry", "GetAssetsByClass");
		if (!func)
		{
			LOG_WARN("ItemRegistry: could not resolve IAssetRegistry::GetAssetsByClass.");
			return false;
		}

		SDK::Params::AssetRegistry_GetAssetsByClass parms{};
		parms.ClassPathName     = classPath;
		parms.bSearchSubClasses = true;

		const auto flags = func->FunctionFlags;
		func->FunctionFlags |= 0x400;
		registryObject->ProcessEvent(func, &parms);
		func->FunctionFlags = flags;

		outAssets = std::move(parms.OutAssetData);
		return parms.ReturnValue;
	}

	SDK::UAuItemDataBase* ResolveItemFromBlueprintAsset(const SDK::FAssetData& assetData, bool verbose)
	{
		if (!ResolveEngineLookupFunctions())
			return nullptr;

		const std::string assetName   = assetData.AssetName.ToString();
		const std::string packageName = assetData.PackageName.GetRawString();
		if (packageName.empty())
			return nullptr;

		const std::wstring packageNameW(packageName.begin(), packageName.end());
		const std::wstring generatedClassNameW = std::wstring(assetName.begin(), assetName.end()) + L"_C";

		SDK::UPackage* package = g_findPackage(nullptr, packageNameW.c_str());
		if (package)
			g_packageFullyLoad(package);
		else
			package = g_loadPackage(nullptr, packageNameW.c_str(), 0, nullptr, nullptr);

		if (!package)
		{
			if (verbose)
				LOG_DEBUG("ItemRegistry:   [%s] could not find or load package '%s'.", assetName.c_str(), packageName.c_str());
			return nullptr;
		}
		if (verbose)
			LOG_DEBUG("ItemRegistry:   [%s] package '%s' loaded ('%s')", assetName.c_str(), packageName.c_str(),
				package->GetName().c_str());

		SDK::UObject* generatedObj = g_staticFindObjectByName(SDK::UClass::StaticClass(), package, generatedClassNameW.c_str(), false);
		if (!generatedObj)
		{
			if (verbose)
				LOG_DEBUG("ItemRegistry:   [%s] StaticFindObject could not find generated class '%s_C' in package '%s'.",
					assetName.c_str(), assetName.c_str(), packageName.c_str());
			return nullptr;
		}

		SDK::UClass* generatedClass = static_cast<SDK::UClass*>(generatedObj);
		if (verbose)
			LOG_DEBUG("ItemRegistry:   [%s] generated class '%s'", assetName.c_str(), generatedClass->GetName().c_str());

		SDK::UObject* cdo = generatedClass->ClassDefaultObject;
		if (!cdo)
		{
			if (verbose)
				LOG_DEBUG("ItemRegistry:   [%s] ClassDefaultObject is null.", assetName.c_str());
			return nullptr;
		}
		if (!cdo->IsA(SDK::UAuItemDataBase::StaticClass()))
		{
			if (verbose)
				LOG_DEBUG("ItemRegistry:   [%s] CDO '%s' is not a UAuItemDataBase (class '%s').", assetName.c_str(),
					cdo->GetName().c_str(), cdo->Class ? cdo->Class->GetName().c_str() : "<none>");
			return nullptr;
		}

		return static_cast<SDK::UAuItemDataBase*>(cdo);
	}
}
