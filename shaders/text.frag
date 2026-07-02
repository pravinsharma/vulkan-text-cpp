#version 450

layout(push_constant) uniform PushConstants {
    mat4 proj;
    int  useTexture;
} pc;

layout(location = 0) in vec2 fragUV;
layout(location = 1) in vec4 fragColor;

layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform sampler2D tex;

void main() {
    if (pc.useTexture != 0) {
        float a = texture(tex, fragUV).r;
        if (a < 0.01) {
            discard;
        }
        outColor = vec4(fragColor.rgb, fragColor.a * a);
    } else {
        outColor = fragColor;
    }
}
