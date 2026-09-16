#include <OpenXRPlugin/OpenXRPluginPCH.h>

#include <Core/Input/InputManager.h>
#include <Foundation/Configuration/CVar.h>
#include <Foundation/Math/Math.h>
#include <Foundation/Profiling/Profiling.h>
#include <Foundation/Threading/ThreadUtils.h>
#include <OpenXRPlugin/OpenXRDeclarations.h>
#include <OpenXRPlugin/OpenXRInputDevice.h>
#include <OpenXRPlugin/OpenXRSingleton.h>

// Off by default: SteamVR's XR_KHR_locate_spaces implementation faults inside vrclient_x64.dll on a well-formed
// xrLocateSpacesKHR call. The per-space xrLocateSpace path below is the conventional one and costs two extra calls per
// hand per input tick, which is not measurable. Turn this on only to test a runtime's batched path.
plCVarBool cvar_XrBatchedSpaceLocation("XR.BatchedSpaceLocation", false, plCVarFlags::Save, "Use xrLocateSpacesKHR to locate both hand spaces in one call.");

// clang-format off
PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plOpenXRInputDevice, 1, plRTTINoAllocator);
// no properties or message handlers
PL_END_DYNAMIC_REFLECTED_TYPE;
// clang-format on


#define XR_Trigger "trigger_value"
#define XR_Trigger_Touch "trigger_touch"

#define XR_Select_Click "select_click"
#define XR_Menu_Click "menu_click"

#define XR_Grip "grip_value"
#define XR_Grip_Click "grip_click"

#define XR_Button_A "button_a"
#define XR_Button_A_Touch "button_a_touch"
#define XR_Button_B "button_b"
#define XR_Button_B_Touch "button_b_touch"
#define XR_Thumbrest "thumbrest"

#define XR_Primary_Analog_Stick_Axis "primary_analog_stick"
#define XR_Primary_Analog_Stick_Click "primary_analog_stick_click"
#define XR_Primary_Analog_Stick_Touch "primary_analog_stick_touch"

#define XR_Secondary_Analog_Stick_Axis "secondary_analog_stick"
#define XR_Secondary_Analog_Stick_Click "secondary_analog_stick_click"
#define XR_Secondary_Analog_Stick_Touch "secondary_analog_stick_touch"

#define XR_Grip_Pose "grip_pose"
#define XR_Aim_Pose "aim_pose"

#define XR_Haptic "haptic"

void plOpenXRInputDevice::GetDeviceList(plHybridArray<plXRDeviceID, 64>& out_devices) const
{
  PL_ASSERT_DEV(m_pOpenXR->IsInitialized(), "Need to call 'Initialize' first.");
  // The HMD (0) is always present; start the scan at 1 so it is not listed twice.
  out_devices.PushBack(0);
  for (plXRDeviceID i = 1; i < s_iMaxDevices; i++)
  {
    if (m_DeviceState[i].m_bDeviceIsConnected)
    {
      out_devices.PushBack(i);
    }
  }
}

plXRDeviceID plOpenXRInputDevice::GetDeviceIDByType(plXRDeviceType::Enum type) const
{
  plXRDeviceID deviceID = -1;
  switch (type)
  {
    case plXRDeviceType::HMD:
      deviceID = 0;
      break;
    case plXRDeviceType::LeftController:
      deviceID = m_iLeftControllerDeviceID;
      break;
    case plXRDeviceType::RightController:
      deviceID = m_iRightControllerDeviceID;
      break;
    case plXRDeviceType::LeftShoulder:
      deviceID = m_iLeftShoulderDeviceId;
      break;
    default:
      deviceID = -1;
      break;
  }

  if (deviceID != -1 && !m_DeviceState[deviceID].m_bDeviceIsConnected)
  {
    deviceID = -1;
  }
  return deviceID;
}

const plXRDeviceState& plOpenXRInputDevice::GetDeviceState(plXRDeviceID deviceID) const
{

  PL_ASSERT_DEV(m_pOpenXR->IsInitialized(), "Need to call 'Initialize' first.");
  PL_ASSERT_DEV(deviceID < s_iMaxDevices && deviceID >= 0, "Invalid device ID.");
  PL_ASSERT_DEV(m_DeviceState[deviceID].m_bDeviceIsConnected, "Invalid device ID.");
  return m_DeviceState[deviceID];
}

plString plOpenXRInputDevice::GetDeviceName(plXRDeviceID deviceID) const
{
  PL_ASSERT_DEV(m_pOpenXR->IsInitialized(), "Need to call 'Initialize' first.");
  PL_ASSERT_DEV(deviceID < s_iMaxDevices && deviceID >= 0, "Invalid device ID.");
  PL_ASSERT_DEV(m_DeviceState[deviceID].m_bDeviceIsConnected, "Invalid device ID.");
  return m_sActiveProfile[deviceID];
}

plBitflags<plXRDeviceFeatures> plOpenXRInputDevice::GetDeviceFeatures(plXRDeviceID deviceID) const
{
  PL_ASSERT_DEV(m_pOpenXR->IsInitialized(), "Need to call 'Initialize' first.");
  PL_ASSERT_DEV(deviceID < s_iMaxDevices && deviceID >= 0, "Invalid device ID.");
  PL_ASSERT_DEV(m_DeviceState[deviceID].m_bDeviceIsConnected, "Invalid device ID.");
  return m_SupportedFeatures[deviceID];
}

plOpenXRInputDevice::plOpenXRInputDevice(plOpenXR* pOpenXR)
  : plXRInputDevice()
  , m_pOpenXR(pOpenXR)
{
  m_pInstance = m_pOpenXR->m_pInstance;
}

