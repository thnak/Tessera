#version 450

layout(push_constant) uniform PushConstants {
    vec2 resolution;
} pc;

layout(location = 0) out vec4 outColor;

void main() {
    vec2 uv = gl_FragCoord.xy / pc.resolution;

    // Checkerboard + vertical gradient: easy to verify by eye and by pixel diff.
    float checker = mod(floor(gl_FragCoord.x / 32.0) + floor(gl_FragCoord.y / 32.0), 2.0);
    vec3 a = vec3(0.10, 0.42, 0.90);
    vec3 b = vec3(0.94, 0.95, 0.98);
    vec3 color = mix(a, b, checker);
    color *= 0.70 + 0.30 * uv.y;

    outColor = vec4(color, 1.0);
}
