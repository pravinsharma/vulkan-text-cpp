#include "Application.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <set>
#include <stdexcept>
#include <string>

static constexpr uint32_t kWindowWidth = 800;
static constexpr uint32_t kWindowHeight = 600;
static constexpr uint32_t kFontPixelSize = 36;
static const char *const kText =
    "A quick brown fox jumped over a lazy dog!";

namespace
{
    VkResult CreateDebugUtilsMessengerEXT(VkInstance, const VkDebugUtilsMessengerCreateInfoEXT *,
                                          const VkAllocationCallbacks *, VkDebugUtilsMessengerEXT *) { return VK_SUCCESS; }
    void DestroyDebugUtilsMessengerEXT(VkInstance, VkDebugUtilsMessengerEXT, const VkAllocationCallbacks *) {}
}

Application::Application() = default;

Application::~Application()
{
    cleanup();
}

void Application::run()
{
    initWindow();
    initVulkan();
    mainLoop();
}

void Application::initWindow()
{
    if (!glfwInit())
    {
        throw std::runtime_error("Failed to initialize GLFW");
    }

    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);

    window_ = glfwCreateWindow(kWindowWidth, kWindowHeight, "Vulkan FreeType Text", nullptr, nullptr);
    if (!window_)
    {
        glfwTerminate();
        throw std::runtime_error("Failed to create GLFW window");
    }

    glfwSetWindowUserPointer(window_, this);
    glfwSetFramebufferSizeCallback(window_, framebufferResizeCallback);
    glfwSetMouseButtonCallback(window_, mouseButtonCallback);
    glfwSetCursorPosCallback(window_, cursorPosCallback);
}

void Application::initVulkan()
{
    fonts_ = {
        {"C:/Windows/Fonts/segoeui.ttf",   "Segoe UI"},
        {"C:/Windows/Fonts/calibri.ttf",   "Calibri"},
        {"C:/Windows/Fonts/verdana.ttf",   "Verdana"},
        {"C:/Windows/Fonts/consola.ttf",   "Consolas"},
        {"C:/Windows/Fonts/seguiemj.ttf",  "Segoe UI Emoji"},
    };

    const std::string kEmojiFontPath = "C:/Windows/Fonts/seguiemj.ttf";

    createInstance();
    createSurface();
    pickPhysicalDevice();
    createLogicalDevice();
    createSwapchain();
    createImageViews();
    createRenderPass();
    createCommandPool();
    createCommandBuffers();
    createSyncObjects();
    createFramebuffers();

    TextRenderer::InitInfo info{};
    info.physicalDevice = physicalDevice_;
    info.device = device_;
    info.commandPool = commandPool_;
    info.graphicsQueue = graphicsQueue_;
    info.renderPass = renderPass_;
    info.screenWidth = swapchainExtent_.width;
    info.screenHeight = swapchainExtent_.height;
    textRenderer_.init(info, fonts_[currentFontIndex_].path, kFontPixelSize);
    textRenderer_.setEmojiFont(kEmojiFontPath);

    uiRenderer_.init(info, "C:/Windows/Fonts/segoeui.ttf", kUiFontPixelSize);
    uiRenderer_.setEmojiFont(kEmojiFontPath);
}

void Application::mainLoop()
{
    while (!glfwWindowShouldClose(window_))
    {
        glfwPollEvents();
        drawFrame();
    }

    vkDeviceWaitIdle(device_);
}

