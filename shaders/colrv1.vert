#version 450

layout(location = 0) in vec2 inPos;
layout(location = 1) in vec2 inUV;

layout(push_constant) uniform PushConstants {
    float uTxx;
    float uTxy;
    float uTyx;
    float uTyy;
    float uTdx;
    float uTdy;
    vec2 uTranslate;
    vec2 uScreenSize;
    vec2 uPadding;
} pc;

layout(location = 0) out vec2 vLocal;
layout(location = 1) out vec2 vUV;

void main()
{
    vLocal = inPos;
    vUV = inUV;
    vec2 pos = vec2(
        pc.uTxx * inPos.x + pc.uTyx * inPos.y + pc.uTdx,
        pc.uTxy * inPos.x + pc.uTyy * inPos.y + pc.uTdy
    );
    pos += pc.uTranslate;
    vec2 ndc = (pos / pc.uScreenSize) * 2.0 - 1.0;
    ndc.y = -ndc.y;
    gl_Position = vec4(ndc, 0.0, 1.0);
}
