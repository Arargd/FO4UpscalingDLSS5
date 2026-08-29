#include "DX11Hooks.h"

#include <d3d11.h>

#include "Streamline.h"

extern bool enbLoaded;

struct hkD3D11CreateDeviceAndSwapChain
{
	static HRESULT WINAPI thunk(
		IDXGIAdapter* pAdapter,
		D3D_DRIVER_TYPE DriverType,
		HMODULE Software,
		UINT Flags,
		const D3D_FEATURE_LEVEL* pFeatureLevels,
		UINT FeatureLevels,
		UINT SDKVersion,
		const DXGI_SWAP_CHAIN_DESC* pSwapChainDesc,
		IDXGISwapChain** ppSwapChain,
		ID3D11Device** ppDevice,
		D3D_FEATURE_LEVEL* pFeatureLevel,
		ID3D11DeviceContext** ppImmediateContext)
	{
		REX::INFO("[DX11] D3D11CreateDeviceAndSwapChain called, forcing feature level 11_1");
		const D3D_FEATURE_LEVEL featureLevel = D3D_FEATURE_LEVEL_11_1;
		pFeatureLevels = &featureLevel;
		FeatureLevels = 1;

		DX::ThrowIfFailed(func(pAdapter,
			DriverType,
			Software,
			Flags,
			pFeatureLevels,
			FeatureLevels,
			SDKVersion,
			pSwapChainDesc,
			ppSwapChain,
			ppDevice,
			pFeatureLevel,
			ppImmediateContext));

		REX::INFO("[DX11] Device created successfully, feature level: 0x{:x}", static_cast<uint>(*pFeatureLevel));
		if (pSwapChainDesc) {
			REX::INFO("[DX11] SwapChain: {}x{}, format={}, bufferCount={}",
				pSwapChainDesc->BufferDesc.Width,
				pSwapChainDesc->BufferDesc.Height,
				static_cast<uint>(pSwapChainDesc->BufferDesc.Format),
				pSwapChainDesc->BufferCount);
		}

		// V7: Fallout's swap chain remains completely D3D11. Streamline owns only a
		// sidecar D3D12 compute queue used for DLSS/Neural Rendering. Do NOT upgrade
		// or replace the game's swap chain and do NOT pass the D3D11 device to
		// slSetD3DDevice.
		auto streamline = Streamline::GetSingleton();
		if (streamline->interposer && !streamline->conflictDetected) {
			REX::INFO("[DLSS-NR-V7] Initializing standalone D3D12 Streamline backend");
			if (!streamline->InitializeD3D12(pAdapter, *ppDevice, *ppImmediateContext)) {
				REX::WARN("[DLSS-NR-V7] D3D12 DLSS backend unavailable; normal FSR fallback remains available");
			}
		} else {
			REX::WARN("[DLSS-NR-V7] Streamline D3D12 backend was not initialized");
		}

		return S_OK;
	}
	static inline REL::Relocation<decltype(thunk)> func;
};

namespace DX11Hooks
{
	void Install()
	{
		auto streamline = Streamline::GetSingleton();
		streamline->LoadInterposer();

		uintptr_t moduleBase = (uintptr_t)GetModuleHandle(nullptr);
		REX::INFO("[HOOK] Module base: {:#x}", moduleBase);

		REX::INFO("[HOOK] Installing IAT hook for D3D11CreateDeviceAndSwapChain");
		(uintptr_t&)hkD3D11CreateDeviceAndSwapChain::func = Detours::IATHook(
			moduleBase,
			"d3d11.dll",
			"D3D11CreateDeviceAndSwapChain",
			(uintptr_t)hkD3D11CreateDeviceAndSwapChain::thunk);
		REX::INFO("[HOOK] IAT hook installed, original func: {:#x}", (uintptr_t)hkD3D11CreateDeviceAndSwapChain::func.get());
	}
}
