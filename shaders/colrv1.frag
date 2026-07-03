#version 450

layout(location = 0) in vec2 vLocal;
layout(location = 1) in vec2 vUV;

layout(set = 0, binding = 0) uniform PaintUBO {
    int  uType;
    vec4 uColor;
    vec2 uP0;
    vec2 uP1;
    vec2 uP2;
    float uRadius0;
    float uRadius1;
    int  uExtend;
    int  uComposite;
    int  uUseBackdrop;
    int  uStopCount;
    uint uStopOffset;
} paint;

struct GradientStop
{
    float position;
    vec4  color;
};

layout(set = 0, binding = 1) readonly buffer StopsBuffer {
    GradientStop stops[];
} stopsBuf;

layout(location = 0) out vec4 outColor;

vec4 sampleStops(float t)
{
    int n = paint.uStopCount;
    if (n <= 0) return vec4(0.0);
    if (n == 1) return stopsBuf.stops[paint.uStopOffset].color;
    float clamped = t;
    if (paint.uExtend == 0) clamped = clamp(t, 0.0, 1.0);
    int idx = int(floor(clamped * float(n - 1)));
    if (paint.uExtend == 1)
    {
        idx = int(mod(clamped * float(n), float(n)));
    }
    else if (paint.uExtend == 2)
    {
        float m = mod(clamped * float(n - 1), float(n - 1));
        idx = int(m);
    }
    idx = clamp(idx, 0, n - 1);
    int idx1 = min(idx + 1, n - 1);
    float local = clamped * float(n - 1) - float(idx);
    if (paint.uExtend == 1)
    {
        local = fract(clamped * float(n));
        idx = int(floor(clamped * float(n)));
        idx1 = (idx + 1) % n;
        if (idx1 == idx) idx1 = (idx + 1) % n;
    }
    return mix(stopsBuf.stops[paint.uStopOffset + idx].color,
               stopsBuf.stops[paint.uStopOffset + idx1].color,
               clamp(local, 0.0, 1.0));
}

void main()
{
    if (paint.uType == 0)
    {
        outColor = paint.uColor;
    }
    else if (paint.uType == 1)
    {
        vec2 d = paint.uP1 - paint.uP0;
        float L = length(d);
        float t = L > 0.0 ? dot(vLocal - paint.uP0, d) / (L * L) : 0.0;
        outColor = sampleStops(t);
    }
    else if (paint.uType == 2)
    {
        vec2 d = vLocal - paint.uP0;
        float r0 = paint.uRadius0;
        float r1 = paint.uRadius1;
        float rr = length(d);
        float t = (r1 - r0) > 0.0 ? (rr - r0) / (r1 - r0) : 0.0;
        outColor = sampleStops(t);
    }
    else if (paint.uType == 3)
    {
        vec2 d = vLocal - paint.uP0;
        float a = atan(d.y, d.x);
        if (a < 0.0) a += 6.2831853;
        float span = paint.uP1.x;
        if (span <= 0.0) span = 6.2831853;
        float start = paint.uP1.y;
        float t = (a - start) / span;
        if (paint.uExtend == 0) t = clamp(t, 0.0, 1.0);
        outColor = sampleStops(t);
    }
    else
    {
        outColor = vec4(1.0, 0.0, 1.0, 1.0);
    }
}
