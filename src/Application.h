#pragma once

#include <vulkan/vulkan.h>

#include <GLFW/glfw3.h>

#include <stdexcept>
#include <string>
#include <vector>

#include "TextRenderer.h"

class Application
{
public:
    Application();
    ~Application();

    Application(const Application&) = delete;
    Application& operator=(const Application&) = delete;

    void run();

private:
    struct FontOption
    {
        std::string path;
        std::string label;
    };

    struct QueueFamilyIndices
    {
        uint32_t graphics = UINT32_MAX;
        uint32_t present = UINT32_MAX;
        bool hasGraphics() const { return graphics != UINT32_MAX; }
        bool hasPresent() const { return present != UINT32_MAX; }
        bool isComplete() const { return hasGraphics() && hasPresent(); }
    };

    struct SwapchainSupportDetails
    {
        VkSurfaceCapabilitiesKHR capabilities{};
        std::vector<VkSurfaceFormatKHR> formats;
        std::vector<VkPresentModeKHR> presentModes;
    };

    void initWindow();
    void initVulkan();
    void mainLoop();
    void cleanup();

    void createInstance();
    void createSurface();
    void pickPhysicalDevice();
    void createLogicalDevice();
    void createSwapchain();
    void createImageViews();
    void createRenderPass();
    void createFramebuffers();
    void createCommandPool();
    void createCommandBuffers();
    void createSyncObjects();

    void drawFrame();
    void recreateSwapchain();
    void cleanupSwapchain();

    void renderUi(VkCommandBuffer cmd, uint32_t imageWidth, uint32_t imageHeight);
    void selectFont(size_t index);

    bool isPhysicalDeviceSuitable(VkPhysicalDevice device);
    QueueFamilyIndices findQueueFamilies(VkPhysicalDevice device);
    SwapchainSupportDetails querySwapchainSupport(VkPhysicalDevice device);
    VkSurfaceFormatKHR chooseSwapSurfaceFormat(const std::vector<VkSurfaceFormatKHR>& formats);
    VkPresentModeKHR chooseSwapPresentMode(const std::vector<VkPresentModeKHR>& modes);
    VkExtent2D chooseSwapExtent(const VkSurfaceCapabilitiesKHR& capabilities);

    static void framebufferResizeCallback(GLFWwindow* window, int width, int height);
    static void mouseButtonCallback(GLFWwindow* window, int button, int action, int mods);
    static void cursorPosCallback(GLFWwindow* window, double xpos, double ypos);
    static std::vector<const char*> getRequiredExtensions();

    GLFWwindow* window_ = nullptr;
    VkInstance instance_ = VK_NULL_HANDLE;
    VkSurfaceKHR surface_ = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    VkQueue graphicsQueue_ = VK_NULL_HANDLE;
    VkQueue presentQueue_ = VK_NULL_HANDLE;
    QueueFamilyIndices queueIndices_{};

    VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
    std::vector<VkImage> swapchainImages_;
    VkFormat swapchainImageFormat_ = VK_FORMAT_UNDEFINED;
    VkExtent2D swapchainExtent_{};
    std::vector<VkImageView> swapchainImageViews_;
    VkRenderPass renderPass_ = VK_NULL_HANDLE;
    std::vector<VkFramebuffer> swapchainFramebuffers_;
    VkCommandPool commandPool_ = VK_NULL_HANDLE;
    std::vector<VkCommandBuffer> commandBuffers_;

    static constexpr uint32_t kMaxFramesInFlight = 2;
    std::vector<VkSemaphore> imageAvailableSemaphores_;
    std::vector<VkSemaphore> renderFinishedSemaphores_;
    std::vector<VkFence> inFlightFences_;
    std::vector<VkFence> imageInFlight_;
    uint32_t currentFrame_ = 0;
    bool framebufferResized_ = false;

    TextRenderer textRenderer_;
    TextRenderer uiRenderer_;

    static constexpr uint32_t kAppbarHeight = 50;
    static constexpr uint32_t kDropdownWidth = 280;
    static constexpr uint32_t kDropdownHeight = 32;
    static constexpr uint32_t kItemHeight = 28;
    static constexpr uint32_t kUiFontPixelSize = 18;

    std::vector<FontOption> fonts_;
    size_t currentFontIndex_ = 0;
    bool dropdownOpen_ = false;
    double mouseX_ = 0.0;
    double mouseY_ = 0.0;
};
