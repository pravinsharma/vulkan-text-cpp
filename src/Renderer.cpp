#include "Renderer.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <set>
#include <stdexcept>
#include <vector>

#define STB_TRUETYPE_IMPLEMENTATION
#include <stb_truetype.h>

namespace
{
const std::vector<const char*> kFontCandidatePaths = {
    "C:/Windows/Fonts/segoeui.ttf",
    "C:/Windows/Fonts/arial.ttf",
    "C:/Windows/Fonts/seguisb.ttf",
};

std::filesystem::path exeDir()
{
    char buffer[MAX_PATH] = {};
    DWORD len = GetModuleFileNameA(nullptr, buffer, MAX_PATH);
    if (len == 0 || len >= MAX_PATH)
    {
        return std::filesystem::current_path();
    }
    return std::filesystem::path(std::string(buffer, len)).parent_path();
}

std::vector<char> readFileImpl(const std::filesystem::path& path)
{
    std::ifstream file(path, std::ios::ate | std::ios::binary);
    if (!file.is_open())
    {
        throw std::runtime_error("Failed to open file: " + path.string());
    }

    const auto size = static_cast<size_t>(file.tellg());
    std::vector<char> buffer(size);

    file.seekg(0);
    file.read(buffer.data(), static_cast<std::streamsize>(size));
    file.close();

    return buffer;
}
}  // namespace

Renderer::Renderer(VkInstance instance, VkSurfaceKHR surface, GLFWwindow* window)
    : instance_(instance), surface_(surface), window_(window)
{
    pickPhysicalDevice();
    createLogicalDevice();
    createSwapchain();
    createImageViews();
    createRenderPass();
    createDescriptorSetLayout();
    createPipeline();
    createFramebuffers();
    createCommandPool();
    createAtlasResources();
    createVertexBuffer();
    createDescriptorResources();
    createCommandBuffers();
    createSyncObjects();
}

Renderer::~Renderer()
{
    vkDeviceWaitIdle(device_);

    vkDestroyBuffer(device_, vertexBuffer_, nullptr);
    vkFreeMemory(device_, vertexMemory_, nullptr);

    vkDestroySampler(device_, atlasSampler_, nullptr);
    vkDestroyImageView(device_, atlasImageView_, nullptr);
    vkDestroyImage(device_, atlasImage_, nullptr);
    vkFreeMemory(device_, atlasMemory_, nullptr);

    vkDestroyDescriptorSetLayout(device_, descriptorSetLayout_, nullptr);
    vkDestroyDescriptorPool(device_, descriptorPool_, nullptr);

    vkDestroyPipeline(device_, pipeline_, nullptr);
    vkDestroyPipelineLayout(device_, pipelineLayout_, nullptr);
    vkDestroyRenderPass(device_, renderPass_, nullptr);

    for (auto fb : swapchainFramebuffers_) vkDestroyFramebuffer(device_, fb, nullptr);
    cleanupSwapchain();

    for (auto s : renderFinishedSemaphores_) vkDestroySemaphore(device_, s, nullptr);
    renderFinishedSemaphores_.clear();

    vkDestroyFence(device_, inFlightFence_, nullptr);

    vkDestroyCommandPool(device_, commandPool_, nullptr);
    vkDestroyDevice(device_, nullptr);
}

void Renderer::pickPhysicalDevice()
{
    uint32_t deviceCount = 0;
    vkEnumeratePhysicalDevices(instance_, &deviceCount, nullptr);
    if (deviceCount == 0)
    {
        throw std::runtime_error("No Vulkan-capable GPU found");
    }

    std::vector<VkPhysicalDevice> devices(deviceCount);
    vkEnumeratePhysicalDevices(instance_, &deviceCount, devices.data());

    for (const auto& device : devices)
    {
        if (isDeviceSuitable(device))
        {
            physicalDevice_ = device;
            break;
        }
    }

    if (physicalDevice_ == VK_NULL_HANDLE)
    {
        throw std::runtime_error("No suitable GPU found");
    }
}

bool Renderer::isDeviceSuitable(VkPhysicalDevice device) const
{
    QueueFamilyIndices indices = findQueueFamilies(device);
    if (!indices.isComplete()) return false;

    auto swap = querySwapchainSupport(device);
    if (swap.formats.empty() || swap.presentModes.empty()) return false;

    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(device, &props);
    return props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU || true;
}

