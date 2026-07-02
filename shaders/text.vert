#version 450

layout(location = 0) in vec2 inPos;
layout(location = 1) in vec2 inUV;
layout(location = 2) in float inColorGlyph;

layout(push_constant) uniform PushConstants {
    vec2 screenSize;
    vec4 color;
    float isRect;
    float isColorGlyph;
} pc;

layout(location = 0) out vec2 fragUV;
layout(location = 1) out flat float fragColorGlyph;

void main() {
    vec2 ndc = vec2(
        (inPos.x / pc.screenSize.x) * 2.0 - 1.0,
        (inPos.y / pc.screenSize.y) * 2.0 - 1.0
    );
    gl_Position = vec4(ndc, 0.0, 1.0);
    fragUV = inUV;
    fragColorGlyph = inColorGlyph;
}
