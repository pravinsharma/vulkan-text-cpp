#pragma once

#include <vulkan/vulkan.h>

#include <ft2build.h>
#include FT_FREETYPE_H

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

struct ColrV1Vertex
{
    float pos[2];
    float uv[2];
};

struct ColrV1GradientStop
{
    float position;
    float color[4];
};

enum class ColrV1PaintType : int32_t
{
    Solid = 0,
    LinearGradient = 1,
    RadialGradient = 2,
    SweepGradient = 3,
};

enum class ColrV1Extend : int32_t
{
    Pad = 0,
    Repeat = 1,
    Reflect = 2,
};

struct ColrV1Paint
{
    ColrV1PaintType type = ColrV1PaintType::Solid;
    float color[4] = {1, 1, 1, 1};
    float p0[2] = {0, 0};
    float p1[2] = {0, 0};
    float p2[2] = {0, 0};
    float radius0 = 0.0f;
    float radius1 = 0.0f;
    ColrV1Extend extend = ColrV1Extend::Pad;
    uint32_t stopOffset = 0;
    uint32_t stopCount = 0;
};

enum class ColrV1Composite : int32_t
{
    Clear = 0,
    Src = 1,
    Dest = 2,
    SrcOver = 3,
    DestOver = 4,
    SrcIn = 5,
    DestIn = 6,
    SrcOut = 7,
    DestOut = 8,
    SrcAtop = 9,
    DestAtop = 10,
    Xor = 11,
    Plus = 12,
    Screen = 13,
    Overlay = 14,
    Darken = 15,
    Lighten = 16,
    ColorDodge = 17,
    ColorBurn = 18,
    HardLight = 19,
    SoftLight = 20,
    Difference = 21,
    Exclusion = 22,
    Multiply = 23,
    HslHue = 24,
    HslSaturation = 25,
    HslColor = 26,
    HslLuminosity = 27,
};

struct ColrV1DrawOp
{
    uint32_t indexOffset;
    uint32_t indexCount;
    float transform[6];
    ColrV1Paint paint;
    ColrV1Composite composite;
    bool useBackdrop;
};

struct ColrV1ColoredGlyph
{
    float bearing[2];
    float advance;
    uint32_t indexOffset;
    uint32_t indexCount;
    std::vector<ColrV1DrawOp> ops;
};

class ColrV1Renderer
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

    ColrV1Renderer() = default;
    ~ColrV1Renderer();

    ColrV1Renderer(const ColrV1Renderer&) = delete;
    ColrV1Renderer& operator=(const ColrV1Renderer&) = delete;

    void init(const InitInfo& info);
    void shutdown();

    void setScreenSize(uint32_t width, uint32_t height);

    bool ensureGlyph(uint32_t glyphIndex, FT_Face face);
    bool hasGlyph(uint32_t glyphIndex) const;
    bool drawGlyph(VkCommandBuffer commandBuffer,
                   uint32_t glyphIndex,
                   float x, float y,
                   float scale,
                   float screenWidth, float screenHeight);

    const ColrV1ColoredGlyph* getGlyph(uint32_t glyphIndex) const;

private:
    struct PaintUBO
    {
        int32_t type;
        float color[4];
        float p0[2];
        float p1[2];
        float p2[2];
        float radius0;
        float radius1;
        int32_t extend;
        int32_t composite;
        int32_t useBackdrop;
        int32_t stopCount;
        uint32_t stopOffset;
    };
    static_assert(sizeof(PaintUBO) == 72, "PaintUBO layout");

    struct PushConstants
    {
        float transform[6];
        float translate[2];
        float screenSize[2];
        float padding[2];
    };
    static_assert(sizeof(PushConstants) == 48, "PushConstants layout");

    void createDescriptorResources();
    void createPipelines();
    void createDefaultPipeline();
    void createVertexBuffer();
    void ensureVertexBufferCapacity(size_t vertexCount, size_t indexCount);
    void uploadStaging();

    void destroyPipelines();
    void destroyBuffers();

    VkPhysicalDevice physicalDevice_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    VkCommandPool commandPool_ = VK_NULL_HANDLE;
    VkQueue graphicsQueue_ = VK_NULL_HANDLE;
    VkRenderPass renderPass_ = VK_NULL_HANDLE;
    uint32_t screenWidth_ = 0;
    uint32_t screenHeight_ = 0;

    VkBuffer vertexBuffer_ = VK_NULL_HANDLE;
    VkDeviceMemory vertexBufferMemory_ = VK_NULL_HANDLE;
    void* vertexBufferMapped_ = nullptr;
    size_t vertexBufferCapacity_ = 0;
    size_t vertexBufferSize_ = 0;

    VkBuffer indexBuffer_ = VK_NULL_HANDLE;
    VkDeviceMemory indexBufferMemory_ = VK_NULL_HANDLE;
    void* indexBufferMapped_ = nullptr;
    size_t indexBufferCapacity_ = 0;
    size_t indexBufferSize_ = 0;

    VkBuffer gradientStopsBuffer_ = VK_NULL_HANDLE;
    VkDeviceMemory gradientStopsMemory_ = VK_NULL_HANDLE;
    void* gradientStopsMapped_ = nullptr;
    size_t gradientStopsCapacity_ = 0;
    size_t gradientStopsSize_ = 0;

    static constexpr VkDeviceSize kPaintUboStride = 256;
    static constexpr uint32_t kPaintUboSlots = 256;
    VkBuffer paintUboBuffer_ = VK_NULL_HANDLE;
    VkDeviceMemory paintUboMemory_ = VK_NULL_HANDLE;
    void* paintUboMapped_ = nullptr;
    size_t paintUboCapacity_ = 0;
    uint32_t paintUboCursor_ = 0;

    VkDescriptorSetLayout descriptorSetLayout_ = VK_NULL_HANDLE;
    VkDescriptorPool descriptorPool_ = VK_NULL_HANDLE;
    VkDescriptorSet descriptorSet_ = VK_NULL_HANDLE;

    VkShaderModule vertShader_ = VK_NULL_HANDLE;
    VkShaderModule fragShader_ = VK_NULL_HANDLE;

    struct PipelineKey
    {
        ColrV1Composite composite;
        bool clip;
        bool backdrop;
        bool operator==(const PipelineKey& o) const noexcept
        {
            return composite == o.composite && clip == o.clip && backdrop == o.backdrop;
        }
    };
    struct PipelineKeyHash
    {
        size_t operator()(const PipelineKey& k) const noexcept;
    };
    std::unordered_map<PipelineKey, VkPipeline, PipelineKeyHash> pipelines_;
    std::unordered_map<PipelineKey, VkPipelineLayout, PipelineKeyHash> pipelineLayouts_;

    std::unordered_map<uint32_t, ColrV1ColoredGlyph> glyphs_;
    std::vector<ColrV1GradientStop> gradientStops_;
    std::vector<ColrV1Vertex> pendingVertices_;
    std::vector<uint32_t> pendingIndices_;
};
