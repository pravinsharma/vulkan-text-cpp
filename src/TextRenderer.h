#pragma once

#include <vulkan/vulkan.h>

#include <ft2build.h>
#include FT_FREETYPE_H

#include <hb.h>
#include <hb-ft.h>

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "ColrV1Renderer.h"

struct TextVertex
{
    float pos[2];
    float uv[2];
    float colorGlyph;
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
    void setFont(const std::string& fontPath, uint32_t fontPixelSize);
    void setEmojiFont(const std::string& fontPath);

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

    void drawRect(VkCommandBuffer commandBuffer,
                  float x,
                  float y,
                  float w,
                  float h,
                  float r,
                  float g,
                  float b,
                  float a);

    float measureText(const std::string& text) const;
    float fontAscent() const { return static_cast<float>(fontPixelSize_); }
    bool hasColorGlyphs() const { return hasColorGlyphs_; }
    bool hasColorGlyph(uint32_t glyphIndex) const;

private:
    struct Glyph
    {
        float uvMin[2];
        float uvMax[2];
        float size[2];
        float bearing[2];
        float advance;
    };

    struct ShapedGlyph
    {
        uint32_t glyphIndex;
        uint32_t emojiGlyphIndex;
        uint32_t codepoint;
        float x;
        float y;
        float width;
        float height;
        float bearingX;
        float bearingY;
        float advance;
        float uvMin[2];
        float uvMax[2];
        bool isColorGlyph;
        bool useColrV1;
    };

    struct ColorGlyphInfo
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
        float padding[2];
        float color[4];
        float isRect;
        float padding2[3];
    };
    static_assert(sizeof(PushConstants) == 48, "PushConstants must match GLSL std140 layout and push constant range");

    void createAtlas(const std::string& fontPath, uint32_t fontPixelSize);
    void createAtlasImage();
    void uploadAtlasImage();
    void createDescriptorResources();
    void createPipeline();
    void createRectPipeline();
    void createVertexBuffer();
    void ensureVertexBufferCapacity(size_t vertexCount);
    void writeQuad(TextVertex* out,
                   float x, float y, float w, float h,
                   float u0, float v0, float u1, float v1,
                   float colorGlyph);

    void createColorAtlasImage();
    void uploadColorAtlasImage();
    void buildColorAtlas();
    bool ensureGlyph(uint32_t glyphIndex);
    bool ensureColorGlyph(uint32_t glyphIndex);
    bool ensureColorGlyphFromFace(uint32_t glyphIndex, FT_Face face);
    bool probeFaceHasColorGlyphs() const;
    int probeColrVersion() const;

    std::vector<ShapedGlyph> shapeText(const std::string& text) const;

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
    FT_Face emojiFace_ = nullptr;
    std::string emojiFontPath_;
    hb_font_t* hbFont_ = nullptr;
    int colrVersion_ = 0;

    VkImage atlasImage_ = VK_NULL_HANDLE;
    VkDeviceMemory atlasMemory_ = VK_NULL_HANDLE;
    VkImageView atlasView_ = VK_NULL_HANDLE;
    VkSampler atlasSampler_ = VK_NULL_HANDLE;
    uint32_t atlasWidth_ = 0;
    uint32_t atlasHeight_ = 0;
    std::vector<unsigned char> atlasPixels_;

    VkImage colorAtlasImage_ = VK_NULL_HANDLE;
    VkDeviceMemory colorAtlasMemory_ = VK_NULL_HANDLE;
    VkImageView colorAtlasView_ = VK_NULL_HANDLE;
    VkSampler colorAtlasSampler_ = VK_NULL_HANDLE;
    std::vector<std::uint8_t> colorAtlasPixels_;

    std::unordered_map<uint32_t, Glyph> glyphs_;
    std::unordered_map<uint32_t, ColorGlyphInfo> colorGlyphs_;
    bool hasColorGlyphs_ = false;
    bool atlasDirty_ = false;
    bool colorAtlasDirty_ = false;
    uint32_t atlasPenX_ = 1;
    uint32_t atlasPenY_ = 1;
    uint32_t atlasRowHeight_ = 0;
    uint32_t colorPenX_ = 1;
    uint32_t colorPenY_ = 1;
    uint32_t colorRowHeight_ = 0;

    VkShaderModule vertShader_ = VK_NULL_HANDLE;
    VkShaderModule fragShader_ = VK_NULL_HANDLE;
    VkShaderModule rectFragShader_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout descriptorSetLayout_ = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
    VkPipelineLayout rectPipelineLayout_ = VK_NULL_HANDLE;
    VkPipeline rectPipeline_ = VK_NULL_HANDLE;
    VkDescriptorPool descriptorPool_ = VK_NULL_HANDLE;
    VkDescriptorSet descriptorSet_ = VK_NULL_HANDLE;

    VkBuffer vertexBuffer_ = VK_NULL_HANDLE;
    VkDeviceMemory vertexBufferMemory_ = VK_NULL_HANDLE;
    size_t vertexBufferCapacity_ = 0;
    void* vertexBufferMapped_ = nullptr;

    std::unique_ptr<ColrV1Renderer> colrV1_;
};
