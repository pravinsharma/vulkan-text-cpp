#include "Application.h"

#include <cstring>
#include <iostream>

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

    window_ = glfwCreateWindow(800, 600, "Vulkan GLFW App", nullptr, nullptr);
    if (!window_)
    {
        glfwTerminate();
        throw std::runtime_error("Failed to create GLFW window");
    }

    glfwSetWindowUserPointer(window_, this);
    glfwSetFramebufferSizeCallback(window_, framebufferResizeCallback);
}

void Application::initVulkan()
{
    createInstance();
}

void Application::createInstance()
{
    VkApplicationInfo appInfo{};
    appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    appInfo.pApplicationName = "Vulkan GLFW App";
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

    const char* validationLayer = "VK_LAYER_KHRONOS_validation";
    bool layerFound = false;
    for (const auto& layer : availableLayers)
    {
        if (std::strcmp(layer.layerName, validationLayer) == 0)
        {
            layerFound = true;
            break;
        }
    }

    if (layerFound)
    {
        createInfo.enabledLayerCount = 1;
        createInfo.ppEnabledLayerNames = &validationLayer;
    }
#endif

    if (vkCreateInstance(&createInfo, nullptr, &instance_) != VK_SUCCESS)
    {
        throw std::runtime_error("Failed to create Vulkan instance");
    }
}

void Application::mainLoop()
{
    while (!glfwWindowShouldClose(window_))
    {
        glfwPollEvents();
    }
}

void Application::cleanup()
{
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

void Application::framebufferResizeCallback(GLFWwindow* window, int width, int height)
{
    auto* app = static_cast<Application*>(glfwGetWindowUserPointer(window));
    if (app)
    {
        app->framebufferResized_ = true;
    }
}

std::vector<const char*> Application::getRequiredExtensions()
{
    uint32_t glfwExtensionCount = 0;
    const char** glfwExtensions = glfwGetRequiredInstanceExtensions(&glfwExtensionCount);

    return { glfwExtensions, glfwExtensions + glfwExtensionCount };
}
