#include "Streamline.h"
#include "Upscaling.h"

void StreamlineFG::LoadInterposer()
{
	interposer = LoadLibrary(L"Data\\F4SE\\Plugins\\Streamline\\sl.interposer.dll");
	if (!interposer) {
		REX::WARN("[DLSSG] Failed to load interposer: {:#x}", GetLastError());
		return;
	}
	REX::INFO("[DLSSG] Interposer loaded");

	slInit = (PFun_slInit*)GetProcAddress(interposer, "slInit");
	slShutdown = (PFun_slShutdown*)GetProcAddress(interposer, "slShutdown");
	slUpgradeInterface = (PFun_slUpgradeInterface*)GetProcAddress(interposer, "slUpgradeInterface");
	slSetD3DDevice = (PFun_slSetD3DDevice*)GetProcAddress(interposer, "slSetD3DDevice");
	slGetFeatureFunction = (PFun_slGetFeatureFunction*)GetProcAddress(interposer, "slGetFeatureFunction");
	slSetTagForFrame = (PFun_slSetTagForFrame2*)GetProcAddress(interposer, "slSetTagForFrame");
	slSetConstants = (PFun_slSetConstants*)GetProcAddress(interposer, "slSetConstants");
	slGetNewFrameToken = (PFun_slGetNewFrameToken*)GetProcAddress(interposer, "slGetNewFrameToken");
	slEvaluateFeature = (PFun_slEvaluateFeature*)GetProcAddress(interposer, "slEvaluateFeature");
}

bool StreamlineFG::InitStreamline()
{
	if (!interposer || !slInit) return false;

	REX::INFO("[DLSSG] Initializing Streamline");

	// Pre-load plugin DLLs for MO2 USVFS compatibility
	LoadLibrary(L"Data\\F4SE\\Plugins\\Streamline\\sl.common.dll");
	LoadLibrary(L"Data\\F4SE\\Plugins\\Streamline\\sl.dlss.dll");
	LoadLibrary(L"Data\\F4SE\\Plugins\\Streamline\\sl.dlss_g.dll");

	sl::Preferences pref{};
	pref.showConsole = false;
	auto debugLogging = Upscaling::GetSingleton()->settings.debugLogging;
	pref.logLevel = debugLogging ? sl::LogLevel::eVerbose : sl::LogLevel::eDefault;
	if (debugLogging) {
		pref.logMessageCallback = [](sl::LogType, const char* msg) {
			REX::INFO("[SL-INT] {}", msg);
		};
	}
	pref.engine = sl::EngineType::eCustom;
	pref.engineVersion = "1.0.0";
	// Reuse the project ID already used by this repo's D3D11 DLSS integration.
	pref.projectId = "f8776929-c969-43bd-ac2b-294b4de58aac";
	pref.flags = sl::PreferenceFlags::eUseManualHooking | sl::PreferenceFlags::eUseFrameBasedResourceTagging;
	pref.renderAPI = sl::RenderAPI::eD3D12;

	// Resolve real path where sl.interposer.dll lives (MO2 USVFS may virtualize it)
	static wchar_t interposerDir[MAX_PATH];
	GetModuleFileNameW(interposer, interposerDir, MAX_PATH);
	wchar_t* lastSlash = wcsrchr(interposerDir, L'\\');
	if (!lastSlash) lastSlash = wcsrchr(interposerDir, L'/');
	if (lastSlash) *lastSlash = L'\0';

	static const wchar_t* pluginPaths[] = { interposerDir };
	pref.pathsToPlugins = pluginPaths;
	pref.numPathsToPlugins = 1;

	static sl::Feature features[] = { sl::kFeatureDLSS, sl::kFeatureDLSS_G, sl::kFeatureReflex, sl::kFeaturePCL };
	pref.featuresToLoad = features;
	pref.numFeaturesToLoad = _countof(features);

	auto result = slInit(pref, sl::kSDKVersion);
	REX::INFO("[DLSSG] slInit result: {}", (int)result);

	if (result != sl::Result::eOk) {
		REX::ERROR("[DLSSG] Streamline init failed");
		return false;
	}

	slInitialized = true;
	return true;
}

