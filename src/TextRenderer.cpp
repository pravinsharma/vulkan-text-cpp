#include "TextRenderer.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <vector>

#include <hb.h>
#include <hb-ft.h>
#include <freetype/tttables.h>

#include "text.vert.spv.h"
#include "text.frag.spv.h"
#include "rect.frag.spv.h"

namespace
{

constexpr uint32_t kAtlasWidth = 1024;
constexpr uint32_t kAtlasHeight = 1024;
constexpr uint32_t kFirstChar = 32;
constexpr uint32_t kNumChars = 95;

int probeColrVersionForFace(FT_Face face)
{
    FT_ULong length = 0;
    if (FT_Load_Sfnt_Table(face, FT_MAKE_TAG('C','O','L','R'), 0, nullptr, &length) != 0)
    {
        return 0;
    }
    std::vector<uint8_t> buf(length);
    if (FT_Load_Sfnt_Table(face, FT_MAKE_TAG('C','O','L','R'), 0, buf.data(), &length) != 0)
    {
        return 0;
    }
    if (length < 2) return 0;
    uint16_t version = (static_cast<uint16_t>(buf[0]) << 8) | static_cast<uint16_t>(buf[1]);
    if (version == 0x0000) return 1;
    if (version >= 0x0001) return 2;
    return 0;
}

uint32_t findMemoryType(VkPhysicalDevice physicalDevice,
                        uint32_t typeFilter,
                        VkMemoryPropertyFlags properties)
{
    VkPhysicalDeviceMemoryProperties memProps;
    vkGetPhysicalDeviceMemoryProperties(physicalDevice, &memProps);
    for (uint32_t i = 0; i < memProps.memoryTypeCount; ++i)
    {
        if ((typeFilter & (1u << i)) &&
            (memProps.memoryTypes[i].propertyFlags & properties) == properties)
        {
            return i;
        }
    }
    throw std::runtime_error("failed to find suitable memory type");
}

void createBuffer(VkPhysicalDevice physicalDevice,
                  VkDevice device,
                  VkDeviceSize size,
                  VkBufferUsageFlags usage,
                  VkMemoryPropertyFlags properties,
                  VkBuffer& buffer,
                  VkDeviceMemory& memory)
{
    VkBufferCreateInfo bufferInfo{};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = size;
    bufferInfo.usage = usage;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    if (vkCreateBuffer(device, &bufferInfo, nullptr, &buffer) != VK_SUCCESS)
    {
        throw std::runtime_error("failed to create buffer");
    }

    VkMemoryRequirements memReqs;
    vkGetBufferMemoryRequirements(device, buffer, &memReqs);

    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memReqs.size;
    allocInfo.memoryTypeIndex = findMemoryType(physicalDevice, memReqs.memoryTypeBits, properties);

    if (vkAllocateMemory(device, &allocInfo, nullptr, &memory) != VK_SUCCESS)
    {
        throw std::runtime_error("failed to allocate buffer memory");
    }

    vkBindBufferMemory(device, buffer, memory, 0);
}

}

TextRenderer::~TextRenderer()
{
    shutdown();
}

void TextRenderer::init(const InitInfo& info, const std::string& fontPath, uint32_t fontPixelSize)
{
    physicalDevice_ = info.physicalDevice;
    device_ = info.device;
    commandPool_ = info.commandPool;
    graphicsQueue_ = info.graphicsQueue;
    renderPass_ = info.renderPass;
    screenWidth_ = info.screenWidth;
    screenHeight_ = info.screenHeight;
    fontPixelSize_ = fontPixelSize;

    if (FT_Init_FreeType(&ftLibrary_) != 0)
    {
        throw std::runtime_error("failed to initialize FreeType");
    }

    if (FT_New_Face(ftLibrary_, fontPath.c_str(), 0, &ftFace_) != 0)
    {
        throw std::runtime_error("failed to load font: " + fontPath);
    }

    FT_Set_Pixel_Sizes(ftFace_, 0, fontPixelSize);

    hbFont_ = hb_ft_font_create(ftFace_, nullptr);
    if (!hbFont_)
    {
        throw std::runtime_error("failed to create HarfBuzz font");
    }

    colrVersion_ = probeColrVersion();
    if (colrVersion_ != 0)
    {
        std::cerr << "Font COLR version: " << (colrVersion_ == 2 ? "v1" : "v0") << "\n";
    }

    if (!emojiFontPath_.empty())
    {
        setEmojiFont(emojiFontPath_);
    }

    ColrV1Renderer::InitInfo colrInfo{};
    colrInfo.physicalDevice = physicalDevice_;
    colrInfo.device = device_;
    colrInfo.commandPool = commandPool_;
    colrInfo.graphicsQueue = graphicsQueue_;
    colrInfo.renderPass = renderPass_;
    colrInfo.screenWidth = screenWidth_;
    colrInfo.screenHeight = screenHeight_;
    colrV1_ = std::make_unique<ColrV1Renderer>();
    colrV1_->init(colrInfo);

    createAtlas(fontPath, fontPixelSize);
    createAtlasImage();
    uploadAtlasImage();
    buildColorAtlas();
    createColorAtlasImage();
    if (!colorAtlasPixels_.empty())
    {
        uploadColorAtlasImage();
    }
    createDescriptorResources();
    createPipeline();
    createRectPipeline();
    createVertexBuffer();
}