void Application::drawFrame()
{
    vkWaitForFences(device_, 1, &inFlightFences_[currentFrame_], VK_TRUE, UINT64_MAX);

    uint32_t imageIndex = 0;
    VkResult result = vkAcquireNextImageKHR(device_, swapchain_, UINT64_MAX,
                                            imageAvailableSemaphores_[currentFrame_],
                                            VK_NULL_HANDLE, &imageIndex);

    if (result == VK_ERROR_OUT_OF_DATE_KHR)
    {
        recreateSwapchain();
        return;
    }
    if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR)
    {
        throw std::runtime_error("failed to acquire swapchain image");
    }

    if (imageInFlight_[imageIndex] != VK_NULL_HANDLE)
    {
        vkWaitForFences(device_, 1, &imageInFlight_[imageIndex], VK_TRUE, UINT64_MAX);
    }
    imageInFlight_[imageIndex] = inFlightFences_[currentFrame_];

    vkResetFences(device_, 1, &inFlightFences_[currentFrame_]);

    vkResetCommandBuffer(commandBuffers_[currentFrame_], 0);

    VkCommandBuffer cmd = commandBuffers_[currentFrame_];

    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    if (vkBeginCommandBuffer(cmd, &beginInfo) != VK_SUCCESS)
    {
        throw std::runtime_error("failed to begin recording command buffer");
    }

    VkClearValue clearValue{};
    clearValue.color = {{0.08f, 0.08f, 0.12f, 1.0f}};

    VkRenderPassBeginInfo renderPassInfo{};
    renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    renderPassInfo.renderPass = renderPass_;
    renderPassInfo.framebuffer = swapchainFramebuffers_[imageIndex];
    renderPassInfo.renderArea.offset = {0, 0};
    renderPassInfo.renderArea.extent = swapchainExtent_;
    renderPassInfo.clearValueCount = 1;
    renderPassInfo.pClearValues = &clearValue;

    vkCmdBeginRenderPass(cmd, &renderPassInfo, VK_SUBPASS_CONTENTS_INLINE);

    VkViewport viewport{};
    viewport.x = 0.0f;
    viewport.y = 0.0f;
    viewport.width = static_cast<float>(swapchainExtent_.width);
    viewport.height = static_cast<float>(swapchainExtent_.height);
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    vkCmdSetViewport(cmd, 0, 1, &viewport);

    VkRect2D scissor{};
    scissor.offset = {0, 0};
    scissor.extent = swapchainExtent_;
    vkCmdSetScissor(cmd, 0, 1, &scissor);

    {
        const std::string displayText = std::string(reinterpret_cast<const char *>(u8"Hello \U0001F600 Vulkan!"));
        const float textWidth = textRenderer_.measureText(displayText);
        const float ascent = textRenderer_.fontAscent();
        const float H = static_cast<float>(swapchainExtent_.height);
        const float x = (static_cast<float>(swapchainExtent_.width) - textWidth) * 0.5f;
        const float y = (H - ascent) * 0.5f;
        textRenderer_.drawText(cmd, displayText, x, y, 1.0f, 1.0f, 1.0f, 1.0f);
    }

    renderUi(cmd, swapchainExtent_.width, swapchainExtent_.height);

    vkCmdEndRenderPass(cmd);

    if (vkEndCommandBuffer(cmd) != VK_SUCCESS)
    {
        throw std::runtime_error("failed to record command buffer");
    }

    VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSemaphore signalSemaphore = renderFinishedSemaphores_[imageIndex];
    VkSubmitInfo submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.waitSemaphoreCount = 1;
    submitInfo.pWaitSemaphores = &imageAvailableSemaphores_[currentFrame_];
    submitInfo.pWaitDstStageMask = &waitStage;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &cmd;
    submitInfo.signalSemaphoreCount = 1;
    submitInfo.pSignalSemaphores = &signalSemaphore;

    if (vkQueueSubmit(graphicsQueue_, 1, &submitInfo, inFlightFences_[currentFrame_]) != VK_SUCCESS)
    {
        throw std::runtime_error("failed to submit draw command buffer");
    }

    VkPresentInfoKHR presentInfo{};
    presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    presentInfo.waitSemaphoreCount = 1;
    presentInfo.pWaitSemaphores = &signalSemaphore;
    presentInfo.swapchainCount = 1;
    presentInfo.pSwapchains = &swapchain_;
    presentInfo.pImageIndices = &imageIndex;

    result = vkQueuePresentKHR(presentQueue_, &presentInfo);

    if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR || framebufferResized_)
    {
        framebufferResized_ = false;
        recreateSwapchain();
    }
    else if (result != VK_SUCCESS)
    {
        throw std::runtime_error("failed to present swapchain image");
    }

    currentFrame_ = (currentFrame_ + 1) % kMaxFramesInFlight;
}

