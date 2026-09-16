#include <OpenXRPlugin/OpenXRPluginPCH.h>

#include <Foundation/Profiling/Profiling.h>
#include <OpenXRPlugin/OpenXRDeclarations.h>
#include <OpenXRPlugin/OpenXRSingleton.h>
#include <OpenXRPlugin/OpenXRSwapChain.h>
#include <RendererFoundation/Device/Device.h>
#include <vector>

void plGALOpenXRSwapChain::AcquireNextRenderTarget(plGALDevice* pDevice)
{
  PL_PROFILE_SCOPE("AcquireNextRenderTarget");
  if (m_bImageAcquired)
  {
    plLog::Warning("AcquireNextRenderTarget called while image is still acquired. Forcing release.");
    PresentRenderTarget();
    m_bImageAcquired = false;
  }
  
  auto pOpenXR = static_cast<plOpenXR*>(m_pXrInterface);

  auto AquireAndWait = [](Swapchain& swapchain) -> XrResult
  {
    XrSwapchainImageAcquireInfo acquireInfo{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    XR_SUCCEED_OR_RETURN_LOG(xrAcquireSwapchainImage(swapchain.handle, &acquireInfo, &swapchain.imageIndex));

    XrSwapchainImageWaitInfo waitInfo{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    waitInfo.timeout = XR_INFINITE_DURATION;
    XrResult res = xrWaitSwapchainImage(swapchain.handle, &waitInfo);
    if (res != XR_SUCCESS)
    {
      XrSwapchainImageReleaseInfo releaseInfo{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
      xrReleaseSwapchainImage(swapchain.handle, &releaseInfo);
      return res;
    }
    return XR_SUCCESS;
  };

  bool bSuccess = true;
  {
    PL_PROFILE_SCOPE("AquireAndWait");
    if (AquireAndWait(m_ColorSwapchain) != XR_SUCCESS) bSuccess = false;
    if (bSuccess && pOpenXR->GetDepthComposition())
    {
      if (AquireAndWait(m_DepthSwapchain) != XR_SUCCESS) bSuccess = false;
    }
  }

  if (bSuccess)
  {
    m_bImageAcquired = true;
    m_hColorRT = m_ColorRTs[m_ColorSwapchain.imageIndex];
    m_RenderTargets.m_hRTs[0] = m_hColorRT;
    if (pOpenXR->GetDepthComposition())
    {
      m_hDepthRT = m_DepthRTs[m_DepthSwapchain.imageIndex];
      m_RenderTargets.m_hDSTarget = m_hDepthRT;
    }
  }
}

void plGALOpenXRSwapChain::PresentRenderTarget(plGALDevice* pDevice)
{
  PL_PROFILE_SCOPE("PresentRenderTarget");
  if (!m_bImageAcquired)
    return;

  auto pOpenXR = static_cast<plOpenXR*>(m_pXrInterface);
  // If we have a companion view we can't let go of the current swap chain image yet as we need to use it as the input to render to the companion window.
  // Thus, we delay PresentRenderTarget here and plOpenXR will call PresentRenderTarget instead after the companion window was updated.
  if (pOpenXR->m_pCompanion)
  {
    pOpenXR->Present();
  }
  else
  {
    PresentRenderTarget();
  }
  m_bImageAcquired = false;
}

void plGALOpenXRSwapChain::PresentRenderTarget() const
{
  auto pOpenXR = static_cast<plOpenXR*>(m_pXrInterface);
  PL_PROFILE_SCOPE("xrReleaseSwapchainImage");
  XrSwapchainImageReleaseInfo releaseInfo{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
  XR_LOG_ERROR(xrReleaseSwapchainImage(m_ColorSwapchain.handle, &releaseInfo));
  if (pOpenXR->GetDepthComposition())
  {
    XR_LOG_ERROR(xrReleaseSwapchainImage(m_DepthSwapchain.handle, &releaseInfo));
  }
}

plResult plGALOpenXRSwapChain::InitPlatform(plGALDevice* pDevice)
{
  if (InitSwapChain(m_MsaaCount) != XrResult::XR_SUCCESS)
    return PL_FAILURE;

  m_RenderTargets.m_hRTs[0] = m_hColorRT;
  m_RenderTargets.m_hDSTarget = m_hDepthRT;
  return PL_SUCCESS;
}

plResult plGALOpenXRSwapChain::DeInitPlatform(plGALDevice* pDevice)
{
  DeinitSwapChain();
  return PL_SUCCESS;
}

plGALOpenXRSwapChain::plGALOpenXRSwapChain(plOpenXR* pXrInterface, plGALMSAASampleCount::Enum msaaCount)
  : plGALXRSwapChain(pXrInterface)
{
  m_pInstance = pXrInterface->GetInstance();
  m_SystemId = pXrInterface->GetSystemId();
  m_pSession = pXrInterface->GetSession();
  m_MsaaCount = msaaCount;
}

XrResult plGALOpenXRSwapChain::SelectSwapchainFormat(int64_t& colorFormat, int64_t& depthFormat)
{
  auto pOpenXR = static_cast<plOpenXR*>(m_pXrInterface);

  uint32_t swapchainFormatCount;
  XR_SUCCEED_OR_CLEANUP_LOG(xrEnumerateSwapchainFormats(m_pSession, 0, &swapchainFormatCount, nullptr), voidFunction);
  std::vector<int64_t> swapchainFormats(swapchainFormatCount);
  XR_SUCCEED_OR_CLEANUP_LOG(xrEnumerateSwapchainFormats(m_pSession, (uint32_t)swapchainFormats.size(), &swapchainFormatCount, swapchainFormats.data()), voidFunction);

  // Supported swap chain formats in priority order, in the session graphics API's own enumeration.
  static constexpr int64_t s_VulkanColorFormats[] = {VK_FORMAT_B8G8R8A8_SRGB, VK_FORMAT_R8G8B8A8_SRGB, VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM};
  static constexpr int64_t s_VulkanDepthFormats[] = {VK_FORMAT_D32_SFLOAT, VK_FORMAT_D16_UNORM, VK_FORMAT_D24_UNORM_S8_UINT, VK_FORMAT_D32_SFLOAT_S8_UINT};
  static constexpr int64_t s_D3D12ColorFormats[] = {DXGI_FORMAT_B8G8R8A8_UNORM_SRGB, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM};
  // No DXGI_FORMAT_D32_FLOAT_S8X24_UINT: GAL has no matching format, and a D32_FLOAT view of that resource is invalid.
  static constexpr int64_t s_D3D12DepthFormats[] = {DXGI_FORMAT_D32_FLOAT, DXGI_FORMAT_D16_UNORM, DXGI_FORMAT_D24_UNORM_S8_UINT};

  const bool bD3D12 = pOpenXR->GetGraphicsApi() == plOpenXR::GraphicsApi::D3D12;
  plArrayPtr<const int64_t> supportedColorFormats = bD3D12 ? plMakeArrayPtr(s_D3D12ColorFormats) : plMakeArrayPtr(s_VulkanColorFormats);
  plArrayPtr<const int64_t> supportedDepthFormats = bD3D12 ? plMakeArrayPtr(s_D3D12DepthFormats) : plMakeArrayPtr(s_VulkanDepthFormats);

  auto swapchainFormatIt = std::find_first_of(supportedColorFormats.GetPtr(), supportedColorFormats.GetEndPtr(), swapchainFormats.begin(), swapchainFormats.end());
  if (swapchainFormatIt == supportedColorFormats.GetEndPtr())
  {
    return XrResult::XR_ERROR_INITIALIZATION_FAILED;
  }
  colorFormat = *swapchainFormatIt;

  if (pOpenXR->GetDepthComposition())
  {
    auto depthSwapchainFormatIt = std::find_first_of(supportedDepthFormats.GetPtr(), supportedDepthFormats.GetEndPtr(), swapchainFormats.begin(), swapchainFormats.end());
    if (depthSwapchainFormatIt == supportedDepthFormats.GetEndPtr())
    {
      return XrResult::XR_ERROR_INITIALIZATION_FAILED;
    }
    depthFormat = *depthSwapchainFormatIt;
  }
  return XrResult::XR_SUCCESS;
}

XrResult plGALOpenXRSwapChain::CreateSwapchainImages(Swapchain& swapchain, SwapchainType type)
{
  const plOpenXR::GraphicsApi api = static_cast<plOpenXR*>(m_pXrInterface)->GetGraphicsApi();
  plHybridArray<XrSwapchainImageVulkanKHR, 3>& imagesVulkan = type == SwapchainType::Color ? m_ColorSwapChainImagesVulkan : m_DepthSwapChainImagesVulkan;
  plHybridArray<XrSwapchainImageD3D12KHR, 3>& imagesD3D12 = type == SwapchainType::Color ? m_ColorSwapChainImagesD3D12 : m_DepthSwapChainImagesD3D12;
  if (api == plOpenXR::GraphicsApi::D3D12)
  {
    imagesD3D12.SetCount(swapchain.imageCount, {XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR});
    swapchain.images = reinterpret_cast<XrSwapchainImageBaseHeader*>(imagesD3D12.GetData());
  }
  else
  {
    imagesVulkan.SetCount(swapchain.imageCount, {XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR});
    swapchain.images = reinterpret_cast<XrSwapchainImageBaseHeader*>(imagesVulkan.GetData());
  }
  XR_SUCCEED_OR_CLEANUP_LOG(xrEnumerateSwapchainImages(swapchain.handle, swapchain.imageCount, &swapchain.imageCount, swapchain.images), voidFunction);

  plGALDevice* pDevice = plGALDevice::GetDefaultDevice();

  for (plUInt32 i = 0; i < swapchain.imageCount; i++)
  {
    // The runtime-owned VkImage or ID3D12Resource, wrapped without taking ownership.
    void* pNativeImage = api == plOpenXR::GraphicsApi::D3D12 ? static_cast<void*>(imagesD3D12[i].texture) : (void*)(plUInt64)imagesVulkan[i].image;

    plGALTextureCreationDescription textureDesc;
    // Determine dimensions - for stereo the array size is 2
    plUInt32 width = m_PrimaryConfigView.recommendedImageRectWidth;
    plUInt32 height = m_PrimaryConfigView.recommendedImageRectHeight;
    
    plGALResourceFormat::Enum galFormat = plOpenXR::ConvertTextureFormat(swapchain.format, api);
    
    textureDesc.SetAsRenderTarget(width, height, galFormat, m_MsaaCount);
    textureDesc.m_uiArraySize = 2; // Stereo - 2 eyes
    if (textureDesc.m_uiArraySize > 1)
    {
      textureDesc.m_Type = plGALTextureType::Texture2DArray;
    }
    textureDesc.m_pExisitingNativeObject = pNativeImage;
    
    if (type == SwapchainType::Color)
    {
      m_ColorRTs.PushBack(pDevice->CreateTexture(textureDesc));
    }
    else
    {
      m_DepthRTs.PushBack(pDevice->CreateTexture(textureDesc));
    }
  }
  if (type == SwapchainType::Color)
    m_hColorRT = m_ColorRTs[0];
  else
    m_hDepthRT = m_DepthRTs[0];
  return XR_SUCCESS;
}

XrResult plGALOpenXRSwapChain::InitSwapChain(plGALMSAASampleCount::Enum msaaCount)
{
  auto pOpenXR = static_cast<plOpenXR*>(m_pXrInterface);

  // Read graphics properties for preferred swapchain length and logging.
  XrSystemProperties systemProperties{XR_TYPE_SYSTEM_PROPERTIES};
  XR_SUCCEED_OR_CLEANUP_LOG(xrGetSystemProperties(m_pInstance, m_SystemId, &systemProperties), DeinitSwapChain);

  plUInt32 viewCount = 0;
  XR_SUCCEED_OR_CLEANUP_LOG(xrEnumerateViewConfigurationViews(m_pInstance, m_SystemId, pOpenXR->GetViewType(), 0, &viewCount, nullptr), DeinitSwapChain);
  if (viewCount != 2)
  {
    plLog::Error("No stereo view configuration present, can't create swap chain");
    DeinitSwapChain();
    return XR_ERROR_INITIALIZATION_FAILED;
  }
  plHybridArray<XrViewConfigurationView, 2> views;
  views.SetCount(viewCount, {XR_TYPE_VIEW_CONFIGURATION_VIEW});
  XR_SUCCEED_OR_CLEANUP_LOG(xrEnumerateViewConfigurationViews(m_pInstance, m_SystemId, pOpenXR->GetViewType(), viewCount, &viewCount, views.GetData()), DeinitSwapChain);

  // Create the swapchain and get the images.
  // Select a swapchain format.
  m_PrimaryConfigView = views[0];
  XR_SUCCEED_OR_CLEANUP_LOG(SelectSwapchainFormat(m_ColorSwapchain.format, m_DepthSwapchain.format), DeinitSwapChain);

  // The runtime advertises what it can actually composite. Handing xrCreateSwapchain an unsupported sample count fails
  // the call and takes the whole session down, so clamp instead of trusting the request.
  plUInt32 uiSampleCount = plMath::Max(1u, (plUInt32)msaaCount);
  if (m_PrimaryConfigView.maxSwapchainSampleCount > 0 && uiSampleCount > m_PrimaryConfigView.maxSwapchainSampleCount)
  {
    plLog::Warning("OpenXR: {0}x MSAA was requested but the runtime supports at most {1}x for swap chains, clamping.", uiSampleCount, m_PrimaryConfigView.maxSwapchainSampleCount);
    uiSampleCount = m_PrimaryConfigView.maxSwapchainSampleCount;
  }
  m_MsaaCount = (plGALMSAASampleCount::Enum)uiSampleCount;

  // Create the swapchain.
  XrSwapchainCreateInfo swapchainCreateInfo{XR_TYPE_SWAPCHAIN_CREATE_INFO};
  swapchainCreateInfo.arraySize = 2;
  swapchainCreateInfo.format = m_ColorSwapchain.format;
  swapchainCreateInfo.width = m_PrimaryConfigView.recommendedImageRectWidth;
  swapchainCreateInfo.height = m_PrimaryConfigView.recommendedImageRectHeight;
  swapchainCreateInfo.mipCount = 1;
  swapchainCreateInfo.faceCount = 1;
  swapchainCreateInfo.sampleCount = uiSampleCount;
  swapchainCreateInfo.usageFlags = XR_SWAPCHAIN_USAGE_SAMPLED_BIT | XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;

  m_CurrentSize = {swapchainCreateInfo.width, swapchainCreateInfo.height};

  auto CreateSwapChain = [this](const XrSwapchainCreateInfo& swapchainCreateInfo, Swapchain& swapchain, SwapchainType type) -> XrResult
  {
    XR_SUCCEED_OR_CLEANUP_LOG(xrCreateSwapchain(m_pSession, &swapchainCreateInfo, &swapchain.handle), voidFunction);
    XR_SUCCEED_OR_CLEANUP_LOG(xrEnumerateSwapchainImages(swapchain.handle, 0, &swapchain.imageCount, nullptr), voidFunction);
    CreateSwapchainImages(swapchain, type);

    return XrResult::XR_SUCCESS;
  };
  XR_SUCCEED_OR_CLEANUP_LOG(CreateSwapChain(swapchainCreateInfo, m_ColorSwapchain, SwapchainType::Color), DeinitSwapChain);

  if (pOpenXR->GetDepthComposition())
  {
    swapchainCreateInfo.format = m_DepthSwapchain.format;
    swapchainCreateInfo.usageFlags = XR_SWAPCHAIN_USAGE_SAMPLED_BIT | XR_SWAPCHAIN_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    XR_SUCCEED_OR_CLEANUP_LOG(CreateSwapChain(swapchainCreateInfo, m_DepthSwapchain, SwapchainType::Depth), DeinitSwapChain);
  }
  else
  {
    // Create depth buffer in case the API does not support it.
    // SetAsRenderTarget resets m_Type to Texture2D and m_uiArraySize to 1, so both stereo settings must be applied
    // after it - setting the type first left a Texture2D with an array size of 2, which CreateTexture rejects
    // ("m_uiArraySize must be 1 for non array textures!") and the view ended up with no depth target at all.
    plGALDevice* pDevice = plGALDevice::GetDefaultDevice();
    plGALTextureCreationDescription tcd;
    tcd.SetAsRenderTarget(m_CurrentSize.width, m_CurrentSize.height, plGALResourceFormat::DFloat, m_MsaaCount);
    tcd.m_uiArraySize = 2;
    tcd.m_Type = plGALTextureType::Texture2DArray;
    m_hDepthRT = pDevice->CreateTexture(tcd);
  }

  return XrResult::XR_SUCCESS;
}

void plGALOpenXRSwapChain::DeinitSwapChain()
{
  auto pOpenXR = static_cast<plOpenXR*>(m_pXrInterface);

  plGALDevice* pDevice = plGALDevice::GetDefaultDevice();

  for (plGALTextureHandle rt : m_ColorRTs)
  {
    pDevice->DestroyTexture(rt);
  }
  m_ColorRTs.Clear();
  if (pOpenXR->GetDepthComposition())
  {
    for (plGALTextureHandle rt : m_DepthRTs)
    {
      pDevice->DestroyTexture(rt);
    }
  }
  else
  {
    pDevice->DestroyTexture(m_hDepthRT);
    m_hDepthRT.Invalidate();
  }
  m_DepthRTs.Clear();
  m_hColorRT.Invalidate();
  m_hDepthRT.Invalidate();

  auto DeleteSwapchain = [](Swapchain& swapchain)
  {
    if (swapchain.handle != XR_NULL_HANDLE)
    {
      xrDestroySwapchain(swapchain.handle);
      swapchain.handle = 0;
    }
    swapchain.format = 0;
    swapchain.imageCount = 0;
    swapchain.images = nullptr;
    swapchain.imageIndex = 0;
  };
  m_PrimaryConfigView = {XR_TYPE_VIEW_CONFIGURATION_VIEW};
  DeleteSwapchain(m_ColorSwapchain);
  DeleteSwapchain(m_DepthSwapchain);

  m_ColorSwapChainImagesVulkan.Clear();
  m_DepthSwapChainImagesVulkan.Clear();
  m_ColorSwapChainImagesD3D12.Clear();
  m_DepthSwapChainImagesD3D12.Clear();
}