QueueFamilyIndices Renderer::findQueueFamilies(VkPhysicalDevice device) const
{
    QueueFamilyIndices indices;

    uint32_t count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(device, &count, nullptr);
    std::vector<VkQueueFamilyProperties> families(count);
    vkGetPhysicalDeviceQueueFamilyProperties(device, &count, families.data());

    for (uint32_t i = 0; i < count; ++i)
    {
        if (families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)
        {
            indices.graphics = i;
        }

        VkBool32 presentSupport = VK_FALSE;
        vkGetPhysicalDeviceSurfaceSupportKHR(device, i, surface_, &presentSupport);
        if (presentSupport)
        {
            indices.present = i;
        }

        if (indices.isComplete()) break;
    }

    return indices;
}

SwapchainSupportDetails Renderer::querySwapchainSupport(VkPhysicalDevice device) const
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

    uint32_t modeCount = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(device, surface_, &modeCount, nullptr);
    if (modeCount != 0)
    {
        details.presentModes.resize(modeCount);
        vkGetPhysicalDeviceSurfacePresentModesKHR(device, surface_, &modeCount, details.presentModes.data());
    }

    return details;
}

void Renderer::createLogicalDevice()
{
    queueFamilies_ = findQueueFamilies(physicalDevice_);

    std::set<uint32_t> uniqueFamilies = { *queueFamilies_.graphics, *queueFamilies_.present };
    std::vector<VkDeviceQueueCreateInfo> queueCreateInfos;

    float priority = 1.0f;
    for (uint32_t family : uniqueFamilies)
    {
        VkDeviceQueueCreateInfo ci{};
        ci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        ci.queueFamilyIndex = family;
        ci.queueCount = 1;
        ci.pQueuePriorities = &priority;
        queueCreateInfos.push_back(ci);
    }

    VkPhysicalDeviceFeatures features{};
    VkDeviceCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    ci.queueCreateInfoCount = static_cast<uint32_t>(queueCreateInfos.size());
    ci.pQueueCreateInfos = queueCreateInfos.data();
    ci.pEnabledFeatures = &features;

    const char* ext = VK_KHR_SWAPCHAIN_EXTENSION_NAME;
    ci.enabledExtensionCount = 1;
    ci.ppEnabledExtensionNames = &ext;

    if (vkCreateDevice(physicalDevice_, &ci, nullptr, &device_) != VK_SUCCESS)
    {
        throw std::runtime_error("Failed to create logical device");
    }

    vkGetDeviceQueue(device_, *queueFamilies_.graphics, 0, &graphicsQueue_);
    vkGetDeviceQueue(device_, *queueFamilies_.present, 0, &presentQueue_);
}

void Renderer::createSwapchain()
{
    auto support = querySwapchainSupport(physicalDevice_);

    VkSurfaceFormatKHR format = support.formats[0];
    for (const auto& f : support.formats)
    {
        if (f.format == VK_FORMAT_B8G8R8A8_SRGB && f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
        {
            format = f;
            break;
        }
    }

    VkPresentModeKHR presentMode = VK_PRESENT_MODE_FIFO_KHR;
    for (auto m : support.presentModes)
    {
        if (m == VK_PRESENT_MODE_MAILBOX_KHR)
        {
            presentMode = m;
            break;
        }
    }

    if (support.capabilities.currentExtent.width != std::numeric_limits<uint32_t>::max())
    {
        swapchainExtent_ = support.capabilities.currentExtent;
    }
    else
    {
        int w = 0, h = 0;
        glfwGetFramebufferSize(window_, &w, &h);
        swapchainExtent_ = {
            std::clamp(static_cast<uint32_t>(w),
                       support.capabilities.minImageExtent.width,
                       support.capabilities.maxImageExtent.width),
            std::clamp(static_cast<uint32_t>(h),
                       support.capabilities.minImageExtent.height,
                       support.capabilities.maxImageExtent.height),
        };
    }

    uint32_t imageCount = support.capabilities.minImageCount + 1;
    if (support.capabilities.maxImageCount > 0 && imageCount > support.capabilities.maxImageCount)
    {
        imageCount = support.capabilities.maxImageCount;
    }

    VkSwapchainCreateInfoKHR ci{};
    ci.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    ci.surface = surface_;
    ci.minImageCount = imageCount;
    ci.imageFormat = format.format;
    ci.imageColorSpace = format.colorSpace;
    ci.imageExtent = swapchainExtent_;
    ci.imageArrayLayers = 1;
    ci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    ci.preTransform = support.capabilities.currentTransform;
    ci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    ci.presentMode = presentMode;
    ci.clipped = VK_TRUE;
    ci.oldSwapchain = VK_NULL_HANDLE;

    uint32_t qfCount = 2;
    uint32_t qf[2] = { *queueFamilies_.graphics, *queueFamilies_.present };
    if (queueFamilies_.graphics == queueFamilies_.present)
    {
        qfCount = 1;
    }
    ci.imageSharingMode = qfCount == 1 ? VK_SHARING_MODE_EXCLUSIVE : VK_SHARING_MODE_CONCURRENT;
    ci.queueFamilyIndexCount = qfCount;
    ci.pQueueFamilyIndices = qf;

    if (vkCreateSwapchainKHR(device_, &ci, nullptr, &swapchain_) != VK_SUCCESS)
    {
        throw std::runtime_error("Failed to create swapchain");
    }

    vkGetSwapchainImagesKHR(device_, swapchain_, &imageCount, nullptr);
    swapchainImages_.resize(imageCount);
    vkGetSwapchainImagesKHR(device_, swapchain_, &imageCount, swapchainImages_.data());

    swapchainImageFormat_ = format.format;
}

void Renderer::createImageViews()
{
    swapchainImageViews_.resize(swapchainImages_.size());
    for (size_t i = 0; i < swapchainImages_.size(); ++i)
    {
        VkImageViewCreateInfo ci{};
        ci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        ci.image = swapchainImages_[i];
        ci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        ci.format = swapchainImageFormat_;
        ci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        ci.subresourceRange.levelCount = 1;
        ci.subresourceRange.layerCount = 1;

        if (vkCreateImageView(device_, &ci, nullptr, &swapchainImageViews_[i]) != VK_SUCCESS)
        {
            throw std::runtime_error("Failed to create image view");
        }
    }
}

void Renderer::createRenderPass()
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

    VkAttachmentReference colorRef{ 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };

    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &colorRef;

    VkSubpassDependency dep{};
    dep.srcSubpass = VK_SUBPASS_EXTERNAL;
    dep.dstSubpass = 0;
    dep.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.srcAccessMask = 0;
    dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

    VkRenderPassCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    ci.attachmentCount = 1;
    ci.pAttachments = &colorAttachment;
    ci.subpassCount = 1;
    ci.pSubpasses = &subpass;
    ci.dependencyCount = 1;
    ci.pDependencies = &dep;

    if (vkCreateRenderPass(device_, &ci, nullptr, &renderPass_) != VK_SUCCESS)
    {
        throw std::runtime_error("Failed to create render pass");
    }
}