void Application::cleanup()
{
    if (device_ != VK_NULL_HANDLE)
    {
        vkDeviceWaitIdle(device_);
    }

    textRenderer_.shutdown();
    uiRenderer_.shutdown();

    if (!swapchainImageViews_.empty())
    {
        for (auto view : swapchainImageViews_)
            vkDestroyImageView(device_, view, nullptr);
        swapchainImageViews_.clear();
    }
    if (!swapchainFramebuffers_.empty())
    {
        for (auto fb : swapchainFramebuffers_)
            vkDestroyFramebuffer(device_, fb, nullptr);
        swapchainFramebuffers_.clear();
    }
    if (renderPass_ != VK_NULL_HANDLE)
    {
        vkDestroyRenderPass(device_, renderPass_, nullptr);
        renderPass_ = VK_NULL_HANDLE;
    }
    if (swapchain_ != VK_NULL_HANDLE)
    {
        vkDestroySwapchainKHR(device_, swapchain_, nullptr);
        swapchain_ = VK_NULL_HANDLE;
    }
    if (!inFlightFences_.empty())
    {
        for (auto f : inFlightFences_)
            vkDestroyFence(device_, f, nullptr);
        inFlightFences_.clear();
    }
    if (!renderFinishedSemaphores_.empty())
    {
        for (auto s : renderFinishedSemaphores_)
            vkDestroySemaphore(device_, s, nullptr);
        renderFinishedSemaphores_.clear();
    }
    if (!imageAvailableSemaphores_.empty())
    {
        for (auto s : imageAvailableSemaphores_)
            vkDestroySemaphore(device_, s, nullptr);
        imageAvailableSemaphores_.clear();
    }
    if (commandPool_ != VK_NULL_HANDLE)
    {
        vkDestroyCommandPool(device_, commandPool_, nullptr);
        commandPool_ = VK_NULL_HANDLE;
    }
    if (device_ != VK_NULL_HANDLE)
    {
        vkDestroyDevice(device_, nullptr);
        device_ = VK_NULL_HANDLE;
    }
    if (surface_ != VK_NULL_HANDLE)
    {
        vkDestroySurfaceKHR(instance_, surface_, nullptr);
        surface_ = VK_NULL_HANDLE;
    }
    if (instance_ != VK_NULL_HANDLE)
    {
        vkDestroyInstance(instance_, nullptr);
        instance_ = VK_NULL_HANDLE;
    }
    if (window_)
    {
        glfwDestroyWindow(window_);
        window_ = nullptr;
        glfwTerminate();
    }
}

void Application::createInstance()
{
    VkApplicationInfo appInfo{};
    appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    appInfo.pApplicationName = "Vulkan FreeType Text";
    appInfo.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
    appInfo.pEngineName = "No Engine";
    appInfo.engineVersion = VK_MAKE_VERSION(1, 0, 0);
    appInfo.apiVersion = VK_API_VERSION_1_0;

    auto extensions = getRequiredExtensions();

    VkInstanceCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    createInfo.pApplicationInfo = &appInfo;
    createInfo.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
    createInfo.ppEnabledExtensionNames = extensions.data();

#ifndef NDEBUG
    uint32_t layerCount = 0;
    vkEnumerateInstanceLayerProperties(&layerCount, nullptr);
    std::vector<VkLayerProperties> availableLayers(layerCount);
    vkEnumerateInstanceLayerProperties(&layerCount, availableLayers.data());

    const char *validationLayer = "VK_LAYER_KHRONOS_validation";
    for (const auto &layer : availableLayers)
    {
        if (std::strcmp(layer.layerName, validationLayer) == 0)
        {
            createInfo.enabledLayerCount = 1;
            createInfo.ppEnabledLayerNames = &validationLayer;
            break;
        }
    }
#endif

    if (vkCreateInstance(&createInfo, nullptr, &instance_) != VK_SUCCESS)
    {
        throw std::runtime_error("Failed to create Vulkan instance");
    }
}

void Application::createSurface()
{
    if (glfwCreateWindowSurface(instance_, window_, nullptr, &surface_) != VK_SUCCESS)
    {
        throw std::runtime_error("Failed to create window surface");
    }
}

