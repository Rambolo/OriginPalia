#include <Windows.h>
#include <iostream>
#include <thread>
#include <bit>

#include "MinHook.h"
#include "Configuration.h"
#include "PaliaOverlay.h"

#include "console/console.hpp"
#include "hooks/hooks.hpp"
#include "utils/utils.hpp"
#include "backend.hpp"

#include "../PaliaSDK/SDK.hpp"

// Forward declaration
DWORD WINAPI OnProcessAttach(LPVOID lpParam);
DWORD WINAPI OnProcessDetach(LPVOID lpParam);

BOOL WINAPI DllMain(const HMODULE hModule, const DWORD fdwReason, const LPVOID lpReserved) {
	if (fdwReason == DLL_PROCESS_ATTACH) {
		DisableThreadLibraryCalls(hModule);

		HANDLE hHandle = CreateThread(nullptr, 0, OnProcessAttach, hModule, 0, nullptr);
		if (hHandle != nullptr) {
			CloseHandle(hHandle);
		}
	} else if ( fdwReason == DLL_PROCESS_DETACH && !lpReserved) {
		OnProcessDetach(nullptr);
	}
	
	return TRUE;
}

DWORD WINAPI OnProcessAttach(const LPVOID lpParam) {
	HMODULE hModule = static_cast<HMODULE>(lpParam);

	int32_t GMallocOffset = 0x0898E0A0; // 0.182.0

	// Steam Support
	char fileName[MAX_PATH];
	GetModuleFileName(nullptr, fileName, MAX_PATH);
	if (const std::string filePath(fileName); filePath.find("PaliaClientSteam-Win64-Shipping.exe") != std::string::npos) {
		Offsets::GObjects		 = 0x08ADA3C0; // 0.182.0
		Offsets::AppendString	 = 0x00D00F50; // 0.182.0
		Offsets::GNames			 = 0x08A33A00; // 0.182.0
		Offsets::GWorld			 = 0x08C47D50; // 0.182.0
		Offsets::ProcessEvent	 = 0x00EDAAD0; // 0.182.0
		Offsets::ProcessEventIdx = 0x0000004D; // 0.182.0
		GMallocOffset			 = 0x089F9360; // 0.182.0
	}

	// Opens a console window (if enabled in console.hpp)
	Console::Alloc();

	const auto Overlay = new PaliaOverlay();
	MenuBase::Instance = Overlay;

	// Initialize MinHook
	if (MH_Initialize() != MH_OK) {
		LOG("[!] MinHook initialization failed.\n");
		FreeLibraryAndExitThread(hModule, 0);
	}

#ifdef FORCE_BACKEND
	U::SetRenderingBackend(DIRECTX12);
#else
	std::thread findRenderer([&]() {
		H::FindRenderer();
		});

	findRenderer.join();

	if (!H::FoundRenderer()) {
		LOG("[!] Failed to find backend renderer. Exiting.\n");
		FreeLibraryAndExitThread(hModule, 1);
	}
#endif

	// Initialize FMemory
	FMemory::FMalloc::UnrealStaticGMalloc = std::bit_cast<FMemory::FMalloc**>(InSDKUtils::GetImageBase() + GMallocOffset);
	FMemory::GMalloc = *FMemory::FMalloc::UnrealStaticGMalloc;

	H::Init();

	// Load Settings Before Hooks
	Configuration::Load(Overlay);

	// Hook Setups
	DetourManager::SetupHooks();
	
	return 0;
}

DWORD WINAPI OnProcessDetach(const LPVOID lpParam) {
	H::Free();
	MH_Uninitialize();
	
	Console::Free();
	
	return 0;
}