void Renderer::createDescriptorSetLayout()
{
    VkDescriptorSetLayoutBinding binding{};
    binding.binding = 0;
    binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    binding.descriptorCount = 1;
    binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    ci.bindingCount = 1;
    ci.pBindings = &binding;

    if (vkCreateDescriptorSetLayout(device_, &ci, nullptr, &descriptorSetLayout_) != VK_SUCCESS)
    {
        throw std::runtime_error("Failed to create descriptor set layout");
    }
}

VkShaderModule Renderer::createShaderModule(const std::vector<char>& code) const
{
    VkShaderModuleCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    ci.codeSize = code.size();
    ci.pCode = reinterpret_cast<const uint32_t*>(code.data());

    VkShaderModule mod = VK_NULL_HANDLE;
    if (vkCreateShaderModule(device_, &ci, nullptr, &mod) != VK_SUCCESS)
    {
        throw std::runtime_error("Failed to create shader module");
    }
    return mod;
}

void Renderer::createPipeline()
{
    auto vsCode = readFile("shaders/text.vert.spv");
    auto fsCode = readFile("shaders/text.frag.spv");
    auto vs = createShaderModule(vsCode);
    auto fs = createShaderModule(fsCode);

    VkPipelineShaderStageCreateInfo vsStage{};
    vsStage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    vsStage.stage = VK_SHADER_STAGE_VERTEX_BIT;
    vsStage.module = vs;
    vsStage.pName = "main";

    VkPipelineShaderStageCreateInfo fsStage = vsStage;
    fsStage.stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    fsStage.module = fs;

    VkPipelineShaderStageCreateInfo stages[2] = { vsStage, fsStage };

    VkPipelineVertexInputStateCreateInfo vi{};
    vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

    VkVertexInputBindingDescription binding{};
    binding.binding = 0;
    binding.stride = sizeof(float) * 4;
    binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    std::array<VkVertexInputAttributeDescription, 2> attrs{};
    attrs[0].location = 0;
    attrs[0].binding = 0;
    attrs[0].format = VK_FORMAT_R32G32_SFLOAT;
    attrs[0].offset = 0;
    attrs[1].location = 1;
    attrs[1].binding = 0;
    attrs[1].format = VK_FORMAT_R32G32_SFLOAT;
    attrs[1].offset = sizeof(float) * 2;

    vi.vertexBindingDescriptionCount = 1;
    vi.pVertexBindingDescriptions = &binding;
    vi.vertexAttributeDescriptionCount = static_cast<uint32_t>(attrs.size());
    vi.pVertexAttributeDescriptions = attrs.data();

    VkPipelineInputAssemblyStateCreateInfo ia{};
    ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    ia.primitiveRestartEnable = VK_FALSE;

    VkPipelineViewportStateCreateInfo vp{};
    vp.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vp.viewportCount = 1;
    vp.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo rs{};
    rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.lineWidth = 1.0f;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;

    VkPipelineMultisampleStateCreateInfo ms{};
    ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineColorBlendAttachmentState blend{};
    blend.blendEnable = VK_TRUE;
    blend.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    blend.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blend.colorBlendOp = VK_BLEND_OP_ADD;
    blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blend.alphaBlendOp = VK_BLEND_OP_ADD;
    blend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                           VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

    VkPipelineColorBlendStateCreateInfo cb{};
    cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    cb.attachmentCount = 1;
    cb.pAttachments = &blend;

    VkDynamicState dyn[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dsi{};
    dsi.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dsi.dynamicStateCount = 2;
    dsi.pDynamicStates = dyn;

    VkPushConstantRange pcr{};
    pcr.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    pcr.offset = 0;
    pcr.size = sizeof(float) * 16;

    VkPipelineLayoutCreateInfo pl{};
    pl.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pl.setLayoutCount = 1;
    pl.pSetLayouts = &descriptorSetLayout_;
    pl.pushConstantRangeCount = 1;
    pl.pPushConstantRanges = &pcr;

    if (vkCreatePipelineLayout(device_, &pl, nullptr, &pipelineLayout_) != VK_SUCCESS)
    {
        throw std::runtime_error("Failed to create pipeline layout");
    }

    VkGraphicsPipelineCreateInfo gp{};
    gp.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    gp.stageCount = 2;
    gp.pStages = stages;
    gp.pVertexInputState = &vi;
    gp.pInputAssemblyState = &ia;
    gp.pViewportState = &vp;
    gp.pRasterizationState = &rs;
    gp.pMultisampleState = &ms;
    gp.pColorBlendState = &cb;
    gp.pDynamicState = &dsi;
    gp.layout = pipelineLayout_;
    gp.renderPass = renderPass_;
    gp.subpass = 0;

    if (vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &gp, nullptr, &pipeline_) != VK_SUCCESS)
    {
        throw std::runtime_error("Failed to create graphics pipeline");
    }

    vkDestroyShaderModule(device_, vs, nullptr);
    vkDestroyShaderModule(device_, fs, nullptr);
}

