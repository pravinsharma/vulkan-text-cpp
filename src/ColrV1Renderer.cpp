#include "ColrV1Renderer.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

#include <tesselator.h>

#include <freetype/tttables.h>

#include "colrv1.vert.spv.h"
#include "colrv1.frag.spv.h"

namespace
{

constexpr float kF2Dot14 = 1.0f / 16384.0f;

void flattenQuadratic(const std::array<float, 2>& p0,
                      const std::array<float, 2>& p1,
                      const std::array<float, 2>& p2,
                      std::vector<std::array<float, 2>>& out,
                      int depth = 0)
{
    constexpr float kTol = 0.25f;
    constexpr int kMaxDepth = 8;
    if (depth >= kMaxDepth)
    {
        out.push_back(p2);
        return;
    }
    const float dx = p1[0] - p0[0];
    const float dy = p1[1] - p0[1];
    const float ex = p2[0] - p1[0];
    const float ey = p2[1] - p1[1];
    const float cross = std::abs(dx * ey - dy * ex);
    if (cross < kTol)
    {
        out.push_back(p2);
        return;
    }
    const std::array<float, 2> m01 = { (p0[0] + p1[0]) * 0.5f, (p0[1] + p1[1]) * 0.5f };
    const std::array<float, 2> m12 = { (p1[0] + p2[0]) * 0.5f, (p1[1] + p2[1]) * 0.5f };
    const std::array<float, 2> mid = { (m01[0] + m12[0]) * 0.5f, (m01[1] + m12[1]) * 0.5f };
    flattenQuadratic(p0, m01, mid, out, depth + 1);
    flattenQuadratic(mid, m12, p2, out, depth + 1);
}

void flattenCubic(const std::array<float, 2>& p0,
                  const std::array<float, 2>& p1,
                  const std::array<float, 2>& p2,
                  const std::array<float, 2>& p3,
                  std::vector<std::array<float, 2>>& out,
                  int depth = 0)
{
    constexpr float kTol = 0.5f;
    constexpr int kMaxDepth = 8;
    if (depth >= kMaxDepth)
    {
        out.push_back(p3);
        return;
    }
    const float d1x = p1[0] - p0[0];
    const float d1y = p1[1] - p0[1];
    const float d2x = p2[0] - p3[0];
    const float d2y = p2[1] - p3[1];
    const float cross = std::abs(d1x * d2y - d1y * d2x);
    if (cross < kTol)
    {
        out.push_back(p3);
        return;
    }
    const std::array<float, 2> m01 = { (p0[0] + p1[0]) * 0.5f, (p0[1] + p1[1]) * 0.5f };
    const std::array<float, 2> m12 = { (p1[0] + p2[0]) * 0.5f, (p1[1] + p2[1]) * 0.5f };
    const std::array<float, 2> m23 = { (p2[0] + p3[0]) * 0.5f, (p2[1] + p3[1]) * 0.5f };
    const std::array<float, 2> m012 = { (m01[0] + m12[0]) * 0.5f, (m01[1] + m12[1]) * 0.5f };
    const std::array<float, 2> m123 = { (m12[0] + m23[0]) * 0.5f, (m12[1] + m23[1]) * 0.5f };
    const std::array<float, 2> mid = { (m012[0] + m123[0]) * 0.5f, (m012[1] + m123[1]) * 0.5f };
    flattenCubic(p0, m01, m012, mid, out, depth + 1);
    flattenCubic(mid, m123, m23, p3, out, depth + 1);
}

inline int16_t readI16(const std::vector<uint8_t>& buf, size_t off)
{
    return static_cast<int16_t>((buf[off] << 8) | buf[off + 1]);
}

inline uint16_t readU16(const std::vector<uint8_t>& buf, size_t off)
{
    return static_cast<uint16_t>((buf[off] << 8) | buf[off + 1]);
}

inline int32_t readI32(const std::vector<uint8_t>& buf, size_t off)
{
    return static_cast<int32_t>(
        (static_cast<uint32_t>(buf[off]) << 24) |
        (static_cast<uint32_t>(buf[off + 1]) << 16) |
        (static_cast<uint32_t>(buf[off + 2]) << 8) |
        (static_cast<uint32_t>(buf[off + 3])));
}

inline uint32_t readU32(const std::vector<uint8_t>& buf, size_t off)
{
    return (static_cast<uint32_t>(buf[off]) << 24) |
           (static_cast<uint32_t>(buf[off + 1]) << 16) |
           (static_cast<uint32_t>(buf[off + 2]) << 8) |
           (static_cast<uint32_t>(buf[off + 3]));
}

inline float readF16Dot14(const std::vector<uint8_t>& buf, size_t off)
{
    return static_cast<float>(static_cast<int16_t>((buf[off] << 8) | buf[off + 1])) * kF2Dot14;
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

struct CpalPalette
{
    std::vector<std::array<uint8_t, 4>> entries;
};

bool loadCpal(FT_Face face, CpalPalette& out)
{
    FT_ULong length = 0;
    if (FT_Load_Sfnt_Table(face, FT_MAKE_TAG('C','P','A','L'), 0, nullptr, &length) != 0 || length < 12)
    {
        return false;
    }
    std::vector<uint8_t> buf(length);
    if (FT_Load_Sfnt_Table(face, FT_MAKE_TAG('C','P','A','L'), 0, buf.data(), &length) != 0)
    {
        return false;
    }

    const uint16_t version = readU16(buf, 0);
    const uint16_t numEntries = readU16(buf, 2);
    const uint16_t numPalettes = readU16(buf, 4);
    (void)version;
    (void)numPalettes;

    const uint16_t colorRecordArrayOffset = readU16(buf, 6);
    const uint16_t colorRecordSize = readU16(buf, 8);
    if (colorRecordSize < 4) return false;

    out.entries.resize(numEntries);
    for (uint16_t i = 0; i < numEntries; ++i)
    {
        const size_t off = colorRecordArrayOffset + static_cast<size_t>(i) * colorRecordSize;
        out.entries[i][0] = buf[off + 0];
        out.entries[i][1] = buf[off + 1];
        out.entries[i][2] = buf[off + 2];
        out.entries[i][3] = (off + 3 < buf.size()) ? buf[off + 3] : 255;
    }
    return true;
}

bool readColorLine(const std::vector<uint8_t>& colr,
                   size_t colorLineOffset,
                   const CpalPalette& palette,
                   std::vector<ColrV1GradientStop>& outStops,
                   ColrV1Extend& outExtend)
{
    if (colorLineOffset + 4 > colr.size()) return false;
    const uint16_t extend = readU16(colr, colorLineOffset);
    outExtend = static_cast<ColrV1Extend>(std::min<uint16_t>(extend, 2));

    const uint16_t numStops = readU16(colr, colorLineOffset + 2);
    if (colorLineOffset + 4 + static_cast<size_t>(numStops) * 4 > colr.size())
    {
        return false;
    }
    outStops.clear();
    outStops.reserve(numStops);
    for (uint16_t i = 0; i < numStops; ++i)
    {
        const size_t soff = colorLineOffset + 4 + static_cast<size_t>(i) * 4;
        ColrV1GradientStop s;
        s.position = readF16Dot14(colr, soff);
        const uint8_t paletteIndex = colr[soff + 2];
        const uint8_t alpha = colr[soff + 3];
        if (paletteIndex < palette.entries.size())
        {
            const auto& e = palette.entries[paletteIndex];
            s.color[0] = e[0] / 255.0f;
            s.color[1] = e[1] / 255.0f;
            s.color[2] = e[2] / 255.0f;
            s.color[3] = (e[3] / 255.0f) * (alpha / 255.0f);
        }
        else
        {
            s.color[0] = 0.0f; s.color[1] = 0.0f; s.color[2] = 0.0f; s.color[3] = 0.0f;
        }
        outStops.push_back(s);
    }
    return true;
}

struct ColrData
{
    std::vector<uint8_t> buf;
    uint16_t version = 0;
    bool hasV1 = false;
    size_t baseGlyphListOffset = 0;
    size_t layerListOffset = 0;
    size_t clipListOffset = 0;
    size_t clipBoxOffset = 0;
    bool hasClipBox = false;
};

int32_t lookupBaseGlyphLayers(const ColrData& colr, uint32_t glyphIndex)
{
    if (colr.baseGlyphListOffset + 4 > colr.buf.size()) return -1;
    const uint32_t numRecords = readU32(colr.buf, colr.baseGlyphListOffset);
    const size_t recBase = colr.baseGlyphListOffset + 4;
    for (uint32_t i = 0; i < numRecords; ++i)
    {
        const size_t off = recBase + static_cast<size_t>(i) * 6;
        if (off + 6 > colr.buf.size()) return -1;
        const uint16_t gid = readU16(colr.buf, off);
        if (gid == glyphIndex)
        {
            return static_cast<int32_t>(readU32(colr.buf, off + 2));
        }
    }
    return -1;
}

bool readLayerRecord(const ColrData& colr,
                     uint32_t layerIndex,
                     uint16_t& outGlyph,
                     uint32_t& outPaintOffset)
{
    if (colr.layerListOffset + 4 > colr.buf.size()) return false;
    const uint32_t numRecords = readU32(colr.buf, colr.layerListOffset);
    if (layerIndex >= numRecords) return false;
    const size_t off = colr.layerListOffset + 4 + static_cast<size_t>(layerIndex) * 6;
    if (off + 6 > colr.buf.size()) return false;
    outGlyph = readU16(colr.buf, off);
    outPaintOffset = readU32(colr.buf, off + 2);
    return true;
}

bool tessellateSubGlyph(FT_Face face,
                        uint32_t glyphIndex,
                        std::vector<ColrV1Vertex>& appendVertices,
                        std::vector<uint32_t>& appendIndices,
                        uint32_t& outIndexOffset,
                        uint32_t& outIndexCount)
{
    outIndexOffset = static_cast<uint32_t>(appendIndices.size());
    outIndexCount = 0;

    if (FT_Load_Glyph(face, glyphIndex, FT_LOAD_NO_BITMAP) != 0)
    {
        return false;
    }

    const FT_GlyphSlot slot = face->glyph;
    const FT_Outline& outline = slot->outline;
    const int nContours = outline.n_contours;
    const int nPoints = outline.n_points;
    if (nContours <= 0 || nPoints <= 0)
    {
        return true;
    }
    const unsigned char* tags = reinterpret_cast<const unsigned char*>(outline.tags);
    const FT_Vector* pts = outline.points;
    const unsigned short* contours = reinterpret_cast<const unsigned short*>(outline.contours);

    std::vector<std::vector<std::array<float, 2>>> polylines;
    polylines.reserve(nContours);
    for (int c = 0; c < nContours; ++c)
    {
        const int end = contours[c];
        const int start = (c == 0) ? 0 : contours[c - 1] + 1;
        std::vector<std::array<float, 2>> poly;

        auto onCurve = [&](int i) -> bool { return (tags[i] & 0x01) != 0; };
        auto isCubic = [&](int i) -> bool { return (tags[i] & 0x02) != 0; };

        int firstOn = -1;
        for (int i = start; i <= end; ++i)
        {
            if (onCurve(i)) { firstOn = i; break; }
        }
        if (firstOn < 0) continue;
        poly.push_back({ static_cast<float>(pts[firstOn].x) / 64.0f,
                         static_cast<float>(pts[firstOn].y) / 64.0f });

        auto getPt = [&](int i) -> std::array<float, 2> {
            return { static_cast<float>(pts[i].x) / 64.0f, static_cast<float>(pts[i].y) / 64.0f };
        };

        int prev = firstOn;
        int i = (firstOn + 1 > end) ? start : firstOn + 1;
        const int guard = (end - start + 1) * 4 + 16;
        int iter = 0;
        while (iter++ < guard)
        {
            if (onCurve(i))
            {
                poly.push_back(getPt(i));
                prev = i;
                i = (i + 1 > end) ? start : i + 1;
                if (i == firstOn) break;
            }
            else if (isCubic(i))
            {
                int c1i = i;
                int c2i = (i + 1 > end) ? start : i + 1;
                int endi = (c2i + 1 > end) ? start : c2i + 1;
                int search = endi;
                int safety = 0;
                while (!onCurve(search) && safety++ < guard)
                {
                    search = (search + 1 > end) ? start : search + 1;
                }
                if (!onCurve(search))
                {
                    break;
                }
                endi = search;
                std::vector<std::array<float, 2>> q;
                q.push_back(getPt(prev));
                flattenCubic(getPt(prev), getPt(c1i), getPt(c2i), getPt(endi), q);
                for (size_t k = 1; k < q.size(); ++k) poly.push_back(q[k]);
                prev = endi;
                i = (endi + 1 > end) ? start : endi + 1;
                if (i == firstOn) break;
            }
            else
            {
                int ctrl = i;
                int next = (i + 1 > end) ? start : i + 1;
                int safety = 0;
                while (!onCurve(next) && safety++ < guard)
                {
                    next = (next + 1 > end) ? start : next + 1;
                }
                if (!onCurve(next)) break;
                std::vector<std::array<float, 2>> q;
                q.push_back(getPt(prev));
                flattenQuadratic(getPt(prev), getPt(ctrl), getPt(next), q);
                for (size_t k = 1; k < q.size(); ++k) poly.push_back(q[k]);
                prev = next;
                i = (next + 1 > end) ? start : next + 1;
                if (i == firstOn) break;
            }
        }
        if (poly.size() >= 3) polylines.push_back(std::move(poly));
    }

    if (polylines.empty())
    {
        return true;
    }

    TESStesselator* tess = tessNewTess(nullptr);
    if (!tess)
    {
        return false;
    }
    tessSetOption(tess, TESS_CONSTRAINED_DELAUNAY_TRIANGULATION, 1);
    for (auto& poly : polylines)
    {
        std::vector<TESSreal> coords(poly.size() * 2);
        for (size_t k = 0; k < poly.size(); ++k)
        {
            coords[k * 2 + 0] = poly[k][0];
            coords[k * 2 + 1] = poly[k][1];
        }
        tessAddContour(tess, 2, coords.data(), sizeof(TESSreal) * 2, static_cast<int>(poly.size()));
    }
    if (!tessTesselate(tess, TESS_WINDING_ODD, TESS_POLYGONS, 3, 2, nullptr))
    {
        tessDeleteTess(tess);
        return false;
    }
    const TESSreal* verts = tessGetVertices(tess);
    const int nVerts = tessGetVertexCount(tess);
    const TESSindex* idx = tessGetElements(tess);
    const int nIdx = tessGetElementCount(tess) * 3;

    const uint32_t baseVertex = static_cast<uint32_t>(appendVertices.size());
    appendVertices.reserve(appendVertices.size() + nVerts);
    for (int v = 0; v < nVerts; ++v)
    {
        ColrV1Vertex cv{};
        cv.pos[0] = verts[v * 2 + 0];
        cv.pos[1] = verts[v * 2 + 1];
        cv.uv[0] = verts[v * 2 + 0];
        cv.uv[1] = verts[v * 2 + 1];
        appendVertices.push_back(cv);
    }
    appendIndices.reserve(appendIndices.size() + nIdx);
    for (int k = 0; k < nIdx; ++k)
    {
        appendIndices.push_back(baseVertex + idx[k]);
    }
    tessDeleteTess(tess);

    outIndexCount = static_cast<uint32_t>(nIdx);
    return true;
}

bool loadColr(FT_Face face, ColrData& out)
{
    FT_ULong length = 0;
    if (FT_Load_Sfnt_Table(face, FT_MAKE_TAG('C','O','L','R'), 0, nullptr, &length) != 0 || length < 14)
    {
        return false;
    }
    out.buf.resize(length);
    if (FT_Load_Sfnt_Table(face, FT_MAKE_TAG('C','O','L','R'), 0, out.buf.data(), &length) != 0)
    {
        return false;
    }
    out.version = readU16(out.buf, 0);
    if (out.version == 0)
    {
        out.hasV1 = false;
        return true;
    }
    if (out.version == 1 && length >= 34)
    {
        out.hasV1 = true;
        out.baseGlyphListOffset = static_cast<size_t>(readU32(out.buf, 14));
        out.layerListOffset = static_cast<size_t>(readU32(out.buf, 18));
        out.clipListOffset = static_cast<size_t>(readU32(out.buf, 30));
        out.clipBoxOffset = 0;
        out.hasClipBox = false;
        return true;
    }
    return false;
}

struct PaintNode
{
    uint8_t format = 0;
    size_t baseOffset = 0;
    std::array<uint8_t, 64> raw{};
    size_t rawSize = 0;
    std::vector<ColrV1GradientStop> stops;
    ColrV1Extend extend = ColrV1Extend::Pad;
    bool valid = true;
};

struct PaintRef
{
    size_t offset = 0;
};

struct ResolvedPaint
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
    std::vector<ColrV1GradientStop> stops;
    bool isValid = true;

    ColrV1Paint toPaint() const
    {
        ColrV1Paint p{};
        p.type = type;
        std::memcpy(p.color, color, sizeof(color));
        std::memcpy(p.p0, p0, sizeof(p0));
        std::memcpy(p.p1, p1, sizeof(p1));
        std::memcpy(p.p2, p2, sizeof(p2));
        p.radius0 = radius0;
        p.radius1 = radius1;
        p.extend = extend;
        p.stopOffset = stopOffset;
        p.stopCount = static_cast<uint32_t>(stops.size());
        return p;
    }
};

struct PaintTransform
{
    float xx = 1, xy = 0, yx = 0, yy = 1, dx = 0, dy = 0;
};

bool resolveSolidColor(const std::vector<uint8_t>& colr,
                       size_t off,
                       const CpalPalette& palette,
                       float alpha,
                       float outColor[4])
{
    const uint16_t palIdx = readU16(colr, off);
    const uint16_t palEntry = readU16(colr, off + 2);
    (void)palEntry;
    if (palIdx == 0xFFFF)
    {
        outColor[0] = 0; outColor[1] = 0; outColor[2] = 0; outColor[3] = alpha;
        return true;
    }
    const size_t idx = static_cast<size_t>(palIdx);
    if (idx >= palette.entries.size()) return false;
    const auto& e = palette.entries[idx];
    outColor[0] = e[0] / 255.0f;
    outColor[1] = e[1] / 255.0f;
    outColor[2] = e[2] / 255.0f;
    outColor[3] = (e[3] / 255.0f) * alpha;
    return true;
}

bool resolveGradient(const std::vector<uint8_t>& colr,
                     size_t off,
                     const CpalPalette& palette,
                     ResolvedPaint& out)
{
    if (off + 18 > colr.size()) return false;
    const uint16_t colorLineOffset = readU16(colr, off);

    std::vector<ColrV1GradientStop> stops;
    ColrV1Extend extend = ColrV1Extend::Pad;
    if (!readColorLine(colr, colorLineOffset, palette, stops, extend))
    {
        return false;
    }
    out.extend = extend;
    out.stops = std::move(stops);
    out.p0[0] = readF16Dot14(colr, off + 2);
    out.p0[1] = readF16Dot14(colr, off + 4);
    return true;
}

bool resolveSweep(const std::vector<uint8_t>& colr,
                  size_t off,
                  const CpalPalette& palette,
                  ResolvedPaint& out)
{
    if (off + 16 > colr.size()) return false;
    const uint16_t colorLineOffset = readU16(colr, off);
    std::vector<ColrV1GradientStop> stops;
    ColrV1Extend extend = ColrV1Extend::Pad;
    if (!readColorLine(colr, colorLineOffset, palette, stops, extend)) return false;
    out.extend = extend;
    out.stops = std::move(stops);
    out.p0[0] = readF16Dot14(colr, off + 2);
    out.p0[1] = readF16Dot14(colr, off + 4);
    out.p1[0] = readF16Dot14(colr, off + 6);
    out.p1[1] = readF16Dot14(colr, off + 8);
    return true;
}

bool resolveRadial(const std::vector<uint8_t>& colr,
                   size_t off,
                   const CpalPalette& palette,
                   ResolvedPaint& out)
{
    if (off + 18 > colr.size()) return false;
    const uint16_t colorLineOffset = readU16(colr, off);
    std::vector<ColrV1GradientStop> stops;
    ColrV1Extend extend = ColrV1Extend::Pad;
    if (!readColorLine(colr, colorLineOffset, palette, stops, extend)) return false;
    out.extend = extend;
    out.stops = std::move(stops);
    out.p0[0] = readF16Dot14(colr, off + 2);
    out.p0[1] = readF16Dot14(colr, off + 4);
    out.radius0 = readF16Dot14(colr, off + 6);
    out.p1[0] = readF16Dot14(colr, off + 8);
    out.p1[1] = readF16Dot14(colr, off + 10);
    out.radius1 = readF16Dot14(colr, off + 12);
    return true;
}

bool resolvePaintAt(const std::vector<uint8_t>& colr,
                    size_t off,
                    const CpalPalette& palette,
                    ResolvedPaint& out)
{
    if (off >= colr.size()) return false;
    const uint8_t format = colr[off];
    switch (format)
    {
        case 1:
        case 2:
        {
            if (off + 5 > colr.size()) return false;
            const float alpha = readF16Dot14(colr, off + 4);
            out.type = ColrV1PaintType::Solid;
            return resolveSolidColor(colr, off + 1, palette, alpha, out.color);
        }
        case 3:
        case 4:
        {
            out.type = ColrV1PaintType::LinearGradient;
            const bool ok = resolveGradient(colr, off + 1, palette, out);
            if (ok)
            {
                out.p1[0] = readF16Dot14(colr, off + 10);
                out.p1[1] = readF16Dot14(colr, off + 12);
            }
            return ok;
        }
        case 5:
        case 6:
        {
            out.type = ColrV1PaintType::RadialGradient;
            return resolveRadial(colr, off + 1, palette, out);
        }
        case 7:
        case 8:
        {
            out.type = ColrV1PaintType::SweepGradient;
            return resolveSweep(colr, off + 1, palette, out);
        }
        case 9:
        case 10:
        case 33:
        case 0:
        {
            out.isValid = false;
            return false;
        }
        default:
            return false;
    }
}

bool readTransform(const std::vector<uint8_t>& colr, size_t off, PaintTransform& t)
{
    if (off + 12 > colr.size()) return false;
    t.xx = readF16Dot14(colr, off + 0);
    t.xy = readF16Dot14(colr, off + 2);
    t.yx = readF16Dot14(colr, off + 4);
    t.yy = readF16Dot14(colr, off + 6);
    t.dx = readF16Dot14(colr, off + 8);
    t.dy = readF16Dot14(colr, off + 10);
    return true;
}

void mulTransform(const PaintTransform& a, const PaintTransform& b, PaintTransform& out)
{
    out.xx = a.xx * b.xx + a.xy * b.yx;
    out.xy = a.xx * b.xy + a.xy * b.yy;
    out.yx = a.yx * b.xx + a.yy * b.yx;
    out.yy = a.yx * b.xy + a.yy * b.yy;
    out.dx = a.xx * b.dx + a.xy * b.dy + a.dx;
    out.dy = a.yx * b.dx + a.yy * b.dy + a.dy;
}

struct DrawOpBuilder
{
    std::vector<ColrV1DrawOp>* ops;
    PaintTransform current;
    ColrV1Composite currentComposite = ColrV1Composite::SrcOver;
    std::vector<ColrV1GradientStop>* stopsAccum;
    bool* anyValid;
    bool* anyFallback;
    uint32_t meshIndexOffset = 0;
    uint32_t meshIndexCount = 0;
    FT_Face face = nullptr;
    const ColrData* colr = nullptr;
    std::vector<ColrV1Vertex>* appendVertices = nullptr;
    std::vector<uint32_t>* appendIndices = nullptr;
};

void flattenPaint(const std::vector<uint8_t>& colr,
                  size_t off,
                  const CpalPalette& palette,
                  const PaintTransform& parentT,
                  ColrV1Composite parentComp,
                  DrawOpBuilder& ctx);

void processLayerList(const ColrData& colr,
                     FT_Face face,
                     uint32_t firstLayerIndex,
                     uint32_t numLayers,
                     const CpalPalette& palette,
                     const PaintTransform& parentT,
                     ColrV1Composite parentComp,
                     std::vector<ColrV1DrawOp>& ops,
                     std::vector<ColrV1GradientStop>& stopsAccum,
                     bool& anyValid,
                     bool& anyFallback,
                     std::vector<ColrV1Vertex>& appendVertices,
                     std::vector<uint32_t>& appendIndices)
{
    if (colr.layerListOffset + 4 > colr.buf.size()) return;
    const uint32_t totalLayers = readU32(colr.buf, colr.layerListOffset);
    const uint32_t maxLayers = std::min(numLayers, totalLayers > firstLayerIndex
                                                        ? totalLayers - firstLayerIndex
                                                        : 0u);

    for (uint32_t i = 0; i < maxLayers; ++i)
    {
        uint16_t subGlyphID = 0;
        uint32_t paintOff = 0;
        if (!readLayerRecord(colr, firstLayerIndex + i, subGlyphID, paintOff)) break;
        if (subGlyphID == 0) break;

        uint32_t subOff = 0, subCount = 0;
        if (!tessellateSubGlyph(face, subGlyphID, appendVertices, appendIndices, subOff, subCount))
        {
            continue;
        }
        if (subCount == 0) continue;

        DrawOpBuilder child{};
        child.ops = &ops;
        child.stopsAccum = &stopsAccum;
        child.anyValid = &anyValid;
        child.anyFallback = &anyFallback;
        child.face = face;
        child.colr = &colr;
        child.appendVertices = &appendVertices;
        child.appendIndices = &appendIndices;
        child.meshIndexOffset = subOff;
        child.meshIndexCount = subCount;

        flattenPaint(colr.buf, paintOff, palette, parentT, parentComp, child);
    }
}

void flattenPaint(const std::vector<uint8_t>& colr,
                  size_t off,
                  const CpalPalette& palette,
                  const PaintTransform& parentT,
                  ColrV1Composite parentComp,
                  DrawOpBuilder& ctx)
{
    if (off >= colr.size()) return;
    const uint8_t format = colr[off];
    switch (format)
    {
        case 1:
        case 2:
        {
            ResolvedPaint p;
            if (resolvePaintAt(colr, off, palette, p) && p.type == ColrV1PaintType::Solid)
            {
                ColrV1DrawOp op{};
                op.indexOffset = ctx.meshIndexOffset;
                op.indexCount = ctx.meshIndexCount;
                op.transform[0] = parentT.xx;
                op.transform[1] = parentT.xy;
                op.transform[2] = parentT.yx;
                op.transform[3] = parentT.yy;
                op.transform[4] = parentT.dx;
                op.transform[5] = parentT.dy;
                op.paint = p.toPaint();
                op.composite = parentComp;
                op.useBackdrop = false;
                ctx.ops->push_back(op);
                *ctx.anyValid = true;
            }
            else
            {
                *ctx.anyFallback = true;
            }
            break;
        }
        case 3:
        case 4:
        case 5:
        case 6:
        case 7:
        case 8:
        {
            ResolvedPaint p;
            if (resolvePaintAt(colr, off, palette, p) && p.isValid)
            {
                p.stopOffset = static_cast<uint32_t>(ctx.stopsAccum->size());
                for (auto& s : p.stops) { ctx.stopsAccum->push_back(s); }
                ColrV1DrawOp op{};
                op.indexOffset = ctx.meshIndexOffset;
                op.indexCount = ctx.meshIndexCount;
                op.transform[0] = parentT.xx;
                op.transform[1] = parentT.xy;
                op.transform[2] = parentT.yx;
                op.transform[3] = parentT.yy;
                op.transform[4] = parentT.dx;
                op.transform[5] = parentT.dy;
                p.stopOffset = static_cast<uint32_t>(ctx.stopsAccum->size() - p.stops.size());
                op.paint = p.toPaint();
                op.composite = parentComp;
                op.useBackdrop = false;
                ctx.ops->push_back(op);
                *ctx.anyValid = true;
            }
            break;
        }
        case 11:
        case 12:
        {
            if (off + 5 > colr.size()) return;
            const uint16_t subOff = readU16(colr, off + 3);
            PaintTransform subT{};
            if (off + 5 + 12 <= colr.size())
            {
                readTransform(colr, off + 5, subT);
            }
            PaintTransform composed{};
            mulTransform(parentT, subT, composed);
            flattenPaint(colr, subOff, palette, composed, parentComp, ctx);
            break;
        }
        case 13:
        case 14:
        {
            if (off + 5 > colr.size()) return;
            const uint16_t subOff = readU16(colr, off + 1);
            const float dx = readF16Dot14(colr, off + 3);
            const float dy = readF16Dot14(colr, off + 5 == colr.size() ? off + 3 : off + 5);
            PaintTransform subT{};
            subT.dx = dx;
            subT.dy = dy;
            PaintTransform composed{};
            mulTransform(parentT, subT, composed);
            flattenPaint(colr, subOff, palette, composed, parentComp, ctx);
            break;
        }
        case 15:
        case 16:
        {
            if (off + 5 > colr.size()) return;
            const uint16_t subOff = readU16(colr, off + 1);
            const float sx = readF16Dot14(colr, off + 3);
            const float sy = readF16Dot14(colr, off + 5);
            PaintTransform subT{};
            subT.xx = sx; subT.yy = sy;
            PaintTransform composed{};
            mulTransform(parentT, subT, composed);
            flattenPaint(colr, subOff, palette, composed, parentComp, ctx);
            break;
        }
        case 19:
        case 20:
        {
            if (off + 3 > colr.size()) return;
            const uint16_t subOff = readU16(colr, off + 1);
            const float s = readF16Dot14(colr, off + 3);
            PaintTransform subT{};
            subT.xx = s; subT.yy = s;
            PaintTransform composed{};
            mulTransform(parentT, subT, composed);
            flattenPaint(colr, subOff, palette, composed, parentComp, ctx);
            break;
        }
        case 23:
        case 24:
        {
            if (off + 3 > colr.size()) return;
            const uint16_t subOff = readU16(colr, off + 1);
            const float a = readF16Dot14(colr, off + 3);
            const float c = std::cos(a);
            const float si = std::sin(a);
            PaintTransform subT{};
            subT.xx = c; subT.xy = -si; subT.yx = si; subT.yy = c;
            PaintTransform composed{};
            mulTransform(parentT, subT, composed);
            flattenPaint(colr, subOff, palette, composed, parentComp, ctx);
            break;
        }
        case 27:
        case 28:
        {
            if (off + 5 > colr.size()) return;
            const uint16_t subOff = readU16(colr, off + 1);
            const float sx = std::tan(readF16Dot14(colr, off + 3));
            const float sy = std::tan(readF16Dot14(colr, off + 5));
            PaintTransform subT{};
            subT.xy = sy; subT.yx = sx;
            PaintTransform composed{};
            mulTransform(parentT, subT, composed);
            flattenPaint(colr, subOff, palette, composed, parentComp, ctx);
            break;
        }
        case 31:
        case 32:
        {
            if (off + 7 > colr.size()) return;
            const uint16_t srcOff = readU16(colr, off + 1);
            const uint16_t mode = readU16(colr, off + 3);
            const uint16_t bdOff = readU16(colr, off + 5);
            (void)bdOff;
            ColrV1Composite comp = static_cast<ColrV1Composite>(std::min<uint16_t>(mode, 27));
            flattenPaint(colr, srcOff, palette, parentT, comp, ctx);
            break;
        }
        case 0:
        {
            if (off + 5 > colr.size()) return;
            if (!ctx.colr || !ctx.face || !ctx.appendVertices || !ctx.appendIndices) return;
            const uint32_t firstLayer = readU16(colr, off + 1);
            const uint32_t numLayers = readU16(colr, off + 3);
            processLayerList(*ctx.colr, ctx.face,
                             firstLayer, numLayers,
                             palette, parentT, parentComp,
                             *ctx.ops, *ctx.stopsAccum,
                             *ctx.anyValid, *ctx.anyFallback,
                             *ctx.appendVertices, *ctx.appendIndices);
            break;
        }
        default:
            *ctx.anyFallback = true;
            break;
    }
}

bool flattenGlyph(FT_Face face,
                  uint32_t glyphIndex,
                  const CpalPalette& palette,
                  const ColrData& colr,
                  ColrV1ColoredGlyph& out,
                  std::vector<ColrV1Vertex>& appendVertices,
                  std::vector<uint32_t>& appendIndices,
                  std::vector<ColrV1GradientStop>& stopsAccum)
{
    if (FT_Load_Glyph(face, glyphIndex, FT_LOAD_NO_BITMAP) != 0) return false;
    const FT_GlyphSlot baseSlot = face->glyph;
    out.bearing[0] = static_cast<float>(baseSlot->bitmap_left);
    out.bearing[1] = static_cast<float>(baseSlot->bitmap_top);
    out.advance = static_cast<float>(baseSlot->advance.x) / 64.0f;

    out.indexOffset = 0;
    out.indexCount = 0;
    out.ops.clear();

    const int32_t firstLayer = lookupBaseGlyphLayers(colr, glyphIndex);
    PaintTransform identityT{};

    if (firstLayer >= 0)
    {
        bool anyValid = false;
        bool anyFallback = false;
        processLayerList(colr, face,
                         static_cast<uint32_t>(firstLayer),
                         64,
                         palette, identityT, ColrV1Composite::SrcOver,
                         out.ops, stopsAccum,
                         anyValid, anyFallback,
                         appendVertices, appendIndices);

        if (anyValid)
        {
            if (!out.ops.empty())
            {
                out.indexOffset = out.ops.front().indexOffset;
                out.indexCount = out.ops.front().indexCount;
            }
            return true;
        }
    }

    uint32_t baseOff = 0, baseCount = 0;
    if (!tessellateSubGlyph(face, glyphIndex, appendVertices, appendIndices, baseOff, baseCount) ||
        baseCount == 0)
    {
        return true;
    }

    out.indexOffset = baseOff;
    out.indexCount = baseCount;

    ColrV1DrawOp op{};
    op.indexOffset = baseOff;
    op.indexCount = baseCount;
    op.transform[0] = 1.0f; op.transform[3] = 1.0f;
    op.paint.type = ColrV1PaintType::Solid;
    op.paint.color[0] = 1.0f; op.paint.color[1] = 1.0f;
    op.paint.color[2] = 1.0f; op.paint.color[3] = 1.0f;
    op.composite = ColrV1Composite::SrcOver;
    op.useBackdrop = false;
    out.ops.push_back(op);
    return true;
}

}

