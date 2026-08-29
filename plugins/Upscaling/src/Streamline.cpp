#include "Streamline.h"

#include <SimpleIni.h>
#include <algorithm>
#include <cfloat>
#include <cmath>
#include <magic_enum/magic_enum.hpp>

#include "Upscaling.h"
#include "Util.h"

namespace
{
	sl::float4x4 ToSLMatrix(const __m128* a_matrix)
	{
		sl::float4x4 result;
		for (int i = 0; i < 4; ++i) {
			alignas(16) float row[4];
			_mm_store_ps(row, a_matrix[i]);
			result[i] = sl::float4(row[0], row[1], row[2], row[3]);
		}
		return result;
	}

	sl::float3 ToSLFloat3(const __m128* a_vector)
	{
		alignas(16) float v[4];
		_mm_store_ps(v, *a_vector);
		return sl::float3(v[0], v[1], v[2]);
	}

	uint32_t ToDimension(float a_value)
	{
		return std::max(1u, static_cast<uint32_t>(std::lround(a_value)));
	}
}

Streamline::~Streamline()
{
	Shutdown();
	if (interposer && ownsInterposer) {
		FreeLibrary(interposer);
	}
	interposer = nullptr;
}

void Streamline::LoadInterposer()
{
	// Streamline initialization is process-global. The old experimental path used
	// AAAFrameGeneration.dll as the D3D12 owner; the integrated backend must own
	// Streamline itself instead.
	if (GetModuleHandleW(L"AAAFrameGeneration.dll")) {
		conflictDetected = true;
		REX::ERROR("[DLSS-NR-V7] AAAFrameGeneration.dll is loaded. Disable it when using the integrated D3D12 DLSS backend.");
		return;
	}

	interposer = GetModuleHandleW(L"sl.interposer.dll");
	if (interposer) {
		ownsInterposer = false;
		REX::INFO("[DLSS-NR-V7] Reusing already-loaded sl.interposer.dll at {0:p}", static_cast<void*>(interposer));
		return;
	}

	interposer = LoadLibraryW(L"Data\\F4SE\\Plugins\\Streamline\\sl.interposer.dll");
	if (!interposer) {
		REX::ERROR("[DLSS-NR-V7] Failed to load sl.interposer.dll: {:#x}", GetLastError());
		return;
	}

	ownsInterposer = true;
	REX::INFO("[DLSS-NR-V7] Loaded sl.interposer.dll at {0:p}", static_cast<void*>(interposer));
}

bool Streamline::ResolveFunctions()
{
	if (!interposer)
		return false;

	slInit = (PFun_slInit*)GetProcAddress(interposer, "slInit");
	slShutdown = (PFun_slShutdown*)GetProcAddress(interposer, "slShutdown");
	slIsFeatureSupported = (PFun_slIsFeatureSupported*)GetProcAddress(interposer, "slIsFeatureSupported");
	slIsFeatureLoaded = (PFun_slIsFeatureLoaded*)GetProcAddress(interposer, "slIsFeatureLoaded");
	slEvaluateFeature = (PFun_slEvaluateFeature*)GetProcAddress(interposer, "slEvaluateFeature");
	slAllocateResources = (PFun_slAllocateResources*)GetProcAddress(interposer, "slAllocateResources");
	slFreeResources = (PFun_slFreeResources*)GetProcAddress(interposer, "slFreeResources");
	slGetFeatureRequirements = (PFun_slGetFeatureRequirements*)GetProcAddress(interposer, "slGetFeatureRequirements");
	slGetFeatureVersion = (PFun_slGetFeatureVersion*)GetProcAddress(interposer, "slGetFeatureVersion");
	slSetConstants = (PFun_slSetConstants*)GetProcAddress(interposer, "slSetConstants");
	slGetFeatureFunction = (PFun_slGetFeatureFunction*)GetProcAddress(interposer, "slGetFeatureFunction");
	slGetNewFrameToken = (PFun_slGetNewFrameToken*)GetProcAddress(interposer, "slGetNewFrameToken");
	slSetD3DDevice = (PFun_slSetD3DDevice*)GetProcAddress(interposer, "slSetD3DDevice");
	slSetTagForFrame = (PFun_slSetTagForFrame2*)GetProcAddress(interposer, "slSetTagForFrame");

	bool missing = false;
	auto check = [&](const void* a_ptr, const char* a_name) {
		if (!a_ptr) {
			REX::ERROR("[DLSS-NR-V7] Missing Streamline export: {}", a_name);
			missing = true;
		}
	};

	check(slInit, "slInit");
	check(slShutdown, "slShutdown");
	check(slIsFeatureSupported, "slIsFeatureSupported");
	check(slIsFeatureLoaded, "slIsFeatureLoaded");
	check(slEvaluateFeature, "slEvaluateFeature");
	check(slSetConstants, "slSetConstants");
	check(slGetFeatureFunction, "slGetFeatureFunction");
	check(slGetNewFrameToken, "slGetNewFrameToken");
	check(slSetD3DDevice, "slSetD3DDevice");
	check(slSetTagForFrame, "slSetTagForFrame");
	return !missing;
}