void TextRenderer::shutdown()
{
    if (hbFont_)
    {
        hb_font_destroy(hbFont_);
        hbFont_ = nullptr;
    }

    colrV1_.reset();

    if (device_ == VK_NULL_HANDLE) return;

    if (vertexBuffer_ != VK_NULL_HANDLE)
    {
        if (vertexBufferMapped_)
        {
            vkUnmapMemory(device_, vertexBufferMemory_);
            vertexBufferMapped_ = nullptr;
        }
        vkDestroyBuffer(device_, vertexBuffer_, nullptr);
        vkFreeMemory(device_, vertexBufferMemory_, nullptr);
        vertexBuffer_ = VK_NULL_HANDLE;
        vertexBufferMemory_ = VK_NULL_HANDLE;
    }

    if (pipeline_ != VK_NULL_HANDLE)
    {
        vkDestroyPipeline(device_, pipeline_, nullptr);
        pipeline_ = VK_NULL_HANDLE;
    }

    if (rectPipeline_ != VK_NULL_HANDLE)
    {
        vkDestroyPipeline(device_, rectPipeline_, nullptr);
        rectPipeline_ = VK_NULL_HANDLE;
    }

    if (pipelineLayout_ != VK_NULL_HANDLE)
    {
        vkDestroyPipelineLayout(device_, pipelineLayout_, nullptr);
        pipelineLayout_ = VK_NULL_HANDLE;
    }

    if (rectPipelineLayout_ != VK_NULL_HANDLE)
    {
        vkDestroyPipelineLayout(device_, rectPipelineLayout_, nullptr);
        rectPipelineLayout_ = VK_NULL_HANDLE;
    }

    if (descriptorPool_ != VK_NULL_HANDLE)
    {
        vkDestroyDescriptorPool(device_, descriptorPool_, nullptr);
        descriptorPool_ = VK_NULL_HANDLE;
    }

    if (descriptorSetLayout_ != VK_NULL_HANDLE)
    {
        vkDestroyDescriptorSetLayout(device_, descriptorSetLayout_, nullptr);
        descriptorSetLayout_ = VK_NULL_HANDLE;
    }

    if (vertShader_ != VK_NULL_HANDLE)
    {
        vkDestroyShaderModule(device_, vertShader_, nullptr);
        vertShader_ = VK_NULL_HANDLE;
    }

    if (fragShader_ != VK_NULL_HANDLE)
    {
        vkDestroyShaderModule(device_, fragShader_, nullptr);
        fragShader_ = VK_NULL_HANDLE;
    }

    if (rectFragShader_ != VK_NULL_HANDLE)
    {
        vkDestroyShaderModule(device_, rectFragShader_, nullptr);
        rectFragShader_ = VK_NULL_HANDLE;
    }

    if (colorAtlasSampler_ != VK_NULL_HANDLE)
    {
        vkDestroySampler(device_, colorAtlasSampler_, nullptr);
        colorAtlasSampler_ = VK_NULL_HANDLE;
    }

    if (colorAtlasView_ != VK_NULL_HANDLE)
    {
        vkDestroyImageView(device_, colorAtlasView_, nullptr);
        colorAtlasView_ = VK_NULL_HANDLE;
    }

    if (colorAtlasImage_ != VK_NULL_HANDLE)
    {
        vkDestroyImage(device_, colorAtlasImage_, nullptr);
        colorAtlasImage_ = VK_NULL_HANDLE;
    }

    if (colorAtlasMemory_ != VK_NULL_HANDLE)
    {
        vkFreeMemory(device_, colorAtlasMemory_, nullptr);
        colorAtlasMemory_ = VK_NULL_HANDLE;
    }

    if (atlasSampler_ != VK_NULL_HANDLE)
    {
        vkDestroySampler(device_, atlasSampler_, nullptr);
        atlasSampler_ = VK_NULL_HANDLE;
    }

    if (atlasView_ != VK_NULL_HANDLE)
    {
        vkDestroyImageView(device_, atlasView_, nullptr);
        atlasView_ = VK_NULL_HANDLE;
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

    if (emojiFace_)
    {
        FT_Done_Face(emojiFace_);
        emojiFace_ = nullptr;
    }

    if (ftFace_)
    {
        FT_Done_Face(ftFace_);
        ftFace_ = nullptr;
    }

    if (ftLibrary_)
    {
        FT_Done_FreeType(ftLibrary_);
        ftLibrary_ = nullptr;
    }

    device_ = VK_NULL_HANDLE;
}

void TextRenderer::setScreenSize(uint32_t width, uint32_t height)
{
    screenWidth_ = width;
    screenHeight_ = height;
    if (colrV1_)
    {
        colrV1_->setScreenSize(width, height);
    }
}

void TextRenderer::setFont(const std::string& fontPath, uint32_t fontPixelSize)
{
    if (hbFont_)
    {
        hb_font_destroy(hbFont_);
        hbFont_ = nullptr;
    }

    if (ftFace_)
    {
        FT_Done_Face(ftFace_);
        ftFace_ = nullptr;
    }

    if (FT_New_Face(ftLibrary_, fontPath.c_str(), 0, &ftFace_) != 0)
    {
        throw std::runtime_error("failed to load font: " + fontPath);
    }

    FT_Set_Pixel_Sizes(ftFace_, 0, fontPixelSize);
    fontPixelSize_ = fontPixelSize;

    hbFont_ = hb_ft_font_create(ftFace_, nullptr);
    if (!hbFont_)
    {
        throw std::runtime_error("failed to create HarfBuzz font");
    }

    colrVersion_ = probeColrVersion();

    if (!emojiFontPath_.empty() && !emojiFace_)
    {
        setEmojiFont(emojiFontPath_);
    }

    colrV1_.reset();
    if (colrVersion_ == 2)
    {
        ColrV1Renderer::InitInfo colrInfo{};
        colrInfo.physicalDevice = physicalDevice_;
        colrInfo.device = device_;
        colrInfo.commandPool = commandPool_;
        colrInfo.graphicsQueue = graphicsQueue_;
        colrInfo.renderPass = renderPass_;
        colrInfo.screenWidth = screenWidth_;
        colrInfo.screenHeight = screenHeight_;
        colrV1_ = std::make_unique<ColrV1Renderer>();
        colrV1_->init(colrInfo);
    }

    createAtlas(fontPath, fontPixelSize);
    uploadAtlasImage();
    buildColorAtlas();
    if (!colorAtlasPixels_.empty())
    {
        uploadColorAtlasImage();
    }
}

void TextRenderer::createAtlas(const std::string& fontPath, uint32_t fontPixelSize)
{
    atlasWidth_ = kAtlasWidth;
    atlasHeight_ = kAtlasHeight;
    atlasPixels_.assign(atlasWidth_ * atlasHeight_, 0);
    colorAtlasPixels_.assign(kAtlasWidth * kAtlasHeight * 4, 0);

    uint32_t penX = 1;
    uint32_t penY = 1;
    uint32_t rowHeight = 0;
    uint32_t colorPenX = 1;
    uint32_t colorPenY = 1;
    uint32_t colorRowHeight = 0;

    for (uint32_t c = 0; c < kNumChars; ++c)
    {
        const uint32_t charCode = kFirstChar + c;
        hb_codepoint_t glyphIndex = 0;
        if (!hb_font_get_nominal_glyph(hbFont_, charCode, &glyphIndex))
        {
            continue;
        }

        if (FT_Load_Glyph(ftFace_, glyphIndex, FT_LOAD_RENDER) != 0)
        {
            continue;
        }

        const FT_GlyphSlot g = ftFace_->glyph;
        const uint32_t w = g->bitmap.width;
        const uint32_t h = g->bitmap.rows;

        if (w == 0 || h == 0)
        {
            Glyph empty{};
            empty.advance = static_cast<float>(g->advance.x) / 64.0f;
            empty.size[0] = 0.0f;
            empty.size[1] = 0.0f;
            empty.bearing[0] = 0.0f;
            empty.bearing[1] = 0.0f;
            empty.uvMin[0] = 0.0f;
            empty.uvMin[1] = 0.0f;
            empty.uvMax[0] = 0.0f;
            empty.uvMax[1] = 0.0f;
            glyphs_[glyphIndex] = empty;
            continue;
        }

        if (penX + w + 1 > atlasWidth_)
        {
            penY += rowHeight + 1;
            penX = 1;
            rowHeight = 0;
        }

        if (penY + h + 1 > atlasHeight_)
        {
            throw std::runtime_error("glyph atlas overflow");
        }

        for (uint32_t row = 0; row < h; ++row)
        {
            const unsigned char* src = g->bitmap.buffer + static_cast<intptr_t>(row) * g->bitmap.pitch;
            unsigned char* dst = atlasPixels_.data() + (penY + row) * atlasWidth_ + penX;
            std::memcpy(dst, src, w);
        }

        Glyph info{};
        info.size[0] = static_cast<float>(w);
        info.size[1] = static_cast<float>(h);
        info.bearing[0] = static_cast<float>(g->bitmap_left);
        info.bearing[1] = static_cast<float>(g->bitmap_top);
        info.advance = static_cast<float>(g->advance.x) / 64.0f;
        info.uvMin[0] = static_cast<float>(penX) / static_cast<float>(atlasWidth_);
        info.uvMin[1] = static_cast<float>(penY) / static_cast<float>(atlasHeight_);
        info.uvMax[0] = static_cast<float>(penX + w) / static_cast<float>(atlasWidth_);
        info.uvMax[1] = static_cast<float>(penY + h) / static_cast<float>(atlasHeight_);
        glyphs_[glyphIndex] = info;

        penX += w + 1;
        if (h + 1 > rowHeight) rowHeight = h + 1;

        if (FT_Load_Glyph(ftFace_, glyphIndex, FT_LOAD_RENDER | FT_LOAD_COLOR) != 0)
        {
            continue;
        }

        const FT_GlyphSlot cg = ftFace_->glyph;
        if (cg->bitmap.pixel_mode != FT_PIXEL_MODE_BGRA)
        {
            continue;
        }

        const uint32_t cw = cg->bitmap.width;
        const uint32_t ch = cg->bitmap.rows;
        if (cw == 0 || ch == 0)
        {
            continue;
        }

        hasColorGlyphs_ = true;

        if (colorPenX + cw + 1 > kAtlasWidth)
        {
            colorPenY += colorRowHeight + 1;
            colorPenX = 1;
            colorRowHeight = 0;
        }

        if (colorPenY + ch + 1 > kAtlasHeight)
        {
            throw std::runtime_error("color glyph atlas overflow");
        }

        for (uint32_t row = 0; row < ch; ++row)
        {
            const std::uint8_t* src = reinterpret_cast<const std::uint8_t*>(cg->bitmap.buffer) + static_cast<intptr_t>(row) * cg->bitmap.pitch;
            std::uint8_t* dst = colorAtlasPixels_.data() + ((colorPenY + row) * kAtlasWidth + colorPenX) * 4;
            std::memcpy(dst, src, static_cast<size_t>(cw) * 4);
        }

        ColorGlyphInfo colorInfo{};
        colorInfo.size[0] = static_cast<float>(cw);
        colorInfo.size[1] = static_cast<float>(ch);
        colorInfo.bearing[0] = static_cast<float>(cg->bitmap_left);
        colorInfo.bearing[1] = static_cast<float>(cg->bitmap_top);
        colorInfo.advance = static_cast<float>(cg->advance.x) / 64.0f;
        colorInfo.uvMin[0] = static_cast<float>(colorPenX) / static_cast<float>(kAtlasWidth);
        colorInfo.uvMin[1] = static_cast<float>(colorPenY) / static_cast<float>(kAtlasHeight);
        colorInfo.uvMax[0] = static_cast<float>(colorPenX + cw) / static_cast<float>(kAtlasWidth);
        colorInfo.uvMax[1] = static_cast<float>(colorPenY + ch) / static_cast<float>(kAtlasHeight);
        colorGlyphs_[glyphIndex] = colorInfo;

        colorPenX += cw + 1;
        if (ch + 1 > colorRowHeight) colorRowHeight = ch + 1;
    }

    if (colrV1_)
    {
        for (uint32_t c = 0; c < kNumChars; ++c)
        {
            const uint32_t charCode = kFirstChar + c;
            hb_codepoint_t glyphIndex = 0;
            if (!hb_font_get_nominal_glyph(hbFont_, charCode, &glyphIndex))
            {
                continue;
            }
            if (colrV1_->hasGlyph(glyphIndex))
            {
                continue;
            }
            if (colrV1_->ensureGlyph(glyphIndex, ftFace_))
            {
                hasColorGlyphs_ = true;
            }
        }
    }
}

bool TextRenderer::probeFaceHasColorGlyphs() const
{
    if (probeColrVersion() != 0) return true;
    const auto hasTable = [&](uint32_t tag) {
        return FT_Load_Sfnt_Table(ftFace_, tag, 0, nullptr, nullptr) == 0;
    };
    return hasTable(FT_MAKE_TAG('C', 'B', 'D', 'T')) ||
           hasTable(FT_MAKE_TAG('S', 'V', 'G', ' '));
}

int TextRenderer::probeColrVersion() const
{
    return probeColrVersionForFace(ftFace_);
}

void TextRenderer::setEmojiFont(const std::string& fontPath)
{
    if (emojiFace_)
    {
        FT_Done_Face(emojiFace_);
        emojiFace_ = nullptr;
    }
    emojiFontPath_ = fontPath;

    if (FT_New_Face(ftLibrary_, fontPath.c_str(), 0, &emojiFace_) != 0)
    {
        std::cerr << "Warning: failed to load emoji fallback font: " << fontPath << "\n";
        emojiFace_ = nullptr;
        return;
    }

    FT_Set_Pixel_Sizes(emojiFace_, 0, fontPixelSize_);

    int emojiColrVer = probeColrVersionForFace(emojiFace_);
    if (emojiColrVer != 0)
    {
        std::cerr << "Emoji font COLR version: " << (emojiColrVer == 2 ? "v1" : "v0") << "\n";
    }
}

bool TextRenderer::hasColorGlyph(uint32_t glyphIndex) const
{
    if (!hasColorGlyphs_)
    {
        return false;
    }
    return colorGlyphs_.find(glyphIndex) != colorGlyphs_.end();
}

bool TextRenderer::ensureColorGlyph(uint32_t glyphIndex)
{
    if (colorGlyphs_.find(glyphIndex) != colorGlyphs_.end())
    {
        return true;
    }

    if (colrV1_ && colrV1_->ensureGlyph(glyphIndex, ftFace_))
    {
        hasColorGlyphs_ = true;
        return true;
    }

    if (FT_Load_Glyph(ftFace_, glyphIndex, FT_LOAD_RENDER | FT_LOAD_COLOR) != 0)
    {
        return false;
    }

    const FT_GlyphSlot g = ftFace_->glyph;
    if (g->bitmap.pixel_mode != FT_PIXEL_MODE_BGRA)
    {
        return false;
    }

    const uint32_t w = g->bitmap.width;
    const uint32_t h = g->bitmap.rows;
    if (w == 0 || h == 0)
    {
        return false;
    }

    if (colorPenX_ + w + 1 > kAtlasWidth)
    {
        colorPenY_ += colorRowHeight_ + 1;
        colorPenX_ = 1;
        colorRowHeight_ = 0;
    }

    if (colorPenY_ + h + 1 > kAtlasHeight)
    {
        return false;
    }

    for (uint32_t row = 0; row < h; ++row)
    {
        const std::uint8_t* src = reinterpret_cast<const std::uint8_t*>(g->bitmap.buffer) + static_cast<intptr_t>(row) * g->bitmap.pitch;
        std::uint8_t* dst = colorAtlasPixels_.data() + ((colorPenY_ + row) * kAtlasWidth + colorPenX_) * 4;
        std::memcpy(dst, src, static_cast<size_t>(w) * 4);
    }

    ColorGlyphInfo info{};
    info.size[0] = static_cast<float>(w);
    info.size[1] = static_cast<float>(h);
    info.bearing[0] = static_cast<float>(g->bitmap_left);
    info.bearing[1] = static_cast<float>(g->bitmap_top);
    info.advance = static_cast<float>(g->advance.x) / 64.0f;
    info.uvMin[0] = static_cast<float>(colorPenX_) / static_cast<float>(kAtlasWidth);
    info.uvMin[1] = static_cast<float>(colorPenY_) / static_cast<float>(kAtlasHeight);
    info.uvMax[0] = static_cast<float>(colorPenX_ + w) / static_cast<float>(kAtlasWidth);
    info.uvMax[1] = static_cast<float>(colorPenY_ + h) / static_cast<float>(kAtlasHeight);
    colorGlyphs_[glyphIndex] = info;

    colorPenX_ += w + 1;
    if (h + 1 > colorRowHeight_) colorRowHeight_ = h + 1;
    colorAtlasDirty_ = true;

    return true;
}

bool TextRenderer::ensureColorGlyphFromFace(uint32_t glyphIndex, FT_Face face)
{
    if (colorGlyphs_.find(glyphIndex) != colorGlyphs_.end())
    {
        return true;
    }

    if (FT_Load_Glyph(face, glyphIndex, FT_LOAD_RENDER | FT_LOAD_COLOR) != 0)
    {
        return false;
    }

    const FT_GlyphSlot g = face->glyph;
    if (g->bitmap.pixel_mode != FT_PIXEL_MODE_BGRA)
    {
        return false;
    }

    const uint32_t w = g->bitmap.width;
    const uint32_t h = g->bitmap.rows;
    if (w == 0 || h == 0)
    {
        return false;
    }

    if (colorPenX_ + w + 1 > kAtlasWidth)
    {
        colorPenY_ += colorRowHeight_ + 1;
        colorPenX_ = 1;
        colorRowHeight_ = 0;
    }

    if (colorPenY_ + h + 1 > kAtlasHeight)
    {
        return false;
    }

    for (uint32_t row = 0; row < h; ++row)
    {
        const std::uint8_t* src = reinterpret_cast<const std::uint8_t*>(g->bitmap.buffer) + static_cast<intptr_t>(row) * g->bitmap.pitch;
        std::uint8_t* dst = colorAtlasPixels_.data() + ((colorPenY_ + row) * kAtlasWidth + colorPenX_) * 4;
        std::memcpy(dst, src, static_cast<size_t>(w) * 4);
    }

    ColorGlyphInfo info{};
    info.size[0] = static_cast<float>(w);
    info.size[1] = static_cast<float>(h);
    info.bearing[0] = static_cast<float>(g->bitmap_left);
    info.bearing[1] = static_cast<float>(g->bitmap_top);
    info.advance = static_cast<float>(g->advance.x) / 64.0f;
    info.uvMin[0] = static_cast<float>(colorPenX_) / static_cast<float>(kAtlasWidth);
    info.uvMin[1] = static_cast<float>(colorPenY_) / static_cast<float>(kAtlasHeight);
    info.uvMax[0] = static_cast<float>(colorPenX_ + w) / static_cast<float>(kAtlasWidth);
    info.uvMax[1] = static_cast<float>(colorPenY_ + h) / static_cast<float>(kAtlasHeight);
    colorGlyphs_[glyphIndex] = info;

    colorPenX_ += w + 1;
    if (h + 1 > colorRowHeight_) colorRowHeight_ = h + 1;
    colorAtlasDirty_ = true;

    return true;
}

void TextRenderer::createColorAtlasImage()
{
    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
    imageInfo.extent.width = kAtlasWidth;
    imageInfo.extent.height = kAtlasHeight;
    imageInfo.extent.depth = 1;
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    imageInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;

    if (vkCreateImage(device_, &imageInfo, nullptr, &colorAtlasImage_) != VK_SUCCESS)
    {
        throw std::runtime_error("failed to create color atlas image");
    }

    VkMemoryRequirements memReqs;
    vkGetImageMemoryRequirements(device_, colorAtlasImage_, &memReqs);

    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memReqs.size;
    allocInfo.memoryTypeIndex = findMemoryType(physicalDevice_, memReqs.memoryTypeBits,
                                               VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

    if (vkAllocateMemory(device_, &allocInfo, nullptr, &colorAtlasMemory_) != VK_SUCCESS)
    {
        throw std::runtime_error("failed to allocate color atlas memory");
    }

    vkBindImageMemory(device_, colorAtlasImage_, colorAtlasMemory_, 0);

    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = colorAtlasImage_;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.levelCount = 1;
    viewInfo.subresourceRange.layerCount = 1;

    if (vkCreateImageView(device_, &viewInfo, nullptr, &colorAtlasView_) != VK_SUCCESS)
    {
        throw std::runtime_error("failed to create color atlas view");
    }

    VkSamplerCreateInfo samplerInfo{};
    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter = VK_FILTER_NEAREST;
    samplerInfo.minFilter = VK_FILTER_NEAREST;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.anisotropyEnable = VK_FALSE;
    samplerInfo.maxAnisotropy = 1.0f;
    samplerInfo.borderColor = VK_BORDER_COLOR_INT_OPAQUE_BLACK;
    samplerInfo.unnormalizedCoordinates = VK_FALSE;
    samplerInfo.compareEnable = VK_FALSE;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;

    if (vkCreateSampler(device_, &samplerInfo, nullptr, &colorAtlasSampler_) != VK_SUCCESS)
    {
        throw std::runtime_error("failed to create color atlas sampler");
    }
}

void TextRenderer::uploadColorAtlasImage()
{
    if (colorAtlasPixels_.empty()) return;

    VkDeviceSize imageSize = static_cast<VkDeviceSize>(kAtlasWidth) * kAtlasHeight * 4;

    VkBuffer stagingBuffer = VK_NULL_HANDLE;
    VkDeviceMemory stagingMemory = VK_NULL_HANDLE;
    createBuffer(physicalDevice_, device_, imageSize,
                 VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                 stagingBuffer, stagingMemory);

    void* data = nullptr;
    vkMapMemory(device_, stagingMemory, 0, imageSize, 0, &data);
    std::memcpy(data, colorAtlasPixels_.data(), static_cast<size_t>(imageSize));
    vkUnmapMemory(device_, stagingMemory);

    VkCommandBufferAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandPool = commandPool_;
    allocInfo.commandBufferCount = 1;

    VkCommandBuffer cmd = VK_NULL_HANDLE;
    vkAllocateCommandBuffers(device_, &allocInfo, &cmd);

    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &beginInfo);

    VkImageMemoryBarrier toTransfer{};
    toTransfer.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    toTransfer.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    toTransfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    toTransfer.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toTransfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toTransfer.image = colorAtlasImage_;
    toTransfer.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    toTransfer.subresourceRange.levelCount = 1;
    toTransfer.subresourceRange.layerCount = 1;
    toTransfer.srcAccessMask = 0;
    toTransfer.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;

    vkCmdPipelineBarrier(cmd,
        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
        0, 0, nullptr, 0, nullptr, 1, &toTransfer);

    VkBufferImageCopy region{};
    region.bufferOffset = 0;
    region.bufferRowLength = 0;
    region.bufferImageHeight = 0;
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.layerCount = 1;
    region.imageOffset = {0, 0, 0};
    region.imageExtent = {kAtlasWidth, kAtlasHeight, 1};

    vkCmdCopyBufferToImage(cmd, stagingBuffer, colorAtlasImage_,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    VkImageMemoryBarrier toShader{};
    toShader.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    toShader.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    toShader.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    toShader.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toShader.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toShader.image = colorAtlasImage_;
    toShader.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    toShader.subresourceRange.levelCount = 1;
    toShader.subresourceRange.layerCount = 1;
    toShader.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    toShader.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

    vkCmdPipelineBarrier(cmd,
        VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        0, 0, nullptr, 0, nullptr, 1, &toShader);

    vkEndCommandBuffer(cmd);

    VkSubmitInfo submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &cmd;

    vkQueueSubmit(graphicsQueue_, 1, &submitInfo, VK_NULL_HANDLE);
    vkQueueWaitIdle(graphicsQueue_);

    vkFreeCommandBuffers(device_, commandPool_, 1, &cmd);
    vkDestroyBuffer(device_, stagingBuffer, nullptr);
    vkFreeMemory(device_, stagingMemory, nullptr);

    colorAtlasDirty_ = false;
}

void TextRenderer::buildColorAtlas()
{
    colorAtlasPixels_.assign(kAtlasWidth * kAtlasHeight * 4, 0);
    colorGlyphs_.clear();
    hasColorGlyphs_ = false;
    colorPenX_ = 1;
    colorPenY_ = 1;
    colorRowHeight_ = 0;

    for (uint32_t c = 0; c < kNumChars; ++c)
    {
        const uint32_t charCode = kFirstChar + c;
        hb_codepoint_t glyphIndex = 0;
        if (!hb_font_get_nominal_glyph(hbFont_, charCode, &glyphIndex))
        {
            continue;
        }

        if (FT_Load_Glyph(ftFace_, glyphIndex, FT_LOAD_RENDER | FT_LOAD_COLOR) != 0)
        {
            continue;
        }

        const FT_GlyphSlot g = ftFace_->glyph;
        if (g->bitmap.pixel_mode != FT_PIXEL_MODE_BGRA)
        {
            continue;
        }

        const uint32_t w = g->bitmap.width;
        const uint32_t h = g->bitmap.rows;
        if (w == 0 || h == 0)
        {
            continue;
        }

        hasColorGlyphs_ = true;

        if (colorPenX_ + w + 1 > kAtlasWidth)
        {
            colorPenY_ += colorRowHeight_ + 1;
            colorPenX_ = 1;
            colorRowHeight_ = 0;
        }

        if (colorPenY_ + h + 1 > kAtlasHeight)
        {
            throw std::runtime_error("color glyph atlas overflow");
        }

        for (uint32_t row = 0; row < h; ++row)
        {
            const std::uint8_t* src = reinterpret_cast<const std::uint8_t*>(g->bitmap.buffer) + static_cast<intptr_t>(row) * g->bitmap.pitch;
            std::uint8_t* dst = colorAtlasPixels_.data() + ((colorPenY_ + row) * kAtlasWidth + colorPenX_) * 4;
            std::memcpy(dst, src, static_cast<size_t>(w) * 4);
        }

        ColorGlyphInfo info{};
        info.size[0] = static_cast<float>(w);
        info.size[1] = static_cast<float>(h);
        info.bearing[0] = static_cast<float>(g->bitmap_left);
        info.bearing[1] = static_cast<float>(g->bitmap_top);
        info.advance = static_cast<float>(g->advance.x) / 64.0f;
        info.uvMin[0] = static_cast<float>(colorPenX_) / static_cast<float>(kAtlasWidth);
        info.uvMin[1] = static_cast<float>(colorPenY_) / static_cast<float>(kAtlasHeight);
        info.uvMax[0] = static_cast<float>(colorPenX_ + w) / static_cast<float>(kAtlasWidth);
        info.uvMax[1] = static_cast<float>(colorPenY_ + h) / static_cast<float>(kAtlasHeight);
        colorGlyphs_[glyphIndex] = info;

        colorPenX_ += w + 1;
        if (h + 1 > colorRowHeight_) colorRowHeight_ = h + 1;
    }
}

void TextRenderer::createAtlasImage()
{
    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = VK_FORMAT_R8_UNORM;
    imageInfo.extent.width = atlasWidth_;
    imageInfo.extent.height = atlasHeight_;
    imageInfo.extent.depth = 1;
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    imageInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;

    if (vkCreateImage(device_, &imageInfo, nullptr, &atlasImage_) != VK_SUCCESS)
    {
        throw std::runtime_error("failed to create atlas image");
    }

    VkMemoryRequirements memReqs;
    vkGetImageMemoryRequirements(device_, atlasImage_, &memReqs);

    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memReqs.size;
    allocInfo.memoryTypeIndex = findMemoryType(physicalDevice_, memReqs.memoryTypeBits,
                                               VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

    if (vkAllocateMemory(device_, &allocInfo, nullptr, &atlasMemory_) != VK_SUCCESS)
    {
        throw std::runtime_error("failed to allocate atlas memory");
    }

    vkBindImageMemory(device_, atlasImage_, atlasMemory_, 0);

    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = atlasImage_;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = VK_FORMAT_R8_UNORM;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.levelCount = 1;
    viewInfo.subresourceRange.layerCount = 1;

    if (vkCreateImageView(device_, &viewInfo, nullptr, &atlasView_) != VK_SUCCESS)
    {
        throw std::runtime_error("failed to create atlas view");
    }

    VkSamplerCreateInfo samplerInfo{};
    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter = VK_FILTER_NEAREST;
    samplerInfo.minFilter = VK_FILTER_NEAREST;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.anisotropyEnable = VK_FALSE;
    samplerInfo.maxAnisotropy = 1.0f;
    samplerInfo.borderColor = VK_BORDER_COLOR_INT_OPAQUE_BLACK;
    samplerInfo.unnormalizedCoordinates = VK_FALSE;
    samplerInfo.compareEnable = VK_FALSE;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;

    if (vkCreateSampler(device_, &samplerInfo, nullptr, &atlasSampler_) != VK_SUCCESS)
    {
        throw std::runtime_error("failed to create atlas sampler");
    }
}

void TextRenderer::uploadAtlasImage()
{
    VkDeviceSize imageSize = static_cast<VkDeviceSize>(atlasWidth_ * atlasHeight_);

    VkBuffer stagingBuffer = VK_NULL_HANDLE;
    VkDeviceMemory stagingMemory = VK_NULL_HANDLE;
    createBuffer(physicalDevice_, device_, imageSize,
                 VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                 stagingBuffer, stagingMemory);

    void* data = nullptr;
    vkMapMemory(device_, stagingMemory, 0, imageSize, 0, &data);
    std::memcpy(data, atlasPixels_.data(), static_cast<size_t>(imageSize));
    vkUnmapMemory(device_, stagingMemory);

    VkCommandBufferAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandPool = commandPool_;
    allocInfo.commandBufferCount = 1;

    VkCommandBuffer cmd = VK_NULL_HANDLE;
    vkAllocateCommandBuffers(device_, &allocInfo, &cmd);

    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &beginInfo);

    VkImageMemoryBarrier toTransfer{};
    toTransfer.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    toTransfer.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    toTransfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    toTransfer.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toTransfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toTransfer.image = atlasImage_;
    toTransfer.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    toTransfer.subresourceRange.levelCount = 1;
    toTransfer.subresourceRange.layerCount = 1;
    toTransfer.srcAccessMask = 0;
    toTransfer.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;

    vkCmdPipelineBarrier(cmd,
        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
        0, 0, nullptr, 0, nullptr, 1, &toTransfer);

    VkBufferImageCopy region{};
    region.bufferOffset = 0;
    region.bufferRowLength = 0;
    region.bufferImageHeight = 0;
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.layerCount = 1;
    region.imageOffset = {0, 0, 0};
    region.imageExtent = {atlasWidth_, atlasHeight_, 1};

    vkCmdCopyBufferToImage(cmd, stagingBuffer, atlasImage_,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    VkImageMemoryBarrier toShader{};
    toShader.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    toShader.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    toShader.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    toShader.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toShader.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toShader.image = atlasImage_;
    toShader.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    toShader.subresourceRange.levelCount = 1;
    toShader.subresourceRange.layerCount = 1;
    toShader.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    toShader.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

    vkCmdPipelineBarrier(cmd,
        VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        0, 0, nullptr, 0, nullptr, 1, &toShader);

    vkEndCommandBuffer(cmd);

    VkSubmitInfo submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &cmd;

    vkQueueSubmit(graphicsQueue_, 1, &submitInfo, VK_NULL_HANDLE);
    vkQueueWaitIdle(graphicsQueue_);

    vkFreeCommandBuffers(device_, commandPool_, 1, &cmd);
    vkDestroyBuffer(device_, stagingBuffer, nullptr);
    vkFreeMemory(device_, stagingMemory, nullptr);

    atlasPixels_.clear();
    atlasPixels_.shrink_to_fit();
}

void TextRenderer::createDescriptorResources()
{
    VkDescriptorSetLayoutBinding monoBinding{};
    monoBinding.binding = 0;
    monoBinding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    monoBinding.descriptorCount = 1;
    monoBinding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    monoBinding.pImmutableSamplers = nullptr;

    VkDescriptorSetLayoutBinding colorBinding{};
    colorBinding.binding = 1;
    colorBinding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    colorBinding.descriptorCount = 1;
    colorBinding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    colorBinding.pImmutableSamplers = nullptr;

    VkDescriptorSetLayoutBinding bindings[] = { monoBinding, colorBinding };

    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = 2;
    layoutInfo.pBindings = bindings;

    if (vkCreateDescriptorSetLayout(device_, &layoutInfo, nullptr, &descriptorSetLayout_) != VK_SUCCESS)
    {
        throw std::runtime_error("failed to create descriptor set layout");
    }

    VkDescriptorPoolSize poolSize{};
    poolSize.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSize.descriptorCount = 2;

    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes = &poolSize;
    poolInfo.maxSets = 1;

    if (vkCreateDescriptorPool(device_, &poolInfo, nullptr, &descriptorPool_) != VK_SUCCESS)
    {
        throw std::runtime_error("failed to create descriptor pool");
    }

    VkDescriptorSetAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool = descriptorPool_;
    allocInfo.descriptorSetCount = 1;
    allocInfo.pSetLayouts = &descriptorSetLayout_;

    if (vkAllocateDescriptorSets(device_, &allocInfo, &descriptorSet_) != VK_SUCCESS)
    {
        throw std::runtime_error("failed to allocate descriptor set");
    }

    VkDescriptorImageInfo atlasImageInfo{};
    atlasImageInfo.sampler = atlasSampler_;
    atlasImageInfo.imageView = atlasView_;
    atlasImageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    VkDescriptorImageInfo colorAtlasImageInfo{};
    colorAtlasImageInfo.sampler = hasColorGlyphs_ ? colorAtlasSampler_ : atlasSampler_;
    colorAtlasImageInfo.imageView = hasColorGlyphs_ ? colorAtlasView_ : atlasView_;
    colorAtlasImageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    VkWriteDescriptorSet write[2]{};
    write[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write[0].dstSet = descriptorSet_;
    write[0].dstBinding = 0;
    write[0].dstArrayElement = 0;
    write[0].descriptorCount = 1;
    write[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write[0].pImageInfo = &atlasImageInfo;

    write[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write[1].dstSet = descriptorSet_;
    write[1].dstBinding = 1;
    write[1].dstArrayElement = 0;
    write[1].descriptorCount = 1;
    write[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write[1].pImageInfo = &colorAtlasImageInfo;

    vkUpdateDescriptorSets(device_, 2, write, 0, nullptr);
}

void TextRenderer::createPipeline()
{
    VkShaderModuleCreateInfo vertInfo{};
    vertInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    vertInfo.codeSize = text_vert_spv.size() * sizeof(uint32_t);
    vertInfo.pCode = text_vert_spv.data();

    if (vkCreateShaderModule(device_, &vertInfo, nullptr, &vertShader_) != VK_SUCCESS)
    {
        throw std::runtime_error("failed to create vertex shader module");
    }

    VkShaderModuleCreateInfo fragInfo{};
    fragInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    fragInfo.codeSize = text_frag_spv.size() * sizeof(uint32_t);
    fragInfo.pCode = text_frag_spv.data();

    if (vkCreateShaderModule(device_, &fragInfo, nullptr, &fragShader_) != VK_SUCCESS)
    {
        throw std::runtime_error("failed to create fragment shader module");
    }

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vertShader_;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fragShader_;
    stages[1].pName = "main";

    VkVertexInputBindingDescription binding{};
    binding.binding = 0;
    binding.stride = sizeof(TextVertex);
    binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    VkVertexInputAttributeDescription attrs[3]{};
    attrs[0].location = 0;
    attrs[0].binding = 0;
    attrs[0].format = VK_FORMAT_R32G32_SFLOAT;
    attrs[0].offset = offsetof(TextVertex, pos);
    attrs[1].location = 1;
    attrs[1].binding = 0;
    attrs[1].format = VK_FORMAT_R32G32_SFLOAT;
    attrs[1].offset = offsetof(TextVertex, uv);
    attrs[2].location = 2;
    attrs[2].binding = 0;
    attrs[2].format = VK_FORMAT_R32_SFLOAT;
    attrs[2].offset = offsetof(TextVertex, colorGlyph);

    VkPipelineVertexInputStateCreateInfo vertexInput{};
    vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertexInput.vertexBindingDescriptionCount = 1;
    vertexInput.pVertexBindingDescriptions = &binding;
    vertexInput.vertexAttributeDescriptionCount = 3;
    vertexInput.pVertexAttributeDescriptions = attrs;

    VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
    inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    inputAssembly.primitiveRestartEnable = VK_FALSE;

    VkPipelineViewportStateCreateInfo viewportState{};
    viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewportState.viewportCount = 1;
    viewportState.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo rasterizer{};
    rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
    rasterizer.lineWidth = 1.0f;
    rasterizer.cullMode = VK_CULL_MODE_NONE;
    rasterizer.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;

    VkPipelineMultisampleStateCreateInfo multisampling{};
    multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineColorBlendAttachmentState blendAttachment{};
    blendAttachment.blendEnable = VK_TRUE;
    blendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                     VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    blendAttachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    blendAttachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blendAttachment.colorBlendOp = VK_BLEND_OP_ADD;
    blendAttachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    blendAttachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blendAttachment.alphaBlendOp = VK_BLEND_OP_ADD;

    VkPipelineColorBlendStateCreateInfo colorBlending{};
    colorBlending.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    colorBlending.attachmentCount = 1;
    colorBlending.pAttachments = &blendAttachment;

    VkPushConstantRange pushRange{};
    pushRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    pushRange.offset = 0;
    pushRange.size = 48;

    VkPipelineLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.setLayoutCount = 1;
    layoutInfo.pSetLayouts = &descriptorSetLayout_;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &pushRange;

    if (vkCreatePipelineLayout(device_, &layoutInfo, nullptr, &pipelineLayout_) != VK_SUCCESS)
    {
        throw std::runtime_error("failed to create pipeline layout");
    }

    std::vector<VkDynamicState> dynamicStates = {
        VK_DYNAMIC_STATE_VIEWPORT,
        VK_DYNAMIC_STATE_SCISSOR,
    };

    VkPipelineDynamicStateCreateInfo dynamicState{};
    dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamicState.dynamicStateCount = static_cast<uint32_t>(dynamicStates.size());
    dynamicState.pDynamicStates = dynamicStates.data();

    VkGraphicsPipelineCreateInfo pipelineInfo{};
    pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipelineInfo.stageCount = 2;
    pipelineInfo.pStages = stages;
    pipelineInfo.pVertexInputState = &vertexInput;
    pipelineInfo.pInputAssemblyState = &inputAssembly;
    pipelineInfo.pViewportState = &viewportState;
    pipelineInfo.pRasterizationState = &rasterizer;
    pipelineInfo.pMultisampleState = &multisampling;
    pipelineInfo.pColorBlendState = &colorBlending;
    pipelineInfo.pDynamicState = &dynamicState;
    pipelineInfo.layout = pipelineLayout_;
    pipelineInfo.renderPass = renderPass_;
    pipelineInfo.subpass = 0;

    if (vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &pipeline_) != VK_SUCCESS)
    {
        throw std::runtime_error("failed to create graphics pipeline");
    }
}

void TextRenderer::createRectPipeline()
{
    VkShaderModuleCreateInfo fragInfo{};
    fragInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    fragInfo.codeSize = rect_frag_spv.size() * sizeof(uint32_t);
    fragInfo.pCode = rect_frag_spv.data();

    if (vkCreateShaderModule(device_, &fragInfo, nullptr, &rectFragShader_) != VK_SUCCESS)
    {
        throw std::runtime_error("failed to create rect fragment shader module");
    }

    VkPipelineShaderStageCreateInfo fragStage{};
    fragStage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    fragStage.stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    fragStage.module = rectFragShader_;
    fragStage.pName = "main";

    VkPipelineShaderStageCreateInfo vertStage{};
    vertStage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    vertStage.stage = VK_SHADER_STAGE_VERTEX_BIT;
    vertStage.module = vertShader_;
    vertStage.pName = "main";

    VkPipelineShaderStageCreateInfo stages[2] = { vertStage, fragStage };

    VkVertexInputBindingDescription binding{};
    binding.binding = 0;
    binding.stride = sizeof(TextVertex);
    binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    VkVertexInputAttributeDescription attrs[3]{};
    attrs[0].location = 0;
    attrs[0].binding = 0;
    attrs[0].format = VK_FORMAT_R32G32_SFLOAT;
    attrs[0].offset = offsetof(TextVertex, pos);
    attrs[1].location = 1;
    attrs[1].binding = 0;
    attrs[1].format = VK_FORMAT_R32G32_SFLOAT;
    attrs[1].offset = offsetof(TextVertex, uv);
    attrs[2].location = 2;
    attrs[2].binding = 0;
    attrs[2].format = VK_FORMAT_R32_SFLOAT;
    attrs[2].offset = offsetof(TextVertex, colorGlyph);

    VkPipelineVertexInputStateCreateInfo vertexInput{};
    vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertexInput.vertexBindingDescriptionCount = 1;
    vertexInput.pVertexBindingDescriptions = &binding;
    vertexInput.vertexAttributeDescriptionCount = 3;
    vertexInput.pVertexAttributeDescriptions = attrs;

    VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
    inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    inputAssembly.primitiveRestartEnable = VK_FALSE;

    VkPipelineViewportStateCreateInfo viewportState{};
    viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewportState.viewportCount = 1;
    viewportState.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo rasterizer{};
    rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
    rasterizer.lineWidth = 1.0f;
    rasterizer.cullMode = VK_CULL_MODE_NONE;
    rasterizer.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;

    VkPipelineMultisampleStateCreateInfo multisampling{};
    multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineColorBlendAttachmentState blendAttachment{};
    blendAttachment.blendEnable = VK_TRUE;
    blendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                     VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    blendAttachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    blendAttachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blendAttachment.colorBlendOp = VK_BLEND_OP_ADD;
    blendAttachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    blendAttachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blendAttachment.alphaBlendOp = VK_BLEND_OP_ADD;

    VkPipelineColorBlendStateCreateInfo colorBlending{};
    colorBlending.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    colorBlending.attachmentCount = 1;
    colorBlending.pAttachments = &blendAttachment;

    VkPushConstantRange pushRange{};
    pushRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    pushRange.offset = 0;
    pushRange.size = 48;

    VkPipelineLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.setLayoutCount = 1;
    layoutInfo.pSetLayouts = &descriptorSetLayout_;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &pushRange;

    if (vkCreatePipelineLayout(device_, &layoutInfo, nullptr, &rectPipelineLayout_) != VK_SUCCESS)
    {
        throw std::runtime_error("failed to create rect pipeline layout");
    }

    std::vector<VkDynamicState> dynamicStates = {
        VK_DYNAMIC_STATE_VIEWPORT,
        VK_DYNAMIC_STATE_SCISSOR,
    };

    VkPipelineDynamicStateCreateInfo dynamicState{};
    dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamicState.dynamicStateCount = static_cast<uint32_t>(dynamicStates.size());
    dynamicState.pDynamicStates = dynamicStates.data();

    VkGraphicsPipelineCreateInfo pipelineInfo{};
    pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipelineInfo.stageCount = 2;
    pipelineInfo.pStages = stages;
    pipelineInfo.pVertexInputState = &vertexInput;
    pipelineInfo.pInputAssemblyState = &inputAssembly;
    pipelineInfo.pViewportState = &viewportState;
    pipelineInfo.pRasterizationState = &rasterizer;
    pipelineInfo.pMultisampleState = &multisampling;
    pipelineInfo.pColorBlendState = &colorBlending;
    pipelineInfo.pDynamicState = &dynamicState;
    pipelineInfo.layout = rectPipelineLayout_;
    pipelineInfo.renderPass = renderPass_;
    pipelineInfo.subpass = 0;

    if (vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &rectPipeline_) != VK_SUCCESS)
    {
        throw std::runtime_error("failed to create rect graphics pipeline");
    }
}

void TextRenderer::createVertexBuffer()
{
    constexpr VkDeviceSize initialCapacity = 16 * 1024;
    createBuffer(physicalDevice_, device_, initialCapacity,
                 VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                 vertexBuffer_, vertexBufferMemory_);
    vertexBufferCapacity_ = static_cast<size_t>(initialCapacity);
    vkMapMemory(device_, vertexBufferMemory_, 0, vertexBufferCapacity_, 0, &vertexBufferMapped_);
}

void TextRenderer::ensureVertexBufferCapacity(size_t vertexCount)
{
    const size_t required = vertexCount * sizeof(TextVertex);
    if (required <= vertexBufferCapacity_) return;

    size_t newCapacity = vertexBufferCapacity_ == 0 ? 4096 : vertexBufferCapacity_;
    while (newCapacity < required) newCapacity *= 2;

    if (vertexBuffer_ != VK_NULL_HANDLE)
    {
        if (vertexBufferMapped_)
        {
            vkUnmapMemory(device_, vertexBufferMemory_);
            vertexBufferMapped_ = nullptr;
        }
        vkDestroyBuffer(device_, vertexBuffer_, nullptr);
        vkFreeMemory(device_, vertexBufferMemory_, nullptr);
    }

    createBuffer(physicalDevice_, device_, newCapacity,
                 VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                 vertexBuffer_, vertexBufferMemory_);
    vertexBufferCapacity_ = newCapacity;
    vkMapMemory(device_, vertexBufferMemory_, 0, vertexBufferCapacity_, 0, &vertexBufferMapped_);
}

std::vector<TextRenderer::ShapedGlyph> TextRenderer::shapeText(const std::string& text) const
{
    std::vector<ShapedGlyph> result;
    if (text.empty() || !hbFont_) return result;

    hb_buffer_t* buffer = hb_buffer_create();
    hb_buffer_add_utf8(buffer, text.c_str(), -1, 0, -1);
    hb_buffer_set_direction(buffer, HB_DIRECTION_LTR);
    hb_buffer_set_script(buffer, HB_SCRIPT_LATIN);
    hb_buffer_set_language(buffer, hb_language_from_string("en", -1));
    hb_shape(hbFont_, buffer, nullptr, 0);

    unsigned int glyphCount = 0;
    const hb_glyph_info_t* glyphInfos = hb_buffer_get_glyph_infos(buffer, &glyphCount);
    const hb_glyph_position_t* glyphPositions = hb_buffer_get_glyph_positions(buffer, &glyphCount);

    result.reserve(glyphCount);

    float penX = 0.0f;
    float penY = 0.0f;

    for (unsigned int i = 0; i < glyphCount; ++i)
    {
        const hb_glyph_info_t& info = glyphInfos[i];
        const hb_glyph_position_t& pos = glyphPositions[i];

        ShapedGlyph sg{};
        sg.glyphIndex = info.codepoint;
        sg.emojiGlyphIndex = 0;
        sg.isColorGlyph = false;
        sg.useColrV1 = false;
        sg.width = 0.0f;
        sg.height = 0.0f;
        sg.bearingX = 0.0f;
        sg.bearingY = 0.0f;
        sg.uvMin[0] = 0.0f;
        sg.uvMin[1] = 0.0f;
        sg.uvMax[0] = 0.0f;
        sg.uvMax[1] = 0.0f;

        if (info.codepoint != 0)
        {
            auto it = glyphs_.find(info.codepoint);
            if (it != glyphs_.end())
            {
                const Glyph& gl = it->second;
                sg.width = gl.size[0];
                sg.height = gl.size[1];
                sg.bearingX = gl.bearing[0];
                sg.bearingY = gl.bearing[1];
                sg.uvMin[0] = gl.uvMin[0];
                sg.uvMin[1] = gl.uvMin[1];
                sg.uvMax[0] = gl.uvMax[0];
                sg.uvMax[1] = gl.uvMax[1];
            }
            sg.x = penX + static_cast<float>(pos.x_offset) / 64.0f;
            sg.y = penY + static_cast<float>(pos.y_offset) / 64.0f;
            sg.advance = static_cast<float>(pos.x_advance) / 64.0f;

            penX += static_cast<float>(pos.x_advance) / 64.0f;
            penY += static_cast<float>(pos.y_advance) / 64.0f;
        }
        else if (emojiFace_)
        {
            uint32_t clusterCp = 0;
            if (info.cluster < text.size())
            {
                clusterCp = static_cast<unsigned char>(text[info.cluster]);
                if ((clusterCp & 0x80) != 0)
                {
                    clusterCp = static_cast<uint32_t>(text[info.cluster]);
                }
            }

            FT_UInt emojiGid = FT_Get_Char_Index(emojiFace_, clusterCp);
            if (emojiGid != 0)
            {
                if (FT_Load_Glyph(emojiFace_, emojiGid, FT_LOAD_RENDER | FT_LOAD_COLOR) == 0)
                {
                    const FT_GlyphSlot eg = emojiFace_->glyph;
                    sg.width = static_cast<float>(eg->bitmap.width);
                    sg.height = static_cast<float>(eg->bitmap.rows);
                    sg.bearingX = static_cast<float>(eg->bitmap_left);
                    sg.bearingY = static_cast<float>(eg->bitmap_top);
                    sg.advance = static_cast<float>(eg->advance.x) / 64.0f;
                    sg.emojiGlyphIndex = emojiGid;
                    sg.x = penX;
                    sg.y = penY;
                    penX += sg.advance;
                }
            }
        }

        result.push_back(sg);
    }

    hb_buffer_destroy(buffer);
    return result;
}

float TextRenderer::measureText(const std::string& text) const
{
    float width = 0.0f;
    std::vector<ShapedGlyph> shaped = shapeText(text);
    for (const auto& sg : shaped)
    {
        width += sg.advance;
    }
    return width;
}

void TextRenderer::drawText(VkCommandBuffer commandBuffer,
                            const std::string& text,
                            float x,
                            float y,
                            float r,
                            float g,
                            float b,
                            float a)
{
    if (text.empty()) return;

    std::vector<ShapedGlyph> shaped = shapeText(text);
    if (shaped.empty()) return;

    for (auto& sg : shaped)
    {
        if (sg.isColorGlyph) continue;

        if (colrV1_)
        {
            if (colrV1_->hasGlyph(sg.glyphIndex))
            {
                sg.useColrV1 = true;
                continue;
            }
            if (colrVersion_ == 2 &&
                colrV1_->ensureGlyph(sg.glyphIndex, ftFace_))
            {
                sg.useColrV1 = true;
                continue;
            }
        }

        bool inColorAtlas = false;
        uint32_t colorKey = 0;
        if (sg.emojiGlyphIndex != 0 && emojiFace_)
        {
            inColorAtlas = colorGlyphs_.find(sg.emojiGlyphIndex) != colorGlyphs_.end();
            if (!inColorAtlas)
            {
                inColorAtlas = ensureColorGlyphFromFace(sg.emojiGlyphIndex, emojiFace_);
            }
            if (inColorAtlas) colorKey = sg.emojiGlyphIndex;
        }
        if (!inColorAtlas && hasColorGlyphs_)
        {
            inColorAtlas = ensureColorGlyph(sg.glyphIndex);
            if (inColorAtlas) colorKey = sg.glyphIndex;
        }
        if (inColorAtlas)
        {
            const ColorGlyphInfo& cl = colorGlyphs_.at(colorKey);
            sg.width = cl.size[0];
            sg.height = cl.size[1];
            sg.bearingX = cl.bearing[0];
            sg.bearingY = cl.bearing[1];
            sg.uvMin[0] = cl.uvMin[0];
            sg.uvMin[1] = cl.uvMin[1];
            sg.uvMax[0] = cl.uvMax[0];
            sg.uvMax[1] = cl.uvMax[1];
            sg.isColorGlyph = true;
        }
    }

    size_t atlasCount = 0;
    for (const auto& sg : shaped)
    {
        if (sg.useColrV1) continue;
        if (sg.width <= 0.0f || sg.height <= 0.0f) continue;
        ++atlasCount;
    }

    if (atlasCount > 0)
    {
        ensureVertexBufferCapacity(atlasCount * 6);

        auto* out = static_cast<TextVertex*>(vertexBufferMapped_);
        size_t vertexCount = 0;

        for (const auto& sg : shaped)
        {
            if (sg.useColrV1) continue;
            if (sg.width <= 0.0f || sg.height <= 0.0f) continue;

            const float x0 = x + sg.x + sg.bearingX;
            const float y0 = y + sg.y - sg.bearingY;
            const float w = sg.width;
            const float h = sg.height;
            writeQuad(out + vertexCount, x0, y0, w, h,
                      sg.uvMin[0], sg.uvMin[1], sg.uvMax[0], sg.uvMax[1],
                      sg.isColorGlyph ? 1.0f : 0.0f);
            vertexCount += 6;
        }

        if (colorAtlasDirty_)
        {
            uploadColorAtlasImage();
            colorAtlasDirty_ = false;
        }

        PushConstants pc{};
        pc.screenSize[0] = static_cast<float>(screenWidth_);
        pc.screenSize[1] = static_cast<float>(screenHeight_);
        pc.color[0] = r;
        pc.color[1] = g;
        pc.color[2] = b;
        pc.color[3] = a;
        pc.isRect = 0.0f;

        vkCmdPushConstants(commandBuffer, pipelineLayout_,
                           VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                           0, sizeof(PushConstants), &pc);

        vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);

        VkBuffer vertexBuffers[] = { vertexBuffer_ };
        VkDeviceSize offsets[] = { 0 };
        vkCmdBindVertexBuffers(commandBuffer, 0, 1, vertexBuffers, offsets);

        vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                pipelineLayout_, 0, 1, &descriptorSet_, 0, nullptr);

        vkCmdDraw(commandBuffer, static_cast<uint32_t>(vertexCount), 1, 0, 0);
    }

    if (colrV1_)
    {
        for (const auto& sg : shaped)
        {
            if (!sg.useColrV1) continue;
            colrV1_->drawGlyph(commandBuffer, sg.glyphIndex,
                               x + sg.x, y + sg.y,
                               1.0f,
                               static_cast<float>(screenWidth_),
                               static_cast<float>(screenHeight_));
        }
    }
}

void TextRenderer::drawCenteredText(VkCommandBuffer commandBuffer,
                                    const std::string& text,
                                    float r,
                                    float g,
                                    float b,
                                    float a)
{
    const float textWidth = measureText(text);
    const float x = (static_cast<float>(screenWidth_) - textWidth) * 0.5f;
    const float ascent = static_cast<float>(fontPixelSize_);
    const float y = (static_cast<float>(screenHeight_) + ascent) * 0.5f;
    drawText(commandBuffer, text, x, y, r, g, b, a);
}

void TextRenderer::writeQuad(TextVertex* out,
                             float x, float y, float w, float h,
                             float u0, float v0, float u1, float v1,
                             float colorGlyph)
{
    const float x0 = x;
    const float y0 = y;
    const float x1 = x + w;
    const float y1 = y + h;

    out[0] = {{x0, y0}, {u0, v0}, colorGlyph};
    out[1] = {{x1, y0}, {u1, v0}, colorGlyph};
    out[2] = {{x1, y1}, {u1, v1}, colorGlyph};

    out[3] = {{x0, y0}, {u0, v0}, colorGlyph};
    out[4] = {{x1, y1}, {u1, v1}, colorGlyph};
    out[5] = {{x0, y1}, {u0, v1}, colorGlyph};
}

void TextRenderer::drawRect(VkCommandBuffer commandBuffer,
                            float x,
                            float y,
                            float w,
                            float h,
                            float r,
                            float g,
                            float b,
                            float a)
{
    ensureVertexBufferCapacity(6);

    auto* out = static_cast<TextVertex*>(vertexBufferMapped_);
    writeQuad(out, x, y, w, h, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f);

    PushConstants pc{};
    pc.screenSize[0] = static_cast<float>(screenWidth_);
    pc.screenSize[1] = static_cast<float>(screenHeight_);
    pc.color[0] = r;
    pc.color[1] = g;
    pc.color[2] = b;
    pc.color[3] = a;
    pc.isRect = 1.0f;

    vkCmdPushConstants(commandBuffer, pipelineLayout_,
                       VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                       0, sizeof(PushConstants), &pc);

    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);

    VkBuffer vertexBuffers[] = { vertexBuffer_ };
    VkDeviceSize offsets[] = { 0 };
    vkCmdBindVertexBuffers(commandBuffer, 0, 1, vertexBuffers, offsets);

    vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            pipelineLayout_, 0, 1, &descriptorSet_, 0, nullptr);

    vkCmdDraw(commandBuffer, 6, 1, 0, 0);
}
