#include "Renderer.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <limits>
#include <set>
#include <stdexcept>
#include <vector>

#define STB_TRUETYPE_IMPLEMENTATION
#include <stb_truetype.h>

namespace
{
const std::vector<FontOption> kFontCandidates = {
    {"Segoe UI",      "C:/Windows/Fonts/segoeui.ttf"},
    {"Arial",         "C:/Windows/Fonts/arial.ttf"},
    {"Calibri",       "C:/Windows/Fonts/calibri.ttf"},
    {"Consolas",      "C:/Windows/Fonts/consola.ttf"},
    {"Courier New",   "C:/Windows/Fonts/cour.ttf"},
    {"Georgia",       "C:/Windows/Fonts/georgia.ttf"},
    {"Tahoma",        "C:/Windows/Fonts/tahoma.ttf"},
    {"Verdana",       "C:/Windows/Fonts/verdana.ttf"},
};

const std::array<float, 4> kColorAppbarBg       = { 0.13f, 0.13f, 0.18f, 1.0f };
const std::array<float, 4> kColorDropdownBg     = { 0.20f, 0.20f, 0.28f, 1.0f };
const std::array<float, 4> kColorDropdownItem   = { 0.18f, 0.18f, 0.26f, 1.0f };
const std::array<float, 4> kColorDropdownActive = { 0.30f, 0.45f, 0.85f, 1.0f };
const std::array<float, 4> kColorText           = { 1.0f,  1.0f,  1.0f,  1.0f };

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
    selectAvailableFonts();
    if (fonts_.empty())
    {
        throw std::runtime_error("No TTF fonts found in C:/Windows/Fonts");
    }

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

    destroyAtlasResources();

    vkDestroyDescriptorSetLayout(device_, descriptorSetLayout_, nullptr);
    vkDestroyDescriptorPool(device_, descriptorPool_, nullptr);

    vkDestroyPipeline(device_, pipeline_, nullptr);
    vkDestroyPipelineLayout(device_, pipelineLayout_, nullptr);
    vkDestroyRenderPass(device_, renderPass_, nullptr);

    for (auto fb : swapchainFramebuffers_) vkDestroyFramebuffer(device_, fb, nullptr);
    cleanupSwapchain();

    for (auto s : renderFinishedSemaphores_) vkDestroySemaphore(device_, s, nullptr);
    renderFinishedSemaphores_.clear();

    vkDestroySemaphore(device_, imageAvailableSemaphore_, nullptr);
    vkDestroyFence(device_, inFlightFence_, nullptr);

    vkDestroyCommandPool(device_, commandPool_, nullptr);
    vkDestroyDevice(device_, nullptr);
}

void Renderer::selectAvailableFonts()
{
    for (const auto& f : kFontCandidates)
    {
        std::ifstream test(f.path, std::ios::binary | std::ios::ate);
        if (test.is_open())
        {
            fonts_.push_back(f);
        }
    }
    if (!fonts_.empty())
    {
        currentFontIndex_ = 0;
    }
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
    binding.stride = sizeof(float) * 8;
    binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    std::array<VkVertexInputAttributeDescription, 3> attrs{};
    attrs[0].location = 0;
    attrs[0].binding = 0;
    attrs[0].format = VK_FORMAT_R32G32_SFLOAT;
    attrs[0].offset = 0;
    attrs[1].location = 1;
    attrs[1].binding = 0;
    attrs[1].format = VK_FORMAT_R32G32_SFLOAT;
    attrs[1].offset = sizeof(float) * 2;
    attrs[2].location = 2;
    attrs[2].binding = 0;
    attrs[2].format = VK_FORMAT_R32G32B32A32_SFLOAT;
    attrs[2].offset = sizeof(float) * 4;

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
    pcr.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    pcr.offset = 0;
    pcr.size = sizeof(float) * 17;

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

namespace
{
std::vector<unsigned char> readFontFile(const std::string& path)
{
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f.is_open())
    {
        throw std::runtime_error("Failed to open font file: " + path);
    }
    auto sz = static_cast<size_t>(f.tellg());
    f.seekg(0);
    std::vector<unsigned char> data(sz);
    f.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(sz));
    return data;
}