bool Streamline::CreateD3D12Bridge(IDXGIAdapter* a_adapter, ID3D11Device* a_d3d11Device, ID3D11DeviceContext* a_d3d11Context)
{
	if (!a_adapter || !a_d3d11Device || !a_d3d11Context)
		return false;

	if (FAILED(a_d3d11Device->QueryInterface(IID_PPV_ARGS(d3d11Device.put())))) {
		REX::ERROR("[DLSS-NR-V7] ID3D11Device5 is unavailable; D3D11/D3D12 fence interop cannot be created");
		return false;
	}
	if (FAILED(a_d3d11Context->QueryInterface(IID_PPV_ARGS(d3d11Context.put())))) {
		REX::ERROR("[DLSS-NR-V7] ID3D11DeviceContext4 is unavailable; D3D11/D3D12 fence interop cannot be created");
		return false;
	}

	HRESULT hr = D3D12CreateDevice(a_adapter, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(d3d12Device.put()));
	if (FAILED(hr)) {
		REX::ERROR("[DLSS-NR-V7] D3D12CreateDevice failed: {:#x}", static_cast<uint32_t>(hr));
		return false;
	}

	D3D12_COMMAND_QUEUE_DESC queueDesc{};
	queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
	queueDesc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
	queueDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
	queueDesc.NodeMask = 0;
	if (FAILED(hr = d3d12Device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(commandQueue.put())))) {
		REX::ERROR("[DLSS-NR-V7] CreateCommandQueue failed: {:#x}", static_cast<uint32_t>(hr));
		return false;
	}

	for (auto& allocator : commandAllocators) {
		if (FAILED(hr = d3d12Device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(allocator.put())))) {
			REX::ERROR("[DLSS-NR-V7] CreateCommandAllocator failed: {:#x}", static_cast<uint32_t>(hr));
			return false;
		}
	}

	if (FAILED(hr = d3d12Device->CreateCommandList(
		0,
		D3D12_COMMAND_LIST_TYPE_DIRECT,
		commandAllocators[0].get(),
		nullptr,
		IID_PPV_ARGS(commandList.put())))) {
		REX::ERROR("[DLSS-NR-V7] CreateCommandList failed: {:#x}", static_cast<uint32_t>(hr));
		return false;
	}
	commandList->Close();

	if (FAILED(hr = d3d12Device->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(d3d12Fence.put())))) {
		REX::ERROR("[DLSS-NR-V7] CreateFence failed: {:#x}", static_cast<uint32_t>(hr));
		return false;
	}

	HANDLE sharedFenceHandle = nullptr;
	if (FAILED(hr = d3d12Device->CreateSharedHandle(d3d12Fence.get(), nullptr, GENERIC_ALL, nullptr, &sharedFenceHandle))) {
		REX::ERROR("[DLSS-NR-V7] CreateSharedHandle(fence) failed: {:#x}", static_cast<uint32_t>(hr));
		return false;
	}

	hr = d3d11Device->OpenSharedFence(sharedFenceHandle, IID_PPV_ARGS(d3d11Fence.put()));
	CloseHandle(sharedFenceHandle);
	if (FAILED(hr)) {
		REX::ERROR("[DLSS-NR-V7] OpenSharedFence failed: {:#x}", static_cast<uint32_t>(hr));
		return false;
	}

	fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
	if (!fenceEvent) {
		REX::ERROR("[DLSS-NR-V7] CreateEvent for D3D12 fence failed: {:#x}", GetLastError());
		return false;
	}

	REX::INFO("[DLSS-NR-V7] D3D11/D3D12 sidecar bridge created successfully");
	return true;
}