XrResult plOpenXRInputDevice::CreateActions(XrSession session, XrSpace sceneSpace)
{
  m_pSession = session;

  // Latch the batched-location choice for this session - see the member comment for why the input thread
  // must never read the cvar live.
  m_bUseBatchedSpaceLocation = cvar_XrBatchedSpaceLocation && m_pOpenXR->m_Extensions.m_bLocateSpaces && m_pOpenXR->m_Extensions.pfn_xrLocateSpacesKHR != nullptr;
  if (m_bUseBatchedSpaceLocation)
  {
    plLog::SeriousWarning("XR.BatchedSpaceLocation is ON: SteamVR's xrLocateSpacesKHR is known to fault inside vrclient_x64.dll on some setups (documented full system hang on Vive Pro + implicit Vive API layers). Disable the cvar and restart the session if VR becomes unstable.");
  }

  // HMD is always connected or we wouldn't have been able to create a session.
  m_DeviceState[0] = plXRDeviceState();
  m_DeviceState[0].m_bDeviceIsConnected = true;
  m_sActiveProfile[0] = "HMD";
  m_SupportedFeatures[0] = plXRDeviceFeatures::AimPose | plXRDeviceFeatures::GripPose;

  // Devices
  for (plUInt32 uiControllerId : {m_iLeftControllerDeviceID, m_iRightControllerDeviceID, m_iLeftShoulderDeviceId})
  {
    m_DeviceState[uiControllerId] = plXRDeviceState();
    m_sActiveProfile[uiControllerId].Clear();
  }

  XrActionSetCreateInfo actionSetInfo{XR_TYPE_ACTION_SET_CREATE_INFO};
  plStringUtils::Copy(actionSetInfo.actionSetName, XR_MAX_ACTION_SET_NAME_SIZE, "gameplay");
  plStringUtils::Copy(actionSetInfo.localizedActionSetName, XR_MAX_LOCALIZED_ACTION_SET_NAME_SIZE, "Gameplay");
  actionSetInfo.priority = 0;
  XR_SUCCEED_OR_CLEANUP_LOG(xrCreateActionSet(m_pInstance, &actionSetInfo, &m_pActionSet), DestroyActions);

  m_SubActionPrefix.SetCount(3);
  m_SubActionPrefix[0] = "xr_hand_left_";
  m_SubActionPrefix[1] = "xr_hand_right_";
  m_SubActionPrefix[2] = "xr_shoulder_left";

  m_SubActionPath.SetCount(2);
  m_SubActionPath[0] = CreatePath("/user/hand/left");
  m_SubActionPath[1] = CreatePath("/user/hand/right");
  m_SubActionPathVive[0] = CreatePath("/interaction_profiles/htc/vive_tracker_htcx");
  m_SubActionPathVive[1] = CreatePath("/user/vive_tracker_htcx/role/left_shoulder");
  m_SubActionPathVive[2] = CreatePath("/user/vive_tracker_htcx/role/left_shoulder/input/grip/pose");

  // Deice Tracker Inputs
  XrAction LeftShoulder = XR_NULL_HANDLE;

  // Float inputs
  XrAction Trigger = XR_NULL_HANDLE;
  XrAction Grip = XR_NULL_HANDLE;

  // Boolean inputs
  XrAction TriggerTouch = XR_NULL_HANDLE;
  XrAction SelectClick = XR_NULL_HANDLE;
  XrAction MenuClick = XR_NULL_HANDLE;
  XrAction GripClick = XR_NULL_HANDLE;
  XrAction ButtonA = XR_NULL_HANDLE;
  XrAction ButtonATouch = XR_NULL_HANDLE;
  XrAction ButtonB = XR_NULL_HANDLE;
  XrAction ButtonBTouch = XR_NULL_HANDLE;
  XrAction Thumbrest = XR_NULL_HANDLE;

  // Analog stick inputs
  XrAction PrimaryAnalogStickAxis = XR_NULL_HANDLE;
  XrAction PrimaryAnalogStickClick = XR_NULL_HANDLE;
  XrAction PrimaryAnalogStickTouch = XR_NULL_HANDLE;

  XrAction SecondaryAnalogStickAxis = XR_NULL_HANDLE;
  XrAction SecondaryAnalogStickClick = XR_NULL_HANDLE;
  XrAction SecondaryAnalogStickTouch = XR_NULL_HANDLE;

  // Pose inputs
  XrAction GripPose = XR_NULL_HANDLE;
  XrAction AimPose = XR_NULL_HANDLE;

  
  // Create float actions
  XR_SUCCEED_OR_CLEANUP_LOG(CreateAction(plXRDeviceFeatures::Trigger, XR_Trigger, XR_ACTION_TYPE_FLOAT_INPUT, Trigger), DestroyActions);
  XR_SUCCEED_OR_CLEANUP_LOG(CreateAction(plXRDeviceFeatures::Grip, XR_Grip, XR_ACTION_TYPE_FLOAT_INPUT, Grip), DestroyActions);

  // Create boolean actions
  XR_SUCCEED_OR_CLEANUP_LOG(CreateAction(plXRDeviceFeatures::TriggerTouch, XR_Trigger_Touch, XR_ACTION_TYPE_BOOLEAN_INPUT, TriggerTouch), DestroyActions);
  XR_SUCCEED_OR_CLEANUP_LOG(CreateAction(plXRDeviceFeatures::Select, XR_Select_Click, XR_ACTION_TYPE_BOOLEAN_INPUT, SelectClick), DestroyActions);
  XR_SUCCEED_OR_CLEANUP_LOG(CreateAction(plXRDeviceFeatures::Menu, XR_Menu_Click, XR_ACTION_TYPE_BOOLEAN_INPUT, MenuClick), DestroyActions);
  XR_SUCCEED_OR_CLEANUP_LOG(CreateAction(plXRDeviceFeatures::GripClick, XR_Grip_Click, XR_ACTION_TYPE_BOOLEAN_INPUT, GripClick), DestroyActions);
  XR_SUCCEED_OR_CLEANUP_LOG(CreateAction(plXRDeviceFeatures::ButtonA, XR_Button_A, XR_ACTION_TYPE_BOOLEAN_INPUT, ButtonA), DestroyActions);
  XR_SUCCEED_OR_CLEANUP_LOG(CreateAction(plXRDeviceFeatures::ButtonATouch, XR_Button_A_Touch, XR_ACTION_TYPE_BOOLEAN_INPUT, ButtonATouch), DestroyActions);
  XR_SUCCEED_OR_CLEANUP_LOG(CreateAction(plXRDeviceFeatures::ButtonB, XR_Button_B, XR_ACTION_TYPE_BOOLEAN_INPUT, ButtonB), DestroyActions);
  XR_SUCCEED_OR_CLEANUP_LOG(CreateAction(plXRDeviceFeatures::ButtonBTouch, XR_Button_B_Touch, XR_ACTION_TYPE_BOOLEAN_INPUT, ButtonBTouch), DestroyActions);
  XR_SUCCEED_OR_CLEANUP_LOG(CreateAction(plXRDeviceFeatures::Thumbrest, XR_Thumbrest, XR_ACTION_TYPE_BOOLEAN_INPUT, Thumbrest), DestroyActions);

  // Create analog stick actions
  XR_SUCCEED_OR_CLEANUP_LOG(
    CreateAction(plXRDeviceFeatures::PrimaryAnalogStick, XR_Primary_Analog_Stick_Axis, XR_ACTION_TYPE_VECTOR2F_INPUT, PrimaryAnalogStickAxis),
    DestroyActions);
  XR_SUCCEED_OR_CLEANUP_LOG(
    CreateAction(plXRDeviceFeatures::PrimaryAnalogStickClick, XR_Primary_Analog_Stick_Click, XR_ACTION_TYPE_BOOLEAN_INPUT, PrimaryAnalogStickClick),
    DestroyActions);
  XR_SUCCEED_OR_CLEANUP_LOG(
    CreateAction(plXRDeviceFeatures::PrimaryAnalogStickTouch, XR_Primary_Analog_Stick_Touch, XR_ACTION_TYPE_BOOLEAN_INPUT, PrimaryAnalogStickTouch),
    DestroyActions);

  XR_SUCCEED_OR_CLEANUP_LOG(
    CreateAction(plXRDeviceFeatures::SecondaryAnalogStick, XR_Secondary_Analog_Stick_Axis, XR_ACTION_TYPE_VECTOR2F_INPUT, SecondaryAnalogStickAxis),
    DestroyActions);
  XR_SUCCEED_OR_CLEANUP_LOG(CreateAction(plXRDeviceFeatures::SecondaryAnalogStickClick, XR_Secondary_Analog_Stick_Click, XR_ACTION_TYPE_BOOLEAN_INPUT,
                              SecondaryAnalogStickClick),
    DestroyActions);
  XR_SUCCEED_OR_CLEANUP_LOG(CreateAction(plXRDeviceFeatures::SecondaryAnalogStickTouch, XR_Secondary_Analog_Stick_Touch, XR_ACTION_TYPE_BOOLEAN_INPUT,
                              SecondaryAnalogStickTouch),
    DestroyActions);

  // Create pose actions
  XR_SUCCEED_OR_CLEANUP_LOG(CreateAction(plXRDeviceFeatures::GripPose, XR_Grip_Pose, XR_ACTION_TYPE_POSE_INPUT, GripPose), DestroyActions);
  XR_SUCCEED_OR_CLEANUP_LOG(CreateAction(plXRDeviceFeatures::AimPose, XR_Aim_Pose, XR_ACTION_TYPE_POSE_INPUT, AimPose), DestroyActions);

  // Create the vibration output action
  XR_SUCCEED_OR_CLEANUP_LOG(CreateAction(plXRDeviceFeatures::Haptics, XR_Haptic, XR_ACTION_TYPE_VIBRATION_OUTPUT, m_HapticAction), DestroyActions);

  //=============================================================================
  // INTERACTION PROFILE BINDINGS
  // Each controller type has its own profile with appropriate bindings.
  // The goal is to provide a unified experience across all controllers:
  // - Primary stick/trackpad = movement
  // - Trigger = main action
  // - Grip = grab/hold
  // - Face buttons = context-dependent actions
  //=============================================================================

  // KHR Simple Controller (fallback for any controller)
  Bind simpleController[] = {
    {SelectClick, "/user/hand/left/input/select/click"},
    {MenuClick, "/user/hand/left/input/menu/click"},
    {GripPose, "/user/hand/left/input/grip/pose"},
    {AimPose, "/user/hand/left/input/aim/pose"},

    {SelectClick, "/user/hand/right/input/select/click"},
    {MenuClick, "/user/hand/right/input/menu/click"},
    {GripPose, "/user/hand/right/input/grip/pose"},
    {AimPose, "/user/hand/right/input/aim/pose"},

    {m_HapticAction, "/user/hand/left/output/haptic"},
    {m_HapticAction, "/user/hand/right/output/haptic"},
  };
  SuggestInteractionProfileBindings("/interaction_profiles/khr/simple_controller", "Simple Controller", simpleController);

  // Microsoft Mixed Reality Motion Controller
  // Has thumbstick (primary) and trackpad (secondary)
  Bind motionController[] = {
    {Trigger, "/user/hand/left/input/trigger/value"},
    {SelectClick, "/user/hand/left/input/trigger/value"},
    {MenuClick, "/user/hand/left/input/menu/click"},
    {Grip, "/user/hand/left/input/squeeze/click"},
    {GripClick, "/user/hand/left/input/squeeze/click"},
    {PrimaryAnalogStickAxis, "/user/hand/left/input/thumbstick"},
    {PrimaryAnalogStickClick, "/user/hand/left/input/thumbstick/click"},
    {SecondaryAnalogStickAxis, "/user/hand/left/input/trackpad"},
    {SecondaryAnalogStickClick, "/user/hand/left/input/trackpad/click"},
    {SecondaryAnalogStickTouch, "/user/hand/left/input/trackpad/touch"},
    {GripPose, "/user/hand/left/input/grip/pose"},
    {AimPose, "/user/hand/left/input/aim/pose"},

    {Trigger, "/user/hand/right/input/trigger/value"},
    {SelectClick, "/user/hand/right/input/trigger/value"},
    {MenuClick, "/user/hand/right/input/menu/click"},
    {Grip, "/user/hand/right/input/squeeze/click"},
    {GripClick, "/user/hand/right/input/squeeze/click"},
    {PrimaryAnalogStickAxis, "/user/hand/right/input/thumbstick"},
    {PrimaryAnalogStickClick, "/user/hand/right/input/thumbstick/click"},
    {SecondaryAnalogStickAxis, "/user/hand/right/input/trackpad"},
    {SecondaryAnalogStickClick, "/user/hand/right/input/trackpad/click"},
    {SecondaryAnalogStickTouch, "/user/hand/right/input/trackpad/touch"},
    {GripPose, "/user/hand/right/input/grip/pose"},
    {AimPose, "/user/hand/right/input/aim/pose"},

    {m_HapticAction, "/user/hand/left/output/haptic"},
    {m_HapticAction, "/user/hand/right/output/haptic"},
  };
  SuggestInteractionProfileBindings("/interaction_profiles/microsoft/motion_controller", "Mixed Reality Motion Controller", motionController);

  // Oculus Touch Controller (Meta Quest 1/2/3/Pro, Rift, Rift S)
  // Full featured controller with thumbstick, trigger, grip, and face buttons
  // Left: X/Y buttons, Right: A/B buttons
  Bind oculusTouchController[] = {
    // Left hand
    {Trigger, "/user/hand/left/input/trigger/value"},
    {TriggerTouch, "/user/hand/left/input/trigger/touch"},
    {SelectClick, "/user/hand/left/input/trigger/value"},
    {MenuClick, "/user/hand/left/input/menu/click"},
    {Grip, "/user/hand/left/input/squeeze/value"},
    {GripClick, "/user/hand/left/input/squeeze/value"},
    {PrimaryAnalogStickAxis, "/user/hand/left/input/thumbstick"},
    {PrimaryAnalogStickClick, "/user/hand/left/input/thumbstick/click"},
    {PrimaryAnalogStickTouch, "/user/hand/left/input/thumbstick/touch"},
    {ButtonA, "/user/hand/left/input/x/click"}, // X button = Button A on left
    {ButtonATouch, "/user/hand/left/input/x/touch"},
    {ButtonB, "/user/hand/left/input/y/click"}, // Y button = Button B on left
    {ButtonBTouch, "/user/hand/left/input/y/touch"},
    {Thumbrest, "/user/hand/left/input/thumbrest/touch"},
    {GripPose, "/user/hand/left/input/grip/pose"},
    {AimPose, "/user/hand/left/input/aim/pose"},

    // Right hand
    {Trigger, "/user/hand/right/input/trigger/value"},
    {TriggerTouch, "/user/hand/right/input/trigger/touch"},
    {SelectClick, "/user/hand/right/input/trigger/value"},
    {Grip, "/user/hand/right/input/squeeze/value"},
    {GripClick, "/user/hand/right/input/squeeze/value"},
    {PrimaryAnalogStickAxis, "/user/hand/right/input/thumbstick"},
    {PrimaryAnalogStickClick, "/user/hand/right/input/thumbstick/click"},
    {PrimaryAnalogStickTouch, "/user/hand/right/input/thumbstick/touch"},
    {ButtonA, "/user/hand/right/input/a/click"},
    {ButtonATouch, "/user/hand/right/input/a/touch"},
    {ButtonB, "/user/hand/right/input/b/click"},
    {ButtonBTouch, "/user/hand/right/input/b/touch"},
    {Thumbrest, "/user/hand/right/input/thumbrest/touch"},
    {GripPose, "/user/hand/right/input/grip/pose"},
    {AimPose, "/user/hand/right/input/aim/pose"},

    {m_HapticAction, "/user/hand/left/output/haptic"},
    {m_HapticAction, "/user/hand/right/output/haptic"},
  };
  SuggestInteractionProfileBindings("/interaction_profiles/oculus/touch_controller", "Oculus Touch Controller", oculusTouchController);

  // Valve Index Controller (Knuckles)
  // Has thumbstick (primary), trackpad (secondary), and A/B buttons
  // Also has per-finger tracking via capacitive sensors and force grip
  Bind valveIndexController[] = {
    // Left hand
    {Trigger, "/user/hand/left/input/trigger/value"},
    {TriggerTouch, "/user/hand/left/input/trigger/touch"},
    {SelectClick, "/user/hand/left/input/trigger/click"},
    {Grip, "/user/hand/left/input/squeeze/value"},
    {GripClick, "/user/hand/left/input/squeeze/force"}, // Index has force-sensitive grip
    {PrimaryAnalogStickAxis, "/user/hand/left/input/thumbstick"},
    {PrimaryAnalogStickClick, "/user/hand/left/input/thumbstick/click"},
    {PrimaryAnalogStickTouch, "/user/hand/left/input/thumbstick/touch"},
    {SecondaryAnalogStickAxis, "/user/hand/left/input/trackpad"},
    {SecondaryAnalogStickTouch, "/user/hand/left/input/trackpad/touch"},
    {ButtonA, "/user/hand/left/input/a/click"},
    {ButtonATouch, "/user/hand/left/input/a/touch"},
    {ButtonB, "/user/hand/left/input/b/click"},
    {ButtonBTouch, "/user/hand/left/input/b/touch"},
    {Thumbrest, "/user/hand/left/input/thumbstick/touch"}, // Use thumbstick touch as thumbrest
    {GripPose, "/user/hand/left/input/grip/pose"},
    {AimPose, "/user/hand/left/input/aim/pose"},

    // Right hand
    {Trigger, "/user/hand/right/input/trigger/value"},
    {TriggerTouch, "/user/hand/right/input/trigger/touch"},
    {SelectClick, "/user/hand/right/input/trigger/click"},
    {Grip, "/user/hand/right/input/squeeze/value"},
    {GripClick, "/user/hand/right/input/squeeze/force"},
    {PrimaryAnalogStickAxis, "/user/hand/right/input/thumbstick"},
    {PrimaryAnalogStickClick, "/user/hand/right/input/thumbstick/click"},
    {PrimaryAnalogStickTouch, "/user/hand/right/input/thumbstick/touch"},
    {SecondaryAnalogStickAxis, "/user/hand/right/input/trackpad"},
    {SecondaryAnalogStickTouch, "/user/hand/right/input/trackpad/touch"},
    {ButtonA, "/user/hand/right/input/a/click"},
    {ButtonATouch, "/user/hand/right/input/a/touch"},
    {ButtonB, "/user/hand/right/input/b/click"},
    {ButtonBTouch, "/user/hand/right/input/b/touch"},
    {Thumbrest, "/user/hand/right/input/thumbstick/touch"},
    {GripPose, "/user/hand/right/input/grip/pose"},
    {AimPose, "/user/hand/right/input/aim/pose"},

    {m_HapticAction, "/user/hand/left/output/haptic"},
    {m_HapticAction, "/user/hand/right/output/haptic"},
  };
  SuggestInteractionProfileBindings("/interaction_profiles/valve/index_controller", "Valve Index Controller", valveIndexController);

  // HTC Vive Controller (Wands)
  // Has only trackpad (bound as primary since no thumbstick), trigger, grip, and menu

  Bind htcViveController[] = {
    // Left hand
    {Trigger, "/user/hand/left/input/trigger/value"},
    {SelectClick, "/user/hand/left/input/trigger/click"},
    {MenuClick, "/user/hand/left/input/menu/click"},
    {GripClick, "/user/hand/left/input/squeeze/click"},         // Vive grip is click only, no analog
    {PrimaryAnalogStickAxis, "/user/hand/left/input/trackpad"}, // Trackpad as primary
    {PrimaryAnalogStickClick, "/user/hand/left/input/trackpad/click"},
    {PrimaryAnalogStickTouch, "/user/hand/left/input/trackpad/touch"},
    {GripPose, "/user/hand/left/input/grip/pose"},
    {AimPose, "/user/hand/left/input/aim/pose"},

    // Right hand
    {Trigger, "/user/hand/right/input/trigger/value"},
    {SelectClick, "/user/hand/right/input/trigger/click"},
    {MenuClick, "/user/hand/right/input/menu/click"},
    {GripClick, "/user/hand/right/input/squeeze/click"},
    {PrimaryAnalogStickAxis, "/user/hand/right/input/trackpad"},
    {PrimaryAnalogStickClick, "/user/hand/right/input/trackpad/click"},
    {PrimaryAnalogStickTouch, "/user/hand/right/input/trackpad/touch"},
    {GripPose, "/user/hand/right/input/grip/pose"},
    {AimPose, "/user/hand/right/input/aim/pose"},

    {m_HapticAction, "/user/hand/left/output/haptic"},
    {m_HapticAction, "/user/hand/right/output/haptic"},
  };
  SuggestInteractionProfileBindings("/interaction_profiles/htc/vive_controller", "HTC Vive Controller", htcViveController);

  // Microsoft Hand Interaction (hand tracking without controllers)
  if (m_pOpenXR->m_Extensions.m_bHandInteraction)
  {
    Bind handInteraction[] = {
      {SelectClick, "/user/hand/left/input/select/value"},
      {Grip, "/user/hand/left/input/squeeze/value"},
      {GripPose, "/user/hand/left/input/grip/pose"},
      {AimPose, "/user/hand/left/input/aim/pose"},

      {SelectClick, "/user/hand/right/input/select/value"},
      {Grip, "/user/hand/right/input/squeeze/value"},
      {GripPose, "/user/hand/right/input/grip/pose"},
      {AimPose, "/user/hand/right/input/aim/pose"},
    };
    SuggestInteractionProfileBindings("/interaction_profiles/microsoft/hand_interaction", "Hand Interaction", handInteraction);
  }

  //=============================================================================
  // EXTENSION INTERACTION PROFILES
  // These profile paths only exist if the runtime enabled the matching extension
  // (XR_EXT_hand_interaction, XR_FB_touch_controller_pro,
  // XR_HTC_vive_cosmos_controller_interaction, XR_HTC_vive_focus3_controller_interaction,
  // XR_BD_controller_interaction). They are suggested as optional so a runtime
  // without them reports info instead of an error.
  //=============================================================================

  // EXT Hand Interaction (articulated hands, no controller and no haptics)
  Bind extHandInteraction[] = {
    {Trigger, "/user/hand/left/input/aim_activate_ext/value"},
    {SelectClick, "/user/hand/left/input/aim_activate_ext/value"},
    {Grip, "/user/hand/left/input/grasp_ext/value"},
    {GripClick, "/user/hand/left/input/grasp_ext/value"},
    {GripPose, "/user/hand/left/input/grip/pose"},
    {AimPose, "/user/hand/left/input/aim/pose"},

    {Trigger, "/user/hand/right/input/aim_activate_ext/value"},
    {SelectClick, "/user/hand/right/input/aim_activate_ext/value"},
    {Grip, "/user/hand/right/input/grasp_ext/value"},
    {GripClick, "/user/hand/right/input/grasp_ext/value"},
    {GripPose, "/user/hand/right/input/grip/pose"},
    {AimPose, "/user/hand/right/input/aim/pose"},
  };
  SuggestInteractionProfileBindings("/interaction_profiles/ext/hand_interaction_ext", "Hand Interaction", extHandInteraction, true);

  // Meta Quest Touch Pro
  // Superset of the Oculus Touch controller, so the same layout is used.
  Bind touchControllerPro[] = {
    // Left hand
    {Trigger, "/user/hand/left/input/trigger/value"},
    {TriggerTouch, "/user/hand/left/input/trigger/touch"},
    {SelectClick, "/user/hand/left/input/trigger/value"},
    {MenuClick, "/user/hand/left/input/menu/click"},
    {Grip, "/user/hand/left/input/squeeze/value"},
    {GripClick, "/user/hand/left/input/squeeze/value"},
    {PrimaryAnalogStickAxis, "/user/hand/left/input/thumbstick"},
    {PrimaryAnalogStickClick, "/user/hand/left/input/thumbstick/click"},
    {PrimaryAnalogStickTouch, "/user/hand/left/input/thumbstick/touch"},
    {ButtonA, "/user/hand/left/input/x/click"},
    {ButtonATouch, "/user/hand/left/input/x/touch"},
    {ButtonB, "/user/hand/left/input/y/click"},
    {ButtonBTouch, "/user/hand/left/input/y/touch"},
    {Thumbrest, "/user/hand/left/input/thumbrest/touch"},
    {GripPose, "/user/hand/left/input/grip/pose"},
    {AimPose, "/user/hand/left/input/aim/pose"},

    // Right hand
    {Trigger, "/user/hand/right/input/trigger/value"},
    {TriggerTouch, "/user/hand/right/input/trigger/touch"},
    {SelectClick, "/user/hand/right/input/trigger/value"},
    {Grip, "/user/hand/right/input/squeeze/value"},
    {GripClick, "/user/hand/right/input/squeeze/value"},
    {PrimaryAnalogStickAxis, "/user/hand/right/input/thumbstick"},
    {PrimaryAnalogStickClick, "/user/hand/right/input/thumbstick/click"},
    {PrimaryAnalogStickTouch, "/user/hand/right/input/thumbstick/touch"},
    {ButtonA, "/user/hand/right/input/a/click"},
    {ButtonATouch, "/user/hand/right/input/a/touch"},
    {ButtonB, "/user/hand/right/input/b/click"},
    {ButtonBTouch, "/user/hand/right/input/b/touch"},
    {Thumbrest, "/user/hand/right/input/thumbrest/touch"},
    {GripPose, "/user/hand/right/input/grip/pose"},
    {AimPose, "/user/hand/right/input/aim/pose"},

    {m_HapticAction, "/user/hand/left/output/haptic"},
    {m_HapticAction, "/user/hand/right/output/haptic"},
  };
  SuggestInteractionProfileBindings("/interaction_profiles/facebook/touch_controller_pro", "Meta Quest Touch Pro", touchControllerPro, true);

  // HTC Vive Cosmos Controller
  // Thumbstick plus X/Y (left) and A/B (right) buttons, grip is click only.
  Bind viveCosmosController[] = {
    // Left hand
    {Trigger, "/user/hand/left/input/trigger/value"},
    {SelectClick, "/user/hand/left/input/trigger/click"},
    {MenuClick, "/user/hand/left/input/menu/click"},
    {GripClick, "/user/hand/left/input/squeeze/click"},
    {PrimaryAnalogStickAxis, "/user/hand/left/input/thumbstick"},
    {PrimaryAnalogStickClick, "/user/hand/left/input/thumbstick/click"},
    {PrimaryAnalogStickTouch, "/user/hand/left/input/thumbstick/touch"},
    {ButtonA, "/user/hand/left/input/x/click"},
    {ButtonB, "/user/hand/left/input/y/click"},
    {GripPose, "/user/hand/left/input/grip/pose"},
    {AimPose, "/user/hand/left/input/aim/pose"},

    // Right hand
    {Trigger, "/user/hand/right/input/trigger/value"},
    {SelectClick, "/user/hand/right/input/trigger/click"},
    {GripClick, "/user/hand/right/input/squeeze/click"},
    {PrimaryAnalogStickAxis, "/user/hand/right/input/thumbstick"},
    {PrimaryAnalogStickClick, "/user/hand/right/input/thumbstick/click"},
    {PrimaryAnalogStickTouch, "/user/hand/right/input/thumbstick/touch"},
    {ButtonA, "/user/hand/right/input/a/click"},
    {ButtonB, "/user/hand/right/input/b/click"},
    {GripPose, "/user/hand/right/input/grip/pose"},
    {AimPose, "/user/hand/right/input/aim/pose"},

    {m_HapticAction, "/user/hand/left/output/haptic"},
    {m_HapticAction, "/user/hand/right/output/haptic"},
  };
  SuggestInteractionProfileBindings("/interaction_profiles/htc/vive_cosmos_controller", "HTC Vive Cosmos", viveCosmosController, true);

  // HTC Vive Focus 3 Controller
  Bind viveFocus3Controller[] = {
    // Left hand
    {Trigger, "/user/hand/left/input/trigger/value"},
    {SelectClick, "/user/hand/left/input/trigger/click"},
    {MenuClick, "/user/hand/left/input/menu/click"},
    {GripClick, "/user/hand/left/input/squeeze/click"},
    {PrimaryAnalogStickAxis, "/user/hand/left/input/thumbstick"},
    {PrimaryAnalogStickClick, "/user/hand/left/input/thumbstick/click"},
    {PrimaryAnalogStickTouch, "/user/hand/left/input/thumbstick/touch"},
    {ButtonA, "/user/hand/left/input/x/click"},
    {ButtonB, "/user/hand/left/input/y/click"},
    {GripPose, "/user/hand/left/input/grip/pose"},
    {AimPose, "/user/hand/left/input/aim/pose"},

    // Right hand
    {Trigger, "/user/hand/right/input/trigger/value"},
    {SelectClick, "/user/hand/right/input/trigger/click"},
    {GripClick, "/user/hand/right/input/squeeze/click"},
    {PrimaryAnalogStickAxis, "/user/hand/right/input/thumbstick"},
    {PrimaryAnalogStickClick, "/user/hand/right/input/thumbstick/click"},
    {PrimaryAnalogStickTouch, "/user/hand/right/input/thumbstick/touch"},
    {ButtonA, "/user/hand/right/input/a/click"},
    {ButtonB, "/user/hand/right/input/b/click"},
    {GripPose, "/user/hand/right/input/grip/pose"},
    {AimPose, "/user/hand/right/input/aim/pose"},

    {m_HapticAction, "/user/hand/left/output/haptic"},
    {m_HapticAction, "/user/hand/right/output/haptic"},
  };
  SuggestInteractionProfileBindings("/interaction_profiles/htc/vive_focus3_controller", "HTC Vive Focus 3", viveFocus3Controller, true);

  // Pico Neo3 Controller
  Bind picoNeo3Controller[] = {
    // Left hand
    {Trigger, "/user/hand/left/input/trigger/value"},
    {SelectClick, "/user/hand/left/input/trigger/click"},
    {MenuClick, "/user/hand/left/input/menu/click"},
    {Grip, "/user/hand/left/input/squeeze/value"},
    {GripClick, "/user/hand/left/input/squeeze/click"},
    {PrimaryAnalogStickAxis, "/user/hand/left/input/thumbstick"},
    {PrimaryAnalogStickClick, "/user/hand/left/input/thumbstick/click"},
    {PrimaryAnalogStickTouch, "/user/hand/left/input/thumbstick/touch"},
    {ButtonA, "/user/hand/left/input/x/click"},
    {ButtonB, "/user/hand/left/input/y/click"},
    {GripPose, "/user/hand/left/input/grip/pose"},
    {AimPose, "/user/hand/left/input/aim/pose"},

    // Right hand
    {Trigger, "/user/hand/right/input/trigger/value"},
    {SelectClick, "/user/hand/right/input/trigger/click"},
    {Grip, "/user/hand/right/input/squeeze/value"},
    {GripClick, "/user/hand/right/input/squeeze/click"},
    {PrimaryAnalogStickAxis, "/user/hand/right/input/thumbstick"},
    {PrimaryAnalogStickClick, "/user/hand/right/input/thumbstick/click"},
    {PrimaryAnalogStickTouch, "/user/hand/right/input/thumbstick/touch"},
    {ButtonA, "/user/hand/right/input/a/click"},
    {ButtonB, "/user/hand/right/input/b/click"},
    {GripPose, "/user/hand/right/input/grip/pose"},
    {AimPose, "/user/hand/right/input/aim/pose"},

    {m_HapticAction, "/user/hand/left/output/haptic"},
    {m_HapticAction, "/user/hand/right/output/haptic"},
  };
  SuggestInteractionProfileBindings("/interaction_profiles/bytedance/pico_neo3_controller", "Pico Neo3", picoNeo3Controller, true);


  XrActionSpaceCreateInfo spaceCreateInfo{XR_TYPE_ACTION_SPACE_CREATE_INFO};
  spaceCreateInfo.poseInActionSpace = m_pOpenXR->ConvertTransform(plTransform::MakeIdentity());
  for (plUInt32 uiSide : {0, 1})
  {
    spaceCreateInfo.subactionPath = m_SubActionPath[uiSide];
    spaceCreateInfo.action = GripPose;
    XR_SUCCEED_OR_CLEANUP_LOG(xrCreateActionSpace(m_pSession, &spaceCreateInfo, &m_gripSpace[uiSide]), DestroyActions);

    spaceCreateInfo.action = AimPose;
    XR_SUCCEED_OR_CLEANUP_LOG(xrCreateActionSpace(m_pSession, &spaceCreateInfo, &m_aimSpace[uiSide]), DestroyActions);
  }

  // Register input slots now that actions are created
  // This must happen after CreateAction calls populate the action arrays
  RegisterInputSlots();

  // The input thread is deliberately NOT started here. Actions and the spaces derived from them are only usable once
  // the action set is attached to the session, so polling starts in AttachSessionActionSets.

  return XR_SUCCESS;
}