void bakeFont(const std::vector<unsigned char>& fontBuffer,
              std::vector<unsigned char>& bitmap,
              stbtt_bakedchar* outChars,
              float& outAscentPx)
{
    bitmap.assign(static_cast<size_t>(Renderer::kAtlasWidth) * Renderer::kAtlasHeight, 0);
    int rc = stbtt_BakeFontBitmap(
        fontBuffer.data(), 0, Renderer::kFontSize,
        bitmap.data(), Renderer::kAtlasWidth, Renderer::kAtlasHeight,
        Renderer::kFirstChar, Renderer::kCharCount, outChars);
    if (rc <= 0)
    {
        throw std::runtime_error("Failed to bake font bitmap: atlas too small");
    }

    stbtt_fontinfo info{};
    if (!stbtt_InitFont(&info, fontBuffer.data(), 0))
    {
        throw std::runtime_error("stbtt_InitFont failed");
    }
    int ascent = 0;
    stbtt_GetFontVMetrics(&info, &ascent, nullptr, nullptr);
    outAscentPx = ascent * stbtt_ScaleForPixelHeight(&info, Renderer::kFontSize);
}
}  // namespace

void Renderer::createAtlasResources()
{
    std::vector<unsigned char> fontBytes = readFontFile(fonts_[currentFontIndex_].path);
    std::vector<unsigned char> bitmap;
    bakeFont(fontBytes, bitmap, bakedChars_, fontAscentPx_);

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

void Renderer::destroyAtlasResources()
{
    if (atlasSampler_ != VK_NULL_HANDLE)
    {
        vkDestroySampler(device_, atlasSampler_, nullptr);
        atlasSampler_ = VK_NULL_HANDLE;
    }
    if (atlasImageView_ != VK_NULL_HANDLE)
    {
        vkDestroyImageView(device_, atlasImageView_, nullptr);
        atlasImageView_ = VK_NULL_HANDLE;
    }
    if (atlasImage_ != VK_NULL_HANDLE)
    {
        vkDestroyImage(device_, atlasImage_, nullptr);
        atlasImage_ = VK_NULL_HANDLE;
    }
    if (atlasMemory_ != VK_NULL_HANDLE)
    {
        vkFreeMemory(device_, atlasMemory_, nullptr);
        atlasMemory_ = VK_NULL_HANDLE;
    }
}

void Renderer::rebuildAtlasForFont(size_t fontIndex)
{
    if (fontIndex >= fonts_.size())
    {
        throw std::out_of_range("Font index out of range");
    }

    vkDeviceWaitIdle(device_);

    currentFontIndex_ = fontIndex;
    fontBuffer_ = readFontFile(fonts_[fontIndex].path);

    std::vector<unsigned char> bitmap;
    bakeFont(fontBuffer_, bitmap, bakedChars_, fontAscentPx_);

    destroyAtlasResources();

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

void Renderer::rebuildSwapchain()
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

void Renderer::appendText(std::vector<float>& vertices, const std::string& text,
                          float x, float y, std::array<float, 4> color) const
{
    if (text.empty()) return;

    float cursorX = x;
    float cursorY = y + fontAscentPx_;

    const size_t floatsPerVertex = 8;
    for (size_t i = 0; i < text.size(); ++i)
    {
        unsigned char c = static_cast<unsigned char>(text[i]);
        if (c < kFirstChar || c >= kFirstChar + kCharCount) continue;
        if (static_cast<int>(vertices.size() / floatsPerVertex) >= kMaxVerts) break;

        stbtt_aligned_quad q{};
        float qx = cursorX;
        float qy = cursorY;
        stbtt_GetBakedQuad(bakedChars_, kAtlasWidth, kAtlasHeight, c - kFirstChar, &qx, &qy, &q, 0);
        cursorX = qx;

        const float s0 = q.s0, t0 = q.t0, s1 = q.s1, t1 = q.t1;
        const float x0 = q.x0, y0 = q.y0, x1 = q.x1, y1 = q.y1;

        auto push = [&](float px, float py, float u, float v) {
            vertices.push_back(px);
            vertices.push_back(py);
            vertices.push_back(u);
            vertices.push_back(v);
            vertices.push_back(color[0]);
            vertices.push_back(color[1]);
            vertices.push_back(color[2]);
            vertices.push_back(color[3]);
        };
        push(x0, y0, s0, t0);
        push(x1, y0, s1, t0);
        push(x0, y1, s0, t1);
        push(x1, y0, s1, t0);
        push(x1, y1, s1, t1);
        push(x0, y1, s0, t1);
    }
}

void Renderer::appendSolidRect(std::vector<float>& vertices,
                               float x0, float y0, float x1, float y1,
                               std::array<float, 4> color) const
{
    if (static_cast<int>(vertices.size() / 8) + 6 > kMaxVerts) return;

    auto push = [&](float px, float py) {
        vertices.push_back(px);
        vertices.push_back(py);
        vertices.push_back(0.0f);
        vertices.push_back(0.0f);
        vertices.push_back(color[0]);
        vertices.push_back(color[1]);
        vertices.push_back(color[2]);
        vertices.push_back(color[3]);
    };
    push(x0, y0);
    push(x1, y0);
    push(x0, y1);
    push(x1, y0);
    push(x1, y1);
    push(x0, y1);
}

void Renderer::measureText(const std::string& text, float& outWidth, float& outHeight) const
{
    outWidth = 0.0f;
    outHeight = static_cast<float>(kFontSize);
    if (text.empty()) return;

    float cursorX = 0.0f;
    for (size_t i = 0; i < text.size(); ++i)
    {
        unsigned char c = static_cast<unsigned char>(text[i]);
        if (c < kFirstChar || c >= kFirstChar + kCharCount) continue;

        stbtt_aligned_quad q{};
        float qx = cursorX;
        float qy = 0.0f;
        stbtt_GetBakedQuad(bakedChars_, kAtlasWidth, kAtlasHeight, c - kFirstChar, &qx, &qy, &q, 0);
        cursorX = qx;
    }
    outWidth = cursorX;
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

    const float W = static_cast<float>(swapchainExtent_.width);
    const float H = static_cast<float>(swapchainExtent_.height);

    const float dropdownX = (W - static_cast<float>(kDropdownWidth)) * 0.5f;
    const float dropdownY = (static_cast<float>(kAppbarHeight) - static_cast<float>(kDropdownHeight)) * 0.5f;
    const float dropdownX1 = dropdownX + static_cast<float>(kDropdownWidth);
    const float dropdownY1 = dropdownY + static_cast<float>(kDropdownHeight);

    std::vector<float> vertices;
    vertices.reserve(static_cast<size_t>(kMaxVerts) * 8);

    appendSolidRect(vertices, 0.0f, 0.0f, W,
                    static_cast<float>(kAppbarHeight), kColorAppbarBg);
    appendSolidRect(vertices, dropdownX, dropdownY, dropdownX1, dropdownY1, kColorDropdownBg);

    if (dropdownOpen_)
    {
        const float itemY0 = static_cast<float>(kAppbarHeight);
        for (size_t i = 0; i < fonts_.size(); ++i)
        {
            const float y0 = itemY0 + static_cast<float>(i) * static_cast<float>(kDropdownHeight);
            const float y1 = y0 + static_cast<float>(kDropdownHeight);
            const std::array<float, 4> color = (i == currentFontIndex_)
                ? kColorDropdownActive
                : kColorDropdownItem;
            appendSolidRect(vertices, dropdownX, y0, dropdownX1, y1, color);
        }
    }

    const size_t uiVertexCount = vertices.size() / 8;

    if (dropdownOpen_)
    {
        const float itemY0 = static_cast<float>(kAppbarHeight);
        for (size_t i = 0; i < fonts_.size(); ++i)
        {
            const float y0 = itemY0 + static_cast<float>(i) * static_cast<float>(kDropdownHeight);
            const float yCenter = y0 + (static_cast<float>(kDropdownHeight) - static_cast<float>(kFontSize)) * 0.5f;
            appendText(vertices, fonts_[i].name, dropdownX + static_cast<float>(kDropdownPadX), yCenter, kColorText);
        }
    }

    {
        const std::string& label = fonts_[currentFontIndex_].name + "  v";
        float textW = 0.0f, textH = 0.0f;
        measureText(label, textW, textH);
        const float textX = dropdownX + (static_cast<float>(kDropdownWidth) - textW) * 0.5f;
        const float textY = dropdownY + (static_cast<float>(kDropdownHeight) - textH) * 0.5f;
        appendText(vertices, label, textX, textY, kColorText);
    }

    {
        float textW = 0.0f, textH = 0.0f;
        measureText(text_, textW, textH);
        const float contentTop = static_cast<float>(kAppbarHeight);
        const float contentH  = H - contentTop;
        const float offsetX = (W - textW) * 0.5f;
        const float offsetY = contentTop + (contentH - textH) * 0.5f;
        appendText(vertices, text_, offsetX, offsetY, kColorText);
    }

    if (vertices.empty())
    {
        vkCmdEndRenderPass(cmd);
        if (vkEndCommandBuffer(cmd) != VK_SUCCESS)
        {
            throw std::runtime_error("Failed to end command buffer");
        }
        return;
    }

    void* mapped = nullptr;
    vkMapMemory(device_, vertexMemory_, 0, vertexBufferSize_, 0, &mapped);
    std::memcpy(mapped, vertices.data(), vertices.size() * sizeof(float));
    vkUnmapMemory(device_, vertexMemory_);

    float pc[17] = {
         2.0f / W, 0.0f, 0.0f, 0.0f,
         0.0f, 2.0f / H, 0.0f, 0.0f,
         0.0f, 0.0f, 1.0f, 0.0f,
        -1.0f, -1.0f, 0.0f, 1.0f,
         0.0f,
    };

    VkBuffer vbs[] = { vertexBuffer_ };
    VkDeviceSize offs[] = { 0 };
    vkCmdBindVertexBuffers(cmd, 0, 1, vbs, offs);

    VkDescriptorSet sets[] = { descriptorSet_ };
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            pipelineLayout_, 0, 1, sets, 0, nullptr);

    pc[16] = 0.0f;
    vkCmdPushConstants(cmd, pipelineLayout_,
                       VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                       0, sizeof(pc), pc);
    if (uiVertexCount > 0)
    {
        vkCmdDraw(cmd, static_cast<uint32_t>(uiVertexCount), 1, 0, 0);
    }

    const size_t textVertexCount = (vertices.size() / 8) - uiVertexCount;
    if (textVertexCount > 0)
    {
        pc[16] = 1.0f;
        vkCmdPushConstants(cmd, pipelineLayout_,
                           VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                           0, sizeof(pc), pc);
        vkCmdDraw(cmd, static_cast<uint32_t>(textVertexCount), 1,
                  static_cast<uint32_t>(uiVertexCount), 0);
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
        rebuildSwapchain();
        framebufferResized_ = false;
        return;
    }
    if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR)
    {
        throw std::runtime_error("Failed to acquire swapchain image");
    }

    if (framebufferResized_)
    {
        rebuildSwapchain();
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
        rebuildSwapchain();
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

void Renderer::onMouseButton(int button, int action, double fbX, double fbY)
{
    if (button != GLFW_MOUSE_BUTTON_LEFT || action != GLFW_PRESS) return;

    const float W = static_cast<float>(swapchainExtent_.width);
    const float H = static_cast<float>(swapchainExtent_.height);
    const float x = static_cast<float>(fbX);
    const float y = static_cast<float>(fbY);

    const float dropdownX0 = (W - static_cast<float>(kDropdownWidth)) * 0.5f;
    const float dropdownX1 = dropdownX0 + static_cast<float>(kDropdownWidth);
    const float dropdownY0 = (static_cast<float>(kAppbarHeight) - static_cast<float>(kDropdownHeight)) * 0.5f;
    const float dropdownY1 = dropdownY0 + static_cast<float>(kDropdownHeight);

    const bool inButton = (x >= dropdownX0 && x < dropdownX1 &&
                           y >= dropdownY0 && y < dropdownY1);

    if (dropdownOpen_)
    {
        const float itemY0 = static_cast<float>(kAppbarHeight);
        const float itemY1 = itemY0 + static_cast<float>(fonts_.size()) * static_cast<float>(kDropdownHeight);
        if (x >= dropdownX0 && x < dropdownX1 && y >= itemY0 && y < itemY1)
        {
            const size_t idx = static_cast<size_t>((y - itemY0) / static_cast<float>(kDropdownHeight));
            if (idx < fonts_.size() && idx != currentFontIndex_)
            {
                dropdownOpen_ = false;
                rebuildAtlasForFont(idx);
            }
            return;
        }
    }

    if (inButton)
    {
        dropdownOpen_ = !dropdownOpen_;
        return;
    }

    if (dropdownOpen_)
    {
        dropdownOpen_ = false;
    }
}