bool Streamline::InitializeD3D12(IDXGIAdapter* a_adapter, ID3D11Device* a_d3d11Device, ID3D11DeviceContext* a_d3d11Context)
{
	if (initialized)
		return featureDLSS;
	if (conflictDetected || !interposer)
		return false;
	if (!ResolveFunctions())
		return false;
	if (!CreateD3D12Bridge(a_adapter, a_d3d11Device, a_d3d11Context))
		return false;

	// Pre-load the runtime DLLs so virtualized mod managers and Wine resolve the
	// same modules that Streamline will use internally.
	LoadLibraryW(L"Data\\F4SE\\Plugins\\Streamline\\sl.common.dll");
	LoadLibraryW(L"Data\\F4SE\\Plugins\\Streamline\\sl.dlss.dll");

	CSimpleIniA ini;
	ini.SetUnicode();
	ini.LoadFile("Data\\MCM\\Settings\\Upscaling.ini");
	const bool debugLogging = ini.GetBoolValue("Settings", "bEnableDebugLogging", false);

	sl::Preferences pref{};
	pref.showConsole = false;
	pref.logLevel = debugLogging ? sl::LogLevel::eVerbose : sl::LogLevel::eDefault;
	if (debugLogging) {
		pref.logMessageCallback = [](sl::LogType, const char* a_message) {
			REX::INFO("[SL-INT] {}", a_message);
		};
	}
	pref.engine = sl::EngineType::eCustom;
	pref.engineVersion = "1.0.0";
	pref.projectId = "f8776929-c969-43bd-ac2b-294b4de58aac";
	pref.flags = sl::PreferenceFlags::eUseManualHooking | sl::PreferenceFlags::eUseFrameBasedResourceTagging;
	pref.renderAPI = sl::RenderAPI::eD3D12;

	wchar_t interposerPath[MAX_PATH]{};
	GetModuleFileNameW(interposer, interposerPath, MAX_PATH);
	std::wstring pluginDirectory(interposerPath);
	auto slash = pluginDirectory.find_last_of(L"\\/");
	if (slash != std::wstring::npos)
		pluginDirectory.resize(slash);
	const wchar_t* pluginPaths[] = { pluginDirectory.c_str() };
	pref.pathsToPlugins = pluginPaths;
	pref.numPathsToPlugins = 1;

	sl::Feature features[] = { sl::kFeatureDLSS };
	pref.featuresToLoad = features;
	pref.numFeaturesToLoad = _countof(features);

	auto initResult = slInit(pref, sl::kSDKVersion);
	if (initResult != sl::Result::eOk) {
		REX::ERROR("[DLSS-NR-V7] slInit(D3D12) failed: {}", static_cast<int>(initResult));
		return false;
	}

	initialized = true;

	if (SL_FAILED(deviceResult, slSetD3DDevice(d3d12Device.get()))) {
		REX::ERROR("[DLSS-NR-V7] slSetD3DDevice(D3D12) failed: {}", static_cast<int>(deviceResult));
		Shutdown();
		return false;
	}

	DXGI_ADAPTER_DESC adapterDesc{};
	a_adapter->GetDesc(&adapterDesc);
	sl::AdapterInfo adapterInfo{};
	adapterInfo.deviceLUID = reinterpret_cast<uint8_t*>(&adapterDesc.AdapterLuid);
	adapterInfo.deviceLUIDSizeInBytes = sizeof(adapterDesc.AdapterLuid);

	bool loaded = false;
	slIsFeatureLoaded(sl::kFeatureDLSS, loaded);
	if (!loaded) {
		REX::ERROR("[DLSS-NR-V7] Streamline DLSS plugin did not load");
		featureDLSS = false;
		return false;
	}

	auto supportResult = slIsFeatureSupported(sl::kFeatureDLSS, adapterInfo);
	featureDLSS = supportResult == sl::Result::eOk;
	if (!featureDLSS) {
		REX::ERROR("[DLSS-NR-V7] DLSS is not supported on the selected adapter: {}", magic_enum::enum_name(supportResult));
		return false;
	}

	slGetFeatureFunction(sl::kFeatureDLSS, "slDLSSGetOptimalSettings", (void*&)slDLSSGetOptimalSettings);
	slGetFeatureFunction(sl::kFeatureDLSS, "slDLSSGetState", (void*&)slDLSSGetState);
	slGetFeatureFunction(sl::kFeatureDLSS, "slDLSSSetOptions", (void*&)slDLSSSetOptions);

	if (!slDLSSGetOptimalSettings || !slDLSSSetOptions) {
		REX::ERROR("[DLSS-NR-V7] DLSS feature functions are unavailable");
		featureDLSS = false;
		return false;
	}

	REX::INFO("[DLSS-NR-V7] Integrated D3D12 DLSS backend is ready; RenoDX can intercept this path directly");
	RequestReset("backend initialization");
	return true;
}

sl::DLSSMode Streamline::ToDLSSMode(uint a_qualityMode)
{
	switch (a_qualityMode) {
	case 1:
		return sl::DLSSMode::eMaxQuality;
	case 2:
		return sl::DLSSMode::eBalanced;
	case 3:
		return sl::DLSSMode::eMaxPerformance;
	case 4:
		return sl::DLSSMode::eUltraPerformance;
	default:
		return sl::DLSSMode::eDLAA;
	}
}