void StreamlineFG::SetD3DDevice(ID3D12Device* a_device)
{
	d3d12Device = a_device;
	if (slSetD3DDevice && slInitialized) {
		slSetD3DDevice(a_device);
		REX::INFO("[DLSSG] D3D12 device set");
	}
}

bool StreamlineFG::CheckAndEnableDLSSProbe()
{
	if (!slInitialized || !slGetFeatureFunction || !slEvaluateFeature) return false;

	slGetFeatureFunction(sl::kFeatureDLSS, "slDLSSSetOptions", (void*&)slDLSSSetOptions);
	if (!slDLSSSetOptions) {
		REX::WARN("[DLSS-NR-PROBE] Could not resolve slDLSSSetOptions");
		featureDLSS = false;
		return false;
	}

	featureDLSS = true;
	REX::INFO("[DLSS-NR-PROBE] D3D12 DLSS feature function resolved; probe enabled");
	return true;
}

bool StreamlineFG::CheckAndEnableDLSSG()
{
	if (!slInitialized) return false;

	// Probe DLSS is optional: DLSS-G may still work if this fails.
	CheckAndEnableDLSSProbe();

	if (slGetFeatureFunction) {
		slGetFeatureFunction(sl::kFeatureDLSS_G, "slDLSSGSetOptions", (void*&)slDLSSGSetOptions);
		slGetFeatureFunction(sl::kFeatureDLSS_G, "slDLSSGGetState", (void*&)slDLSSGGetState);
		slGetFeatureFunction(sl::kFeatureReflex, "slReflexSetOptions", (void*&)slReflexSetOptions);
		slGetFeatureFunction(sl::kFeatureReflex, "slReflexSleep", (void*&)slReflexSleep);
		slGetFeatureFunction(sl::kFeatureReflex, "slReflexSetMarker", (void*&)slReflexSetMarker);
		REX::INFO("[DLSSG] Feature functions loaded: SetOptions={:#x}, GetState={:#x}, ReflexMarker={:#x}",
			(uintptr_t)slDLSSGSetOptions, (uintptr_t)slDLSSGGetState, (uintptr_t)slReflexSetMarker);
	}

	if (!slDLSSGSetOptions || !slReflexSetMarker) {
		REX::WARN("[DLSSG] Missing required function pointers");
		return false;
	}

	// Query hardware capability for MFG
	uint32_t maxFrames = 1;
	if (slDLSSGGetState) {
		sl::DLSSGState state{};
		slDLSSGGetState(viewport, state, nullptr);
		maxFrames = state.numFramesToGenerateMax;
	}

	// Enable DLSS-G — called once at init, not per-frame
	// Clamp requested frames to hardware max (1 on RTX 40xx, up to 3 on RTX 50xx)
	auto upscaling = Upscaling::GetSingleton();
	uint32_t requestedFrames = std::clamp((uint32_t)upscaling->settings.frameGenFrames, 1u, maxFrames);

	configuredFrameCount = requestedFrames;

	sl::DLSSGOptions options{};
	options.mode = sl::DLSSGMode::eOn;
	options.numFramesToGenerate = requestedFrames;

	auto result = slDLSSGSetOptions(viewport, options);
	if (result != sl::Result::eOk) {
		REX::WARN("[DLSSG] Failed to enable DLSS-G: {}", (int)result);
		return false;
	}

	featureDLSSG = true;
	REX::INFO("[DLSSG] DLSS-G enabled: {}x frame gen (requested {}, hardware max {})",
		requestedFrames + 1, upscaling->settings.frameGenFrames, maxFrames);

	// Reflex must be active when DLSS-G is on
	if (slReflexSetOptions) {
		sl::ReflexOptions reflexOptions{};
		reflexOptions.mode = sl::ReflexMode::eLowLatency;
		slReflexSetOptions(reflexOptions);
	}

	if (slDLSSGGetState) {
		sl::DLSSGState state{};
		slDLSSGGetState(viewport, state, nullptr);
		REX::INFO("[DLSSG] Status: {}, minDim: {}, maxFrames: {}",
			(int)state.status, state.minWidthOrHeight, state.numFramesToGenerateMax);
	}

	return true;
}

void StreamlineFG::SetEnabled(bool a_enabled)
{
	if (!slDLSSGSetOptions || !featureDLSSG) return;

	sl::DLSSGOptions options{};
	options.mode = a_enabled ? sl::DLSSGMode::eOn : sl::DLSSGMode::eOff;
	options.numFramesToGenerate = configuredFrameCount;
	slDLSSGSetOptions(viewport, options);
}

