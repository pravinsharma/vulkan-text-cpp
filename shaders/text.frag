#version 450

layout(location = 0) in vec2 fragUV;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform sampler2D atlas;
layout(push_constant) uniform PushConstants {
    vec2 screenSize;
    vec4 color;
} pc;

void main() {
    float a = texture(atlas, fragUV).r;
    if (a < 0.01) discard;
    outColor = vec4(pc.color.rgb, a);
}
