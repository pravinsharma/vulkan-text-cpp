#pragma once

#include <vulkan/vulkan.h>

#include <GLFW/glfw3.h>

#include <stdexcept>
#include <vector>

class Renderer;

class Application
{
public:
    Application();
    ~Application();

    Application(const Application&) = delete;
    Application& operator=(const Application&) = delete;

    void run();

private:
    void initWindow();
    void initVulkan();
    void mainLoop();
    void cleanup();

    void createInstance();
    void createSurface();

    static void framebufferResizeCallback(GLFWwindow* window, int width, int height);
    static std::vector<const char*> getRequiredExtensions();

    GLFWwindow* window_ = nullptr;
    VkInstance  instance_ = VK_NULL_HANDLE;
    VkSurfaceKHR surface_ = VK_NULL_HANDLE;
    Renderer*   renderer_ = nullptr;
};
