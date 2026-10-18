#version 440
layout(location = 0) out vec2 uv;
layout(std140, binding = 0) uniform Parameters {
    mat4 matrix;
    vec4 window;
    vec4 modes;
    vec4 color0;
    vec4 color1;
    vec4 color2;
    vec4 color3;
};
void main()
{
    vec2 corners[4] = vec2[4](vec2(0, 0), vec2(0, 1), vec2(1, 0), vec2(1, 1));
    uv = corners[gl_VertexIndex];
    gl_Position = matrix * vec4(uv, 0, 1);
}
