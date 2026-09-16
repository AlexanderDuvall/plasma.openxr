#include <OpenXRPlugin/OpenXRPluginPCH.h>

#include <GameEngine/GameApplication/GameApplication.h>
#include <OpenXRPlugin/OpenXRDeclarations.h>
#include <OpenXRPlugin/OpenXRHandTracking.h>
#include <OpenXRPlugin/OpenXRInputDevice.h>
#include <OpenXRPlugin/OpenXRRemoting.h>
#include <OpenXRPlugin/OpenXRSingleton.h>
#include <OpenXRPlugin/OpenXRSpatialAnchors.h>
#include <OpenXRPlugin/OpenXRSwapChain.h>
#include <RendererCore/Components/CameraComponent.h>
#include <RendererCore/Pipeline/View.h>
#include <RendererCore/RenderWorld/RenderWorld.h>
#include <RendererCore/Textures/TextureUtils.h>
#include <Texture/Image/Formats/ImageFormatMappings.h>

#include <Core/ActorSystem/Actor.h>
#include <Core/World/World.h>
#include <Foundation/Configuration/CVar.h>
#include <GameEngine/Configuration/XRConfig.h>
#include <GameEngine/XR/StageSpaceComponent.h>
#include <GameEngine/XR/XRWindow.h>

#include <RendererDX12/Device/DeviceDX12.h>
#include <RendererVulkan/Device/DeviceVulkan.h>

#include <vector>

static_assert(plGALMSAASampleCount::None == 1);
static_assert(plGALMSAASampleCount::TwoSamples == 2);
static_assert(plGALMSAASampleCount::FourSamples == 4);
static_assert(plGALMSAASampleCount::EightSamples == 8);

// See the comment at the AddExtIfSupported block in SelectExtensions for why these default to off.
plCVarBool cvar_XrExtendedInteractionProfiles("XR.ExtendedInteractionProfiles", false, plCVarFlags::Save, "Request the interaction-profile extensions for Quest Pro, Vive Cosmos, Vive Focus 3, Pico and hand interaction.");
plCVarBool cvar_XrDepthComposition("XR.DepthComposition", false, plCVarFlags::Save, "Submit a depth layer so the runtime can do depth-aware late reprojection.");

// Logs the OpenXR API layers the loader will insert, so a fault inside xrCreateInstance can be attributed to a vendor
// layer rather than to the application.
static void LogAvailableApiLayers()
{
  plUInt32 uiLayerCount = 0;
  if (xrEnumerateApiLayerProperties(0, &uiLayerCount, nullptr) != XR_SUCCESS || uiLayerCount == 0)
  {
    plLog::Info("OpenXR: No API layers are registered.");
    return;
  }

  std::vector<XrApiLayerProperties> layers(uiLayerCount, {XR_TYPE_API_LAYER_PROPERTIES});
  if (xrEnumerateApiLayerProperties(uiLayerCount, &uiLayerCount, layers.data()) != XR_SUCCESS)
    return;

  for (const XrApiLayerProperties& layer : layers)
  {
    plLog::Info("OpenXR: API layer present: {} (spec {}.{}, impl {})", layer.layerName,
      XR_VERSION_MAJOR(layer.specVersion), XR_VERSION_MINOR(layer.specVersion), layer.layerVersion);
  }
}

PL_IMPLEMENT_SINGLETON(plOpenXR);

static plOpenXR g_OpenXRSingleton;

XrBool32 XRAPI_CALL xrDebugCallback(XrDebugUtilsMessageSeverityFlagsEXT messageSeverity, XrDebugUtilsMessageTypeFlagsEXT messageTypes, const XrDebugUtilsMessengerCallbackDataEXT* pCallbackData, void* pUserData)
{
  switch (messageSeverity)
  {
    case XR_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT:
      plLog::Debug("XR: {}", pCallbackData->message);
      break;
    case XR_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT:
      plLog::Info("XR: {}", pCallbackData->message);
      break;
    case XR_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT:
      plLog::Warning("XR: {}", pCallbackData->message);
      break;
    case XR_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT:
      plLog::Error("XR: {}", pCallbackData->message);
      break;
    default:
      break;
  }
  // Only layers are allowed to return true here.
  return XR_FALSE;
}

plOpenXR::plOpenXR()
  : m_SingletonRegistrar(this)
{
#ifdef BUILDSYSTEM_ENABLE_OPENXR_REMOTING_SUPPORT
  m_pRemoting = PL_DEFAULT_NEW(plOpenXRRemoting, this);
#endif
}

plOpenXR::~plOpenXR()
{
  // Releases the process-lifetime instance during plugin unload, while the loader is still mapped.
  if (m_ExecutionEventsId != 0 && plGameApplicationBase::GetGameApplicationBaseInstance() != nullptr)
  {
    plGameApplicationBase::GetGameApplicationBaseInstance()->m_ExecutionEvents.RemoveEventHandler(m_ExecutionEventsId);
  }

  m_pInput = nullptr;

  if (m_pInstance)
  {
    xrDestroyInstance(m_pInstance);
    m_pInstance = XR_NULL_HANDLE;
  }
}

bool plOpenXR::GetDepthComposition() const
{
  return m_Extensions.m_bDepthComposition;
}

bool plOpenXR::IsHmdPresent() const
{
  XrSystemGetInfo systemInfo{XR_TYPE_SYSTEM_GET_INFO};
  systemInfo.formFactor = XrFormFactor::XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
  uint64_t systemId = XR_NULL_SYSTEM_ID;

  if (m_pInstance != XR_NULL_HANDLE)
  {
    return xrGetSystem(m_pInstance, &systemInfo, &systemId) == XrResult::XR_SUCCESS;
  }

  // The interface documents this as callable before Initialize, so probe with a throwaway instance. Only the loader
  // and the runtime's system enumeration are exercised; no session or graphics binding is created.
  XrInstanceCreateInfo createInfo{XR_TYPE_INSTANCE_CREATE_INFO};
  plStringUtils::Copy(createInfo.applicationInfo.applicationName, PL_ARRAY_SIZE(createInfo.applicationInfo.applicationName), plApplication::GetApplicationInstance()->GetApplicationName());
  plStringUtils::Copy(createInfo.applicationInfo.engineName, PL_ARRAY_SIZE(createInfo.applicationInfo.engineName), "plEngine");
  createInfo.applicationInfo.engineVersion = 1;
  createInfo.applicationInfo.apiVersion = XR_API_VERSION_1_0;
  createInfo.applicationInfo.applicationVersion = 1;

  XrInstance pProbeInstance = XR_NULL_HANDLE;
  if (xrCreateInstance(&createInfo, &pProbeInstance) != XrResult::XR_SUCCESS)
  {
    // No runtime installed at all.
    return false;
  }

  const bool bPresent = xrGetSystem(pProbeInstance, &systemInfo, &systemId) == XrResult::XR_SUCCESS;
  xrDestroyInstance(pProbeInstance);
  return bPresent;
}