size_t ColrV1Renderer::PipelineKeyHash::operator()(const PipelineKey& k) const noexcept
{
    return std::hash<int>()(static_cast<int>(k.composite)) * 7 +
           (k.clip ? 1 : 0) * 13 +
           (k.backdrop ? 1 : 0) * 19;
}

ColrV1Renderer::~ColrV1Renderer()
{
    shutdown();
}

void ColrV1Renderer::init(const InitInfo& info)
{
    physicalDevice_ = info.physicalDevice;
    device_ = info.device;
    commandPool_ = info.commandPool;
    graphicsQueue_ = info.graphicsQueue;
    renderPass_ = info.renderPass;
    screenWidth_ = info.screenWidth;
    screenHeight_ = info.screenHeight;

    createVertexBuffer();
    createPipelines();
    createDescriptorResources();
}

void ColrV1Renderer::shutdown()
{
    if (device_ == VK_NULL_HANDLE) return;
    destroyPipelines();
    destroyBuffers();
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
    glyphs_.clear();
    gradientStops_.clear();
    device_ = VK_NULL_HANDLE;
}

void ColrV1Renderer::setScreenSize(uint32_t width, uint32_t height)
{
    screenWidth_ = width;
    screenHeight_ = height;
}

void ColrV1Renderer::destroyPipelines()
{
    for (auto& [k, p] : pipelines_)
    {
        vkDestroyPipeline(device_, p, nullptr);
    }
    pipelines_.clear();
    for (auto& [k, l] : pipelineLayouts_)
    {
        vkDestroyPipelineLayout(device_, l, nullptr);
    }
    pipelineLayouts_.clear();
}

