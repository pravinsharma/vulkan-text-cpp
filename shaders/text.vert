#version 450

layout(push_constant) uniform PushConstants {
    mat4 proj;
} pc;

layout(location = 0) in vec2 inPos;
layout(location = 1) in vec2 inUV;

layout(location = 0) out vec2 fragUV;

void main() {
    gl_Position = pc.proj * vec4(inPos, 0.0, 1.0);
    fragUV = inUV;
}