void plOpenXRInputDevice::DestroyActions()
{
  // Stop input thread before destroying actions
  StopInputThread();

  // Action spaces first: destroying an action (or the action set that owns it) implicitly destroys the spaces derived
  // from it, so the spaces must go while their handles are still valid.
  for (plUInt32 uiSide : {0, 1})
  {
    if (m_gripSpace[uiSide])
    {
      XR_LOG_ERROR(xrDestroySpace(m_gripSpace[uiSide]));
      m_gripSpace[uiSide] = XR_NULL_HANDLE;
    }
    if (m_aimSpace[uiSide])
    {
      XR_LOG_ERROR(xrDestroySpace(m_aimSpace[uiSide]));
      m_aimSpace[uiSide] = XR_NULL_HANDLE;
    }
  }

  for (Action& action : m_BooleanActions)
  {
    XR_LOG_ERROR(xrDestroyAction(action.m_Action));
  }
  m_BooleanActions.Clear();

  for (Action& action : m_FloatActions)
  {
    XR_LOG_ERROR(xrDestroyAction(action.m_Action));
  }
  m_FloatActions.Clear();

  for (Vec2Action& action : m_Vec2Actions)
  {
    XR_LOG_ERROR(xrDestroyAction(action.m_Action));
  }
  m_Vec2Actions.Clear();

  for (Action& action : m_PoseActions)
  {
    XR_LOG_ERROR(xrDestroyAction(action.m_Action));
  }
  m_PoseActions.Clear();

  // The vibration output action is tracked on its own as it has no input slots.
  if (m_HapticAction)
  {
    XR_LOG_ERROR(xrDestroyAction(m_HapticAction));
    m_HapticAction = XR_NULL_HANDLE;
  }

  if (m_pActionSet)
  {
    XR_LOG_ERROR(xrDestroyActionSet(m_pActionSet));
    m_pActionSet = XR_NULL_HANDLE;
  }

  for (plUInt32 uiSide : {0, 1})
  {
    if (m_gripSpace[uiSide])
    {
      XR_LOG_ERROR(xrDestroySpace(m_gripSpace[uiSide]));
      m_gripSpace[uiSide] = XR_NULL_HANDLE;
    }
    if (m_aimSpace[uiSide])
    {
      XR_LOG_ERROR(xrDestroySpace(m_aimSpace[uiSide]));
      m_aimSpace[uiSide] = XR_NULL_HANDLE;
    }
  }

  for (plUInt32 i = 0; i < s_iMaxDevices; i++)
  {
    m_sActiveProfile[i].Clear();
    m_SupportedFeatures[i].Clear();
    m_DeviceState[i].m_bDeviceIsConnected = false;
  }

  for (plUInt32 uiSide : {0, 1})
  {
    m_DiscoveredFeatures[uiSide].Clear();
  }
}