void ColrV1Renderer::destroyBuffers()
{
    if (vertexBuffer_ != VK_NULL_HANDLE)
    {
        if (vertexBufferMapped_) { vkUnmapMemory(device_, vertexBufferMemory_); vertexBufferMapped_ = nullptr; }
        vkDestroyBuffer(device_, vertexBuffer_, nullptr);
        vkFreeMemory(device_, vertexBufferMemory_, nullptr);
        vertexBuffer_ = VK_NULL_HANDLE; vertexBufferMemory_ = VK_NULL_HANDLE;
    }
    if (indexBuffer_ != VK_NULL_HANDLE)
    {
        if (indexBufferMapped_) { vkUnmapMemory(device_, indexBufferMemory_); indexBufferMapped_ = nullptr; }
        vkDestroyBuffer(device_, indexBuffer_, nullptr);
        vkFreeMemory(device_, indexBufferMemory_, nullptr);
        indexBuffer_ = VK_NULL_HANDLE; indexBufferMemory_ = VK_NULL_HANDLE;
    }
    if (gradientStopsBuffer_ != VK_NULL_HANDLE)
    {
        if (gradientStopsMapped_) { vkUnmapMemory(device_, gradientStopsMemory_); gradientStopsMapped_ = nullptr; }
        vkDestroyBuffer(device_, gradientStopsBuffer_, nullptr);
        vkFreeMemory(device_, gradientStopsMemory_, nullptr);
        gradientStopsBuffer_ = VK_NULL_HANDLE; gradientStopsMemory_ = VK_NULL_HANDLE;
    }
    if (paintUboBuffer_ != VK_NULL_HANDLE)
    {
        if (paintUboMapped_) { vkUnmapMemory(device_, paintUboMemory_); paintUboMapped_ = nullptr; }
        vkDestroyBuffer(device_, paintUboBuffer_, nullptr);
        vkFreeMemory(device_, paintUboMemory_, nullptr);
        paintUboBuffer_ = VK_NULL_HANDLE; paintUboMemory_ = VK_NULL_HANDLE;
    }
}

