#pragma once

#include <Core/Graphics/Camera.h>
#include <Foundation/Configuration/Singleton.h>
#include <Foundation/Threading/AtomicInteger.h>
#include <GameEngine/XR/XRInputDevice.h>
#include <GameEngine/XR/XRInterface.h>
#include <OpenXRPlugin/Basics.h>
#include <OpenXRPlugin/OpenXRIncludes.h>
#include <RendererCore/Pipeline/Declarations.h>
#include <RendererCore/Shader/ConstantBufferStorage.h>
#include <RendererFoundation/Descriptors/Descriptors.h>
#include <RendererFoundation/Device/SwapChain.h>
#include <RendererFoundation/Resources/RenderTargetSetup.h>

class plOpenXRInputDevice;
class plOpenXRSpatialAnchors;
class plOpenXRHandTracking;
class plWindowOutputTargetXR;
struct plGameApplicationExecutionEvent;

PL_DEFINE_AS_POD_TYPE(XrViewConfigurationView);
PL_DEFINE_AS_POD_TYPE(XrEnvironmentBlendMode);

class PL_OPENXRPLUGIN_DLL plOpenXR : public plXRInterface
{
  PL_DECLARE_SINGLETON_OF_INTERFACE(plOpenXR, plXRInterface);

public:
  plOpenXR();
  ~plOpenXR();

  XrInstance GetInstance() const { return m_pInstance; }
  uint64_t GetSystemId() const { return m_SystemId; }
  XrSession GetSession() const { return m_pSession; }
  XrViewConfigurationType GetViewType() const { return m_PrimaryViewConfigurationType; }
  bool GetDepthComposition() const;

  virtual bool IsHmdPresent() const override;

  virtual plResult Initialize() override;
  virtual void Deinitialize() override;
  virtual bool IsInitialized() const override;

  virtual const plHMDInfo& GetHmdInfo() const override;
  virtual plXRInputDevice& GetXRInput() const override;

  virtual plGALTextureHandle GetCurrentTexture() override;

  virtual plResult TriggerHapticPulse(plXRDeviceID deviceID, plTime duration, float fAmplitude, float fFrequencyHz = 0.0f) override;

  /// \brief The display time xrWaitFrame predicted for the frame currently being produced.
  ///
  /// Read this instead of m_FrameState from any thread other than the main thread: the main thread overwrites the
  /// whole XrFrameState struct in BeginFrame and PreWaitNextFrame, so a concurrent struct read can tear.
  XrTime GetPredictedDisplayTime() const { return (XrTime)m_iPredictedDisplayTime; }

  void DelayPresent();
  void Present();
  void SubmitXrFrame();
  void PreWaitNextFrame();
  void EndFrame();

  virtual plUniquePtr<plActor> CreateActor(plView* pView, plGALMSAASampleCount::Enum msaaCount = plGALMSAASampleCount::None,
    plUniquePtr<plWindowBase> pCompanionWindow = nullptr, plUniquePtr<plWindowOutputTargetGAL> pCompanionWindowOutput = nullptr) override;
  virtual void OnActorDestroyed() override;
  virtual bool SupportsCompanionView() override;

  XrSpace GetBaseSpace() const;

private:
  XrResult SelectExtensions(plHybridArray<const char*, 6>& extensions);
  XrResult SelectLayers(plHybridArray<const char*, 6>& layers);
  XrResult InitSystem();
  void DeinitSystem();
  XrResult InitSession();
  void DeinitSession();
  /// \brief Creates everything that depends on a live XrSession: swap chain, eye render target size, view wiring.
  ///
  /// Split out of CreateActor so a session restart can rebuild it without recreating the actor.
  XrResult CreateSessionResources();
  void DestroySessionResources();
  /// \brief Runs a pending session restart. Main thread only - it destroys and recreates GAL resources.
  void RestartSession();
  /// \brief Tears the session down after the runtime asked us to stop. Main thread only.
  void ShutdownSession();
  XrResult InitGraphicsPlugin();
  void DeinitGraphicsPlugin();
  XrResult InitDebugMessenger();
  void DeinitInitDebugMessenger();

  void GameApplicationEventHandler(const plGameApplicationExecutionEvent& e);
  void GALDeviceEventHandler(const plGALDeviceEvent& e);

  void BeforeUpdatePlugins();
  void UpdatePoses();
  void UpdateCamera();
  void BeginFrame();

  void SetStageSpace(plXRStageSpace::Enum space);
  void SetHMDCamera(plCamera* pCamera);