XrPath plOpenXRInputDevice::CreatePath(const char* szPath)
{
  XrInstance instance = m_pOpenXR->m_pInstance;

  XrPath path;
  if (xrStringToPath(instance, szPath, &path) != XR_SUCCESS)
  {
    plLog::Error("OpenXR path conversion failure: {0}", szPath);
  }
  return path;
}

XrResult plOpenXRInputDevice::CreateAction(plXRDeviceFeatures::Enum feature, const char* actionName, XrActionType actionType, XrAction& out_action)
{
  XrActionCreateInfo actionCreateInfo{XR_TYPE_ACTION_CREATE_INFO};
  actionCreateInfo.actionType = actionType;
  actionCreateInfo.countSubactionPaths = 2;
  actionCreateInfo.subactionPaths = m_SubActionPath.GetData();
  plStringUtils::Copy(actionCreateInfo.actionName, XR_MAX_ACTION_NAME_SIZE, actionName);
  plStringUtils::Copy(actionCreateInfo.localizedActionName, XR_MAX_LOCALIZED_ACTION_NAME_SIZE, actionName);

  XR_SUCCEED_OR_CLEANUP_LOG(xrCreateAction(m_pActionSet, &actionCreateInfo, &out_action), voidFunction);

  plStringBuilder sLeft(m_SubActionPrefix[0], actionName);
  plStringBuilder sRight(m_SubActionPrefix[1], actionName);

  switch (actionType)
  {
    case XR_ACTION_TYPE_BOOLEAN_INPUT:
      m_BooleanActions.PushBack({feature, out_action, sLeft, sRight});
      break;
    case XR_ACTION_TYPE_FLOAT_INPUT:
      m_FloatActions.PushBack({feature, out_action, sLeft, sRight});
      break;
    case XR_ACTION_TYPE_VECTOR2F_INPUT:
      m_Vec2Actions.PushBack(Vec2Action(feature, out_action, sLeft, sRight));
      break;
    case XR_ACTION_TYPE_POSE_INPUT:
      m_PoseActions.PushBack({feature, out_action, sLeft, sRight});
      break;
    case XR_ACTION_TYPE_VIBRATION_OUTPUT:
      // Output actions have no state to poll and no input slot, the caller keeps the handle.
      break;
    default:
      PL_ASSERT_NOT_IMPLEMENTED;
  }

  return XR_SUCCESS;
}