void Renderer::createFramebuffers()
{
    swapchainFramebuffers_.resize(swapchainImageViews_.size());
    for (size_t i = 0; i < swapchainImageViews_.size(); ++i)
    {
        VkImageView att[] = { swapchainImageViews_[i] };
        VkFramebufferCreateInfo ci{};
        ci.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        ci.renderPass = renderPass_;
        ci.attachmentCount = 1;
        ci.pAttachments = att;
        ci.width = swapchainExtent_.width;
        ci.height = swapchainExtent_.height;
        ci.layers = 1;

        if (vkCreateFramebuffer(device_, &ci, nullptr, &swapchainFramebuffers_[i]) != VK_SUCCESS)
        {
            throw std::runtime_error("Failed to create framebuffer");
        }
    }
}

void Renderer::createCommandPool()
{
    VkCommandPoolCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    ci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    ci.queueFamilyIndex = *queueFamilies_.graphics;

    if (vkCreateCommandPool(device_, &ci, nullptr, &commandPool_) != VK_SUCCESS)
    {
        throw std::runtime_error("Failed to create command pool");
    }
}

uint32_t Renderer::findMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags properties) const
{
    VkPhysicalDeviceMemoryProperties memProps{};
    vkGetPhysicalDeviceMemoryProperties(physicalDevice_, &memProps);

    for (uint32_t i = 0; i < memProps.memoryTypeCount; ++i)
    {
        if ((typeFilter & (1 << i)) &&
            (memProps.memoryTypes[i].propertyFlags & properties) == properties)
        {
            return i;
        }
    }
    throw std::runtime_error("Failed to find suitable memory type");
}

void Renderer::createBuffer(VkDeviceSize size, VkBufferUsageFlags usage,
                            VkMemoryPropertyFlags properties,
                            VkBuffer& buffer, VkDeviceMemory& memory) const
{
    VkBufferCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    ci.size = size;
    ci.usage = usage;
    ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    if (vkCreateBuffer(device_, &ci, nullptr, &buffer) != VK_SUCCESS)
    {
        throw std::runtime_error("Failed to create buffer");
    }

    VkMemoryRequirements req{};
    vkGetBufferMemoryRequirements(device_, buffer, &req);

    VkMemoryAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = findMemoryType(req.memoryTypeBits, properties);

    if (vkAllocateMemory(device_, &ai, nullptr, &memory) != VK_SUCCESS)
    {
        throw std::runtime_error("Failed to allocate buffer memory");
    }

    vkBindBufferMemory(device_, buffer, memory, 0);
}

