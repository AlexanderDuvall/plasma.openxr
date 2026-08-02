#pragma once

#include <OpenXRPlugin/Basics.h>
#include <OpenXRPlugin/OpenXRIncludes.h>

#include <GameEngine/XR/XRInputDevice.h>
#include <GameEngine/XR/XRInterface.h>
#include <Foundation/Threading/Thread.h>
#include <Foundation/Threading/Mutex.h>
#include <Foundation/Threading/AtomicInteger.h>

class plOpenXR;

PL_DEFINE_AS_POD_TYPE(XrActionSuggestedBinding);
PL_DEFINE_AS_POD_TYPE(XrActiveActionSet);

class PL_OPENXRPLUGIN_DLL plOpenXRInputDevice : public plXRInputDevice
{
  PL_ADD_DYNAMIC_REFLECTION(plOpenXRInputDevice, plXRInputDevice);

public:
  void GetDeviceList(plHybridArray<plXRDeviceID, 64>& out_devices) const override;
  plXRDeviceID GetDeviceIDByType(plXRDeviceType::Enum type) const override;
  const plXRDeviceState& GetDeviceState(plXRDeviceID deviceID) const override;
  plString GetDeviceName(plXRDeviceID deviceID) const override;
  plBitflags<plXRDeviceFeatures> GetDeviceFeatures(plXRDeviceID deviceID) const override;

private:
  friend class plOpenXR;
  XrAction shoulderPoseAction;
  XrSpace shoulderSpace;

  struct Bind
  {
    XrAction action;
    const char* szPath;
  };

  struct Action
  {
    plXRDeviceFeatures::Enum m_Feature;
    XrAction m_Action;
    plString m_sKey[2];
  };

  struct Vec2Action
  {
    Vec2Action(plXRDeviceFeatures::Enum feature, XrAction pAction, plStringView sLeft, plStringView sRight);
    plXRDeviceFeatures::Enum m_Feature;
    XrAction m_Action;
    plString m_sKey_negx[2];
    plString m_sKey_posx[2];
    plString m_sKey_negy[2];
    plString m_sKey_posy[2];
  };

  plOpenXRInputDevice(plOpenXR* pOpenXR);
  XrResult CreateActions(XrSession session, XrSpace m_sceneSpace);
  void DestroyActions();

  XrPath CreatePath(const char* szPath);
  XrResult CreateAction(plXRDeviceFeatures::Enum feature, const char* actionName, XrActionType actionType, XrAction& out_action);
  /// \brief Suggests bindings for one interaction profile.
  ///
  /// Profiles that come from an extension are only valid if that extension was enabled on the instance. Pass
  /// bOptionalProfile = true for those so an unsupported profile path is reported as info instead of an error.
  XrResult SuggestInteractionProfileBindings(const char* szInteractionProfile, const char* szNiceName, plArrayPtr<Bind> bindings, bool bOptionalProfile = false);
  XrResult AttachSessionActionSets(XrSession session);
  XrResult UpdateCurrentInteractionProfile();

  /// \brief Applies any queued haptic pulses. Input thread only - all XrSession calls stay on that one thread.
  void ApplyPendingHaptics();

  /// \brief Logs a pose-location failure once per hand until the result code changes.
  void ReportLocateFailure(plUInt32 uiSide, const char* szWhat, XrResult res, XrSpace baseSpace, XrTime time);

  /// \brief Applies a vibration to one controller. fAmplitude is clamped to 0-1, fFrequencyHz <= 0 uses the runtime default.
  plResult TriggerHapticPulse(plXRDeviceID deviceID, plTime duration, float fAmplitude, float fFrequencyHz);

  /// \brief Whether the active interaction profile bound the haptic output action for the given hand.
  bool IsHapticActionBound(plUInt32 uiSide) const;

  void InitializeDevice() override;
  void RegisterInputSlots() override;
  void UpdateInputSlotValues() override;

  XrResult UpdateActions();
  void UpdateControllerState();

private:
  plOpenXR* m_pOpenXR = nullptr;
  XrInstance m_pInstance = XR_NULL_HANDLE;
  XrSession m_pSession = XR_NULL_HANDLE;
  //set to 4 so we can add vive tracker left shoulder. og 3
  plXRDeviceState m_DeviceState[4]; // Hard-coded for now
  plString m_sActiveProfile[4];
  plBitflags<plXRDeviceFeatures> m_SupportedFeatures[3];
  const plInt8 m_iLeftControllerDeviceID = 1;
  const plInt8 m_iRightControllerDeviceID = 2;
  const plInt8 m_iLeftShoulderDeviceId = 3;