XrResult plOpenXR::SelectExtensions(plHybridArray<const char*, 6>& extensions)
{
  // Fetch the list of extensions supported by the runtime.
  plUInt32 extensionCount;
  XR_SUCCEED_OR_RETURN_LOG(xrEnumerateInstanceExtensionProperties(nullptr, 0, &extensionCount, nullptr));
  std::vector<XrExtensionProperties> extensionProperties(extensionCount, {XR_TYPE_EXTENSION_PROPERTIES});
  XR_SUCCEED_OR_RETURN_LOG(xrEnumerateInstanceExtensionProperties(nullptr, extensionCount, &extensionCount, extensionProperties.data()));

  // Add a specific extension to the list of extensions to be enabled, if it is supported. This is a lambda function that checks
  // if a certain extension has been installed on the target device. If yes, it will be added to the extensions list.
  auto AddExtIfSupported = [&](const char* extensionName, bool& enableFlag) -> XrResult
  {
    auto it = std::find_if(begin(extensionProperties), end(extensionProperties), [&](const XrExtensionProperties& prop)
      { return plStringUtils::IsEqual(prop.extensionName, extensionName); });
    if (it != end(extensionProperties))
    {
      extensions.PushBack(extensionName);
      enableFlag = true;
      return XR_SUCCESS;
    }
    enableFlag = false;
    return XR_ERROR_EXTENSION_NOT_PRESENT;
  };
  // The graphics binding extension must match the renderer. The instance lives for the whole process, so this choice does too.
  const plStringView sRenderer = plGALDevice::HasDefaultDevice() ? plGALDevice::GetDefaultDevice()->GetRenderer() : plGameApplication::GetActiveRenderer();
  m_GraphicsApi = sRenderer.IsEqual_NoCase("DX12") ? GraphicsApi::D3D12 : GraphicsApi::Vulkan;

  if (m_GraphicsApi == GraphicsApi::D3D12)
  {
    if (AddExtIfSupported(XR_KHR_D3D12_ENABLE_EXTENSION_NAME, m_Extensions.m_bD3D12) != XR_SUCCESS)
    {
      plLog::Error("OpenXR: The active runtime does not support XR_KHR_D3D12_enable. Run with '-renderer Vulkan' to use it.");
      return XR_ERROR_EXTENSION_NOT_PRESENT;
    }
    plLog::Info("OpenXR: Using XR_KHR_D3D12_enable");
  }
  // Prefer XR_KHR_vulkan_enable2 for better GPU synchronization, fall back to vulkan_enable
  else if (AddExtIfSupported(XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME, m_Extensions.m_bVulkan2) != XR_SUCCESS)
  {
    // Fall back to vulkan_enable (required so check that it was added)
    XR_SUCCEED_OR_RETURN_LOG(AddExtIfSupported(XR_KHR_VULKAN_ENABLE_EXTENSION_NAME, m_Extensions.m_bVulkan));
  }
  else
  {
    plLog::Info("OpenXR: Using XR_KHR_vulkan_enable2 for improved GPU synchronization");
  }

  // Submitting a depth layer lets the runtime do depth-aware late reprojection, which keeps geometry stable when the
  // application misses a frame. Behind a cvar because every enabled extension is also handed to whatever OpenXR API
  // layers are installed, and a layer that mishandles one takes down xrCreateInstance for the whole application.
  m_Extensions.m_bDepthComposition = false;
  if (cvar_XrDepthComposition)
  {
    AddExtIfSupported(XR_KHR_COMPOSITION_LAYER_DEPTH_EXTENSION_NAME, m_Extensions.m_bDepthComposition);
    if (m_Extensions.m_bDepthComposition)
    {
      plLog::Info("OpenXR: Using XR_KHR_composition_layer_depth for depth-aware reprojection");
    }
  }
  AddExtIfSupported(XR_MSFT_UNBOUNDED_REFERENCE_SPACE_EXTENSION_NAME, m_Extensions.m_bUnboundedReferenceSpace);
  AddExtIfSupported(XR_MSFT_SPATIAL_ANCHOR_EXTENSION_NAME, m_Extensions.m_bSpatialAnchor);
  AddExtIfSupported(XR_EXT_HAND_TRACKING_EXTENSION_NAME, m_Extensions.m_bHandTracking);
  AddExtIfSupported(XR_MSFT_HAND_INTERACTION_EXTENSION_NAME, m_Extensions.m_bHandInteraction);
  AddExtIfSupported(XR_MSFT_HAND_TRACKING_MESH_EXTENSION_NAME, m_Extensions.m_bHandTrackingMesh);

  // Interaction profiles for hardware beyond the original six. Without the extension enabled the runtime rejects the
  // profile's paths, so the suggested bindings in plOpenXRInputDevice are inert.
  //
  // Off by default, because requesting one of these can activate a vendor OpenXR API layer that then inserts itself
  // into every input call. XR_EXT_hand_interaction in particular brings up HTC's ViveOpenXRHandTracking layer, which
  // aborts inside aristo_interface.dll on a headset that has no hand-tracking cameras - taking the app with it. Only
  // enable this when running hardware that needs one of these profiles.
  if (cvar_XrExtendedInteractionProfiles)
  {
    AddExtIfSupported(XR_EXT_HAND_INTERACTION_EXTENSION_NAME, m_Extensions.m_bHandInteractionExt);
    AddExtIfSupported(XR_FB_TOUCH_CONTROLLER_PRO_EXTENSION_NAME, m_Extensions.m_bTouchControllerPro);
    AddExtIfSupported(XR_HTC_VIVE_COSMOS_CONTROLLER_INTERACTION_EXTENSION_NAME, m_Extensions.m_bViveCosmosController);
    AddExtIfSupported(XR_HTC_VIVE_FOCUS3_CONTROLLER_INTERACTION_EXTENSION_NAME, m_Extensions.m_bViveFocus3Controller);
    AddExtIfSupported(XR_BD_CONTROLLER_INTERACTION_EXTENSION_NAME, m_Extensions.m_bByteDanceController);
  }

  // Performance settings extension - allows setting performance hints
  AddExtIfSupported(XR_EXT_PERFORMANCE_SETTINGS_EXTENSION_NAME, m_Extensions.m_bPerformanceSettings);

  // XR_KHR_locate_spaces - batched space location queries for better performance
  AddExtIfSupported(XR_KHR_LOCATE_SPACES_EXTENSION_NAME, m_Extensions.m_bLocateSpaces);
  AddExtIfSupported(XR_HTCX_VIVE_TRACKER_INTERACTION_EXTENSION_NAME, m_Extensions.m_ViveTracker);

#if PL_ENABLED(PL_COMPILE_FOR_DEBUG)
  AddExtIfSupported(XR_EXT_DEBUG_UTILS_EXTENSION_NAME, m_Extensions.m_bDebugUtils);
#endif

#ifdef BUILDSYSTEM_ENABLE_OPENXR_REMOTING_SUPPORT
  AddExtIfSupported(XR_MSFT_HOLOGRAPHIC_REMOTING_EXTENSION_NAME, m_Extensions.m_bRemoting);
#endif

#ifdef BUILDSYSTEM_ENABLE_OPENXR_PREVIEW_SUPPORT
#endif
  return XR_SUCCESS;
}

XrResult plOpenXR::SelectLayers(plHybridArray<const char*, 6>& layers)
{
  plUInt32 layerCount;
  XR_SUCCEED_OR_RETURN_LOG(xrEnumerateApiLayerProperties(0, &layerCount, nullptr));
  std::vector<XrApiLayerProperties> layerProperties(layerCount, {XR_TYPE_API_LAYER_PROPERTIES});
  XR_SUCCEED_OR_RETURN_LOG(xrEnumerateApiLayerProperties(layerCount, &layerCount, layerProperties.data()));

  // Add a specific extension to the list of extensions to be enabled, if it is supported.
  auto AddExtIfSupported = [&](const char* layerName, bool& enableFlag) -> XrResult
  {
    auto it = std::find_if(begin(layerProperties), end(layerProperties), [&](const XrApiLayerProperties& prop)
      { return plStringUtils::IsEqual(prop.layerName, layerName); });
    if (it != end(layerProperties))
    {
      layers.PushBack(layerName);
      enableFlag = true;
      return XR_SUCCESS;
    }
    enableFlag = false;
    return XR_ERROR_EXTENSION_NOT_PRESENT;
  };

#if PL_ENABLED(PL_COMPILE_FOR_DEBUG)
  AddExtIfSupported("XR_APILAYER_LUNARG_core_validation", m_Extensions.m_bValidation);
#endif

  return XR_SUCCESS;
}

#define PL_GET_INSTANCE_PROC_ADDR(name) (void)xrGetInstanceProcAddr(m_pInstance, #name, reinterpret_cast<PFN_xrVoidFunction*>(&m_Extensions.pfn_##name));