void Renderer::copyBuffer(VkBuffer src, VkBuffer dst, VkDeviceSize size) const
{
    VkCommandBufferAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandPool = commandPool_;
    ai.commandBufferCount = 1;

    VkCommandBuffer cmd = VK_NULL_HANDLE;
    vkAllocateCommandBuffers(device_, &ai, &cmd);

    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &bi);

    VkBufferCopy region{};
    region.size = size;
    vkCmdCopyBuffer(cmd, src, dst, 1, &region);

    vkEndCommandBuffer(cmd);

    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    vkQueueSubmit(graphicsQueue_, 1, &si, VK_NULL_HANDLE);
    vkQueueWaitIdle(graphicsQueue_);

    vkFreeCommandBuffers(device_, commandPool_, 1, &cmd);
}

void Renderer::transitionImageLayout(VkImage image, VkFormat format,
                                     VkImageLayout oldLayout, VkImageLayout newLayout) const
{
    (void)format;

    VkCommandBufferAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandPool = commandPool_;
    ai.commandBufferCount = 1;

    VkCommandBuffer cmd = VK_NULL_HANDLE;
    vkAllocateCommandBuffers(device_, &ai, &cmd);

    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &bi);

    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = oldLayout;
    barrier.newLayout = newLayout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.layerCount = 1;

    VkPipelineStageFlags srcStage = 0;
    VkPipelineStageFlags dstStage = 0;

    if (oldLayout == VK_IMAGE_LAYOUT_UNDEFINED && newLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL)
    {
        barrier.srcAccessMask = 0;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        srcStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        dstStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    }
    else if (oldLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL &&
             newLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
    {
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        srcStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        dstStage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    }
    else
    {
        vkFreeCommandBuffers(device_, commandPool_, 1, &cmd);
        throw std::invalid_argument("Unsupported layout transition");
    }

    vkCmdPipelineBarrier(cmd, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &barrier);
    vkEndCommandBuffer(cmd);

    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    vkQueueSubmit(graphicsQueue_, 1, &si, VK_NULL_HANDLE);
    vkQueueWaitIdle(graphicsQueue_);

    vkFreeCommandBuffers(device_, commandPool_, 1, &cmd);
}

void Renderer::copyBufferToImage(VkBuffer buffer, VkImage image, uint32_t width, uint32_t height) const
{
    VkCommandBufferAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandPool = commandPool_;
    ai.commandBufferCount = 1;

    VkCommandBuffer cmd = VK_NULL_HANDLE;
    vkAllocateCommandBuffers(device_, &ai, &cmd);

    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &bi);

    VkBufferImageCopy region{};
    region.bufferOffset = 0;
    region.bufferRowLength = 0;
    region.bufferImageHeight = 0;
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.layerCount = 1;
    region.imageOffset = { 0, 0, 0 };
    region.imageExtent = { width, height, 1 };

    vkCmdCopyBufferToImage(cmd, buffer, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    vkEndCommandBuffer(cmd);

    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    vkQueueSubmit(graphicsQueue_, 1, &si, VK_NULL_HANDLE);
    vkQueueWaitIdle(graphicsQueue_);

    vkFreeCommandBuffers(device_, commandPool_, 1, &cmd);
}