void ColrV1Renderer::createDescriptorResources()
{
    VkDescriptorSetLayoutBinding paintBinding{};
    paintBinding.binding = 0;
    paintBinding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
    paintBinding.descriptorCount = 1;
    paintBinding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    paintBinding.pImmutableSamplers = nullptr;

    VkDescriptorSetLayoutBinding stopsBinding{};
    stopsBinding.binding = 1;
    stopsBinding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    stopsBinding.descriptorCount = 1;
    stopsBinding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    stopsBinding.pImmutableSamplers = nullptr;

    VkDescriptorSetLayoutBinding bindings[] = { paintBinding, stopsBinding };

    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = 2;
    layoutInfo.pBindings = bindings;

    if (vkCreateDescriptorSetLayout(device_, &layoutInfo, nullptr, &descriptorSetLayout_) != VK_SUCCESS)
    {
        throw std::runtime_error("colrv1: failed to create descriptor set layout");
    }

    VkDescriptorPoolSize poolSizes[2]{};
    poolSizes[0].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
    poolSizes[0].descriptorCount = 1;
    poolSizes[1].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    poolSizes[1].descriptorCount = 1;

    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.poolSizeCount = 2;
    poolInfo.pPoolSizes = poolSizes;
    poolInfo.maxSets = 1;

    if (vkCreateDescriptorPool(device_, &poolInfo, nullptr, &descriptorPool_) != VK_SUCCESS)
    {
        throw std::runtime_error("colrv1: failed to create descriptor pool");
    }

    VkDescriptorSetAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool = descriptorPool_;
    allocInfo.descriptorSetCount = 1;
    allocInfo.pSetLayouts = &descriptorSetLayout_;

    if (vkAllocateDescriptorSets(device_, &allocInfo, &descriptorSet_) != VK_SUCCESS)
    {
        throw std::runtime_error("colrv1: failed to allocate descriptor set");
    }

    VkDescriptorBufferInfo paintInfo{};
    paintInfo.buffer = paintUboBuffer_;
    paintInfo.offset = 0;
    paintInfo.range = sizeof(PaintUBO);

    VkDescriptorBufferInfo stopsInfo{};
    stopsInfo.buffer = gradientStopsBuffer_;
    stopsInfo.offset = 0;
    stopsInfo.range = gradientStopsCapacity_;

    VkWriteDescriptorSet writes[2]{};
    writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[0].dstSet = descriptorSet_;
    writes[0].dstBinding = 0;
    writes[0].dstArrayElement = 0;
    writes[0].descriptorCount = 1;
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
    writes[0].pBufferInfo = &paintInfo;

    writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[1].dstSet = descriptorSet_;
    writes[1].dstBinding = 1;
    writes[1].dstArrayElement = 0;
    writes[1].descriptorCount = 1;
    writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[1].pBufferInfo = &stopsInfo;

    vkUpdateDescriptorSets(device_, 2, writes, 0, nullptr);

    createDefaultPipeline();
}