plResult plOpenXR::Initialize()
{
  if (m_pInstance != XR_NULL_HANDLE)
  {
    // The instance survived the previous session's Deinitialize; only the system was released and must be re-acquired.
    if (m_SystemId == XR_NULL_SYSTEM_ID)
    {
      XrResult res = InitSystem();
      if (res != XR_SUCCESS)
      {
        plLog::Error("OpenXR: Re-acquiring the system for a new session failed: {}", res);
        return PL_FAILURE;
      }
    }

    return PL_SUCCESS;
  }

  // Build out the extensions to enable. Some extensions are required and some are optional.
  plHybridArray<const char*, 6> enabledExtensions;
  if (SelectExtensions(enabledExtensions) != XR_SUCCESS)
    return PL_FAILURE;

  plHybridArray<const char*, 6> enabledLayers;
  if (SelectLayers(enabledLayers) != XR_SUCCESS)
    return PL_FAILURE;

  // Create the instance with desired extensions.
  XrInstanceCreateInfo createInfo{XR_TYPE_INSTANCE_CREATE_INFO};
  createInfo.enabledExtensionCount = (uint32_t)enabledExtensions.GetCount();
  createInfo.enabledExtensionNames = enabledExtensions.GetData();
  createInfo.enabledApiLayerCount = (uint32_t)enabledLayers.GetCount();
  createInfo.enabledApiLayerNames = enabledLayers.GetData();

  plStringUtils::Copy(createInfo.applicationInfo.applicationName, PL_ARRAY_SIZE(createInfo.applicationInfo.applicationName), plApplication::GetApplicationInstance()->GetApplicationName());
  plStringUtils::Copy(createInfo.applicationInfo.engineName, PL_ARRAY_SIZE(createInfo.applicationInfo.engineName), "plEngine");
  createInfo.applicationInfo.engineVersion = 1;
  // Use OpenXR 1.0 API version for maximum runtime compatibility
  // XR_API_VERSION_1_0 = XR_MAKE_VERSION(1, 0, patch) where patch comes from the SDK version
  createInfo.applicationInfo.apiVersion = XR_API_VERSION_1_0;
  createInfo.applicationInfo.applicationVersion = 1;
  // Both lists go through every installed API layer, so record them before the call: if xrCreateInstance faults inside
  // a layer, this is the only record of what it was handed.
  LogAvailableApiLayers();
  for (const char* szExtension : enabledExtensions)
  {
    plLog::Info("OpenXR: Requesting extension {}", szExtension);
  }

  XrResult res = xrCreateInstance(&createInfo, &m_pInstance);
  if (res != XR_SUCCESS)
  {
    plLog::Error("InitSystem xrCreateInstance failed: {}", res);
    Deinitialize();
    return PL_FAILURE;
  }
  XrInstanceProperties instanceProperties{XR_TYPE_INSTANCE_PROPERTIES};
  res = xrGetInstanceProperties(m_pInstance, &instanceProperties);
  if (res != XR_SUCCESS)
  {
    plLog::Error("InitSystem xrGetInstanceProperties failed: {}", res);
    Deinitialize();
    return PL_FAILURE;
  }

  plStringBuilder sTemp;
  m_Info.m_sDeviceDriver = plConversionUtils::ToString(instanceProperties.runtimeVersion, sTemp);

  PL_GET_INSTANCE_PROC_ADDR(xrGetVulkanInstanceExtensionsKHR);
  PL_GET_INSTANCE_PROC_ADDR(xrGetVulkanDeviceExtensionsKHR);
  PL_GET_INSTANCE_PROC_ADDR(xrGetVulkanGraphicsDeviceKHR);
  PL_GET_INSTANCE_PROC_ADDR(xrGetVulkanGraphicsRequirementsKHR);

  if (m_Extensions.m_bD3D12)
  {
    PL_GET_INSTANCE_PROC_ADDR(xrGetD3D12GraphicsRequirementsKHR);
  }

  // Load vulkan_enable2 function pointers if the extension is available
  if (m_Extensions.m_bVulkan2)
  {
    PL_GET_INSTANCE_PROC_ADDR(xrCreateVulkanInstanceKHR);
    PL_GET_INSTANCE_PROC_ADDR(xrCreateVulkanDeviceKHR);
    PL_GET_INSTANCE_PROC_ADDR(xrGetVulkanGraphicsDevice2KHR);
    PL_GET_INSTANCE_PROC_ADDR(xrGetVulkanGraphicsRequirements2KHR);
  }

  if (m_Extensions.m_bSpatialAnchor)
  {
    PL_GET_INSTANCE_PROC_ADDR(xrCreateSpatialAnchorMSFT);
    PL_GET_INSTANCE_PROC_ADDR(xrCreateSpatialAnchorSpaceMSFT);
    PL_GET_INSTANCE_PROC_ADDR(xrDestroySpatialAnchorMSFT);
  }

  if (m_Extensions.m_bHandTracking)
  {
    PL_GET_INSTANCE_PROC_ADDR(xrCreateHandTrackerEXT);
    PL_GET_INSTANCE_PROC_ADDR(xrDestroyHandTrackerEXT);
    PL_GET_INSTANCE_PROC_ADDR(xrLocateHandJointsEXT);
  }

  if (m_Extensions.m_bHandTrackingMesh)
  {
    PL_GET_INSTANCE_PROC_ADDR(xrCreateHandMeshSpaceMSFT);
    PL_GET_INSTANCE_PROC_ADDR(xrUpdateHandMeshMSFT);
  }

  if (m_Extensions.m_bPerformanceSettings)
  {
    PL_GET_INSTANCE_PROC_ADDR(xrPerfSettingsSetPerformanceLevelEXT);
  }

  if (m_Extensions.m_bLocateSpaces)
  {
    PL_GET_INSTANCE_PROC_ADDR(xrLocateSpacesKHR);
    plLog::Info("OpenXR: Using XR_KHR_locate_spaces for batched space queries");
  }

#if PL_ENABLED(PL_COMPILE_FOR_DEBUG)
  if (m_Extensions.m_bDebugUtils)
  {
    PL_GET_INSTANCE_PROC_ADDR(xrCreateDebugUtilsMessengerEXT);
    PL_GET_INSTANCE_PROC_ADDR(xrDestroyDebugUtilsMessengerEXT);
  }
#endif

#ifdef BUILDSYSTEM_ENABLE_OPENXR_REMOTING_SUPPORT
  if (m_Extensions.m_bRemoting)
  {
    PL_GET_INSTANCE_PROC_ADDR(xrRemotingSetContextPropertiesMSFT);
    PL_GET_INSTANCE_PROC_ADDR(xrRemotingConnectMSFT);
    PL_GET_INSTANCE_PROC_ADDR(xrRemotingDisconnectMSFT);
    PL_GET_INSTANCE_PROC_ADDR(xrRemotingGetConnectionStateMSFT);
  }
#endif

  m_pInput = PL_DEFAULT_NEW(plOpenXRInputDevice, this);

  m_ExecutionEventsId = plGameApplicationBase::GetGameApplicationBaseInstance()->m_ExecutionEvents.AddEventHandler(plMakeDelegate(&plOpenXR::GameApplicationEventHandler, this));

  res = InitSystem();
  if (res != XR_SUCCESS)
  {
    plLog::Error("InitSystem failed: {}", res);
    Deinitialize();
    return PL_FAILURE;
  }

  plLog::Success("OpenXR {0} v{1} initialized successfully.", instanceProperties.runtimeName, instanceProperties.runtimeVersion);
  return PL_SUCCESS;
}

void plOpenXR::Deinitialize()
{
#ifdef BUILDSYSTEM_ENABLE_OPENXR_REMOTING_SUPPORT
  m_pRemoting->Disconnect().IgnoreResult();
#endif

  // Same ordering constraint as OnActorDestroyed: the queued xrDestroySwapchain has to be flushed while the session is
  // still alive.
  DestroySessionResources();

  DeinitSession();
  DeinitSystem();

}

bool plOpenXR::IsInitialized() const
{
  return m_pInstance != XR_NULL_HANDLE;
}

const plHMDInfo& plOpenXR::GetHmdInfo() const
{
  PL_ASSERT_DEV(IsInitialized(), "Need to call 'Initialize' first.");
  return m_Info;
}

plXRInputDevice& plOpenXR::GetXRInput() const
{
  return *(m_pInput.Borrow());
}

plUniquePtr<plActor> plOpenXR::CreateActor(plView* pView, plGALMSAASampleCount::Enum msaaCount, plUniquePtr<plWindowBase> pCompanionWindow, plUniquePtr<plWindowOutputTargetGAL> pCompanionWindowOutput)
{
  PL_ASSERT_DEV(IsInitialized(), "Need to call 'Initialize' first.");

  XrResult res = InitSession();
  if (res != XrResult::XR_SUCCESS)
  {
    plLog::Error("InitSession failed: {}", res);
    return {};
  }

  m_MsaaCount = msaaCount;

  PL_ASSERT_DEV(pView->GetCamera() != nullptr, "The provided view requires a camera to be set.");
  m_hView = pView->GetHandle();

  if (CreateSessionResources() != XrResult::XR_SUCCESS)
  {
    m_hView.Invalidate();
    DeinitSession();
    return {};
  }

  SetHMDCamera(pView->GetCamera());

  plUniquePtr<plActor> pActor = PL_DEFAULT_NEW(plActor, "OpenXR", this);

  PL_ASSERT_DEV((pCompanionWindow != nullptr) == (pCompanionWindowOutput != nullptr), "Both companionWindow and companionWindowOutput must either be null or valid.");
  PL_ASSERT_DEV(pCompanionWindow == nullptr || SupportsCompanionView(), "If a companionWindow is set, SupportsCompanionView() must be true.");

  plUniquePtr<plActorPluginWindowXR> pActorPlugin = PL_DEFAULT_NEW(plActorPluginWindowXR, this, std::move(pCompanionWindow), std::move(pCompanionWindowOutput));
  m_pCompanion = static_cast<plWindowOutputTargetXR*>(pActorPlugin->GetOutputTarget());
  pActor->AddPlugin(std::move(pActorPlugin));

  return std::move(pActor);
}

XrResult plOpenXR::CreateSessionResources()
{
  PL_ASSERT_DEV(m_pSession != XR_NULL_HANDLE, "The session must exist before its resources can be created.");

  plView* pView = nullptr;
  if (!plRenderWorld::TryGetView(m_hView, pView))
  {
    plLog::Error("OpenXR: Cannot create session resources, the XR view is gone.");
    return XR_ERROR_INITIALIZATION_FAILED;
  }

  const plGALMSAASampleCount::Enum msaaCount = m_MsaaCount;
  plGALXRSwapChain::SetFactoryMethod([this, msaaCount](plXRInterface* pXrInterface) -> plGALSwapChainHandle
    { return plGALDevice::GetDefaultDevice()->CreateSwapChain([this, pXrInterface, msaaCount](plAllocator* pAllocator) -> plGALSwapChain*
        { return PL_NEW(pAllocator, plGALOpenXRSwapChain, this, msaaCount); }); });
  PL_SCOPE_EXIT(plGALXRSwapChain::SetFactoryMethod({}););

  m_hSwapChain = plGALXRSwapChain::Create(this);
  if (m_hSwapChain.IsInvalidated())
  {
    plLog::Error("OpenXR: Creating the XR swap chain failed.");
    return XR_ERROR_INITIALIZATION_FAILED;
  }

  const plGALOpenXRSwapChain* pSwapChain = static_cast<const plGALOpenXRSwapChain*>(plGALDevice::GetDefaultDevice()->GetSwapChain(m_hSwapChain));
  m_Info.m_vEyeRenderTargetSize = pSwapChain->GetRenderTargetSize();

  pView->SetSwapChain(m_hSwapChain);

  // Both viewports are the eye render target: plGameState::SetupMainView skips render-target setup for XR, so nothing
  // else sets them. Leaving the target viewport at its 0x0 default makes plGlobalConstants::TargetViewportSize carry
  // 1/0 and forces SyncViewportFromSwapChain to correct it mid-frame, after the graph already read it.
  const plRectFloat eyeViewport(0.0f, 0.0f, (float)m_Info.m_vEyeRenderTargetSize.width, (float)m_Info.m_vEyeRenderTargetSize.height);
  pView->SetViewport(eyeViewport);
  pView->SetTargetViewport(eyeViewport);

  return XrResult::XR_SUCCESS;
}

