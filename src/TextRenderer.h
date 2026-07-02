#pragma once

#include <vulkan/vulkan.h>

#include <ft2build.h>
#include FT_FREETYPE_H

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

struct TextVertex
{
    float pos[2];
    float uv[2];
};

class TextRenderer
{
public:
    struct InitInfo
    {
        VkPhysicalDevice physicalDevice;
        VkDevice device;
        VkCommandPool commandPool;
        VkQueue graphicsQueue;
        VkRenderPass renderPass;
        uint32_t screenWidth;
        uint32_t screenHeight;
    };

    TextRenderer() = default;
    ~TextRenderer();

    TextRenderer(const TextRenderer&) = delete;
    TextRenderer& operator=(const TextRenderer&) = delete;

    void init(const InitInfo& info, const std::string& fontPath, uint32_t fontPixelSize);
    void shutdown();

    void setScreenSize(uint32_t width, uint32_t height);

    void drawText(VkCommandBuffer commandBuffer,
                  const std::string& text,
                  float x,
                  float y,
                  float r,
                  float g,
                  float b,
                  float a);

    void drawCenteredText(VkCommandBuffer commandBuffer,
                          const std::string& text,
                          float r,
                          float g,
                          float b,
                          float a);

    float measureText(const std::string& text) const;

private:
    struct Glyph
    {
        float uvMin[2];
        float uvMax[2];
        float size[2];
        float bearing[2];
        float advance;
    };

    struct PushConstants
    {
        float screenSize[2];
        float color[4];
    };

    void createAtlas(const std::string& fontPath, uint32_t fontPixelSize);
    void createAtlasImage();
    void uploadAtlasImage();
    void createDescriptorResources();
    void createPipeline();
    void createVertexBuffer();
    void ensureVertexBufferCapacity(size_t vertexCount);

    VkPhysicalDevice physicalDevice_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    VkCommandPool commandPool_ = VK_NULL_HANDLE;
    VkQueue graphicsQueue_ = VK_NULL_HANDLE;
    VkRenderPass renderPass_ = VK_NULL_HANDLE;

    uint32_t screenWidth_ = 0;
    uint32_t screenHeight_ = 0;
    uint32_t fontPixelSize_ = 0;

    FT_Library ftLibrary_ = nullptr;
    FT_Face ftFace_ = nullptr;

    VkImage atlasImage_ = VK_NULL_HANDLE;
    VkDeviceMemory atlasMemory_ = VK_NULL_HANDLE;
    VkImageView atlasView_ = VK_NULL_HANDLE;
    VkSampler atlasSampler_ = VK_NULL_HANDLE;
    uint32_t atlasWidth_ = 0;
    uint32_t atlasHeight_ = 0;
    std::vector<unsigned char> atlasPixels_;

    std::unordered_map<uint32_t, Glyph> glyphs_;

    VkShaderModule vertShader_ = VK_NULL_HANDLE;
    VkShaderModule fragShader_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout descriptorSetLayout_ = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
    VkDescriptorPool descriptorPool_ = VK_NULL_HANDLE;
    VkDescriptorSet descriptorSet_ = VK_NULL_HANDLE;

    VkBuffer vertexBuffer_ = VK_NULL_HANDLE;
    VkDeviceMemory vertexBufferMemory_ = VK_NULL_HANDLE;
    size_t vertexBufferCapacity_ = 0;
    void* vertexBufferMapped_ = nullptr;
};