void ColrV1Renderer::createDefaultPipeline()
{
    if (pipelines_.find(PipelineKey{ColrV1Composite::SrcOver, false, false}) != pipelines_.end())
    {
        return;
    }

    VkPipelineLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.setLayoutCount = 1;
    layoutInfo.pSetLayouts = &descriptorSetLayout_;
    VkPushConstantRange pushRange{};
    pushRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    pushRange.offset = 0;
    pushRange.size = sizeof(PushConstants);
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &pushRange;

    VkPipelineLayout layout = VK_NULL_HANDLE;
    if (vkCreatePipelineLayout(device_, &layoutInfo, nullptr, &layout) != VK_SUCCESS)
    {
        throw std::runtime_error("colrv1: failed to create pipeline layout");
    }
    pipelineLayouts_[PipelineKey{ColrV1Composite::SrcOver, false, false}] = layout;

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
    binding.stride = sizeof(ColrV1Vertex);
    binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    VkVertexInputAttributeDescription attrs[2]{};
    attrs[0].location = 0;
    attrs[0].binding = 0;
    attrs[0].format = VK_FORMAT_R32G32_SFLOAT;
    attrs[0].offset = offsetof(ColrV1Vertex, pos);
    attrs[1].location = 1;
    attrs[1].binding = 0;
    attrs[1].format = VK_FORMAT_R32G32_SFLOAT;
    attrs[1].offset = offsetof(ColrV1Vertex, uv);

    VkPipelineVertexInputStateCreateInfo vertexInput{};
    vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertexInput.vertexBindingDescriptionCount = 1;
    vertexInput.pVertexBindingDescriptions = &binding;
    vertexInput.vertexAttributeDescriptionCount = 2;
    vertexInput.pVertexAttributeDescriptions = attrs;

    VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
    inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo viewportState{};
    viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewportState.viewportCount = 1;
    viewportState.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo rasterizer{};
    rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
    rasterizer.cullMode = VK_CULL_MODE_NONE;
    rasterizer.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rasterizer.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo multisampling{};
    multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineColorBlendAttachmentState blendAttachment{};
    blendAttachment.blendEnable = VK_TRUE;
    blendAttachment.colorWriteMask = 0xF;
    blendAttachment.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
    blendAttachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blendAttachment.colorBlendOp = VK_BLEND_OP_ADD;
    blendAttachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    blendAttachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blendAttachment.alphaBlendOp = VK_BLEND_OP_ADD;

    VkPipelineColorBlendStateCreateInfo colorBlending{};
    colorBlending.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    colorBlending.attachmentCount = 1;
    colorBlending.pAttachments = &blendAttachment;

    std::vector<VkDynamicState> dyn = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dynamicState{};
    dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamicState.dynamicStateCount = static_cast<uint32_t>(dyn.size());
    dynamicState.pDynamicStates = dyn.data();

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
    pipelineInfo.layout = layout;
    pipelineInfo.renderPass = renderPass_;
    pipelineInfo.subpass = 0;

    VkPipeline pipeline = VK_NULL_HANDLE;
    if (vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &pipeline) != VK_SUCCESS)
    {
        throw std::runtime_error("colrv1: failed to create graphics pipeline");
    }
    pipelines_[PipelineKey{ColrV1Composite::SrcOver, false, false}] = pipeline;
}