float Streamline::GetResolutionScale(uint a_qualityMode, uint a_outputWidth, uint a_outputHeight)
{
	if (!featureDLSS || !slDLSSGetOptimalSettings || !a_outputWidth || !a_outputHeight)
		return 0.0f;

	if (cachedOptimalQuality == a_qualityMode &&
		cachedOptimalOutputWidth == a_outputWidth &&
		cachedOptimalOutputHeight == a_outputHeight &&
		cachedOptimalRenderWidth && cachedOptimalRenderHeight) {
		return static_cast<float>(cachedOptimalRenderWidth) / static_cast<float>(a_outputWidth);
	}

	sl::DLSSOptions options{};
	options.mode = ToDLSSMode(a_qualityMode);
	options.outputWidth = a_outputWidth;
	options.outputHeight = a_outputHeight;
	options.colorBuffersHDR = sl::Boolean::eFalse;
	options.useAutoExposure = sl::Boolean::eTrue;
	options.preExposure = 1.0f;
	options.exposureScale = 1.0f;

	sl::DLSSOptimalSettings optimal{};
	auto result = slDLSSGetOptimalSettings(options, optimal);
	if (result != sl::Result::eOk || !optimal.optimalRenderWidth || !optimal.optimalRenderHeight) {
		REX::WARN("[DLSS-NR-V7] slDLSSGetOptimalSettings failed: {}", static_cast<int>(result));
		return 0.0f;
	}

	cachedOptimalQuality = a_qualityMode;
	cachedOptimalOutputWidth = a_outputWidth;
	cachedOptimalOutputHeight = a_outputHeight;
	cachedOptimalRenderWidth = optimal.optimalRenderWidth;
	cachedOptimalRenderHeight = optimal.optimalRenderHeight;

	const float xScale = static_cast<float>(optimal.optimalRenderWidth) / static_cast<float>(a_outputWidth);
	const float yScale = static_cast<float>(optimal.optimalRenderHeight) / static_cast<float>(a_outputHeight);
	if (std::abs(xScale - yScale) > 0.0025f) {
		REX::WARN("[DLSS-NR-V7] DLSS optimal aspect scales differ: x={:.5f}, y={:.5f}; using x scale", xScale, yScale);
	}

	REX::INFO("[DLSS-NR-V7] NVIDIA optimal render size for mode {}: {}x{} -> {}x{} (scale {:.4f})",
		a_qualityMode, optimal.optimalRenderWidth, optimal.optimalRenderHeight, a_outputWidth, a_outputHeight, xScale);
	RequestReset("DLSS quality/output size changed");
	return xScale;
}

bool Streamline::CreateSharedTexture(SharedTexture& a_texture, uint32_t a_width, uint32_t a_height, DXGI_FORMAT a_format, UINT a_bindFlags)
{
	a_texture.Reset();

	D3D11_TEXTURE2D_DESC desc{};
	desc.Width = a_width;
	desc.Height = a_height;
	desc.MipLevels = 1;
	desc.ArraySize = 1;
	desc.Format = a_format;
	desc.SampleDesc.Count = 1;
	desc.Usage = D3D11_USAGE_DEFAULT;
	desc.BindFlags = a_bindFlags;
	desc.CPUAccessFlags = 0;
	desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;

	HRESULT hr = d3d11Device->CreateTexture2D(&desc, nullptr, a_texture.resource11.put());
	if (FAILED(hr)) {
		REX::ERROR("[DLSS-NR-V7] CreateTexture2D(shared {}x{}, fmt={}) failed: {:#x}",
			a_width, a_height, static_cast<uint32_t>(a_format), static_cast<uint32_t>(hr));
		return false;
	}

	winrt::com_ptr<IDXGIResource1> dxgiResource;
	hr = a_texture.resource11->QueryInterface(IID_PPV_ARGS(dxgiResource.put()));
	if (FAILED(hr)) {
		REX::ERROR("[DLSS-NR-V7] QueryInterface(IDXGIResource1) failed: {:#x}", static_cast<uint32_t>(hr));
		return false;
	}

	HANDLE sharedHandle = nullptr;
	hr = dxgiResource->CreateSharedHandle(
		nullptr,
		DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE,
		nullptr,
		&sharedHandle);
	if (FAILED(hr)) {
		REX::ERROR("[DLSS-NR-V7] CreateSharedHandle(texture) failed: {:#x}", static_cast<uint32_t>(hr));
		return false;
	}

	hr = d3d12Device->OpenSharedHandle(sharedHandle, IID_PPV_ARGS(a_texture.resource12.put()));
	CloseHandle(sharedHandle);
	if (FAILED(hr)) {
		REX::ERROR("[DLSS-NR-V7] D3D12 OpenSharedHandle(texture) failed: {:#x}", static_cast<uint32_t>(hr));
		return false;
	}

	a_texture.desc = desc;
	return true;
}

bool Streamline::EnsureInteropResources(
	uint32_t a_renderWidth,
	uint32_t a_renderHeight,
	uint32_t a_outputWidth,
	uint32_t a_outputHeight,
	DXGI_FORMAT a_colorFormat)
{
	if (colorInput.resource11 &&
		interopRenderWidth == a_renderWidth && interopRenderHeight == a_renderHeight &&
		interopOutputWidth == a_outputWidth && interopOutputHeight == a_outputHeight &&
		interopColorFormat == a_colorFormat) {
		return true;
	}

	WaitForD3D12Idle();
	if (dlssOptionsConfigured && slFreeResources) {
		slFreeResources(sl::kFeatureDLSS, viewport);
		dlssOptionsConfigured = false;
	}
	ReleaseInteropResources();

	// Inputs are exact render-resolution resources. This is the key difference
	// from the experimental v4 path, which merely tagged a cropped extent inside
	// a full-resolution resource.
	if (!CreateSharedTexture(colorInput, a_renderWidth, a_renderHeight, a_colorFormat, D3D11_BIND_SHADER_RESOURCE))
		return false;
	if (!CreateSharedTexture(depthInput, a_renderWidth, a_renderHeight, DXGI_FORMAT_R32_FLOAT, D3D11_BIND_SHADER_RESOURCE))
		return false;
	if (!CreateSharedTexture(motionVectorInput, a_renderWidth, a_renderHeight, DXGI_FORMAT_R16G16_FLOAT, D3D11_BIND_SHADER_RESOURCE))
		return false;
	if (!CreateSharedTexture(upscaleOutput, a_outputWidth, a_outputHeight, a_colorFormat,
		D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS))
		return false;

	interopRenderWidth = a_renderWidth;
	interopRenderHeight = a_renderHeight;
	interopOutputWidth = a_outputWidth;
	interopOutputHeight = a_outputHeight;
	interopColorFormat = a_colorFormat;
	dlssOptionsConfigured = false;
	RequestReset("interop resources recreated");

	REX::INFO("[DLSS-NR-V7] Shared resources created: color/depth/mvec {}x{} -> output {}x{} format={}",
		a_renderWidth, a_renderHeight, a_outputWidth, a_outputHeight, static_cast<uint32_t>(a_colorFormat));
	return true;
}

