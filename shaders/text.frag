#version 450

layout(location = 0) in vec2 fragUV;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform sampler2D atlas;
layout(push_constant) uniform PushConstants {
    vec2 screenSize;
    float sdSpread;
    float sdSmoothing;
    vec4 color;
    float isRect;
} pc;

void main() {
    if (pc.isRect > 0.5) {
        outColor = pc.color;
        return;
    }

    float sdf = texture(atlas, fragUV).r;
    float signedDist = (sdf - 0.5) * pc.sdSpread;
    float halfSmooth = max(pc.sdSmoothing, 0.001);
    float alpha = smoothstep(-halfSmooth, halfSmooth, signedDist);
    outColor = vec4(pc.color.rgb, alpha * pc.color.a);
}