void ColrV1Renderer::createPipelines()
{
    VkShaderModuleCreateInfo vertInfo{};
    vertInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    vertInfo.codeSize = colrv1_vert_spv.size() * sizeof(uint32_t);
    vertInfo.pCode = colrv1_vert_spv.data();
    if (vkCreateShaderModule(device_, &vertInfo, nullptr, &vertShader_) != VK_SUCCESS)
    {
        throw std::runtime_error("colrv1: failed to create vertex shader");
    }

    VkShaderModuleCreateInfo fragInfo{};
    fragInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    fragInfo.codeSize = colrv1_frag_spv.size() * sizeof(uint32_t);
    fragInfo.pCode = colrv1_frag_spv.data();
    if (vkCreateShaderModule(device_, &fragInfo, nullptr, &fragShader_) != VK_SUCCESS)
    {
        throw std::runtime_error("colrv1: failed to create fragment shader");
    }
}

void ColrV1Renderer::createVertexBuffer()
{
    constexpr VkDeviceSize initialVB = 16 * 1024 * sizeof(ColrV1Vertex);
    constexpr VkDeviceSize initialIB = 16 * 1024 * sizeof(uint32_t);
    constexpr VkDeviceSize initialSB = 4096 * sizeof(ColrV1GradientStop);
    constexpr VkDeviceSize initialPU = kPaintUboSlots * kPaintUboStride;

    createBuffer(physicalDevice_, device_, initialVB,
                 VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                 vertexBuffer_, vertexBufferMemory_);
    vertexBufferCapacity_ = static_cast<size_t>(initialVB);
    vkMapMemory(device_, vertexBufferMemory_, 0, vertexBufferCapacity_, 0, &vertexBufferMapped_);

    createBuffer(physicalDevice_, device_, initialIB,
                 VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                 indexBuffer_, indexBufferMemory_);
    indexBufferCapacity_ = static_cast<size_t>(initialIB);
    vkMapMemory(device_, indexBufferMemory_, 0, indexBufferCapacity_, 0, &indexBufferMapped_);

    createBuffer(physicalDevice_, device_, initialSB,
                 VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                 gradientStopsBuffer_, gradientStopsMemory_);
    gradientStopsCapacity_ = static_cast<size_t>(initialSB);
    vkMapMemory(device_, gradientStopsMemory_, 0, gradientStopsCapacity_, 0, &gradientStopsMapped_);

    createBuffer(physicalDevice_, device_, initialPU,
                 VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                 paintUboBuffer_, paintUboMemory_);
    paintUboCapacity_ = static_cast<size_t>(initialPU);
    vkMapMemory(device_, paintUboMemory_, 0, paintUboCapacity_, 0, &paintUboMapped_);
}

