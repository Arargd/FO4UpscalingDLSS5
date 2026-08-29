#pragma once

#define NV_WINDOWS

#pragma warning(push)
#pragma warning(disable: 4471)
#include <sl.h>
#include <sl_consts.h>
#include <sl_dlss.h>
#include <sl_matrix_helpers.h>
#include <sl_version.h>
#pragma warning(pop)

#include "Buffer.h"

#include <array>
#include <climits>
#include <d3d11_4.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <string>
#include <winrt/base.h>

using PFun_slSetTagForFrame2 = sl::Result(
	const sl::FrameToken& frame,
	const sl::ViewportHandle& viewport,
	const sl::ResourceTag* tags,
	uint32_t numTags,
	sl::CommandBuffer* cmdBuffer);

/**
 * @class Streamline
 * @brief D3D12 Streamline/DLSS backend used by the D3D11 Fallout renderer.
 *
 * Fallout still renders with D3D11/DXVK. This backend creates a sidecar D3D12
 * device on the same adapter, copies the game's genuine low-resolution color,
 * depth and motion-vector inputs into shared D3D11/D3D12 textures, evaluates
 * DLSS on D3D12, and copies the full-resolution result back to D3D11 before the
 * game renders its UI. RenoDX can therefore intercept the real D3D12 DLSS call
 * and replace/augment it with Neural Rendering without owning presentation.
 */
class Streamline
{
public:
	static Streamline* GetSingleton()
	{
		static Streamline singleton;
		return &singleton;
	}

	~Streamline();

	inline std::string GetShortName() { return "Streamline-D3D12"; }

	// Load the Streamline interposer. AAAFrameGeneration.dll must be disabled for
	// this backend because Streamline has process-global initialization state.
	void LoadInterposer();

	// Initialize Streamline as D3D12 and create the D3D11<->D3D12 sidecar bridge.
	bool InitializeD3D12(IDXGIAdapter* a_adapter, ID3D11Device* a_d3d11Device, ID3D11DeviceContext* a_d3d11Context);

	// Query NVIDIA's recommended render scale for the selected DLSS mode. Falls
	// back to 0.0f when no valid recommendation is available.
	float GetResolutionScale(uint a_qualityMode, uint a_outputWidth, uint a_outputHeight);

	// Evaluate D3D12 DLSS using the low-resolution portion rendered by Fallout.
	// The output is copied back into a_upscaleTexture before returning to the
	// normal Upscaling.cpp path, so Fallout's UI remains native and untouched.
	void Upscale(Texture2D* a_upscaleTexture,
		Texture2D* a_dilatedMotionVectorTexture,
		float2 a_jitter,
		float2 a_renderSize,
		uint a_qualityMode);

	// Force a temporal-history reset on the next DLSS evaluation.
	void RequestReset(const char* a_reason);

	// Disable DLSS and release both Streamline and interop resources.
	void DestroyDLSSResources();
	void Shutdown();

	bool initialized = false;
	bool featureDLSS = false;
	bool conflictDetected = false;

	// Streamline state
	sl::ViewportHandle viewport{ 0 };
	sl::FrameToken* frameToken = nullptr;
	HMODULE interposer = nullptr;

private:
	static constexpr uint32_t kFramesInFlight = 3;

	struct SharedTexture
	{
		winrt::com_ptr<ID3D11Texture2D> resource11;
		winrt::com_ptr<ID3D12Resource> resource12;
		D3D11_TEXTURE2D_DESC desc{};

		void Reset()
		{
			resource12 = nullptr;
			resource11 = nullptr;
			desc = {};
		}
	};