XrResult plOpenXRInputDevice::SuggestInteractionProfileBindings(
  const char* szInteractionProfile, const char* szNiceName, plArrayPtr<Bind> bindings, bool bOptionalProfile)
{
  XrInstance instance = m_pOpenXR->m_pInstance;

  XrPath InteractionProfile = CreatePath(szInteractionProfile);

  plDynamicArray<XrActionSuggestedBinding> xrBindings;
  xrBindings.Reserve(bindings.GetCount());
  for (const Bind& binding : bindings)
  {
    xrBindings.PushBack({binding.action, CreatePath(binding.szPath)});
  }


  XrInteractionProfileSuggestedBinding profileBindings{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
  profileBindings.interactionProfile = InteractionProfile;
  profileBindings.suggestedBindings = xrBindings.GetData();
  profileBindings.countSuggestedBindings = xrBindings.GetCount();

  const XrResult res = xrSuggestInteractionProfileBindings(instance, &profileBindings);
  if (XR_FAILED_RESULT(res))
  {
    if (bOptionalProfile)
    {
      plLog::Info("OpenXR: Interaction profile '{0}' is unavailable ({1}), its bindings are skipped.", szInteractionProfile, res);
    }
    else
    {
      plLog::Error("OpenXR: Suggesting bindings for interaction profile '{0}' failed with: {1}", szInteractionProfile, res);
    }
    return res;
  }

  m_InteractionProfileToNiceName[InteractionProfile] = szNiceName;

  return XR_SUCCESS;
}

XrResult plOpenXRInputDevice::AttachSessionActionSets(XrSession session)
{
  m_pSession = session;
  XrSessionActionSetsAttachInfo attachInfo{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
  plHybridArray<XrActionSet, 1> actionSets;
  actionSets.PushBack(m_pActionSet);

  attachInfo.countActionSets = actionSets.GetCount();
  attachInfo.actionSets = actionSets.GetData();
  XR_SUCCEED_OR_RETURN_LOG(xrAttachSessionActionSets(session, &attachInfo));

  // The poll thread must not start before attachment: until then xrSyncActions and xrGetActionState* return
  // XR_ERROR_ACTIONSET_NOT_ATTACHED and the action spaces are not locatable.
  StartInputThread();

  return XR_SUCCESS;
}

XrResult plOpenXRInputDevice::UpdateCurrentInteractionProfile()
{
  // This function is triggered by the XR_TYPE_EVENT_DATA_INTERACTION_PROFILE_CHANGED event.
  // Unfortunately it does not seem to provide any info in regards to what top level path is affected
  // so we check both controllers again.
  auto GetActiveControllerProfile = [this](plUInt32 uiSide) -> XrPath
  {
    XrInteractionProfileState state{XR_TYPE_INTERACTION_PROFILE_STATE};
    XrResult res = xrGetCurrentInteractionProfile(m_pSession, m_SubActionPath[uiSide], &state);
    if (res == XR_SUCCESS)
    {
#if PL_ENABLED(PL_COMPILE_FOR_DEBUG)
      if (state.interactionProfile != XR_NULL_PATH && !m_InteractionProfileToNiceName.Contains(state.interactionProfile))
      {
        char buffer[256];
        plUInt32 temp;
        xrPathToString(m_pInstance, state.interactionProfile, 256, &temp, buffer);
        PL_REPORT_FAILURE("Unknown interaction profile was selected by the OpenXR runtime: '{}'", buffer);
      }
#endif
      return state.interactionProfile;
    }
    return XR_NULL_PATH;
  };

  for (plUInt32 uiSide : {0, 1})
  {
    const plUInt32 uiControllerId = uiSide == 0 ? 1 : 2;
    XrPath path = GetActiveControllerProfile(uiSide);
    plString newProfile = m_InteractionProfileToNiceName[path];

    // Only invalidate cache if the profile actually changed
    if (m_sActiveProfile[uiControllerId] && m_sActiveProfile[uiControllerId] != newProfile)
    {
      plLog::Info("OpenXR: Controller {} profile changed from '{}' to '{}'",
        uiSide == 0 ? "Left" : "Right",
        m_sActiveProfile[uiControllerId],
        newProfile);
      m_sActiveProfile[uiControllerId] = newProfile;
      m_bBoundActionsCached[uiSide] = false;
    }
  }

  UpdateActions();

  return XR_SUCCESS;
}

void plOpenXRInputDevice::InitializeDevice()
{
  RegisterInputSlots();
}

void plOpenXRInputDevice::RegisterInputSlots()
{
  for (const Action& action : m_BooleanActions)
  {
    for (plUInt32 uiSide : {0, 1})
    {
      RegisterInputSlot(action.m_sKey[uiSide], action.m_sKey[uiSide], plInputSlotFlags::IsButton);
    }
  }
  for (const Action& action : m_FloatActions)
  {
    for (plUInt32 uiSide : {0, 1})
    {
      RegisterInputSlot(action.m_sKey[uiSide], action.m_sKey[uiSide], plInputSlotFlags::IsAnalogTrigger);
    }
  }
  for (const Vec2Action& action : m_Vec2Actions)
  {
    for (plUInt32 uiSide : {0, 1})
    {
      RegisterInputSlot(action.m_sKey_negx[uiSide], action.m_sKey_negx[uiSide], plInputSlotFlags::IsAnalogStick);
      RegisterInputSlot(action.m_sKey_posx[uiSide], action.m_sKey_posx[uiSide], plInputSlotFlags::IsAnalogStick);
      RegisterInputSlot(action.m_sKey_negy[uiSide], action.m_sKey_negy[uiSide], plInputSlotFlags::IsAnalogStick);
      RegisterInputSlot(action.m_sKey_posy[uiSide], action.m_sKey_posy[uiSide], plInputSlotFlags::IsAnalogStick);
    }
  }
}

void plOpenXRInputDevice::UpdateInputSlotValues()
{
  // Input values are updated by UpdateActions() called from plOpenXR::UpdatePoses().
  // m_InputSlotValues already contains current values for the input manager to read.
}

XrResult plOpenXRInputDevice::UpdateActions()
{
  if (m_pSession == XR_NULL_HANDLE)
    return XR_SUCCESS;

  PL_PROFILE_SCOPE("UpdateActions");

  // Copy latest input state from background thread's snapshot
  CopySnapshotToMainThread();

  UpdateControllerState();
  return XR_SUCCESS;
}

void plOpenXRInputDevice::CopySnapshotToMainThread()
{
  PL_PROFILE_SCOPE("CopyInputSnapshot");

  // Read from the current readable snapshot (lock-free read of atomic index)
  const plInt32 readIdx = m_iReadableSnapshot;
  const InputSnapshot& snapshot = m_InputSnapshots[readIdx];

  // Copy device states
  for (plUInt32 i = 0; i < s_iMaxDevices; ++i)
  {
    m_DeviceState[i] = snapshot.m_DeviceState[i];
    m_SupportedFeatures[i] = snapshot.m_SupportedFeatures[i];
  }

  // Copy input slot values
  for (auto it = snapshot.m_InputSlotValues.GetIterator(); it.IsValid(); ++it)
  {
    m_InputSlotValues[it.Key()] = it.Value();
  }
}


XrSpaceLocation plOpenXRInputDevice::UpdateLeftShoulderTracking(plXRDeviceState& deviceState)
{
  const XrFrameState& frameState = m_pOpenXR->m_FrameState;

  const XrSpace baseSpace = m_pOpenXR->GetBaseSpace();
  const XrTime time = frameState.predictedDisplayTime;

  XrActiveActionSet activeActionSet{};
  activeActionSet.actionSet = m_pActionSet;
  activeActionSet.subactionPath = m_SubActionPathVive[1];

  XrActionsSyncInfo syncInfo{XR_TYPE_ACTIONS_SYNC_INFO};
  syncInfo.countActiveActionSets = 1;
  syncInfo.activeActionSets = &activeActionSet;

  xrSyncActions(m_pSession, &syncInfo);

  // 2. Check Action State
  XrActionStateGetInfo getInfo{XR_TYPE_ACTION_STATE_GET_INFO};
  getInfo.action = shoulderPoseAction;
  getInfo.subactionPath = m_SubActionPathVive[1];

  XrActionStatePose poseState{XR_TYPE_ACTION_STATE_POSE};
  xrGetActionStatePose(m_pSession, &getInfo, &poseState);
  XrSpaceLocation location{XR_TYPE_SPACE_LOCATION};

  if (poseState.isActive)
  {
    // 3. Locate the space relative to world reference space (e.g., STAGE or LOCAL)
    xrLocateSpace(shoulderSpace, baseSpace, time, &location);

    if ((location.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) &&
        (location.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT))
    {

      XrVector3f pos = location.pose.position;
      XrQuaternionf rot = location.pose.orientation;

      // Pass position and rotation to your engine's tracking/IK solver
    }
  }
  // Individual pose queries
  if (xrLocateSpace(shoulderSpace, baseSpace, time, &location) == XR_SUCCESS)
  {
    if ((location.locationFlags & (XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT)) ==
        (XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT))
    {
      deviceState.m_vGripPosition = plOpenXR::ConvertPosition(location.pose.position);
      deviceState.m_qGripRotation = plOpenXR::ConvertOrientation(location.pose.orientation);
      deviceState.m_bGripPoseIsValid = true;
      deviceState.m_vAimPosition = plOpenXR::ConvertPosition(location.pose.position);
      deviceState.m_qAimRotation = plOpenXR::ConvertOrientation(location.pose.orientation);
      deviceState.m_bAimPoseIsValid = true;
    }
    else
    {

      deviceState.m_bGripPoseIsValid = false;
      deviceState.m_bAimPoseIsValid = false;
    }
  }
  return location;
}
void plOpenXRInputDevice::UpdateActionsOnInputThread()
{
  if (m_pSession == XR_NULL_HANDLE)
    return;
  PL_PROFILE_SCOPE("UpdateActionsThreaded");

  // Write to the buffer that main thread is NOT reading
  const plInt32 writeIdx = 1 - m_iReadableSnapshot;
  InputSnapshot& snapshot = m_InputSnapshots[writeIdx];

  // Sync actions
  XrActiveActionSet activeActionSet{m_pActionSet, XR_NULL_PATH};
  XrActionsSyncInfo syncInfo{XR_TYPE_ACTIONS_SYNC_INFO};
  syncInfo.countActiveActionSets = 1;
  syncInfo.activeActionSets = &activeActionSet;

  XrResult res = xrSyncActions(m_pSession, &syncInfo);
  if (res != XR_SUCCESS)
    return;

  ApplyPendingHaptics();

  // Read the published copy rather than plOpenXR::m_FrameState: the main thread overwrites that whole struct in
  // BeginFrame and PreWaitNextFrame, so reading it from this thread can tear.
  const XrTime time = m_pOpenXR->GetPredictedDisplayTime();
  if (time == 0)
    return;

  const XrSpace baseSpace = m_pOpenXR->GetBaseSpace();
  UpdateLeftShoulderTracking(snapshot.m_DeviceState[m_iLeftShoulderDeviceId]);

  for (plUInt32 uiSide : {0, 1})
  {
    const plUInt32 uiControllerId = uiSide == 0 ? m_iLeftControllerDeviceID : m_iRightControllerDeviceID;
    const XrPath subActionPath = m_SubActionPath[uiSide];

    // Check if controller is active
    XrActionStatePose poseState{XR_TYPE_ACTION_STATE_POSE};
    XrActionStateGetInfo poseGetInfo{XR_TYPE_ACTION_STATE_GET_INFO};
    poseGetInfo.action = m_PoseActions[0].m_Action;
    poseGetInfo.subactionPath = subActionPath;
    if (xrGetActionStatePose(m_pSession, &poseGetInfo, &poseState) != XR_SUCCESS)
      continue;

    const bool bControllerActive = poseState.isActive;

    if (!bControllerActive)
    {
      if (m_bBoundActionsCached[uiSide])
      {
        // Clear values for disconnected controller
        for (plUInt8 idx : m_BoundBooleanActions[uiSide])
          snapshot.m_InputSlotValues[m_BooleanActions[idx].m_sKey[uiSide]] = 0.0f;
        for (plUInt8 idx : m_BoundFloatActions[uiSide])
          snapshot.m_InputSlotValues[m_FloatActions[idx].m_sKey[uiSide]] = 0.0f;
        for (plUInt8 idx : m_BoundVec2Actions[uiSide])
        {
          const Vec2Action& action = m_Vec2Actions[idx];
          snapshot.m_InputSlotValues[action.m_sKey_negx[uiSide]] = 0.0f;
          snapshot.m_InputSlotValues[action.m_sKey_posx[uiSide]] = 0.0f;
          snapshot.m_InputSlotValues[action.m_sKey_negy[uiSide]] = 0.0f;
          snapshot.m_InputSlotValues[action.m_sKey_posy[uiSide]] = 0.0f;
        }
        m_bBoundActionsCached[uiSide] = false;
      }
      m_DiscoveredFeatures[uiSide].Clear();
      snapshot.m_SupportedFeatures[uiControllerId].Clear();
      continue;
    }

    // Discover or query bound actions
    if (!m_bBoundActionsCached[uiSide])
    {
      m_BoundBooleanActions[uiSide].Clear();
      m_BoundFloatActions[uiSide].Clear();
      m_BoundVec2Actions[uiSide].Clear();
      m_DiscoveredFeatures[uiSide] = plXRDeviceFeatures::GripPose | plXRDeviceFeatures::AimPose;

      if (IsHapticActionBound(uiSide))
      {
        m_DiscoveredFeatures[uiSide].Add(plXRDeviceFeatures::Haptics);
      }

      XrActionStateGetInfo getInfo{XR_TYPE_ACTION_STATE_GET_INFO};
      getInfo.subactionPath = subActionPath;

      for (plUInt8 i = 0; i < m_BooleanActions.GetCount(); ++i)
      {
        XrActionStateBoolean state{XR_TYPE_ACTION_STATE_BOOLEAN};
        getInfo.action = m_BooleanActions[i].m_Action;
        if (xrGetActionStateBoolean(m_pSession, &getInfo, &state) == XR_SUCCESS)
        {
          if (state.isActive)
          {
            m_BoundBooleanActions[uiSide].PushBack(i);
            m_DiscoveredFeatures[uiSide].Add(m_BooleanActions[i].m_Feature);
          }
          snapshot.m_InputSlotValues[m_BooleanActions[i].m_sKey[uiSide]] = state.currentState ? 1.0f : 0.0f;
        }
      }

      for (plUInt8 i = 0; i < m_FloatActions.GetCount(); ++i)
      {
        XrActionStateFloat state{XR_TYPE_ACTION_STATE_FLOAT};
        getInfo.action = m_FloatActions[i].m_Action;
        if (xrGetActionStateFloat(m_pSession, &getInfo, &state) == XR_SUCCESS)
        {
          if (state.isActive)
          {
            m_BoundFloatActions[uiSide].PushBack(i);
            m_DiscoveredFeatures[uiSide].Add(m_FloatActions[i].m_Feature);
          }
          snapshot.m_InputSlotValues[m_FloatActions[i].m_sKey[uiSide]] = state.currentState;
        }
      }

      for (plUInt8 i = 0; i < m_Vec2Actions.GetCount(); ++i)
      {
        XrActionStateVector2f state{XR_TYPE_ACTION_STATE_VECTOR2F};
        getInfo.action = m_Vec2Actions[i].m_Action;
        if (xrGetActionStateVector2f(m_pSession, &getInfo, &state) == XR_SUCCESS)
        {
          if (state.isActive)
          {
            m_BoundVec2Actions[uiSide].PushBack(i);
            m_DiscoveredFeatures[uiSide].Add(m_Vec2Actions[i].m_Feature);
          }
          const float x = state.currentState.x;
          const float y = state.currentState.y;
          snapshot.m_InputSlotValues[m_Vec2Actions[i].m_sKey_negx[uiSide]] = x < 0 ? -x : 0.0f;
          snapshot.m_InputSlotValues[m_Vec2Actions[i].m_sKey_posx[uiSide]] = x > 0 ? x : 0.0f;
          snapshot.m_InputSlotValues[m_Vec2Actions[i].m_sKey_negy[uiSide]] = y < 0 ? -y : 0.0f;
          snapshot.m_InputSlotValues[m_Vec2Actions[i].m_sKey_posy[uiSide]] = y > 0 ? y : 0.0f;
        }
      }

      plLog::Info("OpenXR: Controller {} bound actions - Boolean: {}, Float: {}, Vec2: {}",
        uiSide == 0 ? "Left" : "Right",
        m_BoundBooleanActions[uiSide].GetCount(),
        m_BoundFloatActions[uiSide].GetCount(),
        m_BoundVec2Actions[uiSide].GetCount());

      m_bBoundActionsCached[uiSide] = true;
    }
    else
    {
      // Fast path: query only bound actions
      // Always write values (don't use changedSinceLastSync) because with double-buffering
      // the back buffer may contain stale data from when it was previously the front buffer
      XrActionStateGetInfo getInfo{XR_TYPE_ACTION_STATE_GET_INFO};
      getInfo.subactionPath = subActionPath;

      for (plUInt8 idx : m_BoundBooleanActions[uiSide])
      {
        const Action& action = m_BooleanActions[idx];
        XrActionStateBoolean state{XR_TYPE_ACTION_STATE_BOOLEAN};
        getInfo.action = action.m_Action;
        if (xrGetActionStateBoolean(m_pSession, &getInfo, &state) == XR_SUCCESS)
          snapshot.m_InputSlotValues[action.m_sKey[uiSide]] = state.currentState ? 1.0f : 0.0f;
      }

      for (plUInt8 idx : m_BoundFloatActions[uiSide])
      {
        const Action& action = m_FloatActions[idx];
        XrActionStateFloat state{XR_TYPE_ACTION_STATE_FLOAT};
        getInfo.action = action.m_Action;
        if (xrGetActionStateFloat(m_pSession, &getInfo, &state) == XR_SUCCESS)
          snapshot.m_InputSlotValues[action.m_sKey[uiSide]] = state.currentState;
      }

      for (plUInt8 idx : m_BoundVec2Actions[uiSide])
      {
        const Vec2Action& action = m_Vec2Actions[idx];
        XrActionStateVector2f state{XR_TYPE_ACTION_STATE_VECTOR2F};
        getInfo.action = action.m_Action;
        if (xrGetActionStateVector2f(m_pSession, &getInfo, &state) == XR_SUCCESS)
        {
          const float x = state.currentState.x;
          const float y = state.currentState.y;
          snapshot.m_InputSlotValues[action.m_sKey_negx[uiSide]] = x < 0 ? -x : 0.0f;
          snapshot.m_InputSlotValues[action.m_sKey_posx[uiSide]] = x > 0 ? x : 0.0f;
          snapshot.m_InputSlotValues[action.m_sKey_negy[uiSide]] = y < 0 ? -y : 0.0f;
          snapshot.m_InputSlotValues[action.m_sKey_posy[uiSide]] = y > 0 ? y : 0.0f;
        }
      }
    }

    // Update poses. Never hand the runtime a null space: SteamVR breaks inside vrclient rather than returning
    // XR_ERROR_HANDLE_INVALID, which turns a recoverable state into a debugger stop.
    if (m_gripSpace[uiSide] == XR_NULL_HANDLE || m_aimSpace[uiSide] == XR_NULL_HANDLE || baseSpace == XR_NULL_HANDLE)
      continue;

    plXRDeviceState& deviceState = snapshot.m_DeviceState[uiControllerId];
    XrSpaceLocation viewInScene = {XR_TYPE_SPACE_LOCATION};

    if (m_bUseBatchedSpaceLocation)
    {
      // Batched pose query
      XrSpace spaces[2] = {m_gripSpace[uiSide], m_aimSpace[uiSide]};
      XrSpacesLocateInfo locateInfo{XR_TYPE_SPACES_LOCATE_INFO_KHR};
      locateInfo.baseSpace = baseSpace;
      locateInfo.time = time;
      locateInfo.spaceCount = 2;
      locateInfo.spaces = spaces;

      XrSpaceLocationData locationData[2] = {};
      XrSpaceLocations locations{XR_TYPE_SPACE_LOCATIONS_KHR};
      locations.locationCount = 2;
      locations.locations = locationData;

      const XrResult locateRes = m_pOpenXR->m_Extensions.pfn_xrLocateSpacesKHR(m_pSession, &locateInfo, &locations);
      if (locateRes != XR_SUCCESS)
      {
        ReportLocateFailure(uiSide, "xrLocateSpacesKHR", locateRes, baseSpace, time);
      }
      else
      {
        const XrSpaceLocationData& gripLoc = locationData[0];
        const XrSpaceLocationData& aimLoc = locationData[1];

        if ((gripLoc.locationFlags & (XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT)) ==
            (XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT))
        {
          deviceState.m_vGripPosition = plOpenXR::ConvertPosition(gripLoc.pose.position);
          deviceState.m_qGripRotation = plOpenXR::ConvertOrientation(gripLoc.pose.orientation);
          deviceState.m_bGripPoseIsValid = true;
        }
        else
          deviceState.m_bGripPoseIsValid = false;

        if ((aimLoc.locationFlags & (XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT)) ==
            (XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT))
        {
          deviceState.m_vAimPosition = plOpenXR::ConvertPosition(aimLoc.pose.position);
          deviceState.m_qAimRotation = plOpenXR::ConvertOrientation(aimLoc.pose.orientation);
          deviceState.m_bAimPoseIsValid = true;
        }
        else
          deviceState.m_bAimPoseIsValid = false;
      }
    }
    else
    {
      // Individual pose queries
      const XrResult gripRes = xrLocateSpace(m_gripSpace[uiSide], baseSpace, time, &viewInScene);
      if (gripRes != XR_SUCCESS)
      {
        ReportLocateFailure(uiSide, "xrLocateSpace(grip)", gripRes, baseSpace, time);
      }
      else
      {
        if ((viewInScene.locationFlags & (XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT)) ==
            (XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT))
        {
          deviceState.m_vGripPosition = plOpenXR::ConvertPosition(viewInScene.pose.position);
          deviceState.m_qGripRotation = plOpenXR::ConvertOrientation(viewInScene.pose.orientation);
          deviceState.m_bGripPoseIsValid = true;
        }
        else
          deviceState.m_bGripPoseIsValid = false;
      }

      const XrResult aimRes = xrLocateSpace(m_aimSpace[uiSide], baseSpace, time, &viewInScene);
      if (aimRes != XR_SUCCESS)
      {
        ReportLocateFailure(uiSide, "xrLocateSpace(aim)", aimRes, baseSpace, time);
      }
      else
      {
        if ((viewInScene.locationFlags & (XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT)) ==
            (XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT))
        {
          deviceState.m_vAimPosition = plOpenXR::ConvertPosition(viewInScene.pose.position);
          deviceState.m_qAimRotation = plOpenXR::ConvertOrientation(viewInScene.pose.orientation);
          deviceState.m_bAimPoseIsValid = true;
        }
        else
          deviceState.m_bAimPoseIsValid = false;
      }
    }

    m_DiscoveredFeatures[uiSide].Add(plXRDeviceFeatures::GripPose);
    m_DiscoveredFeatures[uiSide].Add(plXRDeviceFeatures::AimPose);
    snapshot.m_SupportedFeatures[uiControllerId] = m_DiscoveredFeatures[uiSide];
  }

  // Atomically swap which buffer is readable
  // Main thread will pick up new data on next frame
  m_iReadableSnapshot = writeIdx;
}

void plOpenXRInputDevice::UpdateControllerState()
{
  for (plUInt32 uiSide : {0,1,2})
  {

  ///  const plUInt32 uiControllerId = uiSide +1;
    const plUInt32 uiControllerId = uiSide == 0 ? m_iLeftControllerDeviceID : m_iRightControllerDeviceID;

    //const plUInt32 uiControllerId = uiSide;

    const bool bDeviceConnected = m_SupportedFeatures[uiControllerId].IsSet(plXRDeviceFeatures::AimPose);

    if (!m_DeviceState[uiControllerId].m_bDeviceIsConnected && bDeviceConnected)
    {
      // Connected
      m_DeviceState[uiControllerId].m_bDeviceIsConnected = true;

      plXRDeviceEventData data;
      data.m_Type = plXRDeviceEventData::Type::DeviceAdded;
      data.uiDeviceID = uiControllerId;
      m_InputEvents.Broadcast(data);
    }
    else if (m_DeviceState[uiControllerId].m_bDeviceIsConnected && !bDeviceConnected)
    {
      // Disconnected
      m_DeviceState[uiControllerId] = plXRDeviceState();
      m_SupportedFeatures[uiControllerId] = plXRDeviceFeatures::None;

      plXRDeviceEventData data;
      data.m_Type = plXRDeviceEventData::Type::DeviceRemoved;
      data.uiDeviceID = uiControllerId;
      m_InputEvents.Broadcast(data);
    }
  }
}

bool plOpenXRInputDevice::IsHapticActionBound(plUInt32 uiSide) const
{
  if (m_pSession == XR_NULL_HANDLE || m_HapticAction == XR_NULL_HANDLE)
    return false;

  XrBoundSourcesForActionEnumerateInfo enumerateInfo{XR_TYPE_BOUND_SOURCES_FOR_ACTION_ENUMERATE_INFO};
  enumerateInfo.action = m_HapticAction;

  plUInt32 uiSourceCount = 0;
  if (XR_FAILED_RESULT(xrEnumerateBoundSourcesForAction(m_pSession, &enumerateInfo, 0, &uiSourceCount, nullptr)) || uiSourceCount == 0)
    return false;

  plHybridArray<XrPath, 4> sources;
  sources.SetCount(uiSourceCount);
  if (XR_FAILED_RESULT(xrEnumerateBoundSourcesForAction(m_pSession, &enumerateInfo, uiSourceCount, &uiSourceCount, sources.GetData())))
    return false;

  // The enumeration covers both subaction paths, so the hand is identified by the source path prefix.
  const char* szPrefix = uiSide == 0 ? "/user/hand/left" : "/user/hand/right";
  for (XrPath source : sources)
  {
    char szBuffer[XR_MAX_PATH_LENGTH] = {};
    plUInt32 uiWritten = 0;
    if (xrPathToString(m_pInstance, source, XR_MAX_PATH_LENGTH, &uiWritten, szBuffer) == XR_SUCCESS && plStringUtils::StartsWith(szBuffer, szPrefix))
      return true;
  }

  return false;
}

plResult plOpenXRInputDevice::TriggerHapticPulse(plXRDeviceID deviceID, plTime duration, float fAmplitude, float fFrequencyHz)
{
  if (m_pSession == XR_NULL_HANDLE || m_HapticAction == XR_NULL_HANDLE)
    return PL_FAILURE;

  plUInt32 uiSide = 0;
  if (deviceID == m_iLeftControllerDeviceID)
    uiSide = 0;
  else if (deviceID == m_iRightControllerDeviceID)
    uiSide = 1;
  else
    return PL_FAILURE;

  // Queued rather than applied here. This runs on the main thread while the input thread is calling into the same
  // XrSession at 200 Hz, and SteamVR's client library breaks when two threads touch one session; the input thread
  // applies the pulse on its next tick instead. Success therefore means "accepted", not "the runtime played it".
  {
    PL_LOCK(m_HapticMutex);

    PendingHaptic& pending = m_PendingHaptics[uiSide];
    pending.m_fAmplitude = plMath::Clamp(fAmplitude, 0.0f, 1.0f);
    pending.m_fFrequencyHz = fFrequencyHz;
    pending.m_Duration = duration;
    pending.m_bPending = true;
  }

  return PL_SUCCESS;
}

void plOpenXRInputDevice::ReportLocateFailure(plUInt32 uiSide, const char* szWhat, XrResult res, XrSpace baseSpace, XrTime time)
{
  // Runs at the input thread's tick rate, so only report a given hand's result once until it changes.
  if (m_LastLocateFailure[uiSide] == res)
    return;

  m_LastLocateFailure[uiSide] = res;

  plLog::Error("OpenXR: {} failed for the {} hand: {}. grip={}, aim={}, base={}, displayTime={}", szWhat, uiSide == 0 ? "left" : "right", res,
    (plUInt64)m_gripSpace[uiSide], (plUInt64)m_aimSpace[uiSide], (plUInt64)baseSpace, (plInt64)time);
}

void plOpenXRInputDevice::ApplyPendingHaptics()
{
  if (m_pSession == XR_NULL_HANDLE || m_HapticAction == XR_NULL_HANDLE)
    return;

  for (plUInt32 uiSide : {0, 1})
  {
    PendingHaptic request;
    {
      PL_LOCK(m_HapticMutex);

      if (!m_PendingHaptics[uiSide].m_bPending)
        continue;

      request = m_PendingHaptics[uiSide];
      m_PendingHaptics[uiSide].m_bPending = false;
    }

    XrHapticVibration vibration{XR_TYPE_HAPTIC_VIBRATION};
    vibration.amplitude = request.m_fAmplitude;
    vibration.frequency = request.m_fFrequencyHz > 0.0f ? request.m_fFrequencyHz : XR_FREQUENCY_UNSPECIFIED;
    vibration.duration = request.m_Duration > plTime::MakeZero() ? static_cast<XrDuration>(request.m_Duration.GetNanoseconds()) : XR_MIN_HAPTIC_DURATION;

    XrHapticActionInfo hapticActionInfo{XR_TYPE_HAPTIC_ACTION_INFO};
    hapticActionInfo.action = m_HapticAction;
    hapticActionInfo.subactionPath = m_SubActionPath[uiSide];

    xrApplyHapticFeedback(m_pSession, &hapticActionInfo, reinterpret_cast<const XrHapticBaseHeader*>(&vibration));
  }
}

plOpenXRInputDevice::Vec2Action::Vec2Action(plXRDeviceFeatures::Enum feature, XrAction pAction, plStringView sLeft, plStringView sRight)
{
  m_Feature = feature;
  m_Action = pAction;

  plStringView sides[2] = {sLeft, sRight};
  for (plUInt32 uiSide : {0, 1})
  {
    plStringBuilder temp = sides[uiSide];
    temp.Append("_negx");
    m_sKey_negx[uiSide] = temp;

    temp.Shrink(0, 5);
    temp.Append("_posx");
    m_sKey_posx[uiSide] = temp;

    temp.Shrink(0, 5);
    temp.Append("_negy");
    m_sKey_negy[uiSide] = temp;

    temp.Shrink(0, 5);
    temp.Append("_posy");
    m_sKey_posy[uiSide] = temp;
  }
}

// ============================================================================
// Input Thread Implementation
// ============================================================================

void plOpenXRInputDevice::StartInputThread()
{
  if (m_pInputThread != nullptr)
    return;

  // Initialize double-buffer snapshots
  m_iReadableSnapshot = 0;
  m_bInputThreadRunning = true;

  m_pInputThread = PL_DEFAULT_NEW(InputThread, this);
  m_pInputThread->Start();

  plLog::Info("OpenXR: Input thread started");
}

void plOpenXRInputDevice::StopInputThread()
{
  if (m_pInputThread == nullptr)
    return;

  m_bInputThreadRunning = false;
  m_pInputThread->Join();
  m_pInputThread = nullptr;

  plLog::Info("OpenXR: Input thread stopped");
}

plOpenXRInputDevice::InputThread::InputThread(plOpenXRInputDevice* pOwner)
  : plThread("OpenXR Input")
  , m_pOwner(pOwner)
{
}

plUInt32 plOpenXRInputDevice::InputThread::Run()
{
  plLog::Info("OpenXR: Input thread running at 200Hz");

  while (m_pOwner->m_bInputThreadRunning)
  {
    // Poll OpenXR actions and update the back buffer
    m_pOwner->UpdateActionsOnInputThread();

    // Run at approximately 200Hz (5ms between updates)
    // This is faster than typical VR frame rates (90Hz = 11ms)
    // to minimize latency while keeping CPU usage low
    plThreadUtils::Sleep(plTime::MakeFromMilliseconds(5));
  }

  return 0;
}


PL_STATICLINK_FILE(OpenXRPlugin, OpenXRPlugin_OpenXRInputDevice);