bool Streamline::ConfigureDLSS(uint a_qualityMode, uint32_t a_outputWidth, uint32_t a_outputHeight)
{
	if (!slDLSSSetOptions)
		return false;

	if (dlssOptionsConfigured && configuredQualityMode == a_qualityMode &&
		configuredOutputWidth == a_outputWidth && configuredOutputHeight == a_outputHeight) {
		return true;
	}

	if (dlssOptionsConfigured && slFreeResources) {
		slFreeResources(sl::kFeatureDLSS, viewport);
	}

	sl::DLSSOptions options{};
	options.mode = ToDLSSMode(a_qualityMode);
	options.outputWidth = a_outputWidth;
	options.outputHeight = a_outputHeight;
	options.colorBuffersHDR = sl::Boolean::eFalse;
	options.useAutoExposure = sl::Boolean::eTrue;
	options.preExposure = 1.0f;
	options.exposureScale = 1.0f;
	options.alphaUpscalingEnabled = sl::Boolean::eFalse;

	auto result = slDLSSSetOptions(viewport, options);
	if (result != sl::Result::eOk) {
		REX::ERROR("[DLSS-NR-V7] slDLSSSetOptions failed: {}", static_cast<int>(result));
		dlssOptionsConfigured = false;
		return false;
	}

	dlssOptionsConfigured = true;
	configuredQualityMode = a_qualityMode;
	configuredOutputWidth = a_outputWidth;
	configuredOutputHeight = a_outputHeight;
	RequestReset("DLSS options changed");

	REX::INFO("[DLSS-NR-V7] DLSS configured once: mode={} output={}x{} autoExposure=on",
		a_qualityMode, a_outputWidth, a_outputHeight);
	return true;
}

void Streamline::RequestReset(const char* a_reason)
{
	if (!resetRequested) {
		REX::INFO("[DLSS-NR-V7] Temporal reset requested: {}", a_reason ? a_reason : "unspecified");
	}
	resetRequested = true;
	resetReason = a_reason ? a_reason : "unspecified";
}