static sl::float4x4 toSLMatrix(const __m128* mat)
{
	sl::float4x4 result;
	for (int i = 0; i < 4; i++) {
		alignas(16) float row[4];
		_mm_store_ps(row, mat[i]);
		result[i] = sl::float4(row[0], row[1], row[2], row[3]);
	}
	return result;
}

static sl::float3 toSLFloat3(const __m128* v)
{
	alignas(16) float vals[4];
	_mm_store_ps(vals, *v);
	return sl::float3(vals[0], vals[1], vals[2]);
}

void StreamlineFG::AcquireFrameToken()
{
	// Never reuse a previous frame's DLSS output if this frame cannot evaluate.
	dlssOutputValid = false;

	if (!slGetNewFrameToken || (!featureDLSSG && !featureDLSS)) return;

	if (SL_FAILED(res, slGetNewFrameToken(frameToken, nullptr))) {
		static bool loggedOnce = false;
		if (!loggedOnce) { REX::ERROR("[DLSSG] Failed to get frame token"); loggedOnce = true; }
	}
}

void StreamlineFG::SetPCLMarker(sl::PCLMarker marker)
{
	if (!slReflexSetMarker || !frameToken) return;
	slReflexSetMarker(marker, *frameToken);
}

bool StreamlineFG::RunDLSSProbe(
	ID3D12GraphicsCommandList* a_cmdList,
	ID3D12Resource* a_depth,
	ID3D12Resource* a_motionVectors,
	ID3D12Resource* a_color,
	float2 a_screenSize)
{
	dlssOutputValid = false;

	if (!dlssProbeEnabled || !featureDLSS || !frameToken || !slDLSSSetOptions || !slEvaluateFeature ||
		!d3d12Device || !a_cmdList || !a_depth || !a_motionVectors || !a_color)
		return false;

	auto inputDesc = a_color->GetDesc();
	const uint32_t width = (uint32_t)a_screenSize.x;
	const uint32_t height = (uint32_t)a_screenSize.y;

	// Create/recreate a full-resolution UAV-capable scratch output. Keeping it separate
	// from the input avoids relying on in-place DLSS behavior and makes this a valid
	// D3D12 DLAA evaluation for hook/probe purposes.
	if (!dlssProbeOutput || dlssProbeWidth != width || dlssProbeHeight != height || dlssProbeFormat != inputDesc.Format) {
		if (dlssProbeOutput) {
			dlssProbeOutput->Release();
			dlssProbeOutput = nullptr;
		}

		D3D12_HEAP_PROPERTIES heapProps{};
		heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;
		heapProps.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
		heapProps.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
		heapProps.CreationNodeMask = 1;
		heapProps.VisibleNodeMask = 1;

		D3D12_RESOURCE_DESC outDesc = inputDesc;
		outDesc.Width = width;
		outDesc.Height = height;
		outDesc.MipLevels = 1;
		outDesc.DepthOrArraySize = 1;
		outDesc.SampleDesc.Count = 1;
		outDesc.SampleDesc.Quality = 0;
		outDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
		outDesc.Flags = (D3D12_RESOURCE_FLAGS)(outDesc.Flags | D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);

		HRESULT hr = d3d12Device->CreateCommittedResource(
			&heapProps,
			D3D12_HEAP_FLAG_NONE,
			&outDesc,
			D3D12_RESOURCE_STATE_COMMON,
			nullptr,
			IID_PPV_ARGS(&dlssProbeOutput));
		if (FAILED(hr)) {
			REX::ERROR("[DLSS-NR-PROBE] Failed to create scratch output: {:#x}", (uint32_t)hr);
			return false;
		}

		dlssProbeWidth = width;
		dlssProbeHeight = height;
		dlssProbeFormat = inputDesc.Format;
		REX::INFO("[DLSS-NR-PROBE] Scratch output created: {}x{}, format={}", width, height, (int)inputDesc.Format);
	}

	// V4.1 correctness pass: keep the DLSS input extent equal to the actual
	// source texture extent. Merely tagging the top-left 1280x720 region of a
	// 1920x1080 texture does NOT downsample the frame; it crops it. A genuine
	// Quality-mode 1280x720 -> 1920x1080 path needs a real 1280x720 source
	// resource (or a game render target that is actually 1280x720).
	//
	// For now use native-resolution DLAA/Neural Rendering so every tagged pixel
	// corresponds to real source data. This makes the visual path correct first;
	// a real low-resolution input path can be added separately.
	const uint32_t renderWidth = width;
	const uint32_t renderHeight = height;

	// Log the real resource dimensions once (and whenever they change). This is
	// important because the swap-chain size alone does not prove that the scene,
	// depth and motion-vector resources have matching dimensions.
	auto depthDesc = a_depth->GetDesc();
	auto mvecDesc = a_motionVectors->GetDesc();
	static uint32_t loggedColorW = 0, loggedColorH = 0;
	static uint32_t loggedDepthW = 0, loggedDepthH = 0;
	static uint32_t loggedMvecW = 0, loggedMvecH = 0;
	if (loggedColorW != inputDesc.Width || loggedColorH != inputDesc.Height ||
		loggedDepthW != depthDesc.Width || loggedDepthH != depthDesc.Height ||
		loggedMvecW != mvecDesc.Width || loggedMvecH != mvecDesc.Height)
	{
		REX::INFO(
			"[DLSS-NR-V4.1] Resource sizes: color={}x{} depth={}x{} mvec={}x{} requested={}x{}",
			(uint32_t)inputDesc.Width, inputDesc.Height,
			(uint32_t)depthDesc.Width, depthDesc.Height,
			(uint32_t)mvecDesc.Width, mvecDesc.Height,
			width, height);
		loggedColorW = (uint32_t)inputDesc.Width;
		loggedColorH = inputDesc.Height;
		loggedDepthW = (uint32_t)depthDesc.Width;
		loggedDepthH = depthDesc.Height;
		loggedMvecW = (uint32_t)mvecDesc.Width;
		loggedMvecH = mvecDesc.Height;
	}

	// Refuse to feed DLSS an extent larger than the backing resources. This avoids
	// undefined reads and the classic "only the top-left part looks valid" failure.
	if (inputDesc.Width < width || inputDesc.Height < height ||
		depthDesc.Width < width || depthDesc.Height < height ||
		mvecDesc.Width < width || mvecDesc.Height < height)
	{
		static bool loggedSizeMismatch = false;
		if (!loggedSizeMismatch) {
			REX::ERROR(
				"[DLSS-NR-V4.1] Resource-size mismatch; skipping visual DLSS instead of presenting a partial frame");
			loggedSizeMismatch = true;
		}
		return false;
	}

	// DLSS required tags are needed through Evaluate, not through Present. Keeping
	// them scoped to Evaluate prevents stale scaling tags from leaking into the
	// later DLSS-G/UI tagging in the same frame.
	sl::Extent inputExtent{ 0, 0, renderWidth, renderHeight };
	sl::Extent fullExtent{ 0, 0, width, height };
	sl::Resource colorIn = { sl::ResourceType::eTex2d, a_color, 0 };
	sl::Resource colorOut = { sl::ResourceType::eTex2d, dlssProbeOutput, 0 };
	sl::Resource depth = { sl::ResourceType::eTex2d, a_depth, 0 };
	sl::Resource mvec = { sl::ResourceType::eTex2d, a_motionVectors, 0 };

	sl::ResourceTag tags[] = {
		{ &colorIn, sl::kBufferTypeScalingInputColor, sl::ResourceLifecycle::eValidUntilEvaluate, &inputExtent },
		{ &colorOut, sl::kBufferTypeScalingOutputColor, sl::ResourceLifecycle::eValidUntilEvaluate, &fullExtent },
		{ &depth, sl::kBufferTypeDepth, sl::ResourceLifecycle::eValidUntilEvaluate, &inputExtent },
		{ &mvec, sl::kBufferTypeMotionVectors, sl::ResourceLifecycle::eValidUntilEvaluate, &inputExtent },
	};

	if (slSetTagForFrame) {
		auto tagResult = slSetTagForFrame(*frameToken, viewport, tags, _countof(tags), (sl::CommandBuffer*)a_cmdList);
		if (tagResult != sl::Result::eOk) {
			static bool loggedTagFailure = false;
			if (!loggedTagFailure) {
				REX::ERROR("[DLSS-NR-PROBE] slSetTagForFrame failed: {}", (int)tagResult);
				loggedTagFailure = true;
			}
			return false;
		}
	}

	// Configure DLSS only when the output resolution changes. V3 called
	// slDLSSSetOptions every frame, which correlated with RenoDX rebuilding
	// feature 18 repeatedly and causing the severe frame-rate collapse.
	if (!dlssOptionsConfigured || dlssConfiguredOutputWidth != width || dlssConfiguredOutputHeight != height) {
		sl::DLSSOptions options{};
		options.mode = sl::DLSSMode::eDLAA;
		options.outputWidth = width;
		options.outputHeight = height;
		options.colorBuffersHDR = sl::Boolean::eFalse;
		if (SL_FAILED(setResult, slDLSSSetOptions(viewport, options))) {
			REX::ERROR("[DLSS-NR-V4.1] slDLSSSetOptions failed: {}", (int)setResult);
			dlssOptionsConfigured = false;
			return false;
		}

		dlssOptionsConfigured = true;
		dlssConfiguredOutputWidth = width;
		dlssConfiguredOutputHeight = height;
		REX::INFO("[DLSS-NR-V4.1] Native DLAA configured once: {}x{} -> {}x{}",
			renderWidth, renderHeight, width, height);
	}

	sl::ViewportHandle view(viewport);
	const sl::BaseStructure* inputs[] = { &view };
	auto result = slEvaluateFeature(sl::kFeatureDLSS, *frameToken, inputs, _countof(inputs), (sl::CommandBuffer*)a_cmdList);

	static bool loggedEval = false;
	if (!loggedEval) {
		REX::INFO("[DLSS-NR-PROBE] First D3D12 DLSS/DLAA evaluation result: {}", (int)result);
		loggedEval = true;
	}

	dlssOutputValid = (result == sl::Result::eOk);
	if (dlssOutputValid) {
		static bool loggedVisualReady = false;
		if (!loggedVisualReady) {
			REX::INFO("[DLSS-NR-VISUAL] DLSS output is valid and ready for presentation");
			loggedVisualReady = true;
		}
	}
	return dlssOutputValid;
}

