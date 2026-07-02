#version 450

layout(location = 0) in vec2 fragUV;
layout(location = 1) in flat float fragColorGlyph;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform sampler2D atlas;
layout(set = 0, binding = 1) uniform sampler2D colorAtlas;
layout(push_constant) uniform PushConstants {
    vec2 screenSize;
    vec4 color;
    float isRect;
} pc;

void main() {
    if (pc.isRect > 0.5) {
        outColor = pc.color;
        return;
    }

    if (fragColorGlyph > 0.5) {
        vec4 texColor = texture(colorAtlas, fragUV);
        outColor = vec4(texColor.rgb, texColor.a * pc.color.a);
        return;
    }

    float alpha = texture(atlas, fragUV).r;
    outColor = vec4(pc.color.rgb, alpha * pc.color.a);
}