void ColrV1Renderer::ensureVertexBufferCapacity(size_t vertexCount, size_t indexCount)
{
    const size_t needVB = vertexCount * sizeof(ColrV1Vertex);
    if (needVB > vertexBufferCapacity_)
    {
        if (vertexBufferMapped_) { vkUnmapMemory(device_, vertexBufferMemory_); vertexBufferMapped_ = nullptr; }
        vkDestroyBuffer(device_, vertexBuffer_, nullptr);
        vkFreeMemory(device_, vertexBufferMemory_, nullptr);
        createBuffer(physicalDevice_, device_, needVB,
                     VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                     vertexBuffer_, vertexBufferMemory_);
        vertexBufferCapacity_ = needVB;
        vkMapMemory(device_, vertexBufferMemory_, 0, vertexBufferCapacity_, 0, &vertexBufferMapped_);
    }
    const size_t needIB = indexCount * sizeof(uint32_t);
    if (needIB > indexBufferCapacity_)
    {
        if (indexBufferMapped_) { vkUnmapMemory(device_, indexBufferMemory_); indexBufferMapped_ = nullptr; }
        vkDestroyBuffer(device_, indexBuffer_, nullptr);
        vkFreeMemory(device_, indexBufferMemory_, nullptr);
        createBuffer(physicalDevice_, device_, needIB,
                     VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                     indexBuffer_, indexBufferMemory_);
        indexBufferCapacity_ = needIB;
        vkMapMemory(device_, indexBufferMemory_, 0, indexBufferCapacity_, 0, &indexBufferMapped_);
    }
}