  plWorld* GetWorld();

public:
  static XrPosef ConvertTransform(const plTransform& tr);
  static XrQuaternionf ConvertOrientation(const plQuat& q);
  static XrVector3f ConvertPosition(const plVec3& vPos);
  static plQuat ConvertOrientation(const XrQuaternionf& q);
  static plVec3 ConvertPosition(const XrVector3f& pos);
  static plMat4 ConvertPoseToMatrix(const XrPosef& pose);
  static plGALResourceFormat::Enum ConvertTextureFormat(int64_t format);

private:
  friend class plOpenXRInputDevice;
  friend class plOpenXRSpatialAnchors;
  friend class plOpenXRHandTracking;
  friend class plOpenXRRemoting;
  friend class plGALOpenXRSwapChain;

  struct Extensions
  {
    bool m_bValidation = false;
    bool m_bDebugUtils = false;
    PFN_xrCreateDebugUtilsMessengerEXT pfn_xrCreateDebugUtilsMessengerEXT;
    PFN_xrDestroyDebugUtilsMessengerEXT pfn_xrDestroyDebugUtilsMessengerEXT;

    bool m_bVulkan = false;
    PFN_xrGetVulkanInstanceExtensionsKHR pfn_xrGetVulkanInstanceExtensionsKHR;
    PFN_xrGetVulkanDeviceExtensionsKHR pfn_xrGetVulkanDeviceExtensionsKHR;
    PFN_xrGetVulkanGraphicsDeviceKHR pfn_xrGetVulkanGraphicsDeviceKHR;
    PFN_xrGetVulkanGraphicsRequirementsKHR pfn_xrGetVulkanGraphicsRequirementsKHR;

    // XR_KHR_vulkan_enable2 extension (preferred for better GPU synchronization)
    bool m_bVulkan2 = false;
    PFN_xrCreateVulkanInstanceKHR pfn_xrCreateVulkanInstanceKHR;
    PFN_xrCreateVulkanDeviceKHR pfn_xrCreateVulkanDeviceKHR;
    PFN_xrGetVulkanGraphicsDevice2KHR pfn_xrGetVulkanGraphicsDevice2KHR;
    PFN_xrGetVulkanGraphicsRequirements2KHR pfn_xrGetVulkanGraphicsRequirements2KHR;

    bool m_bDepthComposition = false;

    bool m_bUnboundedReferenceSpace = false;

    bool m_bSpatialAnchor = false;
    PFN_xrCreateSpatialAnchorMSFT pfn_xrCreateSpatialAnchorMSFT;
    PFN_xrCreateSpatialAnchorSpaceMSFT pfn_xrCreateSpatialAnchorSpaceMSFT;
    PFN_xrDestroySpatialAnchorMSFT pfn_xrDestroySpatialAnchorMSFT;

    bool m_bHandInteraction = false;

    // Interaction-profile-only extensions: they add no entry points, they just make the runtime accept the profile's
    // paths in xrSuggestInteractionProfileBindings.
    bool m_bHandInteractionExt = false;
    bool m_bTouchControllerPro = false;
    bool m_bViveCosmosController = false;
    bool m_bViveFocus3Controller = false;
    bool m_bByteDanceController = false;

    bool m_bHandTracking = false;
    PFN_xrCreateHandTrackerEXT pfn_xrCreateHandTrackerEXT;
    PFN_xrDestroyHandTrackerEXT pfn_xrDestroyHandTrackerEXT;
    PFN_xrLocateHandJointsEXT pfn_xrLocateHandJointsEXT;

    bool m_bHandTrackingMesh = false;
    PFN_xrCreateHandMeshSpaceMSFT pfn_xrCreateHandMeshSpaceMSFT;
    PFN_xrUpdateHandMeshMSFT pfn_xrUpdateHandMeshMSFT;

    bool m_bHolographicWindowAttachment = false;

    // XR_EXT_performance_settings extension
    bool m_bPerformanceSettings = false;
    PFN_xrPerfSettingsSetPerformanceLevelEXT pfn_xrPerfSettingsSetPerformanceLevelEXT = nullptr;

    // XR_KHR_locate_spaces extension (batched space queries for better performance)
    bool m_bLocateSpaces = false;
    PFN_xrLocateSpacesKHR pfn_xrLocateSpacesKHR = nullptr;

