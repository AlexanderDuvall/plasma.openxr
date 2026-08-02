#pragma once

#include <Foundation/Basics/Platform/Win/IncludeWindows.h>

// Include COM interfaces required by OpenXR platform headers
#if PL_ENABLED(PL_PLATFORM_WINDOWS)
#  include <unknwn.h>
#endif

#define VK_USE_PLATFORM_WIN32_KHR
#include <vulkan/vulkan.h>
#define XR_USE_GRAPHICS_API_VULKAN
#if PL_ENABLED(PL_PLATFORM_WINDOWS)
#  define XR_USE_PLATFORM_WIN32
#endif
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include <openxr/openxr_platform_defines.h>

#ifdef BUILDSYSTEM_ENABLE_OPENXR_REMOTING_SUPPORT
#  include <openxr/openxr_msft_holographic_remoting.h>
#endif