void Renderer::createAtlasResources()
{
    std::string fontPath;
    std::vector<unsigned char> fontBuffer;
    for (const auto& p : kFontCandidatePaths)
    {
        std::ifstream f(p, std::ios::binary | std::ios::ate);
        if (f.is_open())
        {
            auto sz = static_cast<size_t>(f.tellg());
            f.seekg(0);
            fontBuffer.resize(sz);
            f.read(reinterpret_cast<char*>(fontBuffer.data()), static_cast<std::streamsize>(sz));
            fontPath = p;
            break;
        }
    }
    if (fontPath.empty())
    {
        throw std::runtime_error("No TTF font found in C:/Windows/Fonts (tried segoeui/arial/seguisb)");
    }

    std::vector<unsigned char> bitmap(kAtlasWidth * kAtlasHeight);
    int rc = stbtt_BakeFontBitmap(
        fontBuffer.data(), 0, kFontSize,
        bitmap.data(), kAtlasWidth, kAtlasHeight,
        kFirstChar, kCharCount, bakedChars_);
    if (rc <= 0)
    {
        throw std::runtime_error("Failed to bake font bitmap: atlas too small");
    }

    int ascent = 0;
    stbtt_fontinfo fontInfo{};
    if (!stbtt_InitFont(&fontInfo, fontBuffer.data(), 0))
    {
        throw std::runtime_error("stbtt_InitFont failed for " + fontPath);
    }
    stbtt_GetFontVMetrics(&fontInfo, &ascent, nullptr, nullptr);
    fontAscentPx_ = ascent * stbtt_ScaleForPixelHeight(&fontInfo, kFontSize);

    VkImageCreateInfo ii{};
    ii.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = VK_FORMAT_R8_UNORM;
    ii.extent = { static_cast<uint32_t>(kAtlasWidth), static_cast<uint32_t>(kAtlasHeight), 1 };
    ii.mipLevels = 1;
    ii.arrayLayers = 1;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = VK_IMAGE_TILING_LINEAR;
    ii.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    ii.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    if (vkCreateImage(device_, &ii, nullptr, &atlasImage_) != VK_SUCCESS)
    {
        throw std::runtime_error("Failed to create atlas image");
    }

    VkMemoryRequirements req{};
    vkGetImageMemoryRequirements(device_, atlasImage_, &req);

    VkMemoryAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = findMemoryType(
        req.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

    if (vkAllocateMemory(device_, &ai, nullptr, &atlasMemory_) != VK_SUCCESS)
    {
        throw std::runtime_error("Failed to allocate atlas memory");
    }
    vkBindImageMemory(device_, atlasImage_, atlasMemory_, 0);

    VkImageViewCreateInfo vi{};
    vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vi.image = atlasImage_;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = VK_FORMAT_R8_UNORM;
    vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    vi.subresourceRange.levelCount = 1;
    vi.subresourceRange.layerCount = 1;

    if (vkCreateImageView(device_, &vi, nullptr, &atlasImageView_) != VK_SUCCESS)
    {
        throw std::runtime_error("Failed to create atlas image view");
    }

    VkSamplerCreateInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    si.magFilter = VK_FILTER_LINEAR;
    si.minFilter = VK_FILTER_LINEAR;
    si.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.anisotropyEnable = VK_FALSE;
    si.borderColor = VK_BORDER_COLOR_INT_OPAQUE_BLACK;
    si.unnormalizedCoordinates = VK_FALSE;
    si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;

    if (vkCreateSampler(device_, &si, nullptr, &atlasSampler_) != VK_SUCCESS)
    {
        throw std::runtime_error("Failed to create sampler");
    }

    VkBuffer staging = VK_NULL_HANDLE;
    VkDeviceMemory stagingMem = VK_NULL_HANDLE;
    VkDeviceSize size = static_cast<VkDeviceSize>(bitmap.size());
    createBuffer(size,
                 VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                 staging, stagingMem);

    void* mapped = nullptr;
    vkMapMemory(device_, stagingMem, 0, size, 0, &mapped);
    std::memcpy(mapped, bitmap.data(), static_cast<size_t>(size));
    vkUnmapMemory(device_, stagingMem);

    transitionImageLayout(atlasImage_, VK_FORMAT_R8_UNORM,
                          VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    copyBufferToImage(staging, atlasImage_, kAtlasWidth, kAtlasHeight);
    transitionImageLayout(atlasImage_, VK_FORMAT_R8_UNORM,
                          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

    vkDestroyBuffer(device_, staging, nullptr);
    vkFreeMemory(device_, stagingMem, nullptr);
}

void Renderer::createVertexBuffer()
{
    createBuffer(vertexBufferSize_,
                 VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                 vertexBuffer_, vertexMemory_);
}

void Renderer::createDescriptorResources()
{
    VkDescriptorPoolSize poolSize{};
    poolSize.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSize.descriptorCount = 1;

    VkDescriptorPoolCreateInfo pi{};
    pi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pi.poolSizeCount = 1;
    pi.pPoolSizes = &poolSize;
    pi.maxSets = 1;

    if (vkCreateDescriptorPool(device_, &pi, nullptr, &descriptorPool_) != VK_SUCCESS)
    {
        throw std::runtime_error("Failed to create descriptor pool");
    }

    VkDescriptorSetAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    ai.descriptorPool = descriptorPool_;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &descriptorSetLayout_;

    if (vkAllocateDescriptorSets(device_, &ai, &descriptorSet_) != VK_SUCCESS)
    {
        throw std::runtime_error("Failed to allocate descriptor set");
    }

    VkDescriptorImageInfo imgInfo{};
    imgInfo.sampler = atlasSampler_;
    imgInfo.imageView = atlasImageView_;
    imgInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = descriptorSet_;
    write.dstBinding = 0;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo = &imgInfo;

    vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);
}

void Renderer::createCommandBuffers()
{
    commandBuffers_.resize(swapchainFramebuffers_.size());

    VkCommandBufferAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    ai.commandPool = commandPool_;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = static_cast<uint32_t>(commandBuffers_.size());

    if (vkAllocateCommandBuffers(device_, &ai, commandBuffers_.data()) != VK_SUCCESS)
    {
        throw std::runtime_error("Failed to allocate command buffers");
    }
}

void Renderer::createSyncObjects()
{
    VkSemaphoreCreateInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;

    VkFenceCreateInfo fi{};
    fi.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;

    if (vkCreateSemaphore(device_, &si, nullptr, &imageAvailableSemaphore_) != VK_SUCCESS)
    {
        throw std::runtime_error("Failed to create image-available semaphore");
    }

    const size_t n = swapchainImages_.size();
    renderFinishedSemaphores_.resize(n, VK_NULL_HANDLE);
    for (size_t i = 0; i < n; ++i)
    {
        if (vkCreateSemaphore(device_, &si, nullptr, &renderFinishedSemaphores_[i]) != VK_SUCCESS)
        {
            throw std::runtime_error("Failed to create per-image render-finished semaphore");
        }
    }

    if (vkCreateFence(device_, &fi, nullptr, &inFlightFence_) != VK_SUCCESS)
    {
        throw std::runtime_error("Failed to create in-flight fence");
    }
}

void Renderer::cleanupSwapchain()
{
    for (auto iv : swapchainImageViews_) vkDestroyImageView(device_, iv, nullptr);
    swapchainImageViews_.clear();

    if (swapchain_ != VK_NULL_HANDLE)
    {
        vkDestroySwapchainKHR(device_, swapchain_, nullptr);
        swapchain_ = VK_NULL_HANDLE;
    }

    for (auto s : renderFinishedSemaphores_) vkDestroySemaphore(device_, s, nullptr);
    renderFinishedSemaphores_.clear();
}

void Renderer::recreateSwapchain()
{
    int w = 0, h = 0;
    glfwGetFramebufferSize(window_, &w, &h);
    while (w == 0 || h == 0)
    {
        glfwGetFramebufferSize(window_, &w, &h);
        glfwWaitEvents();
    }

    vkDeviceWaitIdle(device_);

    cleanupSwapchain();

    createSwapchain();
    createImageViews();
    createFramebuffers();
    createCommandBuffers();
    createSyncObjects();
}

void Renderer::buildTextVertices(std::vector<float>& vertices)
{
    vertices.clear();

    float cursorX = 0.0f;
    float cursorY = fontAscentPx_;

    for (size_t i = 0; i < text_.size(); ++i)
    {
        unsigned char c = static_cast<unsigned char>(text_[i]);
        if (c < kFirstChar || c >= kFirstChar + kCharCount) continue;
        if (static_cast<int>(vertices.size() / 4) >= kMaxVerts) break;

        stbtt_aligned_quad q{};
        float x = cursorX;
        float y = cursorY;
        stbtt_GetBakedQuad(bakedChars_, kAtlasWidth, kAtlasHeight, c - kFirstChar, &x, &y, &q, 0);
        cursorX = x;

        vertices.push_back(q.x0); vertices.push_back(q.y0); vertices.push_back(q.s0); vertices.push_back(q.t0);
        vertices.push_back(q.x1); vertices.push_back(q.y0); vertices.push_back(q.s1); vertices.push_back(q.t0);
        vertices.push_back(q.x0); vertices.push_back(q.y1); vertices.push_back(q.s0); vertices.push_back(q.t1);

        vertices.push_back(q.x1); vertices.push_back(q.y0); vertices.push_back(q.s1); vertices.push_back(q.t0);
        vertices.push_back(q.x1); vertices.push_back(q.y1); vertices.push_back(q.s1); vertices.push_back(q.t1);
        vertices.push_back(q.x0); vertices.push_back(q.y1); vertices.push_back(q.s0); vertices.push_back(q.t1);
    }

    float textWidth = cursorX;
    float textHeight = kFontSize;
    float offsetX = (static_cast<float>(swapchainExtent_.width)  - textWidth)  * 0.5f;
    float offsetY = (static_cast<float>(swapchainExtent_.height) - textHeight) * 0.5f;

    for (size_t i = 0; i < vertices.size(); i += 4)
    {
        vertices[i + 0] += offsetX;
        vertices[i + 1] += offsetY;
    }
}

void Renderer::recordCommandBuffer(VkCommandBuffer cmd, uint32_t imageIndex)
{
    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;

    if (vkBeginCommandBuffer(cmd, &bi) != VK_SUCCESS)
    {
        throw std::runtime_error("Failed to begin command buffer");
    }

    VkClearValue clear{};
    clear.color = { { 0.05f, 0.05f, 0.08f, 1.0f } };

    VkRenderPassBeginInfo ri{};
    ri.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    ri.renderPass = renderPass_;
    ri.framebuffer = swapchainFramebuffers_[imageIndex];
    ri.renderArea.offset = { 0, 0 };
    ri.renderArea.extent = swapchainExtent_;
    ri.clearValueCount = 1;
    ri.pClearValues = &clear;

    vkCmdBeginRenderPass(cmd, &ri, VK_SUBPASS_CONTENTS_INLINE);

    VkViewport vp{};
    vp.x = 0.0f;
    vp.y = 0.0f;
    vp.width = static_cast<float>(swapchainExtent_.width);
    vp.height = static_cast<float>(swapchainExtent_.height);
    vp.minDepth = 0.0f;
    vp.maxDepth = 1.0f;
    vkCmdSetViewport(cmd, 0, 1, &vp);

    VkRect2D scissor{};
    scissor.offset = { 0, 0 };
    scissor.extent = swapchainExtent_;
    vkCmdSetScissor(cmd, 0, 1, &scissor);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);

    std::vector<float> vertices;
    buildTextVertices(vertices);

    if (!vertices.empty())
    {
        void* mapped = nullptr;
        vkMapMemory(device_, vertexMemory_, 0, vertexBufferSize_, 0, &mapped);
        std::memcpy(mapped, vertices.data(), vertices.size() * sizeof(float));
        vkUnmapMemory(device_, vertexMemory_);

        float proj[16] = {
             2.0f / static_cast<float>(swapchainExtent_.width), 0.0f, 0.0f, 0.0f,
             0.0f,  2.0f / static_cast<float>(swapchainExtent_.height), 0.0f, 0.0f,
             0.0f, 0.0f, 1.0f, 0.0f,
            -1.0f, -1.0f, 0.0f, 1.0f,
        };
        vkCmdPushConstants(cmd, pipelineLayout_, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(proj), proj);

        VkBuffer vbs[] = { vertexBuffer_ };
        VkDeviceSize offs[] = { 0 };
        vkCmdBindVertexBuffers(cmd, 0, 1, vbs, offs);

        VkDescriptorSet sets[] = { descriptorSet_ };
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                pipelineLayout_, 0, 1, sets, 0, nullptr);

        vkCmdDraw(cmd, static_cast<uint32_t>(vertices.size() / 4), 1, 0, 0);
    }

    vkCmdEndRenderPass(cmd);

    if (vkEndCommandBuffer(cmd) != VK_SUCCESS)
    {
        throw std::runtime_error("Failed to end command buffer");
    }
}