bool Streamline::UpdateConstants(float2 a_jitter, float2 a_renderSize, bool a_forceReset)
{
	if (!slGetNewFrameToken || !slSetConstants)
		return false;

	if (SL_FAILED(tokenResult, slGetNewFrameToken(frameToken, nullptr)) || !frameToken) {
		REX::ERROR("[DLSS-NR-V7] Could not get Streamline frame token");
		return false;
	}

	auto gameViewport = Util::State_GetSingleton();
	auto& camView = gameViewport->cameraState.camViewData;
	auto& camState = gameViewport->cameraState;

	const uint64_t currentFrame = static_cast<uint64_t>(gameViewport->frameCount);
	bool reset = resetRequested || a_forceReset;

	if (lastDLSSFrame != UINT64_MAX && currentFrame > lastDLSSFrame + 1) {
		reset = true;
		resetReason = "DLSS frame gap/menu transition";
	}

	sl::Constants constants{};

	// All Streamline matrices must be unjittered. Reuse the same Fallout camera
	// data that the working DLSS-G integration uses.
	sl::float4x4 viewMatrix = ToSLMatrix(camView.viewMat);
	sl::float4x4 invView{};
	sl::matrixFullInvert(invView, viewMatrix);
	sl::float4x4 vpUnjittered = ToSLMatrix(camView.viewProjUnjittered);
	sl::matrixMul(constants.cameraViewToClip, invView, vpUnjittered);
	sl::matrixFullInvert(constants.clipToCameraView, constants.cameraViewToClip);

	sl::float4x4 currentVP = ToSLMatrix(camView.currentViewProjUnjittered);
	sl::float4x4 previousVP = ToSLMatrix(camView.previousViewProjUnjittered);
	sl::float4x4 invCurrentVP{};
	sl::matrixFullInvert(invCurrentVP, currentVP);
	sl::matrixMul(constants.clipToPrevClip, invCurrentVP, previousVP);
	sl::matrixFullInvert(constants.prevClipToClip, constants.clipToPrevClip);

	constants.cameraPos = sl::float3(camState.posAdjust.x, camState.posAdjust.y, camState.posAdjust.z);
	constants.cameraUp = ToSLFloat3(&camView.viewUp);
	constants.cameraRight = ToSLFloat3(&camView.viewRight);
	constants.cameraFwd = ToSLFloat3(&camView.viewDir);
	constants.cameraNear = *(float*)REL::ID({ 57985, 2712882, 2712882 }).address();
	constants.cameraFar = *(float*)REL::ID({ 958877, 2712883, 2712883 }).address();
	constants.cameraAspectRatio = static_cast<float>(interopOutputWidth) / static_cast<float>(std::max(1u, interopOutputHeight));

	const float projectionY = constants.cameraViewToClip[1].y;
	constants.cameraFOV = std::abs(projectionY) > 0.00001f ? 2.0f * std::atan(1.0f / std::abs(projectionY)) : 1.0f;
	constants.cameraMotionIncluded = sl::Boolean::eTrue;
	constants.cameraPinholeOffset = { 0.0f, 0.0f };
	constants.depthInverted = sl::Boolean::eTrue;
	constants.jitterOffset = { -a_jitter.x, -a_jitter.y };

	// Fallout's motion vectors are normalized; FidelityFX multiplies them by the
	// render dimensions to obtain pixel-space vectors. Streamline expects a scale
	// that normalizes the values to [-1, 1], so {1,1} is the matching convention.
	constants.mvecScale = { 1.0f, 1.0f };
	constants.motionVectors3D = sl::Boolean::eFalse;
	constants.motionVectorsInvalidValue = FLT_MIN;
	constants.orthographicProjection = sl::Boolean::eFalse;
	constants.motionVectorsDilated = sl::Boolean::eTrue;
	constants.motionVectorsJittered = sl::Boolean::eFalse;

	// Catch large camera discontinuities that otherwise poison temporal history.
	if (havePreviousCamera) {
		const float dx = camState.posAdjust.x - lastCameraX;
		const float dy = camState.posAdjust.y - lastCameraY;
		const float dz = camState.posAdjust.z - lastCameraZ;
		const float distanceSq = dx * dx + dy * dy + dz * dz;
		if (distanceSq > 4096.0f * 4096.0f) {
			reset = true;
			resetReason = "large camera jump";
		}
		if (std::abs(constants.cameraFOV - lastCameraFov) > 0.25f) {
			reset = true;
			resetReason = "large FOV change";
		}
	}

	constants.reset = reset ? sl::Boolean::eTrue : sl::Boolean::eFalse;

	auto result = slSetConstants(constants, *frameToken, viewport);
	if (result != sl::Result::eOk) {
		REX::ERROR("[DLSS-NR-V7] slSetConstants failed: {}", static_cast<int>(result));
		return false;
	}

	if (reset) {
		REX::INFO("[DLSS-NR-V7] Temporal history reset applied: {}", resetReason);
	}
	resetRequested = false;
	lastDLSSFrame = currentFrame;
	lastCameraX = camState.posAdjust.x;
	lastCameraY = camState.posAdjust.y;
	lastCameraZ = camState.posAdjust.z;
	lastCameraFov = constants.cameraFOV;
	havePreviousCamera = true;

	static bool loggedConstants = false;
	if (!loggedConstants) {
		REX::INFO("[DLSS-NR-V7] Temporal constants: render={}x{} jitter=({:.4f},{:.4f}) near={:.4f} far={:.2f} fov={:.4f} depthInverted=1 mvecScale=(1,1) dilated=1",
			ToDimension(a_renderSize.x), ToDimension(a_renderSize.y), a_jitter.x, a_jitter.y,
			constants.cameraNear, constants.cameraFar, constants.cameraFOV);
		loggedConstants = true;
	}
	return true;
}

bool Streamline::WaitForAllocator(uint32_t a_slot)
{
	const uint64_t required = allocatorFenceValues[a_slot];
	if (!required || !d3d12Fence)
		return true;
	if (d3d12Fence->GetCompletedValue() >= required)
		return true;

	HRESULT hr = d3d12Fence->SetEventOnCompletion(required, fenceEvent);
	if (FAILED(hr)) {
		REX::ERROR("[DLSS-NR-V7] SetEventOnCompletion failed: {:#x}", static_cast<uint32_t>(hr));
		return false;
	}
	WaitForSingleObject(fenceEvent, INFINITE);
	return true;
}

void Streamline::WaitForD3D12Idle()
{
	if (!commandQueue || !d3d12Fence || !fenceEvent)
		return;
	const uint64_t value = ++fenceValue;
	if (FAILED(commandQueue->Signal(d3d12Fence.get(), value)))
		return;
	if (d3d12Fence->GetCompletedValue() < value) {
		if (SUCCEEDED(d3d12Fence->SetEventOnCompletion(value, fenceEvent)))
			WaitForSingleObject(fenceEvent, INFINITE);
	}
}