	bool ResolveFunctions();
	bool CreateD3D12Bridge(IDXGIAdapter* a_adapter, ID3D11Device* a_d3d11Device, ID3D11DeviceContext* a_d3d11Context);
	bool CreateSharedTexture(SharedTexture& a_texture, uint32_t a_width, uint32_t a_height, DXGI_FORMAT a_format, UINT a_bindFlags);
	bool EnsureInteropResources(uint32_t a_renderWidth, uint32_t a_renderHeight, uint32_t a_outputWidth, uint32_t a_outputHeight, DXGI_FORMAT a_colorFormat);
	bool ConfigureDLSS(uint a_qualityMode, uint32_t a_outputWidth, uint32_t a_outputHeight);
	bool UpdateConstants(float2 a_jitter, float2 a_renderSize, bool a_forceReset);
	bool WaitForAllocator(uint32_t a_slot);
	void WaitForD3D12Idle();
	void ReleaseInteropResources();
	static sl::DLSSMode ToDLSSMode(uint a_qualityMode);

	// D3D11 side of the bridge
	winrt::com_ptr<ID3D11Device5> d3d11Device;
	winrt::com_ptr<ID3D11DeviceContext4> d3d11Context;
	winrt::com_ptr<ID3D11Fence> d3d11Fence;

	// D3D12 side of the bridge
	winrt::com_ptr<ID3D12Device> d3d12Device;
	winrt::com_ptr<ID3D12CommandQueue> commandQueue;
	std::array<winrt::com_ptr<ID3D12CommandAllocator>, kFramesInFlight> commandAllocators;
	winrt::com_ptr<ID3D12GraphicsCommandList> commandList;
	winrt::com_ptr<ID3D12Fence> d3d12Fence;
	std::array<uint64_t, kFramesInFlight> allocatorFenceValues{};
	HANDLE fenceEvent = nullptr;
	uint64_t fenceValue = 0;
	uint32_t frameSlot = 0;

	SharedTexture colorInput;
	SharedTexture depthInput;
	SharedTexture motionVectorInput;
	SharedTexture upscaleOutput;
	uint32_t interopRenderWidth = 0;
	uint32_t interopRenderHeight = 0;
	uint32_t interopOutputWidth = 0;
	uint32_t interopOutputHeight = 0;
	DXGI_FORMAT interopColorFormat = DXGI_FORMAT_UNKNOWN;

	bool dlssOptionsConfigured = false;
	uint configuredQualityMode = UINT_MAX;
	uint32_t configuredOutputWidth = 0;
	uint32_t configuredOutputHeight = 0;

	bool resetRequested = true;
	std::string resetReason = "first frame";
	uint64_t lastDLSSFrame = UINT64_MAX;
	float lastCameraX = 0.0f;
	float lastCameraY = 0.0f;
	float lastCameraZ = 0.0f;
	float lastCameraFov = 0.0f;
	bool havePreviousCamera = false;

	// Cached NVIDIA optimal-settings query
	uint cachedOptimalQuality = UINT_MAX;
	uint32_t cachedOptimalOutputWidth = 0;
	uint32_t cachedOptimalOutputHeight = 0;
	uint32_t cachedOptimalRenderWidth = 0;
	uint32_t cachedOptimalRenderHeight = 0;

	bool ownsInterposer = false;

	// Core Streamline function pointers
	PFun_slInit* slInit{};
	PFun_slShutdown* slShutdown{};
	PFun_slIsFeatureSupported* slIsFeatureSupported{};
	PFun_slIsFeatureLoaded* slIsFeatureLoaded{};
	PFun_slEvaluateFeature* slEvaluateFeature{};
	PFun_slAllocateResources* slAllocateResources{};
	PFun_slFreeResources* slFreeResources{};
	PFun_slGetFeatureRequirements* slGetFeatureRequirements{};
	PFun_slGetFeatureVersion* slGetFeatureVersion{};
	PFun_slSetConstants* slSetConstants{};
	PFun_slGetFeatureFunction* slGetFeatureFunction{};
	PFun_slGetNewFrameToken* slGetNewFrameToken{};
	PFun_slSetD3DDevice* slSetD3DDevice{};
	PFun_slSetTagForFrame2* slSetTagForFrame{};

	// DLSS feature functions
	PFun_slDLSSGetOptimalSettings* slDLSSGetOptimalSettings{};
	PFun_slDLSSGetState* slDLSSGetState{};
	PFun_slDLSSSetOptions* slDLSSSetOptions{};
};