void Renderer::drawFrame()
{
    vkWaitForFences(device_, 1, &inFlightFence_, VK_TRUE, UINT64_MAX);

    uint32_t imageIndex = 0;
    VkResult r = vkAcquireNextImageKHR(device_, swapchain_, UINT64_MAX,
                                       imageAvailableSemaphore_, VK_NULL_HANDLE, &imageIndex);
    if (r == VK_ERROR_OUT_OF_DATE_KHR)
    {
        recreateSwapchain();
        framebufferResized_ = false;
        return;
    }
    if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR)
    {
        throw std::runtime_error("Failed to acquire swapchain image");
    }

    if (framebufferResized_)
    {
        recreateSwapchain();
        framebufferResized_ = false;
        return;
    }

    vkResetFences(device_, 1, &inFlightFence_);

    vkResetCommandBuffer(commandBuffers_[imageIndex], 0);
    recordCommandBuffer(commandBuffers_[imageIndex], imageIndex);

    VkSemaphore waitSem = imageAvailableSemaphore_;
    VkSemaphore signalSem = renderFinishedSemaphores_[imageIndex];

    VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.waitSemaphoreCount = 1;
    si.pWaitSemaphores = &waitSem;
    si.pWaitDstStageMask = &waitStage;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &commandBuffers_[imageIndex];
    si.signalSemaphoreCount = 1;
    si.pSignalSemaphores = &signalSem;

    if (vkQueueSubmit(graphicsQueue_, 1, &si, inFlightFence_) != VK_SUCCESS)
    {
        throw std::runtime_error("Failed to submit draw command buffer");
    }

    VkPresentInfoKHR pi{};
    pi.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &signalSem;
    pi.swapchainCount = 1;
    pi.pSwapchains = &swapchain_;
    pi.pImageIndices = &imageIndex;

    VkResult pr = vkQueuePresentKHR(presentQueue_, &pi);
    if (pr == VK_ERROR_OUT_OF_DATE_KHR || pr == VK_SUBOPTIMAL_KHR || framebufferResized_)
    {
        recreateSwapchain();
        framebufferResized_ = false;
    }
    else if (pr != VK_SUCCESS)
    {
        throw std::runtime_error("Failed to present swapchain image");
    }
}

std::vector<char> Renderer::readFile(const std::string& path) const
{
    return readFileImpl(exeDir() / path);
}