void Streamline::Upscale(
	Texture2D* a_upscaleTexture,
	Texture2D* a_dilatedMotionVectorTexture,
	float2 a_jitter,
	float2 a_renderSize,
	uint a_qualityMode)
{
	if (!initialized || !featureDLSS || !a_upscaleTexture || !a_dilatedMotionVectorTexture)
		return;

	auto upscaling = Upscaling::GetSingleton();
	if (!upscaling->depthOverrideTexture) {
		static bool loggedMissingDepth = false;
		if (!loggedMissingDepth) {
			REX::ERROR("[DLSS-NR-V7] depthOverrideTexture is unavailable; refusing to evaluate DLSS with stale/unknown depth");
			loggedMissingDepth = true;
		}
		return;
	}

	auto gameViewport = Util::State_GetSingleton();
	const uint32_t renderWidth = ToDimension(a_renderSize.x);
	const uint32_t renderHeight = ToDimension(a_renderSize.y);
	const uint32_t outputWidth = gameViewport->screenWidth;
	const uint32_t outputHeight = gameViewport->screenHeight;

	D3D11_TEXTURE2D_DESC colorDesc{};
	D3D11_TEXTURE2D_DESC depthDesc{};
	D3D11_TEXTURE2D_DESC mvecDesc{};
	a_upscaleTexture->resource->GetDesc(&colorDesc);
	upscaling->depthOverrideTexture->resource->GetDesc(&depthDesc);
	a_dilatedMotionVectorTexture->resource->GetDesc(&mvecDesc);

	if (colorDesc.Width < renderWidth || colorDesc.Height < renderHeight ||
		depthDesc.Width < renderWidth || depthDesc.Height < renderHeight ||
		mvecDesc.Width < renderWidth || mvecDesc.Height < renderHeight) {
		REX::ERROR("[DLSS-NR-V7] Source-size mismatch: color={}x{} depth={}x{} mvec={}x{} requested={}x{}",
			colorDesc.Width, colorDesc.Height, depthDesc.Width, depthDesc.Height,
			mvecDesc.Width, mvecDesc.Height, renderWidth, renderHeight);
		RequestReset("source-size mismatch");
		return;
	}

	if (!EnsureInteropResources(renderWidth, renderHeight, outputWidth, outputHeight, colorDesc.Format))
		return;
	if (!ConfigureDLSS(a_qualityMode, outputWidth, outputHeight))
		return;

	// Stage the genuine low-resolution render region into exact-size shared
	// resources. The D3D12 side therefore sees 1280x720 resources in Quality at
	// 1080p, rather than a 1920x1080 texture with a cropped extent.
	D3D11_BOX sourceBox{};
	sourceBox.left = 0;
	sourceBox.top = 0;
	sourceBox.front = 0;
	sourceBox.right = renderWidth;
	sourceBox.bottom = renderHeight;
	sourceBox.back = 1;

	d3d11Context->CopySubresourceRegion(colorInput.resource11.get(), 0, 0, 0, 0, a_upscaleTexture->resource.get(), 0, &sourceBox);
	d3d11Context->CopySubresourceRegion(depthInput.resource11.get(), 0, 0, 0, 0, upscaling->depthOverrideTexture->resource.get(), 0, &sourceBox);
	d3d11Context->CopySubresourceRegion(motionVectorInput.resource11.get(), 0, 0, 0, 0, a_dilatedMotionVectorTexture->resource.get(), 0, &sourceBox);

	const uint64_t inputReady = ++fenceValue;
	if (FAILED(d3d11Context->Signal(d3d11Fence.get(), inputReady)) ||
		FAILED(commandQueue->Wait(d3d12Fence.get(), inputReady))) {
		REX::ERROR("[DLSS-NR-V7] Failed to synchronize D3D11 -> D3D12");
		return;
	}

	const uint32_t slot = frameSlot % kFramesInFlight;
	if (!WaitForAllocator(slot))
		return;

	HRESULT hr = commandAllocators[slot]->Reset();
	if (FAILED(hr)) {
		REX::ERROR("[DLSS-NR-V7] Command allocator reset failed: {:#x}", static_cast<uint32_t>(hr));
		return;
	}
	hr = commandList->Reset(commandAllocators[slot].get(), nullptr);
	if (FAILED(hr)) {
		REX::ERROR("[DLSS-NR-V7] Command list reset failed: {:#x}", static_cast<uint32_t>(hr));
		return;
	}

	if (!UpdateConstants(a_jitter, a_renderSize, false)) {
		commandList->Close();
		return;
	}

	sl::Extent inputExtent{ 0, 0, renderWidth, renderHeight };
	sl::Extent outputExtent{ 0, 0, outputWidth, outputHeight };
	sl::Resource colorIn{ sl::ResourceType::eTex2d, colorInput.resource12.get(), 0 };
	sl::Resource colorOut{ sl::ResourceType::eTex2d, upscaleOutput.resource12.get(), 0 };
	sl::Resource depth{ sl::ResourceType::eTex2d, depthInput.resource12.get(), 0 };
	sl::Resource mvec{ sl::ResourceType::eTex2d, motionVectorInput.resource12.get(), 0 };

	sl::ResourceTag tags[] = {
		{ &colorIn, sl::kBufferTypeScalingInputColor, sl::ResourceLifecycle::eValidUntilEvaluate, &inputExtent },
		{ &colorOut, sl::kBufferTypeScalingOutputColor, sl::ResourceLifecycle::eValidUntilEvaluate, &outputExtent },
		{ &depth, sl::kBufferTypeDepth, sl::ResourceLifecycle::eValidUntilEvaluate, &inputExtent },
		{ &mvec, sl::kBufferTypeMotionVectors, sl::ResourceLifecycle::eValidUntilEvaluate, &inputExtent },
	};

	auto tagResult = slSetTagForFrame(*frameToken, viewport, tags, _countof(tags), (sl::CommandBuffer*)commandList.get());
	if (tagResult != sl::Result::eOk) {
		REX::ERROR("[DLSS-NR-V7] slSetTagForFrame failed: {}", static_cast<int>(tagResult));
		commandList->Close();
		return;
	}

	sl::ViewportHandle view(viewport);
	const sl::BaseStructure* inputs[] = { &view };
	auto evalResult = slEvaluateFeature(
		sl::kFeatureDLSS,
		*frameToken,
		inputs,
		_countof(inputs),
		(sl::CommandBuffer*)commandList.get());

	if (FAILED(hr = commandList->Close())) {
		REX::ERROR("[DLSS-NR-V7] Command list close failed: {:#x}", static_cast<uint32_t>(hr));
		return;
	}

	ID3D12CommandList* lists[] = { commandList.get() };
	commandQueue->ExecuteCommandLists(1, lists);

	const uint64_t outputReady = ++fenceValue;
	if (FAILED(commandQueue->Signal(d3d12Fence.get(), outputReady))) {
		REX::ERROR("[DLSS-NR-V7] D3D12 output fence signal failed");
		return;
	}
	allocatorFenceValues[slot] = outputReady;
	frameSlot++;

	if (FAILED(d3d11Context->Wait(d3d11Fence.get(), outputReady))) {
		REX::ERROR("[DLSS-NR-V7] Failed to synchronize D3D12 -> D3D11");
		return;
	}

	if (evalResult == sl::Result::eOk) {
		// This copy is queued behind the D3D11 fence wait. Upscaling.cpp then copies
		// the result to Fallout's real framebuffer, after which Fallout renders UI
		// normally at native resolution.
		d3d11Context->CopyResource(a_upscaleTexture->resource.get(), upscaleOutput.resource11.get());
	} else {
		REX::ERROR("[DLSS-NR-V7] slEvaluateFeature failed: {}. DLSS will be marked unavailable for FSR fallback on the next frame.",
			static_cast<int>(evalResult));
		featureDLSS = false;
		RequestReset("DLSS evaluate failure");
	}

	static bool loggedFirstSuccess = false;
	if (!loggedFirstSuccess && evalResult == sl::Result::eOk) {
		REX::INFO("[DLSS-NR-V7] First integrated D3D12 DLSS evaluation succeeded: {}x{} -> {}x{}; output returned pre-UI",
			renderWidth, renderHeight, outputWidth, outputHeight);
		loggedFirstSuccess = true;
	}
}