  XrActionSet m_pActionSet = XR_NULL_HANDLE;
  plHashTable<XrPath, plString> m_InteractionProfileToNiceName;

  plStaticArray<const char*, 3> m_SubActionPrefix;
  plStaticArray<XrPath, 2> m_SubActionPath;
  plStaticArray<XrPath, 3> m_SubActionPathVive;

  plHybridArray<Action, 4> m_BooleanActions;
  plHybridArray<Action, 4> m_FloatActions;
  plHybridArray<Vec2Action, 4> m_Vec2Actions;
  plHybridArray<Action, 4> m_PoseActions;

  // Vibration output action, shared by both hands via the subaction paths.
  XrAction m_HapticAction = XR_NULL_HANDLE;

  // Cached indices of bound actions per controller (updated when interaction profile changes)
  plHybridArray<plUInt8, 16> m_BoundBooleanActions[2];
  plHybridArray<plUInt8, 4> m_BoundFloatActions[2];
  plHybridArray<plUInt8, 4> m_BoundVec2Actions[2];
  bool m_bBoundActionsCached[2] = {false, false};

  // Features found during action discovery. Owned by the input thread and written into both snapshot buffers
  // every update, as each buffer is only touched every other frame.
  plBitflags<plXRDeviceFeatures> m_DiscoveredFeatures[2];

  XrSpace m_gripSpace[2] = {XR_NULL_HANDLE, XR_NULL_HANDLE};
  XrSpace m_aimSpace[2] = {XR_NULL_HANDLE, XR_NULL_HANDLE};

  //////////////////////////////////////////////////////////////////////////
  // Background Input Thread
  //////////////////////////////////////////////////////////////////////////

  class InputThread : public plThread
  {
  public:
    InputThread(plOpenXRInputDevice* pOwner);
    virtual plUInt32 Run() override;
    plOpenXRInputDevice* m_pOwner = nullptr;
  };

  // Double-buffered input snapshot for lock-free reading from main thread
  struct InputSnapshot
  {
    plXRDeviceState m_DeviceState[4];
    plBitflags<plXRDeviceFeatures> m_SupportedFeatures[3];
    plHashTable<plString, float> m_InputSlotValues;
  };

  // Haptic request handed from the main thread to the input thread, so only one thread ever calls into the XrSession.
  struct PendingHaptic
  {
    plTime m_Duration;
    float m_fAmplitude = 0.0f;
    float m_fFrequencyHz = 0.0f;
    bool m_bPending = false;
  };

  plUniquePtr<InputThread> m_pInputThread;
  plAtomicBool m_bInputThreadRunning{false};

  // Double buffer: thread writes to back buffer, then swaps atomically
  InputSnapshot m_InputSnapshots[2];
  plAtomicInteger32 m_iReadableSnapshot{0};  // Index that main thread should read from
  plMutex m_SnapshotSwapMutex;               // Only held briefly during pointer swap

  PendingHaptic m_PendingHaptics[2];
  plMutex m_HapticMutex;

  // Last reported pose-location result per hand, so the input thread's tick rate cannot spam the log.
  XrResult m_LastLocateFailure[2] = {XR_SUCCESS, XR_SUCCESS};

  // XR.BatchedSpaceLocation, latched at action creation (session start). The cvar must NOT be read live on the
  // input thread: it can now be toggled from inside the headset (VR CVars panel), and switching the input thread
  // onto the xrLocateSpacesKHR path mid-session faults inside SteamVR's vrclient - a full system hang on this
  // hardware. Latching turns the toggle into a deliberate next-session decision.
  bool m_bUseBatchedSpaceLocation = false;

  void StartInputThread();
  void StopInputThread();
  XrSpaceLocation UpdateLeftShoulderTracking(plXRDeviceState& deviceState); // Called by input thread
  
  void UpdateActionsOnInputThread();  // Called by input thread
  void CopySnapshotToMainThread();    // Called by main thread in UpdateActions
};