void Application::pickPhysicalDevice()
{
    uint32_t deviceCount = 0;
    vkEnumeratePhysicalDevices(instance_, &deviceCount, nullptr);
    if (deviceCount == 0)
    {
        throw std::runtime_error("failed to find GPUs with Vulkan support");
    }
    std::vector<VkPhysicalDevice> devices(deviceCount);
    vkEnumeratePhysicalDevices(instance_, &deviceCount, devices.data());

    for (const auto &device : devices)
    {
        if (isPhysicalDeviceSuitable(device))
        {
            physicalDevice_ = device;
            break;
        }
    }

    if (physicalDevice_ == VK_NULL_HANDLE)
    {
        throw std::runtime_error("failed to find a suitable GPU");
    }

    queueIndices_ = findQueueFamilies(physicalDevice_);
}

bool Application::isPhysicalDeviceSuitable(VkPhysicalDevice device)
{
    QueueFamilyIndices indices = findQueueFamilies(device);
    if (!indices.isComplete())
        return false;

    auto swapchainSupport = querySwapchainSupport(device);
    if (swapchainSupport.formats.empty() || swapchainSupport.presentModes.empty())
        return false;

    return true;
}

Application::QueueFamilyIndices Application::findQueueFamilies(VkPhysicalDevice device)
{
    QueueFamilyIndices indices;

    uint32_t queueFamilyCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(device, &queueFamilyCount, nullptr);
    std::vector<VkQueueFamilyProperties> queueFamilies(queueFamilyCount);
    vkGetPhysicalDeviceQueueFamilyProperties(device, &queueFamilyCount, queueFamilies.data());

    for (uint32_t i = 0; i < queueFamilies.size(); ++i)
    {
        if (queueFamilies[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)
        {
            indices.graphics = i;
        }

        VkBool32 presentSupport = VK_FALSE;
        vkGetPhysicalDeviceSurfaceSupportKHR(device, i, surface_, &presentSupport);
        if (presentSupport)
            indices.present = i;

        if (indices.isComplete())
            break;
    }

    return indices;
}

Application::SwapchainSupportDetails Application::querySwapchainSupport(VkPhysicalDevice device)
{
    SwapchainSupportDetails details;

    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(device, surface_, &details.capabilities);

    uint32_t formatCount = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(device, surface_, &formatCount, nullptr);
    if (formatCount != 0)
    {
        details.formats.resize(formatCount);
        vkGetPhysicalDeviceSurfaceFormatsKHR(device, surface_, &formatCount, details.formats.data());
    }

    uint32_t presentModeCount = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(device, surface_, &presentModeCount, nullptr);
    if (presentModeCount != 0)
    {
        details.presentModes.resize(presentModeCount);
        vkGetPhysicalDeviceSurfacePresentModesKHR(device, surface_, &presentModeCount, details.presentModes.data());
    }

    return details;
}

VkSurfaceFormatKHR Application::chooseSwapSurfaceFormat(const std::vector<VkSurfaceFormatKHR> &formats)
{
    for (const auto &format : formats)
    {
        if (format.format == VK_FORMAT_B8G8R8A8_SRGB && format.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
        {
            return format;
        }
    }
    return formats[0];
}

VkPresentModeKHR Application::chooseSwapPresentMode(const std::vector<VkPresentModeKHR> &modes)
{
    for (const auto &mode : modes)
    {
        if (mode == VK_PRESENT_MODE_MAILBOX_KHR)
            return mode;
    }
    return VK_PRESENT_MODE_FIFO_KHR;
}

VkExtent2D Application::chooseSwapExtent(const VkSurfaceCapabilitiesKHR &capabilities)
{
    if (capabilities.currentExtent.width != std::numeric_limits<uint32_t>::max())
    {
        return capabilities.currentExtent;
    }
    int width = 0;
    int height = 0;
    glfwGetFramebufferSize(window_, &width, &height);
    VkExtent2D actualExtent = {
        static_cast<uint32_t>(width),
        static_cast<uint32_t>(height)};
    actualExtent.width = std::clamp(actualExtent.width,
                                    capabilities.minImageExtent.width,
                                    capabilities.maxImageExtent.width);
    actualExtent.height = std::clamp(actualExtent.height,
                                     capabilities.minImageExtent.height,
                                     capabilities.maxImageExtent.height);
    return actualExtent;
}

void Application::createLogicalDevice()
{
    std::set<uint32_t> uniqueQueueFamilies = {queueIndices_.graphics, queueIndices_.present};
    std::vector<VkDeviceQueueCreateInfo> queueCreateInfos;
    float queuePriority = 1.0f;
    for (uint32_t qf : uniqueQueueFamilies)
    {
        VkDeviceQueueCreateInfo qci{};
        qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        qci.queueFamilyIndex = qf;
        qci.queueCount = 1;
        qci.pQueuePriorities = &queuePriority;
        queueCreateInfos.push_back(qci);
    }

    VkPhysicalDeviceFeatures deviceFeatures{};

    const char *deviceExtensions[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};

    VkDeviceCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    createInfo.queueCreateInfoCount = static_cast<uint32_t>(queueCreateInfos.size());
    createInfo.pQueueCreateInfos = queueCreateInfos.data();
    createInfo.pEnabledFeatures = &deviceFeatures;
    createInfo.enabledExtensionCount = 1;
    createInfo.ppEnabledExtensionNames = deviceExtensions;

#ifndef NDEBUG
    uint32_t layerCount = 0;
    vkEnumerateInstanceLayerProperties(&layerCount, nullptr);
    std::vector<VkLayerProperties> availableLayers(layerCount);
    vkEnumerateInstanceLayerProperties(&layerCount, availableLayers.data());
    const char *validationLayer = "VK_LAYER_KHRONOS_validation";
    for (const auto &layer : availableLayers)
    {
        if (std::strcmp(layer.layerName, validationLayer) == 0)
        {
            createInfo.enabledLayerCount = 1;
            createInfo.ppEnabledLayerNames = &validationLayer;
            break;
        }
    }
#endif

    if (vkCreateDevice(physicalDevice_, &createInfo, nullptr, &device_) != VK_SUCCESS)
    {
        throw std::runtime_error("failed to create logical device");
    }

    vkGetDeviceQueue(device_, queueIndices_.graphics, 0, &graphicsQueue_);
    vkGetDeviceQueue(device_, queueIndices_.present, 0, &presentQueue_);
}

void Application::createSwapchain()
{
    auto swapSupport = querySwapchainSupport(physicalDevice_);
    auto surfaceFormat = chooseSwapSurfaceFormat(swapSupport.formats);
    auto presentMode = chooseSwapPresentMode(swapSupport.presentModes);
    auto extent = chooseSwapExtent(swapSupport.capabilities);

    uint32_t imageCount = swapSupport.capabilities.minImageCount + 1;
    if (swapSupport.capabilities.maxImageCount > 0 && imageCount > swapSupport.capabilities.maxImageCount)
    {
        imageCount = swapSupport.capabilities.maxImageCount;
    }

    VkSwapchainCreateInfoKHR createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    createInfo.surface = surface_;
    createInfo.minImageCount = imageCount;
    createInfo.imageFormat = surfaceFormat.format;
    createInfo.imageColorSpace = surfaceFormat.colorSpace;
    createInfo.imageExtent = extent;
    createInfo.imageArrayLayers = 1;
    createInfo.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    createInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    createInfo.preTransform = swapSupport.capabilities.currentTransform;
    createInfo.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    createInfo.presentMode = presentMode;
    createInfo.clipped = VK_TRUE;
    createInfo.oldSwapchain = VK_NULL_HANDLE;

    if (vkCreateSwapchainKHR(device_, &createInfo, nullptr, &swapchain_) != VK_SUCCESS)
    {
        throw std::runtime_error("failed to create swap chain");
    }

    vkGetSwapchainImagesKHR(device_, swapchain_, &imageCount, nullptr);
    swapchainImages_.resize(imageCount);
    vkGetSwapchainImagesKHR(device_, swapchain_, &imageCount, swapchainImages_.data());

    swapchainImageFormat_ = surfaceFormat.format;
    swapchainExtent_ = extent;
}

void Application::createImageViews()
{
    swapchainImageViews_.resize(swapchainImages_.size());
    for (size_t i = 0; i < swapchainImages_.size(); ++i)
    {
        VkImageViewCreateInfo createInfo{};
        createInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        createInfo.image = swapchainImages_[i];
        createInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        createInfo.format = swapchainImageFormat_;
        createInfo.components = {VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
                                 VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY};
        createInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        createInfo.subresourceRange.baseMipLevel = 0;
        createInfo.subresourceRange.levelCount = 1;
        createInfo.subresourceRange.baseArrayLayer = 0;
        createInfo.subresourceRange.layerCount = 1;
        if (vkCreateImageView(device_, &createInfo, nullptr, &swapchainImageViews_[i]) != VK_SUCCESS)
        {
            throw std::runtime_error("failed to create image views");
        }
    }
}

void Application::createRenderPass()
{
    VkAttachmentDescription colorAttachment{};
    colorAttachment.format = swapchainImageFormat_;
    colorAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
    colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    colorAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    colorAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    colorAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    colorAttachment.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

    VkAttachmentReference colorAttachmentRef{};
    colorAttachmentRef.attachment = 0;
    colorAttachmentRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &colorAttachmentRef;

    VkSubpassDependency dependency{};
    dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
    dependency.dstSubpass = 0;
    dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependency.srcAccessMask = 0;
    dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

    VkRenderPassCreateInfo renderPassInfo{};
    renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    renderPassInfo.attachmentCount = 1;
    renderPassInfo.pAttachments = &colorAttachment;
    renderPassInfo.subpassCount = 1;
    renderPassInfo.pSubpasses = &subpass;
    renderPassInfo.dependencyCount = 1;
    renderPassInfo.pDependencies = &dependency;

    if (vkCreateRenderPass(device_, &renderPassInfo, nullptr, &renderPass_) != VK_SUCCESS)
    {
        throw std::runtime_error("failed to create render pass");
    }
}

void Application::createFramebuffers()
{
    swapchainFramebuffers_.resize(swapchainImageViews_.size());
    for (size_t i = 0; i < swapchainImageViews_.size(); ++i)
    {
        VkImageView attachments[] = {swapchainImageViews_[i]};

        VkFramebufferCreateInfo framebufferInfo{};
        framebufferInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        framebufferInfo.renderPass = renderPass_;
        framebufferInfo.attachmentCount = 1;
        framebufferInfo.pAttachments = attachments;
        framebufferInfo.width = swapchainExtent_.width;
        framebufferInfo.height = swapchainExtent_.height;
        framebufferInfo.layers = 1;

        if (vkCreateFramebuffer(device_, &framebufferInfo, nullptr, &swapchainFramebuffers_[i]) != VK_SUCCESS)
        {
            throw std::runtime_error("failed to create framebuffer");
        }
    }
}

void Application::createCommandPool()
{
    VkCommandPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    poolInfo.queueFamilyIndex = queueIndices_.graphics;

    if (vkCreateCommandPool(device_, &poolInfo, nullptr, &commandPool_) != VK_SUCCESS)
    {
        throw std::runtime_error("failed to create command pool");
    }
}

void Application::createCommandBuffers()
{
    commandBuffers_.resize(kMaxFramesInFlight);
    VkCommandBufferAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfo.commandPool = commandPool_;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandBufferCount = static_cast<uint32_t>(commandBuffers_.size());

    if (vkAllocateCommandBuffers(device_, &allocInfo, commandBuffers_.data()) != VK_SUCCESS)
    {
        throw std::runtime_error("failed to allocate command buffers");
    }
}

void Application::createSyncObjects()
{
    imageAvailableSemaphores_.resize(kMaxFramesInFlight);
    inFlightFences_.resize(kMaxFramesInFlight);
    imageInFlight_.resize(swapchainImages_.size(), VK_NULL_HANDLE);
    renderFinishedSemaphores_.resize(swapchainImages_.size());

    VkSemaphoreCreateInfo semaphoreInfo{};
    semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;

    VkFenceCreateInfo fenceInfo{};
    fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;

    for (size_t i = 0; i < kMaxFramesInFlight; ++i)
    {
        if (vkCreateSemaphore(device_, &semaphoreInfo, nullptr, &imageAvailableSemaphores_[i]) != VK_SUCCESS ||
            vkCreateFence(device_, &fenceInfo, nullptr, &inFlightFences_[i]) != VK_SUCCESS)
        {
            throw std::runtime_error("failed to create sync objects");
        }
    }

    for (size_t i = 0; i < renderFinishedSemaphores_.size(); ++i)
    {
        if (vkCreateSemaphore(device_, &semaphoreInfo, nullptr, &renderFinishedSemaphores_[i]) != VK_SUCCESS)
        {
            throw std::runtime_error("failed to create render-finished semaphores");
        }
    }
}

void Application::recreateSwapchain()
{
    int width = 0;
    int height = 0;
    glfwGetFramebufferSize(window_, &width, &height);
    while (width == 0 || height == 0)
    {
        glfwGetFramebufferSize(window_, &width, &height);
        glfwWaitEvents();
    }

    vkDeviceWaitIdle(device_);

    cleanupSwapchain();

    for (auto s : renderFinishedSemaphores_)
        vkDestroySemaphore(device_, s, nullptr);
    renderFinishedSemaphores_.clear();

    createSwapchain();
    createImageViews();
    createFramebuffers();

    VkSemaphoreCreateInfo semaphoreInfo{};
    semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    renderFinishedSemaphores_.resize(swapchainImages_.size());
    for (auto &s : renderFinishedSemaphores_)
    {
        if (vkCreateSemaphore(device_, &semaphoreInfo, nullptr, &s) != VK_SUCCESS)
        {
            throw std::runtime_error("failed to recreate render-finished semaphores");
        }
    }
    imageInFlight_.assign(swapchainImages_.size(), VK_NULL_HANDLE);

    textRenderer_.setScreenSize(swapchainExtent_.width, swapchainExtent_.height);
    uiRenderer_.setScreenSize(swapchainExtent_.width, swapchainExtent_.height);
}

void Application::cleanupSwapchain()
{
    for (auto view : swapchainImageViews_)
        vkDestroyImageView(device_, view, nullptr);
    swapchainImageViews_.clear();
    for (auto fb : swapchainFramebuffers_)
        vkDestroyFramebuffer(device_, fb, nullptr);
    swapchainFramebuffers_.clear();
    if (swapchain_ != VK_NULL_HANDLE)
    {
        vkDestroySwapchainKHR(device_, swapchain_, nullptr);
        swapchain_ = VK_NULL_HANDLE;
    }
}

void Application::framebufferResizeCallback(GLFWwindow *window, int width, int height)
{
    auto *app = static_cast<Application *>(glfwGetWindowUserPointer(window));
    if (app)
    {
        app->framebufferResized_ = true;
    }
}

void Application::cursorPosCallback(GLFWwindow *window, double xpos, double ypos)
{
    auto *app = static_cast<Application *>(glfwGetWindowUserPointer(window));
    if (!app)
        return;

    int winW = 0, winH = 0;
    glfwGetWindowSize(window, &winW, &winH);
    int fbW = 0, fbH = 0;
    glfwGetFramebufferSize(window, &fbW, &fbH);
    const double sx = winW > 0 ? static_cast<double>(fbW) / winW : 1.0;
    const double sy = winH > 0 ? static_cast<double>(fbH) / winH : 1.0;
    app->mouseX_ = xpos * sx;
    app->mouseY_ = ypos * sy;
}

void Application::mouseButtonCallback(GLFWwindow *window, int button, int action, int mods)
{
    auto *app = static_cast<Application *>(glfwGetWindowUserPointer(window));
    if (!app || button != GLFW_MOUSE_BUTTON_LEFT || action != GLFW_PRESS)
        return;

    int winW = 0, winH = 0;
    glfwGetWindowSize(window, &winW, &winH);
    int fbW = 0, fbH = 0;
    glfwGetFramebufferSize(window, &fbW, &fbH);
    const double sx = winW > 0 ? static_cast<double>(fbW) / winW : 1.0;
    const double sy = winH > 0 ? static_cast<double>(fbH) / winH : 1.0;
    const double mx = app->mouseX_;
    const double my = app->mouseY_;

    const float centerX = static_cast<float>(fbW) * 0.5f;
    const float buttonX0 = centerX - kDropdownWidth * 0.5f;
    const float buttonX1 = centerX + kDropdownWidth * 0.5f;
    const float buttonY0 = 9.0f;
    const float buttonY1 = buttonY0 + kDropdownHeight;

    if (mx >= buttonX0 && mx < buttonX1 && my >= buttonY0 && my < buttonY1)
    {
        app->dropdownOpen_ = !app->dropdownOpen_;
        return;
    }

    if (app->dropdownOpen_)
    {
        for (size_t i = 0; i < app->fonts_.size(); ++i)
        {
            const float itemY0 = static_cast<float>(kAppbarHeight) +
                                 static_cast<float>(i) * kItemHeight;
            const float itemY1 = itemY0 + kItemHeight;
            if (mx >= buttonX0 && mx < buttonX1 && my >= itemY0 && my < itemY1)
            {
                if (i != app->currentFontIndex_)
                {
                    app->selectFont(i);
                }
                app->dropdownOpen_ = false;
                return;
            }
        }
        app->dropdownOpen_ = false;
    }
}

void Application::selectFont(size_t index)
{
    if (index >= fonts_.size())
        return;
    currentFontIndex_ = index;
    textRenderer_.setFont(fonts_[index].path, kFontPixelSize);
}

void Application::renderUi(VkCommandBuffer cmd, uint32_t imageWidth, uint32_t imageHeight)
{
    const float W = static_cast<float>(imageWidth);

    textRenderer_.drawRect(cmd, 0.0f, 0.0f,
                           W, static_cast<float>(kAppbarHeight),
                           0.15f, 0.15f, 0.18f, 1.0f);

    const float centerX = W * 0.5f;
    const float buttonX0 = centerX - kDropdownWidth * 0.5f;
    const float buttonY0 = 9.0f;

    if (dropdownOpen_)
    {
        for (size_t i = 0; i < fonts_.size(); ++i)
        {
            const float itemY0 = static_cast<float>(kAppbarHeight) +
                                 static_cast<float>(i) * kItemHeight;
            const float r = (i == currentFontIndex_) ? 0.30f : 0.20f;
            const float g = (i == currentFontIndex_) ? 0.30f : 0.20f;
            const float b = (i == currentFontIndex_) ? 0.35f : 0.22f;
            textRenderer_.drawRect(cmd, buttonX0, itemY0,
                                   static_cast<float>(kDropdownWidth),
                                   static_cast<float>(kItemHeight), r, g, b, 1.0f);

            const std::string &label = fonts_[i].label;
            const float labelWidth = uiRenderer_.measureText(label);
            const float labelX = centerX - labelWidth * 0.5f;
            const float itemCenterY = itemY0 + kItemHeight * 0.5f;
            const float labelY = itemCenterY - uiRenderer_.fontAscent() * 0.5f;
            const float textR = (i == currentFontIndex_) ? 1.0f : 0.85f;
            const float textG = (i == currentFontIndex_) ? 1.0f : 0.85f;
            const float textB = (i == currentFontIndex_) ? 1.0f : 0.85f;
            uiRenderer_.drawText(cmd, label, labelX, labelY, textR, textG, textB, 1.0f);
        }
    }

    textRenderer_.drawRect(cmd, buttonX0, buttonY0,
                           static_cast<float>(kDropdownWidth),
                           static_cast<float>(kDropdownHeight), 0.22f, 0.22f, 0.28f, 1.0f);

    const std::string &label = fonts_[currentFontIndex_].label;
    const float labelWidth = uiRenderer_.measureText(label);
    const float labelX = centerX - labelWidth * 0.5f;
    const float buttonCenterY = buttonY0 + kDropdownHeight * 0.5f;
    const float labelY = buttonCenterY - uiRenderer_.fontAscent() * 0.5f;
    uiRenderer_.drawText(cmd, label, labelX, labelY, 1.0f, 1.0f, 1.0f, 1.0f);

    const float caretX = buttonX0 + static_cast<float>(kDropdownWidth) - 18.0f;
    const float caretY = buttonY0 + kDropdownHeight * 0.5f - 0.75f;
    const float caretHalf = 5.0f;
    textRenderer_.drawRect(cmd, caretX - caretHalf, caretY, caretHalf * 2.0f, 1.5f, 0.8f, 0.8f, 0.85f, 1.0f);
}

std::vector<const char *> Application::getRequiredExtensions()
{
    uint32_t glfwExtensionCount = 0;
    const char **glfwExtensions = glfwGetRequiredInstanceExtensions(&glfwExtensionCount);
    return {glfwExtensions, glfwExtensions + glfwExtensionCount};
}