void plOpenXR::DestroySessionResources()
{
  if (m_hSwapChain.IsInvalidated())
    return;

  plGALDevice* pDevice = plGALDevice::GetDefaultDevice();
  pDevice->DestroySwapChain(m_hSwapChain);
  m_hSwapChain.Invalidate();

  // DestroySwapChain only queues the destruction; WaitIdle runs it (DestroyDeadObjects -> DeInitPlatform ->
  // xrDestroySwapchain). It has to happen while the session is still alive: xrDestroySession implicitly destroys the
  // session's swapchains, so a later flush would hand xrDestroySwapchain a dangling handle.
  pDevice->WaitIdle();
}

void plOpenXR::OnActorDestroyed()
{
  if (m_hView.IsInvalidated())
    return;

  m_pCompanion = nullptr;
  SetHMDCamera(nullptr);

  plRenderWorld::RemoveMainView(m_hView);
  m_hView.Invalidate();

  // Must run before DeinitSession: without it the flush arrives via the companion output target's own WaitIdle during
  // member teardown - after the session is gone - and crashes.
  DestroySessionResources();

  DeinitSession();
}

bool plOpenXR::SupportsCompanionView()
{
#if PL_ENABLED(PL_PLATFORM_WINDOWS_DESKTOP)
  return true;
#else
  // E.g. on UWP OpenXR creates its own main window and other resources that conflict with our window.
  // Thus we must prevent the creation of a companion view or OpenXR crashes.
  return false;
#endif
}

XrSpace plOpenXR::GetBaseSpace() const
{
  return m_StageSpace == plXRStageSpace::Standing ? m_pSceneSpace : m_pLocalSpace;
}

XrResult plOpenXR::InitSystem()
{
  PL_ASSERT_DEV(m_SystemId == XR_NULL_SYSTEM_ID, "OpenXR actor already exists.");
  XrSystemGetInfo systemInfo{XR_TYPE_SYSTEM_GET_INFO};
  systemInfo.formFactor = XrFormFactor::XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
  XR_SUCCEED_OR_CLEANUP_LOG(xrGetSystem(m_pInstance, &systemInfo, &m_SystemId), DeinitSystem);

  XrSystemProperties systemProperties{XR_TYPE_SYSTEM_PROPERTIES};
  XR_SUCCEED_OR_CLEANUP_LOG(xrGetSystemProperties(m_pInstance, m_SystemId, &systemProperties), DeinitSystem);
  m_Info.m_sDeviceName = systemProperties.systemName;

  return XrResult::XR_SUCCESS;
}

void plOpenXR::DeinitSystem()
{
  m_SystemId = XR_NULL_SYSTEM_ID;
}