void StreamlineFG::Present(
	ID3D12GraphicsCommandList* a_cmdList,
	ID3D12Resource* a_depth,
	ID3D12Resource* a_motionVectors,
	ID3D12Resource* a_hudlessColor,
	ID3D12Resource* a_uiColorAlpha,
	float2 a_screenSize,
	float2 a_jitter,
	float a_cameraNear, float a_cameraFar,
	const CameraData& a_camera)
{
	if ((!featureDLSSG && !featureDLSS) || !frameToken) return;

	// Set per-frame constants — matrices MUST be unjittered per DLSS-G docs
	if (slSetConstants) {
		sl::Constants constants{};

		// Derive unjittered projection: inv(viewMat) * viewProjUnjittered
		sl::float4x4 viewMatrix = toSLMatrix(a_camera.viewMat);
		sl::float4x4 invView;
		sl::matrixFullInvert(invView, viewMatrix);
		sl::float4x4 vpUnjittered = toSLMatrix(a_camera.viewProjUnjittered);
		sl::matrixMul(constants.cameraViewToClip, invView, vpUnjittered);
		sl::matrixFullInvert(constants.clipToCameraView, constants.cameraViewToClip);

		sl::float4x4 currentVP = toSLMatrix(a_camera.currentViewProjUnjittered);
		sl::float4x4 previousVP = toSLMatrix(a_camera.previousViewProjUnjittered);
		sl::float4x4 invCurrentVP;
		sl::matrixFullInvert(invCurrentVP, currentVP);
		sl::matrixMul(constants.clipToPrevClip, invCurrentVP, previousVP);
		sl::matrixFullInvert(constants.prevClipToClip, constants.clipToPrevClip);

		constants.cameraPos = sl::float3(a_camera.posX, a_camera.posY, a_camera.posZ);
		constants.cameraUp = toSLFloat3(a_camera.viewUp);
		constants.cameraRight = toSLFloat3(a_camera.viewRight);
		constants.cameraFwd = toSLFloat3(a_camera.viewDir);
		constants.cameraNear = a_cameraNear;
		constants.cameraFar = a_cameraFar;
		constants.cameraAspectRatio = a_screenSize.x / a_screenSize.y;
		// Extract vertical FOV from unjittered projection matrix
		constants.cameraFOV = 2.0f * std::atan(1.0f / constants.cameraViewToClip[1].y);
		constants.cameraMotionIncluded = sl::Boolean::eTrue;
		constants.cameraPinholeOffset = { 0.f, 0.f };
		constants.depthInverted = sl::Boolean::eTrue;
		constants.jitterOffset = { -a_jitter.x, -a_jitter.y };
		constants.mvecScale = { 1.0f, 1.0f };
		constants.reset = sl::Boolean::eFalse;
		constants.motionVectors3D = sl::Boolean::eFalse;
		constants.orthographicProjection = sl::Boolean::eFalse;
		constants.motionVectorsDilated = sl::Boolean::eFalse;
		constants.motionVectorsJittered = sl::Boolean::eFalse;

		if (SL_FAILED(res, slSetConstants(constants, *frameToken, viewport))) {
			static bool loggedOnce = false;
			if (!loggedOnce) { REX::ERROR("[DLSSG] Failed to set constants"); loggedOnce = true; }
		}
	}

	// Tag D3D12 resources
	if (a_depth && a_motionVectors && a_hudlessColor && slSetTagForFrame) {
		sl::Resource depth = { sl::ResourceType::eTex2d, a_depth, 0 };
		sl::Resource mvec = { sl::ResourceType::eTex2d, a_motionVectors, 0 };
		sl::Resource hudless = { sl::ResourceType::eTex2d, a_hudlessColor, 0 };

		// Explicit extent matching screen size
		sl::Extent fullExtent = { 0, 0, (uint32_t)a_screenSize.x, (uint32_t)a_screenSize.y };

		if (a_uiColorAlpha) {
			sl::Resource uiColor = { sl::ResourceType::eTex2d, a_uiColorAlpha, 0 };

			sl::ResourceTag tags[] = {
				{ &depth, sl::kBufferTypeDepth, sl::ResourceLifecycle::eValidUntilPresent, &fullExtent },
				{ &mvec, sl::kBufferTypeMotionVectors, sl::ResourceLifecycle::eValidUntilPresent, &fullExtent },
				{ &hudless, sl::kBufferTypeHUDLessColor, sl::ResourceLifecycle::eValidUntilPresent, &fullExtent },
				{ &uiColor, sl::kBufferTypeUIColorAndAlpha, sl::ResourceLifecycle::eValidUntilPresent, &fullExtent },
			};
			slSetTagForFrame(*frameToken, viewport, tags, _countof(tags), (sl::CommandBuffer*)a_cmdList);
		} else {
			sl::ResourceTag tags[] = {
				{ &depth, sl::kBufferTypeDepth, sl::ResourceLifecycle::eValidUntilPresent, &fullExtent },
				{ &mvec, sl::kBufferTypeMotionVectors, sl::ResourceLifecycle::eValidUntilPresent, &fullExtent },
				{ &hudless, sl::kBufferTypeHUDLessColor, sl::ResourceLifecycle::eValidUntilPresent, &fullExtent },
				{ nullptr, sl::kBufferTypeUIColorAndAlpha, sl::ResourceLifecycle::eValidUntilPresent, nullptr },
			};
			slSetTagForFrame(*frameToken, viewport, tags, _countof(tags), (sl::CommandBuffer*)a_cmdList);
		}
	}

	// Visual D3D12 DLSS Quality dispatch. RenoDX hooks this evaluation and the resulting
	// texture is copied to the swap chain later in DX12SwapChain::Present.
	RunDLSSProbe(a_cmdList, a_depth, a_motionVectors, a_hudlessColor, a_screenSize);
}

void StreamlineFG::Shutdown()
{
	dlssOutputValid = false;
	dlssOptionsConfigured = false;
	dlssConfiguredOutputWidth = 0;
	dlssConfiguredOutputHeight = 0;

	if (dlssProbeOutput) {
		dlssProbeOutput->Release();
		dlssProbeOutput = nullptr;
	}

	if (slInitialized && slShutdown) {
		slShutdown();
		slInitialized = false;
	}
}
