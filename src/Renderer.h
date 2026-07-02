#pragma once

#include <vulkan/vulkan.h>

#include <GLFW/glfw3.h>

#include <stb_truetype.h>

#include <optional>
#include <string>
#include <vector>

struct QueueFamilyIndices
{
    std::optional<uint32_t> graphics;
    std::optional<uint32_t> present;

    bool isComplete() const { return graphics.has_value() && present.has_value(); }
};

struct SwapchainSupportDetails
{
    VkSurfaceCapabilitiesKHR        capabilities{};
    std::vector<VkSurfaceFormatKHR> formats;
    std::vector<VkPresentModeKHR>   presentModes;
};

class Renderer
{
public:
    Renderer(VkInstance instance, VkSurfaceKHR surface, GLFWwindow* window);
    ~Renderer();

    Renderer(const Renderer&)            = delete;
    Renderer& operator=(const Renderer&) = delete;

    void drawFrame();
    void setFramebufferResized() { framebufferResized_ = true; }

private:
    void pickPhysicalDevice();
    void createLogicalDevice();
    void createSurface();  // not used; surface is owned externally and passed in

    void createSwapchain();
    void createImageViews();
    void createRenderPass();
    void createDescriptorSetLayout();
    void createPipeline();
    void createFramebuffers();
    void createCommandPool();
    void createAtlasResources();
    void createVertexBuffer();
    void createDescriptorResources();
    void createCommandBuffers();
    void createSyncObjects();

    void recordCommandBuffer(VkCommandBuffer cmd, uint32_t imageIndex);
    void buildTextVertices(std::vector<float>& vertices);
    void recreateSwapchain();
    void cleanupSwapchain();

    QueueFamilyIndices   findQueueFamilies(VkPhysicalDevice device) const;
    SwapchainSupportDetails querySwapchainSupport(VkPhysicalDevice device) const;
    bool                 isDeviceSuitable(VkPhysicalDevice device) const;

    VkShaderModule       createShaderModule(const std::vector<char>& code) const;
    uint32_t             findMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags properties) const;
    void                 createBuffer(VkDeviceSize size, VkBufferUsageFlags usage,
                                      VkMemoryPropertyFlags properties,
                                      VkBuffer& buffer, VkDeviceMemory& memory) const;
    void                 copyBuffer(VkBuffer src, VkBuffer dst, VkDeviceSize size) const;
    void                 transitionImageLayout(VkImage image, VkFormat format,
                                              VkImageLayout oldLayout, VkImageLayout newLayout) const;
    void                 copyBufferToImage(VkBuffer buffer, VkImage image, uint32_t width, uint32_t height) const;
    std::vector<char>    readFile(const std::string& path) const;

    static constexpr int   kAtlasWidth  = 512;
    static constexpr int   kAtlasHeight = 512;
    static constexpr int   kFirstChar   = 32;
    static constexpr int   kCharCount   = 96;
    static constexpr float kFontSize    = 32.0f;
    static constexpr int   kMaxChars    = 256;
    static constexpr int   kMaxVerts    = kMaxChars * 6;

    VkInstance    instance_   = VK_NULL_HANDLE;
    VkSurfaceKHR  surface_    = VK_NULL_HANDLE;
    GLFWwindow*   window_     = nullptr;

    VkPhysicalDevice physicalDevice_ = VK_NULL_HANDLE;
    VkDevice         device_         = VK_NULL_HANDLE;

    QueueFamilyIndices queueFamilies_{};
    VkQueue           graphicsQueue_ = VK_NULL_HANDLE;
    VkQueue           presentQueue_  = VK_NULL_HANDLE;

    VkSwapchainKHR           swapchain_ = VK_NULL_HANDLE;
    std::vector<VkImage>     swapchainImages_;
    VkFormat                 swapchainImageFormat_{};
    VkExtent2D               swapchainExtent_{};
    std::vector<VkImageView> swapchainImageViews_;
    std::vector<VkFramebuffer> swapchainFramebuffers_;

    VkRenderPass        renderPass_        = VK_NULL_HANDLE;
    VkPipelineLayout    pipelineLayout_    = VK_NULL_HANDLE;
    VkPipeline          pipeline_          = VK_NULL_HANDLE;

    VkDescriptorSetLayout descriptorSetLayout_ = VK_NULL_HANDLE;
    VkDescriptorPool      descriptorPool_      = VK_NULL_HANDLE;
    VkDescriptorSet       descriptorSet_       = VK_NULL_HANDLE;

    VkCommandPool                commandPool_ = VK_NULL_HANDLE;
    std::vector<VkCommandBuffer> commandBuffers_;

    VkImage        atlasImage_       = VK_NULL_HANDLE;
    VkDeviceMemory atlasMemory_      = VK_NULL_HANDLE;
    VkImageView    atlasImageView_   = VK_NULL_HANDLE;
    VkSampler      atlasSampler_     = VK_NULL_HANDLE;
    stbtt_bakedchar bakedChars_[kCharCount]{};

    VkBuffer       vertexBuffer_     = VK_NULL_HANDLE;
    VkDeviceMemory vertexMemory_     = VK_NULL_HANDLE;
    VkDeviceSize   vertexBufferSize_ = sizeof(float) * 4 * kMaxVerts;

    VkSemaphore imageAvailableSemaphore_ = VK_NULL_HANDLE;
    std::vector<VkSemaphore> renderFinishedSemaphores_;
    VkFence     inFlightFence_           = VK_NULL_HANDLE;

    bool framebufferResized_ = false;
    float fontAscentPx_ = 0.0f;

    std::string text_ = "A quick brown fox jumped over a lazy dog!";
};