XrResult plOpenXR::InitSession()
{
  PL_ASSERT_DEV(m_pSession == XR_NULL_HANDLE, "");

  plUInt32 count;
  XR_SUCCEED_OR_CLEANUP_LOG(xrEnumerateEnvironmentBlendModes(m_pInstance, m_SystemId, m_PrimaryViewConfigurationType, 0, &count, nullptr), DeinitSystem);

  plHybridArray<XrEnvironmentBlendMode, 4> environmentBlendModes;
  environmentBlendModes.SetCount(count);
  XR_SUCCEED_OR_CLEANUP_LOG(xrEnumerateEnvironmentBlendModes(m_pInstance, m_SystemId, m_PrimaryViewConfigurationType, count, &count, environmentBlendModes.GetData()), DeinitSession);

  // Pick the blend mode the project asked for. Taking environmentBlendModes[0] unconditionally would put a VR title
  // into passthrough compositing on any device that happens to enumerate an AR mode first.
  {
    XrEnvironmentBlendMode wanted = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    if (const plXRConfig* pConfig = plGameApplicationBase::GetGameApplicationBaseInstance()->GetPlatformProfile().GetTypeConfig<plXRConfig>())
    {
      switch (pConfig->m_BlendMode)
      {
        case plXREnvironmentBlendMode::Additive:
          wanted = XR_ENVIRONMENT_BLEND_MODE_ADDITIVE;
          break;
        case plXREnvironmentBlendMode::AlphaBlend:
          wanted = XR_ENVIRONMENT_BLEND_MODE_ALPHA_BLEND;
          break;
        default:
          wanted = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
          break;
      }
    }

    if (environmentBlendModes.Contains(wanted))
    {
      m_BlendMode = wanted;
    }
    else
    {
      m_BlendMode = environmentBlendModes[0];
      plLog::Warning("OpenXR: The configured environment blend mode is not supported by this runtime, falling back to the runtime's first choice ({}).", (int)m_BlendMode);
    }
  }

  XR_SUCCEED_OR_CLEANUP_LOG(InitGraphicsPlugin(), DeinitSession);

  if (m_Extensions.m_bDebugUtils)
  {
    XR_SUCCEED_OR_CLEANUP_LOG(InitDebugMessenger(), DeinitSession);
  }

  XrSessionCreateInfo sessionCreateInfo{XR_TYPE_SESSION_CREATE_INFO};
  sessionCreateInfo.systemId = m_SystemId;
  if (m_GraphicsApi == GraphicsApi::D3D12)
  {
    sessionCreateInfo.next = &m_XrGraphicsBindingD3D12;
  }
  else
  {
    sessionCreateInfo.next = &m_XrGraphicsBindingVulkan;
  }

  XR_SUCCEED_OR_CLEANUP_LOG(xrCreateSession(m_pInstance, &sessionCreateInfo, &m_pSession), DeinitSession);

  XrReferenceSpaceCreateInfo spaceCreateInfo{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
  spaceCreateInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_STAGE;
  spaceCreateInfo.poseInReferenceSpace = ConvertTransform(plTransform::MakeIdentity());
  XR_SUCCEED_OR_CLEANUP_LOG(xrCreateReferenceSpace(m_pSession, &spaceCreateInfo, &m_pSceneSpace), DeinitSession);

  spaceCreateInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
  XR_SUCCEED_OR_CLEANUP_LOG(xrCreateReferenceSpace(m_pSession, &spaceCreateInfo, &m_pLocalSpace), DeinitSession);

  XR_SUCCEED_OR_CLEANUP_LOG(m_pInput->CreateActions(m_pSession, m_pSceneSpace), DeinitSession);
  XR_SUCCEED_OR_CLEANUP_LOG(m_pInput->AttachSessionActionSets(m_pSession), DeinitSession);

  // Set performance hints to request sustained high performance from the runtime.
  // This may help prevent aggressive throttling by runtimes like SteamVR.
  if (m_Extensions.m_bPerformanceSettings && m_Extensions.pfn_xrPerfSettingsSetPerformanceLevelEXT)
  {
    XrResult cpuResult = m_Extensions.pfn_xrPerfSettingsSetPerformanceLevelEXT(m_pSession, XR_PERF_SETTINGS_DOMAIN_CPU_EXT, XR_PERF_SETTINGS_LEVEL_SUSTAINED_HIGH_EXT);
    XrResult gpuResult = m_Extensions.pfn_xrPerfSettingsSetPerformanceLevelEXT(m_pSession, XR_PERF_SETTINGS_DOMAIN_GPU_EXT, XR_PERF_SETTINGS_LEVEL_SUSTAINED_HIGH_EXT);
    if (cpuResult == XR_SUCCESS && gpuResult == XR_SUCCESS)
    {
      plLog::Info("OpenXR: Set performance level to SUSTAINED_HIGH for CPU and GPU");
    }
  }

  m_GALdeviceEventsId = plGALDevice::s_Events.AddEventHandler(plMakeDelegate(&plOpenXR::GALDeviceEventHandler, this));

  SetStageSpace(plXRStageSpace::Standing);
  if (m_Extensions.m_bSpatialAnchor)
  {
    m_pAnchors = PL_DEFAULT_NEW(plOpenXRSpatialAnchors, this);
  }
  if (m_Extensions.m_bHandTracking && plOpenXRHandTracking::IsHandTrackingSupported(this))
  {
    m_pHandTracking = PL_DEFAULT_NEW(plOpenXRHandTracking, this);
  }
  return XrResult::XR_SUCCESS;
}

void plOpenXR::DeinitSession()
{
  // m_pCompanion is owned by the actor plugin, which outlives a session restart - only OnActorDestroyed clears it.
  m_bSessionRunning = false;
  m_bExitRenderLoop = false;
  m_bRequestRestart = false;
  m_bRenderInProgress = false;
  m_bPreWaitComplete = false;
  m_PreWaitResult = XR_SUCCESS;
  m_iPredictedDisplayTime = 0;
  m_SessionState = XR_SESSION_STATE_UNKNOWN;

  m_pHandTracking = nullptr;
  m_pAnchors = nullptr;
  if (m_GALdeviceEventsId != 0)
  {
    plGALDevice::s_Events.RemoveEventHandler(m_GALdeviceEventsId);
  }

  // Must come before the reference spaces are destroyed: DestroyActions stops the input thread, which locates poses
  // against GetBaseSpace() until it is joined.
  m_pInput->DestroyActions();

  if (m_pSceneSpace)
  {
    xrDestroySpace(m_pSceneSpace);
    m_pSceneSpace = XR_NULL_HANDLE;
  }

  if (m_pLocalSpace)
  {
    xrDestroySpace(m_pLocalSpace);
    m_pLocalSpace = XR_NULL_HANDLE;
  }

  if (m_pSession)
  {
    // The session's swapchain images are GAL textures that in-flight command buffers may still reference, and
    // xrDestroySession implicitly destroys those swapchains. Flush first so nothing is submitted against freed images.
    if (plGALDevice::HasDefaultDevice())
    {
      plGALDevice::GetDefaultDevice()->WaitIdle();
    }

    xrDestroySession(m_pSession);
    m_pSession = XR_NULL_HANDLE;
  }

  DeinitGraphicsPlugin();
  if (m_Extensions.m_bDebugUtils)
  {
    DeinitInitDebugMessenger();
  }
}

XrResult plOpenXR::InitGraphicsPlugin()
{
  // The binding extension was fixed at instance creation; a renderer that differs from it cannot bind a session.
  const plStringView sRenderer = plGALDevice::GetDefaultDevice()->GetRenderer();
  const GraphicsApi deviceApi = sRenderer.IsEqual_NoCase("DX12") ? GraphicsApi::D3D12 : GraphicsApi::Vulkan;
  if (deviceApi != m_GraphicsApi)
  {
    plLog::Error("OpenXR: The instance was created for {} but the renderer is '{}'.", m_GraphicsApi == GraphicsApi::D3D12 ? "D3D12" : "Vulkan", sRenderer);
    return XR_ERROR_GRAPHICS_DEVICE_INVALID;
  }

  return m_GraphicsApi == GraphicsApi::D3D12 ? InitGraphicsPluginD3D12() : InitGraphicsPluginVulkan();
}

XrResult plOpenXR::InitGraphicsPluginD3D12()
{
  PL_ASSERT_DEV(m_XrGraphicsBindingD3D12.device == nullptr, "");

  plGALDeviceDX12* pDX12Device = static_cast<plGALDeviceDX12*>(plGALDevice::GetDefaultDevice());

  // Required before xrCreateSession. Names the adapter the headset is connected to.
  XrGraphicsRequirementsD3D12KHR graphicsRequirements{XR_TYPE_GRAPHICS_REQUIREMENTS_D3D12_KHR};
  XR_SUCCEED_OR_CLEANUP_LOG(m_Extensions.pfn_xrGetD3D12GraphicsRequirementsKHR(m_pInstance, m_SystemId, &graphicsRequirements), DeinitGraphicsPlugin);

  const LUID& adapterLuid = pDX12Device->GetAdapterDesc().AdapterLuid;
  if (adapterLuid.LowPart != graphicsRequirements.adapterLuid.LowPart || adapterLuid.HighPart != graphicsRequirements.adapterLuid.HighPart)
  {
    plLog::Warning("OpenXR: The headset is connected to a different adapter than the D3D12 device in use ('{}'). The runtime may refuse the session.", pDX12Device->GetCapabilities().m_sAdapterName);
  }

  m_XrGraphicsBindingD3D12.device = pDX12Device->GetD3DDevice();
  m_XrGraphicsBindingD3D12.queue = pDX12Device->GetGraphicsQueue().m_pQueue;

  return XrResult::XR_SUCCESS;
}

XrResult plOpenXR::InitGraphicsPluginVulkan()
{
  PL_ASSERT_DEV(m_XrGraphicsBindingVulkan.device == VK_NULL_HANDLE, "");
  
  plGALDevice* pDevice = plGALDevice::GetDefaultDevice();
  plGALDeviceVulkan* pVulkanDevice = static_cast<plGALDeviceVulkan*>(pDevice);

  // Get Vulkan instance from the device
  VkInstance vkInstance = (VkInstance)pVulkanDevice->GetVulkanInstance();
  VkPhysicalDevice xrPhysicalDevice = VK_NULL_HANDLE;
  
  if (m_Extensions.m_bVulkan2)
  {
    // Use vulkan_enable2 extension for better GPU synchronization
    XrGraphicsRequirementsVulkan2KHR graphicsRequirements{XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN2_KHR};
    XrVulkanGraphicsDeviceGetInfoKHR deviceGetInfo{XR_TYPE_VULKAN_GRAPHICS_DEVICE_GET_INFO_KHR};
    deviceGetInfo.systemId = m_SystemId;
    deviceGetInfo.vulkanInstance = vkInstance;
    
    XR_SUCCEED_OR_CLEANUP_LOG(m_Extensions.pfn_xrGetVulkanGraphicsRequirements2KHR(m_pInstance, m_SystemId, &graphicsRequirements), DeinitGraphicsPlugin);
    XR_SUCCEED_OR_CLEANUP_LOG(m_Extensions.pfn_xrGetVulkanGraphicsDevice2KHR(m_pInstance, &deviceGetInfo, &xrPhysicalDevice), DeinitGraphicsPlugin);
  }
  else
  {
    // Fall back to vulkan_enable extension
    XrGraphicsRequirementsVulkanKHR graphicsRequirements{XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN_KHR};
    XR_SUCCEED_OR_CLEANUP_LOG(m_Extensions.pfn_xrGetVulkanGraphicsRequirementsKHR(m_pInstance, m_SystemId, &graphicsRequirements), DeinitGraphicsPlugin);
    XR_SUCCEED_OR_CLEANUP_LOG(m_Extensions.pfn_xrGetVulkanGraphicsDeviceKHR(m_pInstance, m_SystemId, vkInstance, &xrPhysicalDevice), DeinitGraphicsPlugin);
  }
  
  // Verify that OpenXR selected the same physical device we're using
  VkPhysicalDevice ourPhysicalDevice = (VkPhysicalDevice)pVulkanDevice->GetVulkanPhysicalDevice();
  if (xrPhysicalDevice != ourPhysicalDevice)
  {
    plLog::Warning("OpenXR selected a different Vulkan physical device than the one in use. This may cause issues.");
  }

  m_XrGraphicsBindingVulkan.instance = vkInstance;
  m_XrGraphicsBindingVulkan.physicalDevice = xrPhysicalDevice;
  m_XrGraphicsBindingVulkan.device = (VkDevice)pVulkanDevice->GetVulkanDevice();
  m_XrGraphicsBindingVulkan.queueFamilyIndex = pVulkanDevice->GetGraphicsQueue().m_uiQueueFamily;
  m_XrGraphicsBindingVulkan.queueIndex = pVulkanDevice->GetGraphicsQueue().m_uiQueueIndex;

  return XrResult::XR_SUCCESS;
}

void plOpenXR::DeinitGraphicsPlugin()
{
  m_XrGraphicsBindingVulkan.device = VK_NULL_HANDLE;
  m_XrGraphicsBindingD3D12.device = nullptr;
  m_XrGraphicsBindingD3D12.queue = nullptr;
}

XrResult plOpenXR::InitDebugMessenger()
{
  XrDebugUtilsMessengerCreateInfoEXT create_info{XR_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
  create_info.messageSeverities = XR_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT |
                                  XR_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT |
                                  XR_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                                  XR_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
  create_info.messageTypes = XR_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                             XR_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                             XR_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
  create_info.userCallback = xrDebugCallback;

  XR_SUCCEED_OR_CLEANUP_LOG(m_Extensions.pfn_xrCreateDebugUtilsMessengerEXT(m_pInstance, &create_info, &m_pDebugMessenger), DeinitInitDebugMessenger);

  return XrResult::XR_SUCCESS;
}

void plOpenXR::DeinitInitDebugMessenger()
{
  if (m_pDebugMessenger != XR_NULL_HANDLE)
  {
    XR_LOG_ERROR(m_Extensions.pfn_xrDestroyDebugUtilsMessengerEXT(m_pDebugMessenger));
    m_pDebugMessenger = XR_NULL_HANDLE;
  }
}

void plOpenXR::BeforeUpdatePlugins()
{
  PL_PROFILE_SCOPE("BeforeUpdatePlugins");

  // Caching the stage transform BEFORE world simulation runs prevents jitter caused by the timing mismatch between XR
  // pose prediction and world transform updates (especially noticeable in editor mode).
  m_bStageTransformCached = false;
  if (plWorld* pWorld = GetWorld())
  {
    bool bNeedsStereoMode = false;

    {
      // Must go through a const plWorld: the non-const GetComponentManager overload asserts WRITE access, so calling it
      // under a read marker trips plWorld::CheckForWriteAccess.
      const plWorld* pReadOnlyWorld = pWorld;

      PL_LOCK(pReadOnlyWorld->GetReadMarker());

      if (const plStageSpaceComponentManager* pStageMan = pReadOnlyWorld->GetComponentManager<plStageSpaceComponentManager>())
      {
        if (const plStageSpaceComponent* pStage = pStageMan->GetSingletonComponent())
        {
          m_CachedStageTransform = pStage->GetOwner()->GetGlobalTransform();
          m_bStageTransformCached = true;
        }
      }

      if (const plCameraComponentManager* pCCM = pReadOnlyWorld->GetComponentManager<plCameraComponentManager>())
      {
        if (const plCameraComponent* pCameraComponent = pCCM->GetCameraByUsageHint(plCameraUsageHint::MainView))
        {
          bNeedsStereoMode = pCameraComponent->GetCameraMode() != plCameraMode::Stereo;
        }
      }
    }

    // Only escalate to a write lock when the mode actually has to change; this runs on a task worker every frame and
    // the write marker serialises against the whole world update.
    if (bNeedsStereoMode)
    {
      PL_LOCK(pWorld->GetWriteMarker());

      if (auto* pCCM = pWorld->GetComponentManager<plCameraComponentManager>())
      {
        if (plCameraComponent* pCameraComponent = pCCM->GetCameraByUsageHint(plCameraUsageHint::MainView))
        {
          pCameraComponent->SetCameraMode(plCameraMode::Stereo);
        }
      }
    }
  }

  XrEventDataBuffer event{XR_TYPE_EVENT_DATA_BUFFER, nullptr};

  while (xrPollEvent(m_pInstance, &event) == XR_SUCCESS)
  {
#ifdef BUILDSYSTEM_ENABLE_OPENXR_REMOTING_SUPPORT
    m_pRemoting->HandleEvent(event);
#endif
    switch (event.type)
    {
      case XR_TYPE_EVENT_DATA_INTERACTION_PROFILE_CHANGED:
      {
        m_pInput->UpdateCurrentInteractionProfile();
      }
      break;
      case XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED:
      {
        const XrEventDataSessionStateChanged& session_state_changed_event = *reinterpret_cast<XrEventDataSessionStateChanged*>(&event);
        m_SessionState = session_state_changed_event.state;
        switch (m_SessionState)
        {
          case XR_SESSION_STATE_READY:
          {
            XrSessionBeginInfo sessionBeginInfo{XR_TYPE_SESSION_BEGIN_INFO};
            sessionBeginInfo.primaryViewConfigurationType = m_PrimaryViewConfigurationType;
            if (xrBeginSession(m_pSession, &sessionBeginInfo) == XR_SUCCESS)
            {
              m_bSessionRunning = true;
            }
            break;
          }
          case XR_SESSION_STATE_STOPPING:
          {
            m_bSessionRunning = false;
            if (xrEndSession(m_pSession) != XR_SUCCESS)
            {
              // TODO log
            }
            break;
          }
          case XR_SESSION_STATE_EXITING:
          {
            // Do not attempt to restart because user closed this session.
            m_bExitRenderLoop = true;
            m_bRequestRestart = false;
            break;
          }
          case XR_SESSION_STATE_LOSS_PENDING:
          {
            // Poll for a new systemId
            m_bExitRenderLoop = true;
            m_bRequestRestart = true;
            break;
          }
          default:
            break;
        }
      }
      break;
      case XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING:
      {
        const XrEventDataInstanceLossPending& instance_loss_pending_event = *reinterpret_cast<XrEventDataInstanceLossPending*>(&event);
        PL_IGNORE_UNUSED(instance_loss_pending_event);
        m_bExitRenderLoop = true;
        m_bRequestRestart = false;
        m_bInstanceLost = true;
      }
      break;
      default:
        break;
    }
    event = {XR_TYPE_EVENT_DATA_BUFFER, nullptr};
  }

  if (m_bExitRenderLoop)
  {
    // This function runs on a task worker (Run_UpdatePlugins is called from inside UpdateWorldsAndExtractViews), so it
    // must not destroy or create GAL resources here. Hand the work to the main thread and clear the request, otherwise
    // it latches and re-fires every frame.
    m_bSessionRunning = false;
    m_bExitRenderLoop = false;

    if (m_bRequestRestart)
    {
      m_bRequestRestart = false;
      m_bRestartPending = true;
    }
    else
    {
      m_bShutdownPending = true;
    }
  }
}

void plOpenXR::ShutdownSession()
{
  PL_PROFILE_SCOPE("ShutdownSession");

  // The runtime asked us to stop: either the user closed the session from its overlay (XR_SESSION_STATE_EXITING) or the
  // instance is going away. Release the session so the runtime gets the HMD back instead of holding it until the
  // process exits.
  DestroySessionResources();
  DeinitSession();
  DeinitSystem();

  if (m_bInstanceLost)
  {
    // Nothing may be called on a lost instance, not even xrPollEvent, so drop it. XR stays down until something calls
    // Initialize again.
    if (m_pInstance != XR_NULL_HANDLE)
    {
      xrDestroyInstance(m_pInstance);
      m_pInstance = XR_NULL_HANDLE;
    }
    m_bInstanceLost = false;
    plLog::Warning("OpenXR: The runtime instance was lost, XR has been shut down.");
  }
  else
  {
    plLog::Info("OpenXR: The runtime ended the session, XR has been shut down.");
  }
}

void plOpenXR::RestartSession()
{
  PL_PROFILE_SCOPE("RestartSession");
  plLog::Info("OpenXR: Restarting the session after runtime or device loss.");

  const bool bHadView = !m_hView.IsInvalidated();
  plCamera* pCamera = m_pCameraToSynchronize;

  DestroySessionResources();
  DeinitSession();
  DeinitSystem();

  if (InitSystem() != XR_SUCCESS)
  {
    plLog::Error("OpenXR: Could not re-acquire a system after session loss, XR stays down.");
    return;
  }

  if (InitSession() != XR_SUCCESS)
  {
    plLog::Error("OpenXR: Could not re-create the session after loss, XR stays down.");
    DeinitSystem();
    return;
  }

  if (bHadView && CreateSessionResources() != XrResult::XR_SUCCESS)
  {
    plLog::Error("OpenXR: Could not re-create the session's resources after loss, XR stays down.");
    DeinitSession();
    DeinitSystem();
    return;
  }

  // SetHMDCamera early-outs when the pointer is unchanged, so force the projection to be rebuilt for the new session.
  m_pCameraToSynchronize = nullptr;
  SetHMDCamera(pCamera);
  m_bProjectionChanged = true;

  plLog::Success("OpenXR: Session restarted.");
}

void plOpenXR::UpdatePoses()
{
  PL_ASSERT_DEV(IsInitialized(), "Need to call 'Initialize' first.");

  PL_PROFILE_SCOPE("UpdatePoses");
  m_ViewState = XrViewState{XR_TYPE_VIEW_STATE};
  plUInt32 viewCapacityInput = 2;
  plUInt32 viewCountOutput;

  XrViewLocateInfo viewLocateInfo{XR_TYPE_VIEW_LOCATE_INFO};
  viewLocateInfo.viewConfigurationType = m_PrimaryViewConfigurationType;
  viewLocateInfo.displayTime = m_FrameState.predictedDisplayTime;
  viewLocateInfo.space = GetBaseSpace();
  m_Views[0].type = XR_TYPE_VIEW;
  m_Views[1].type = XR_TYPE_VIEW;
  XrFovf previousFov[2];
  previousFov[0] = m_Views[0].fov;
  previousFov[1] = m_Views[1].fov;

  XrResult res = xrLocateViews(m_pSession, &viewLocateInfo, &m_ViewState, viewCapacityInput, &viewCountOutput, m_Views);

  if (!XR_FAILED_RESULT(res))
  {
    m_pInput->m_DeviceState[0].m_bGripPoseIsValid = ((m_ViewState.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT) && (m_ViewState.viewStateFlags & XR_VIEW_STATE_POSITION_VALID_BIT));
    m_pInput->m_DeviceState[0].m_bAimPoseIsValid = m_pInput->m_DeviceState[0].m_bGripPoseIsValid;
  }
  else
  {
    m_pInput->m_DeviceState[0].m_bGripPoseIsValid = false;
    m_pInput->m_DeviceState[0].m_bAimPoseIsValid = false;
  }

  // Needed as workaround for broken XR runtimes.
  auto FovIsNull = [](const XrFovf& fov)
  {
    return fov.angleLeft == 0.0f && fov.angleRight == 0.0f && fov.angleDown == 0.0f && fov.angleUp == 0.0f;
  };

  auto IdentityFov = [](XrFovf& fov)
  {
    fov.angleLeft = -plAngle::MakeFromDegree(45.0f).GetRadian();
    fov.angleRight = plAngle::MakeFromDegree(45.0f).GetRadian();
    fov.angleUp = plAngle::MakeFromDegree(45.0f).GetRadian();
    fov.angleDown = -plAngle::MakeFromDegree(45.0f).GetRadian();
  };

  if (FovIsNull(m_Views[0].fov) || FovIsNull(m_Views[1].fov))
  {
    IdentityFov(m_Views[0].fov);
    IdentityFov(m_Views[1].fov);
  }

  m_bProjectionChanged = plMemoryUtils::Compare(&previousFov[0], &m_Views[0].fov, 1) != 0 || plMemoryUtils::Compare(&previousFov[1], &m_Views[1].fov, 1) != 0;

  for (plUInt32 uiEyeIndex : {0, 1})
  {
    plQuat rot = ConvertOrientation(m_Views[uiEyeIndex].pose.orientation);
    if (!rot.IsValid())
    {
      m_Views[uiEyeIndex].pose.orientation = XrQuaternionf{0, 0, 0, 1};
    }
  }

  UpdateCamera();
  m_pInput->UpdateActions();

  if (m_pHandTracking)
  {
    m_pHandTracking->UpdateJointTransforms();
  }
}

plResult plOpenXR::TriggerHapticPulse(plXRDeviceID deviceID, plTime duration, float fAmplitude, float fFrequencyHz)
{
  if (m_pSession == XR_NULL_HANDLE || m_pInput == nullptr)
    return PL_FAILURE;

  return m_pInput->TriggerHapticPulse(deviceID, duration, fAmplitude, fFrequencyHz);
}

void plOpenXR::UpdateCamera()
{
  if (!m_pCameraToSynchronize)
  {
    return;
  }
  // Update camera projection
  if (m_uiSettingsModificationCounter != m_pCameraToSynchronize->GetSettingsModificationCounter() || m_bProjectionChanged)
  {
    m_bProjectionChanged = false;
    const float fAspectRatio = (float)m_Info.m_vEyeRenderTargetSize.width / (float)m_Info.m_vEyeRenderTargetSize.height;
    auto CreateProjection = [](const XrView& view, plCamera* cam)
    {
      return plGraphicsUtils::CreatePerspectiveProjectionMatrix(plMath::Tan(plAngle::MakeFromRadian(view.fov.angleLeft)) * cam->GetNearPlane(), plMath::Tan(plAngle::MakeFromRadian(view.fov.angleRight)) * cam->GetNearPlane(), plMath::Tan(plAngle::MakeFromRadian(view.fov.angleDown)) * cam->GetNearPlane(),
        plMath::Tan(plAngle::MakeFromRadian(view.fov.angleUp)) * cam->GetNearPlane(), cam->GetNearPlane(), cam->GetFarPlane());
    };

    // Update projection with newest near/ far values. If not sync camera is set, just use the last value from XR
    // camera.
    const plMat4 projLeft = CreateProjection(m_Views[0], m_pCameraToSynchronize);
    const plMat4 projRight = CreateProjection(m_Views[1], m_pCameraToSynchronize);
    m_pCameraToSynchronize->SetStereoProjection(projLeft, projRight, fAspectRatio);
    m_uiSettingsModificationCounter = m_pCameraToSynchronize->GetSettingsModificationCounter();
  }

  // Update camera view
  {
    plTransform add;
    add.SetIdentity();
    
    // Use cached stage transform from BeforeUpdatePlugins to prevent jitter.
    // The stage transform was captured before world simulation, ensuring it's
    // consistent with the XR pose prediction timing.
    if (m_bStageTransformCached)
    {
      add = m_CachedStageTransform;
    }
    
    // Update stage space setting if needed
    plView* pView = nullptr;
    if (plRenderWorld::TryGetView(m_hView, pView))
    {
      if (const plWorld* pWorld = pView->GetWorld())
      {
        PL_LOCK(pWorld->GetReadMarker());
        if (const plStageSpaceComponentManager* pStageMan = pWorld->GetComponentManager<plStageSpaceComponentManager>())
        {
          if (const plStageSpaceComponent* pStage = pStageMan->GetSingletonComponent())
          {
            plEnum<plXRStageSpace> stageSpace = pStage->GetStageSpace();
            if (m_StageSpace != stageSpace)
              SetStageSpace(pStage->GetStageSpace());
          }
        }
      }
    }

    if (m_pInput->m_DeviceState[0].m_bGripPoseIsValid)
    {
      // Update device state (average of both eyes).
      const plQuat rot = plQuat::MakeSlerp(ConvertOrientation(m_Views[0].pose.orientation), ConvertOrientation(m_Views[1].pose.orientation), 0.5f);
      const plVec3 pos = plMath::Lerp(ConvertPosition(m_Views[0].pose.position), ConvertPosition(m_Views[1].pose.position), 0.5f);

      m_pInput->m_DeviceState[0].m_vGripPosition = pos;
      m_pInput->m_DeviceState[0].m_qGripRotation = rot;
      m_pInput->m_DeviceState[0].m_vAimPosition = pos;
      m_pInput->m_DeviceState[0].m_qAimRotation = rot;
      m_pInput->m_DeviceState[0].m_Type = plXRDeviceType::HMD;
      m_pInput->m_DeviceState[0].m_bGripPoseIsValid = true;
      m_pInput->m_DeviceState[0].m_bAimPoseIsValid = true;
      m_pInput->m_DeviceState[0].m_bDeviceIsConnected = true;
    }

    // Set view matrix
    if (m_pInput->m_DeviceState[0].m_bGripPoseIsValid)
    {
      const plMat4 mStageTransform = add.GetAsMat4();
      const plMat4 poseLeft = mStageTransform * ConvertPoseToMatrix(m_Views[0].pose);
      const plMat4 poseRight = mStageTransform * ConvertPoseToMatrix(m_Views[1].pose);

      // PL Forward is +X, need to add this to align the forward projection
      const plMat4 viewMatrix = plGraphicsUtils::CreateLookAtViewMatrix(plVec3::MakeZero(), plVec3(1, 0, 0), plVec3(0, 0, 1));
      const plMat4 mViewTransformLeft = viewMatrix * poseLeft.GetInverse();
      const plMat4 mViewTransformRight = viewMatrix * poseRight.GetInverse();

      m_pCameraToSynchronize->SetViewMatrix(mViewTransformLeft, plCameraEye::Left);
      m_pCameraToSynchronize->SetViewMatrix(mViewTransformRight, plCameraEye::Right);
    }
  }
}

void plOpenXR::BeginFrame()
{
  if (m_hView.IsInvalidated() || !m_bSessionRunning)
    return;

  PL_PROFILE_SCOPE("OpenXrBeginFrame");

  // Use pre-waited frame state if available from previous EndFrame
  if (m_bPreWaitComplete)
  {
    m_bPreWaitComplete = false;
    if (XR_FAILED_RESULT(m_PreWaitResult))
    {
      m_bRenderInProgress = false;
      return;
    }
    // m_FrameState already populated by EndFrame
  }
  else
  {
    // Fallback: wait here if this is the first frame or EndFrame wasn't called
    PL_PROFILE_SCOPE("xrWaitFrame");
    m_FrameWaitInfo = XrFrameWaitInfo{XR_TYPE_FRAME_WAIT_INFO};
    m_FrameState = XrFrameState{XR_TYPE_FRAME_STATE};
    XrResult result = xrWaitFrame(m_pSession, &m_FrameWaitInfo, &m_FrameState);
    if (XR_FAILED_RESULT(result))
    {
      m_iPredictedDisplayTime = 0;
      m_bRenderInProgress = false;
      return;
    }
    m_iPredictedDisplayTime = (plInt64)m_FrameState.predictedDisplayTime;
  }

  {
    PL_PROFILE_SCOPE("xrBeginFrame");
    m_FrameBeginInfo = XrFrameBeginInfo{XR_TYPE_FRAME_BEGIN_INFO};
    XrResult result = xrBeginFrame(m_pSession, &m_FrameBeginInfo);
    if (XR_FAILED_RESULT(result))
    {
      m_bRenderInProgress = false;
      return;
    }

    // XR_FRAME_DISCARDED is a success code: the runtime dropped a frame we began but never ended, and this frame
    // takes its place. Bailing out here instead would skip UpdatePoses and leave m_bRenderInProgress false, so the
    // next xrEndFrame is skipped too and every following frame discards - tracking freezes and nothing is presented.
    if (result == XR_FRAME_DISCARDED)
    {
      plLog::Debug("OpenXR discarded the previously begun frame, continuing with the current one.");
    }
  }

  // #TODO_XR Swap chain acquire here?

  UpdatePoses();

  // This will update the extracted view from last frame with the new data we got
  // this frame just before starting to render.
  plView* pView = nullptr;
  if (plRenderWorld::TryGetView(m_hView, pView))
  {
    pView->UpdateViewData(plRenderWorld::GetDataIndexForRendering());
  }

  if (m_pCompanion)
  {
    // Throttled: the companion window is a preview nobody is looking at while the headset is on, and the blit sits on
    // the frame's critical path at full eye resolution. A headset runs at 90-120 Hz; the window does not need to.
    m_pCompanion->CompanionViewBeginFrame(true);
  }
  m_bRenderInProgress = true;
}

void plOpenXR::DelayPresent()
{
  PL_ASSERT_DEBUG(!m_bPresentDelayed, "Last present was not flushed");
  m_bPresentDelayed = true;
}

void plOpenXR::Present()
{
  const plGALOpenXRSwapChain* pSwapChain = static_cast<const plGALOpenXRSwapChain*>(plGALDevice::GetDefaultDevice()->GetSwapChain(m_hSwapChain));
  if (!pSwapChain)
    return;

  pSwapChain->PresentRenderTarget();
}

plGALTextureHandle plOpenXR::GetCurrentTexture()
{
  const plGALOpenXRSwapChain* pSwapChain = static_cast<const plGALOpenXRSwapChain*>(plGALDevice::GetDefaultDevice()->GetSwapChain(m_hSwapChain));
  if (!pSwapChain)
    return plGALTextureHandle();

  return pSwapChain->m_hColorRT;
}

void plOpenXR::SubmitXrFrame()
{
  const plGALOpenXRSwapChain* pSwapChain = static_cast<const plGALOpenXRSwapChain*>(plGALDevice::GetDefaultDevice()->GetSwapChain(m_hSwapChain));

  // Every xrBeginFrame must be matched by an xrEndFrame. Returning without ending would make the next xrBeginFrame
  // report XR_FRAME_DISCARDED, and the frame after that, permanently. A missing swap chain only costs us the layer.
  if (!m_bRenderInProgress)
    return;

  plHybridArray<XrCompositionLayerBaseHeader*, 1> layers;

  // Only submit layers if runtime wants us to render (per OpenXR spec)
  if (m_FrameState.shouldRender && pSwapChain != nullptr)
  {
    /// NOTE: (Only Applies When Tracy is Enabled.)Tracy Seems to declare Timers in the same scope, so dual profile macros can throw: '__tracy_scoped_zone' : redefinition; multitple initalization, so we must scope the two events.
    {
      PL_PROFILE_SCOPE("OpenXrEndFrame");
      for (uint32_t i = 0; i < 2; i++)
      {
        m_ProjectionLayerViews[i] = {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};
        m_ProjectionLayerViews[i].pose = m_Views[i].pose;
        m_ProjectionLayerViews[i].fov = m_Views[i].fov;
        m_ProjectionLayerViews[i].subImage.swapchain = pSwapChain->GetColorSwapchain();
        m_ProjectionLayerViews[i].subImage.imageRect.offset = {0, 0};
        m_ProjectionLayerViews[i].subImage.imageRect.extent = {(plInt32)m_Info.m_vEyeRenderTargetSize.width, (plInt32)m_Info.m_vEyeRenderTargetSize.height};
        m_ProjectionLayerViews[i].subImage.imageArrayIndex = i;

        if (m_Extensions.m_bDepthComposition && m_pCameraToSynchronize)
        {
          m_DepthLayerViews[i] = {XR_TYPE_COMPOSITION_LAYER_DEPTH_INFO_KHR};
          m_DepthLayerViews[i].minDepth = 0;
          m_DepthLayerViews[i].maxDepth = 1;
          m_DepthLayerViews[i].nearZ = m_pCameraToSynchronize->GetNearPlane();
          m_DepthLayerViews[i].farZ = m_pCameraToSynchronize->GetFarPlane();
          m_DepthLayerViews[i].subImage.swapchain = pSwapChain->GetDepthSwapchain();
          m_DepthLayerViews[i].subImage.imageRect.offset = {0, 0};
          m_DepthLayerViews[i].subImage.imageRect.extent = {(plInt32)m_Info.m_vEyeRenderTargetSize.width, (plInt32)m_Info.m_vEyeRenderTargetSize.height};
          m_DepthLayerViews[i].subImage.imageArrayIndex = i;

          m_ProjectionLayerViews[i].next = &m_DepthLayerViews[i];
        }
      }
    }

    m_Layer.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
    m_Layer.space = GetBaseSpace();
    m_Layer.viewCount = 2;
    m_Layer.views = m_ProjectionLayerViews;

    layers.PushBack(reinterpret_cast<XrCompositionLayerBaseHeader*>(&m_Layer));
  }

  // Submit the composition layers for the predicted display time.
  // When shouldRender is false, we submit zero layers per OpenXR spec.
  XrFrameEndInfo frameEndInfo{XR_TYPE_FRAME_END_INFO};
  frameEndInfo.displayTime = m_FrameState.predictedDisplayTime;
  frameEndInfo.environmentBlendMode = m_BlendMode;
  frameEndInfo.layerCount = layers.GetCount();
  frameEndInfo.layers = layers.GetData();

  {
    PL_PROFILE_SCOPE("xrEndFrame");
    XR_LOG_ERROR(xrEndFrame(m_pSession, &frameEndInfo));
  }

  m_bRenderInProgress = false;
}

void plOpenXR::PreWaitNextFrame()
{
  // Pre-wait for next frame to reduce latency.
  // This allows xrWaitFrame to complete while GPU is still processing,
  // so BeginFrame doesn't have to wait as long.
  if (m_bSessionRunning && !m_hView.IsInvalidated())
  {
    PL_PROFILE_SCOPE("xrWaitFrame_PreWait");
    m_FrameWaitInfo = XrFrameWaitInfo{XR_TYPE_FRAME_WAIT_INFO};
    m_FrameState = XrFrameState{XR_TYPE_FRAME_STATE};
    m_PreWaitResult = xrWaitFrame(m_pSession, &m_FrameWaitInfo, &m_FrameState);
    m_bPreWaitComplete = true;
    m_iPredictedDisplayTime = XR_FAILED_RESULT(m_PreWaitResult) ? 0 : (plInt64)m_FrameState.predictedDisplayTime;
  }
}

void plOpenXR::EndFrame()
{
  SubmitXrFrame();
  PreWaitNextFrame();
}

void plOpenXR::GALDeviceEventHandler(const plGALDeviceEvent& e)
{
  // Begin frame and end frame need to be encompassing all workload, XR and otherwise as xrWaitFrame will use this time interval to decide when to wake up the application.
  if (e.m_Type == plGALDeviceEvent::Type::BeforeBeginFrame)
  {
    BeginFrame();
  }
  else if (e.m_Type == plGALDeviceEvent::Type::BeforeEndFrame)
  {
    if (m_bRenderInProgress && m_pCompanion)
    {
      m_pCompanion->CompanionViewEndFrame();
    }
  }
  else if (e.m_Type == plGALDeviceEvent::Type::AfterEndFrame)
  {
    EndFrame();
  }
}

void plOpenXR::GameApplicationEventHandler(const plGameApplicationExecutionEvent& e)
{
  PL_ASSERT_DEV(IsInitialized(), "Need to call 'Initialize' first.");

  if (e.m_Type == plGameApplicationExecutionEvent::Type::BeginAppTick)
  {
    // Session restarts are requested from BeforeUpdatePlugins, which runs on a task worker. Run them here instead:
    // this is the main thread, outside any plGALDevice::BeginFrame/EndFrame bracket, so destroying and recreating the
    // swap chain is safe.
    if (m_bRestartPending.Set(false))
    {
      RestartSession();
    }
    else if (m_bShutdownPending.Set(false))
    {
      ShutdownSession();
    }
  }
  else if (e.m_Type == plGameApplicationExecutionEvent::Type::BeforeUpdatePlugins)
  {
    BeforeUpdatePlugins();
  }
}

void plOpenXR::SetStageSpace(plXRStageSpace::Enum space)
{
  m_StageSpace = space;
}

void plOpenXR::SetHMDCamera(plCamera* pCamera)
{
  PL_ASSERT_DEV(IsInitialized(), "Need to call 'Initialize' first.");

  if (m_pCameraToSynchronize == pCamera)
    return;

  m_pCameraToSynchronize = pCamera;
  if (m_pCameraToSynchronize)
  {
    m_uiSettingsModificationCounter = m_pCameraToSynchronize->GetSettingsModificationCounter() + 1;
    m_pCameraToSynchronize->SetCameraMode(plCameraMode::Stereo, m_pCameraToSynchronize->GetFovOrDim(), m_pCameraToSynchronize->GetNearPlane(), m_pCameraToSynchronize->GetFarPlane());
  }
}

plWorld* plOpenXR::GetWorld()
{
  plView* pView = nullptr;
  if (plRenderWorld::TryGetView(m_hView, pView))
  {
    return pView->GetWorld();
  }
  return nullptr;
}

XrPosef plOpenXR::ConvertTransform(const plTransform& tr)
{
  XrPosef pose;
  pose.orientation = ConvertOrientation(tr.m_qRotation);
  pose.position = ConvertPosition(tr.m_vPosition);
  return pose;
}

XrQuaternionf plOpenXR::ConvertOrientation(const plQuat& q)
{
  return {q.y, q.z, -q.x, -q.w};
}

XrVector3f plOpenXR::ConvertPosition(const plVec3& vPos)
{
  return {vPos.y, vPos.z, -vPos.x};
}

plQuat plOpenXR::ConvertOrientation(const XrQuaternionf& q)
{
  return {-q.z, q.x, q.y, -q.w};
}

plVec3 plOpenXR::ConvertPosition(const XrVector3f& pos)
{
  return {-pos.z, pos.x, pos.y};
}

plMat4 plOpenXR::ConvertPoseToMatrix(const XrPosef& pose)
{
  plMat4 m;
  plMat3 rot = ConvertOrientation(pose.orientation).GetAsMat3();
  plVec3 pos = ConvertPosition(pose.position);
  m.SetTransformationMatrix(rot, pos);
  return m;
}

plGALResourceFormat::Enum plOpenXR::ConvertTextureFormat(int64_t format, GraphicsApi api)
{
  // Swap chain formats are in the session graphics API's own enumeration, and the VkFormat and DXGI_FORMAT ranges overlap.
  if (api == GraphicsApi::D3D12)
  {
    switch (static_cast<DXGI_FORMAT>(format))
    {
      case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        return plGALResourceFormat::RGBAUByteNormalizedsRGB;
      case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        return plGALResourceFormat::BGRAUByteNormalizedsRGB;
      case DXGI_FORMAT_R8G8B8A8_UNORM:
        return plGALResourceFormat::RGBAUByteNormalized;
      case DXGI_FORMAT_B8G8R8A8_UNORM:
        return plGALResourceFormat::BGRAUByteNormalized;
      case DXGI_FORMAT_D32_FLOAT:
        return plGALResourceFormat::DFloat;
      case DXGI_FORMAT_D16_UNORM:
        return plGALResourceFormat::D16;
      case DXGI_FORMAT_D24_UNORM_S8_UINT:
        return plGALResourceFormat::D24S8;
      default:
        plLog::Warning("Unknown DXGI format {0}, defaulting to RGBAUByteNormalized", (int)format);
        return plGALResourceFormat::RGBAUByteNormalized;
    }
  }

  switch (static_cast<VkFormat>(format))
  {
    case VK_FORMAT_R8G8B8A8_SRGB:
      return plGALResourceFormat::RGBAUByteNormalizedsRGB;
    case VK_FORMAT_B8G8R8A8_SRGB:
      return plGALResourceFormat::BGRAUByteNormalizedsRGB;
    case VK_FORMAT_R8G8B8A8_UNORM:
      return plGALResourceFormat::RGBAUByteNormalized;
    case VK_FORMAT_B8G8R8A8_UNORM:
      return plGALResourceFormat::BGRAUByteNormalized;
    case VK_FORMAT_D32_SFLOAT:
      return plGALResourceFormat::DFloat;
    case VK_FORMAT_D16_UNORM:
      return plGALResourceFormat::D16;
    case VK_FORMAT_D24_UNORM_S8_UINT:
      return plGALResourceFormat::D24S8;
    case VK_FORMAT_D32_SFLOAT_S8_UINT:
      return plGALResourceFormat::DFloat; // Closest match
    default:
      plLog::Warning("Unknown Vulkan format {0}, defaulting to RGBAUByteNormalized", (int)format);
      return plGALResourceFormat::RGBAUByteNormalized;
  }
}