    bool m_bRemoting = false;
    bool m_ViveTracker = false;
#ifdef BUILDSYSTEM_ENABLE_OPENXR_REMOTING_SUPPORT
    PFN_xrRemotingSetContextPropertiesMSFT pfn_xrRemotingSetContextPropertiesMSFT;
    PFN_xrRemotingConnectMSFT pfn_xrRemotingConnectMSFT;
    PFN_xrRemotingDisconnectMSFT pfn_xrRemotingDisconnectMSFT;
    PFN_xrRemotingGetConnectionStateMSFT pfn_xrRemotingGetConnectionStateMSFT;
#endif
  };

  // Process-lifetime: SteamVR's vrclient faults on a second xrCreateInstance in one process, so Deinitialize keeps
  // the instance and only the destructor releases it.
  XrInstance m_pInstance = XR_NULL_HANDLE;
  Extensions m_Extensions;
#ifdef BUILDSYSTEM_ENABLE_OPENXR_REMOTING_SUPPORT
  plUniquePtr<class plOpenXRRemoting> m_pRemoting;
#endif

  // System
  uint64_t m_SystemId = XR_NULL_SYSTEM_ID;

  // Session
  XrSession m_pSession = XR_NULL_HANDLE;
  XrSpace m_pSceneSpace = XR_NULL_HANDLE;
  XrSpace m_pLocalSpace = XR_NULL_HANDLE;
  plEventSubscriptionID m_ExecutionEventsId = 0;
  plEventSubscriptionID m_BeginRenderEventsId = 0;
  plEventSubscriptionID m_GALdeviceEventsId = 0;
  XrDebugUtilsMessengerEXT m_pDebugMessenger = XR_NULL_HANDLE;

  // Graphics plugin
  XrEnvironmentBlendMode m_BlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
  XrGraphicsBindingVulkanKHR m_XrGraphicsBindingVulkan{XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR};
  XrFormFactor m_FormFactor{XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY};
  XrViewConfigurationType m_PrimaryViewConfigurationType{XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO};

  plGALSwapChainHandle m_hSwapChain;
  // Kept so a session restart can recreate the swap chain with the sample count the actor was created with.
  plGALMSAASampleCount::Enum m_MsaaCount = plGALMSAASampleCount::None;

  // Views
  XrViewState m_ViewState{XR_TYPE_VIEW_STATE};
  XrView m_Views[2];
  bool m_bProjectionChanged = true;
  XrCompositionLayerProjection m_Layer{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
  XrCompositionLayerProjectionView m_ProjectionLayerViews[2];
  XrCompositionLayerDepthInfoKHR m_DepthLayerViews[2];

  // State
  bool m_bSessionRunning = false;
  bool m_bExitRenderLoop = false;
  bool m_bRequestRestart = false;
  bool m_bRenderInProgress = false;
  XrSessionState m_SessionState{XR_SESSION_STATE_UNKNOWN};

  // Set from BeforeUpdatePlugins (a task worker) and acted on at BeginAppTick, because starting or ending a session
  // destroys and recreates GAL resources and must not race the main thread's rendering.
  plAtomicBool m_bRestartPending{false};
  plAtomicBool m_bShutdownPending{false};
  // The runtime told us the instance is going away, so it must be destroyed rather than reused.
  bool m_bInstanceLost = false;

  XrFrameWaitInfo m_FrameWaitInfo{XR_TYPE_FRAME_WAIT_INFO};
  XrFrameState m_FrameState{XR_TYPE_FRAME_STATE};
  XrFrameBeginInfo m_FrameBeginInfo{XR_TYPE_FRAME_BEGIN_INFO};
  // Published copy of m_FrameState.predictedDisplayTime for the input thread; see GetPredictedDisplayTime.
  plAtomicInteger64 m_iPredictedDisplayTime{0};

  // Pre-wait state for pipelined frame timing
  XrResult m_PreWaitResult = XR_SUCCESS;
  bool m_bPreWaitComplete = false;

  // XR interface state
  plHMDInfo m_Info;
  mutable plUniquePtr<plOpenXRInputDevice> m_pInput;
  plUniquePtr<plOpenXRSpatialAnchors> m_pAnchors;
  plUniquePtr<plOpenXRHandTracking> m_pHandTracking;

  plCamera* m_pCameraToSynchronize = nullptr;
  plEnum<plXRStageSpace> m_StageSpace;
  plUInt32 m_uiSettingsModificationCounter = 0;
  plViewHandle m_hView;

  // Cached stage transform to prevent jitter from simulation/render timing mismatch
  plTransform m_CachedStageTransform;
  bool m_bStageTransformCached = false;

  plWindowOutputTargetXR* m_pCompanion = nullptr;
  bool m_bPresentDelayed = false;
};