void ColrV1Renderer::uploadStaging()
{
    (void)0;
}

bool ColrV1Renderer::ensureGlyph(uint32_t glyphIndex, FT_Face face)
{
    if (glyphs_.find(glyphIndex) != glyphs_.end()) return true;
    if (!face) return false;

    CpalPalette palette;
    if (!loadCpal(face, palette))
    {
        return false;
    }
    ColrData colr;
    if (!loadColr(face, colr) || !colr.hasV1)
    {
        return false;
    }

    const size_t prevStopCount = gradientStops_.size();

    ColrV1ColoredGlyph out{};
    if (!flattenGlyph(face, glyphIndex, palette, colr, out,
                      pendingVertices_, pendingIndices_, gradientStops_))
    {
        return false;
    }

    if (gradientStops_.size() > prevStopCount)
    {
        const size_t newBytes = (gradientStops_.size() - prevStopCount) * sizeof(ColrV1GradientStop);
        if (gradientStopsSize_ + newBytes > gradientStopsCapacity_)
        {
            throw std::runtime_error("colrv1: gradient stops buffer overflow");
        }
        std::memcpy(static_cast<char*>(gradientStopsMapped_) + gradientStopsSize_,
                    gradientStops_.data() + prevStopCount,
                    newBytes);
        gradientStopsSize_ += newBytes;
    }

    const size_t needVB = pendingVertices_.size() * sizeof(ColrV1Vertex);
    const size_t needIB = pendingIndices_.size() * sizeof(uint32_t);
    ensureVertexBufferCapacity(pendingVertices_.size(), pendingIndices_.size());
    std::memcpy(vertexBufferMapped_, pendingVertices_.data(), needVB);
    std::memcpy(indexBufferMapped_, pendingIndices_.data(), needIB);
    vertexBufferSize_ = needVB;
    indexBufferSize_ = needIB;

    glyphs_[glyphIndex] = std::move(out);
    return true;
}

bool ColrV1Renderer::hasGlyph(uint32_t glyphIndex) const
{
    return glyphs_.find(glyphIndex) != glyphs_.end();
}

const ColrV1ColoredGlyph* ColrV1Renderer::getGlyph(uint32_t glyphIndex) const
{
    auto it = glyphs_.find(glyphIndex);
    if (it == glyphs_.end()) return nullptr;
    return &it->second;
}

bool ColrV1Renderer::drawGlyph(VkCommandBuffer commandBuffer,
                               uint32_t glyphIndex,
                               float x, float y,
                               float scale,
                               float screenWidth, float screenHeight)
{
    auto it = glyphs_.find(glyphIndex);
    if (it == glyphs_.end()) return false;
    const ColrV1ColoredGlyph& g = it->second;
    if (g.ops.empty() || g.indexCount == 0) return false;

    auto it2 = pipelines_.find(PipelineKey{g.ops[0].composite, false, false});
    if (it2 == pipelines_.end()) return false;
    VkPipeline pipeline = it2->second;

    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    VkDeviceSize vbOffset = 0;
    vkCmdBindVertexBuffers(commandBuffer, 0, 1, &vertexBuffer_, &vbOffset);
    vkCmdBindIndexBuffer(commandBuffer, indexBuffer_, 0, VK_INDEX_TYPE_UINT32);

    for (const ColrV1DrawOp& op : g.ops)
    {
        if (paintUboCursor_ >= kPaintUboSlots)
        {
            paintUboCursor_ = 0;
        }
        const uint32_t slot = paintUboCursor_++;
        PaintUBO ubo{};
        ubo.type = static_cast<int32_t>(op.paint.type);
        std::memcpy(ubo.color, op.paint.color, sizeof(ubo.color));
        std::memcpy(ubo.p0, op.paint.p0, sizeof(ubo.p0));
        std::memcpy(ubo.p1, op.paint.p1, sizeof(ubo.p1));
        std::memcpy(ubo.p2, op.paint.p2, sizeof(ubo.p2));
        ubo.radius0 = op.paint.radius0;
        ubo.radius1 = op.paint.radius1;
        ubo.extend = static_cast<int32_t>(op.paint.extend);
        ubo.composite = static_cast<int32_t>(op.composite);
        ubo.useBackdrop = op.useBackdrop ? 1 : 0;
        ubo.stopCount = static_cast<int32_t>(op.paint.stopCount);
        ubo.stopOffset = op.paint.stopOffset;

        const VkDeviceSize uboOffset = slot * kPaintUboStride;
        std::memcpy(static_cast<char*>(paintUboMapped_) + uboOffset, &ubo, sizeof(ubo));

        PushConstants pc{};
        pc.transform[0] = op.transform[0] * scale;
        pc.transform[1] = -op.transform[1] * scale;
        pc.transform[2] = op.transform[2] * scale;
        pc.transform[3] = -op.transform[3] * scale;
        pc.transform[4] = op.transform[4] * scale + x;
        pc.transform[5] = -op.transform[5] * scale + y;
        pc.translate[0] = 0.0f;
        pc.translate[1] = 0.0f;
        pc.screenSize[0] = screenWidth;
        pc.screenSize[1] = screenHeight;
        pc.padding[0] = 0.0f;
        pc.padding[1] = 0.0f;
        vkCmdPushConstants(commandBuffer,
                           pipelineLayouts_[PipelineKey{op.composite, false, false}],
                           VK_SHADER_STAGE_VERTEX_BIT,
                           0, sizeof(pc), &pc);

        const uint32_t dynamicOffset = static_cast<uint32_t>(uboOffset);
        vkCmdBindDescriptorSets(commandBuffer,
                                VK_PIPELINE_BIND_POINT_GRAPHICS,
                                pipelineLayouts_[PipelineKey{op.composite, false, false}],
                                0, 1, &descriptorSet_, 1, &dynamicOffset);

        vkCmdDrawIndexed(commandBuffer, op.indexCount, 1, op.indexOffset, 0, 0);
    }
    return true;
}
