#version 450

layout(location = 0) in vec2 v_uv;
layout(location = 1) in vec4 v_color;
layout(location = 2) in vec2 v_local;
layout(location = 3) in vec2 v_half_size;
layout(location = 4) in float v_radius;
layout(location = 5) flat in float v_mode;

layout(set = 0, binding = 0) uniform sampler2D u_texture;

layout(push_constant) uniform PushConstants {
    vec2 viewport;
    float aa_radius;
    float pad;
} pc;

layout(location = 0) out vec4 out_color;

float sd_round_box(vec2 p, vec2 b, float r) {
    vec2 q = abs(p) - b + vec2(r);
    return min(max(q.x, q.y), 0.0) + length(max(q, vec2(0.0))) - r;
}

void main() {
    vec4 color = v_color;

    if (v_mode > 0.5 && v_mode < 1.5) {
        float d = sd_round_box(v_local, v_half_size, v_radius);
        float alpha;
        if (pc.aa_radius <= 0.0) {
            alpha = d <= 0.0 ? 1.0 : 0.0;
        } else {
            alpha = 1.0 - smoothstep(-pc.aa_radius, pc.aa_radius, d);
        }
        color.a *= alpha;
    } else if (v_mode > 1.5) {
        color *= texture(u_texture, v_uv);
    }

    out_color = color;
}
