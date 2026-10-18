#version 440
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 fragColor;
layout(std140, binding = 0) uniform Parameters {
    mat4 matrix;
    vec4 window; // minimum, reciprocal range, reciprocal gamma, inverted
    vec4 modes; // color mode, LUT, channel count, mask visible
    vec4 color0;
    vec4 color1;
    vec4 color2;
    vec4 color3;
};
layout(binding = 1) uniform sampler2D sliceTexture;
layout(binding = 2) uniform sampler2D maskTexture;
void main()
{
    vec4 raw = texture(sliceTexture, uv);
    vec3 result;
    if (modes.x > 0.5 && modes.x < 1.5) {
        result = pow(clamp((raw.rgb - vec3(window.x)) * window.y, 0.0, 1.0), vec3(window.z));
        if (window.w > 0.5) result = vec3(1.0) - result;
        // Preserve native RGBA alpha against the canvas background.
        result = mix(vec3(250.0, 247.0, 242.0) / 255.0, result, raw.a);
    } else {
        vec4 value = pow(clamp((raw - vec4(window.x)) * window.y, 0.0, 1.0), vec4(window.z));
        if (window.w > 0.5) value = vec4(1.0) - value;
        if (modes.x > 1.5) {
            result = value.r * color0.rgb;
            if (modes.z > 1.5) result += value.g * color1.rgb;
            if (modes.z > 2.5) result += value.b * color2.rgb;
            if (modes.z > 3.5) result += value.a * color3.rgb;
            result = clamp(result, 0.0, 1.0);
        } else if (modes.y > 1.5) {
            // Piecewise interpolation between the published viridis palette stops.
            const vec3 palette[6] = vec3[6](vec3(.267,.005,.329), vec3(.254,.265,.530),
                vec3(.164,.471,.558), vec3(.135,.659,.518), vec3(.478,.821,.318), vec3(.993,.906,.144));
            float position = value.r * 5.0;
            int index = min(int(position), 4);
            result = mix(palette[index], palette[index + 1], position - float(index));
        } else if (modes.y > 0.5) {
            result = clamp(vec3(3.0 * value.r, 3.0 * value.r - 1.0, 3.0 * value.r - 2.0), 0.0, 1.0);
        } else {
            result = vec3(value.r);
        }
    }
    if (modes.w > 0.5 && texture(maskTexture, uv).r > 0.0)
        result = mix(result, vec3(0.95, 0.25, 0.34), 0.36);
    fragColor = vec4(result, 1.0);
}
