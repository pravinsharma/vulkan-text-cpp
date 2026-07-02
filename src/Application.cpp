#include "Application.h"

#include "Renderer.h"

#include <cstring>
#include <iostream>
#include <stdexcept>
#include <vector>

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

    window_ = glfwCreateWindow(1024, 768, "Vulkan GLFW App", nullptr, nullptr);
    if (!window_)
    {
        glfwTerminate();
        throw std::runtime_error("Failed to create GLFW window");
    }

    glfwSetWindowUserPointer(window_, this);
    glfwSetFramebufferSizeCallback(window_, framebufferResizeCallback);
    glfwSetMouseButtonCallback(window_, mouseButtonCallback);
}

void Application::initVulkan()
{
    createInstance();
    createSurface();
    renderer_ = new Renderer(instance_, surface_, window_);
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

void Application::createSurface()
{
    if (glfwCreateWindowSurface(instance_, window_, nullptr, &surface_) != VK_SUCCESS)
    {
        throw std::runtime_error("Failed to create window surface");
    }
}

void Application::mainLoop()
{
    while (!glfwWindowShouldClose(window_))
    {
        glfwPollEvents();
        renderer_->drawFrame();
    }
}

void Application::cleanup()
{
    delete renderer_;
    renderer_ = nullptr;

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

void Application::framebufferResizeCallback(GLFWwindow* window, int width, int height)
{
    (void)width;
    (void)height;
    auto* app = static_cast<Application*>(glfwGetWindowUserPointer(window));
    if (app && app->renderer_)
    {
        app->renderer_->setFramebufferResized();
    }
}

void Application::mouseButtonCallback(GLFWwindow* window, int button, int action, int mods)
{
    (void)mods;
    auto* app = static_cast<Application*>(glfwGetWindowUserPointer(window));
    if (!app || !app->renderer_) return;

    double cx = 0.0, cy = 0.0;
    glfwGetCursorPos(window, &cx, &cy);

    int winW = 0, winH = 0;
    glfwGetWindowSize(window, &winW, &winH);
    int fbW = 0, fbH = 0;
    glfwGetFramebufferSize(window, &fbW, &fbH);

    double sx = (winW > 0) ? static_cast<double>(fbW) / static_cast<double>(winW) : 1.0;
    double sy = (winH > 0) ? static_cast<double>(fbH) / static_cast<double>(winH) : 1.0;

    app->renderer_->onMouseButton(button, action, cx * sx, cy * sy);
}

std::vector<const char*> Application::getRequiredExtensions()
{
    uint32_t glfwExtensionCount = 0;
    const char** glfwExtensions = glfwGetRequiredInstanceExtensions(&glfwExtensionCount);

    return { glfwExtensions, glfwExtensions + glfwExtensionCount };
}
