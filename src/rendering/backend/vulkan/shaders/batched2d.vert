#version 450

layout(location = 0) in vec2 in_position;
layout(location = 1) in vec2 in_uv;
layout(location = 2) in vec4 in_color;
layout(location = 3) in vec2 in_local;
layout(location = 4) in vec2 in_half_size;
layout(location = 5) in float in_radius;
layout(location = 6) in float in_mode;

layout(push_constant) uniform PushConstants {
    vec2 viewport;
    float aa_radius;
    float pad;
} pc;

layout(location = 0) out vec2 v_uv;
layout(location = 1) out vec4 v_color;
layout(location = 2) out vec2 v_local;
layout(location = 3) out vec2 v_half_size;
layout(location = 4) out float v_radius;
layout(location = 5) flat out float v_mode;

void main() {
    vec2 ndc = vec2(in_position.x / pc.viewport.x * 2.0 - 1.0,
                    in_position.y / pc.viewport.y * 2.0 - 1.0);
    gl_Position = vec4(ndc, 0.0, 1.0);

    v_uv = in_uv;
    v_color = in_color;
    v_local = in_local;
    v_half_size = in_half_size;
    v_radius = in_radius;
    v_mode = in_mode;
}