void Streamline::ReleaseInteropResources()
{
	colorInput.Reset();
	depthInput.Reset();
	motionVectorInput.Reset();
	upscaleOutput.Reset();
	interopRenderWidth = 0;
	interopRenderHeight = 0;
	interopOutputWidth = 0;
	interopOutputHeight = 0;
	interopColorFormat = DXGI_FORMAT_UNKNOWN;
}

void Streamline::DestroyDLSSResources()
{
	if (slDLSSSetOptions) {
		sl::DLSSOptions options{};
		options.mode = sl::DLSSMode::eOff;
		slDLSSSetOptions(viewport, options);
	}
	if (slFreeResources && initialized)
		slFreeResources(sl::kFeatureDLSS, viewport);

	WaitForD3D12Idle();
	ReleaseInteropResources();
	dlssOptionsConfigured = false;
	configuredQualityMode = UINT_MAX;
	configuredOutputWidth = 0;
	configuredOutputHeight = 0;
	cachedOptimalQuality = UINT_MAX;
	cachedOptimalOutputWidth = 0;
	cachedOptimalOutputHeight = 0;
	cachedOptimalRenderWidth = 0;
	cachedOptimalRenderHeight = 0;
	RequestReset("DLSS resources destroyed");
}

void Streamline::Shutdown()
{
	if (!initialized && !d3d12Device && !d3d11Device)
		return;

	if (initialized)
		DestroyDLSSResources();

	if (initialized && slShutdown) {
		slShutdown();
	}
	initialized = false;
	featureDLSS = false;

	WaitForD3D12Idle();
	commandList = nullptr;
	for (auto& allocator : commandAllocators)
		allocator = nullptr;
	commandQueue = nullptr;
	d3d11Fence = nullptr;
	d3d12Fence = nullptr;
	d3d11Context = nullptr;
	d3d11Device = nullptr;
	d3d12Device = nullptr;
	allocatorFenceValues.fill(0);
	fenceValue = 0;
	frameSlot = 0;

	if (fenceEvent) {
		CloseHandle(fenceEvent);
		fenceEvent = nullptr;
	}
}
