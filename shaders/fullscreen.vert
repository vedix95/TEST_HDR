#version 450

layout(location = 0) out vec2 v_uv;

void main() {
    // SDL3 GPU NDC: (-1, -1) is bottom-left, (+1, +1) is top-right.
    // Texture UV: (0, 0) is top-left, (1, 1) is bottom-right.
    vec2 pos;
    if (gl_VertexIndex == 0) {
        pos = vec2(-1.0, -1.0);
        v_uv = vec2(0.0, 1.0);
    } else if (gl_VertexIndex == 1) {
        pos = vec2(3.0, -1.0);
        v_uv = vec2(2.0, 1.0);
    } else {
        pos = vec2(-1.0, 3.0);
        v_uv = vec2(0.0, -1.0);
    }
    gl_Position = vec4(pos, 0.0, 1.0);
}
