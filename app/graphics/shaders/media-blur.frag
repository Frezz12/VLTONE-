#version 440
layout(location = 0) in vec2 qt_TexCoord0;
layout(location = 0) out vec4 fragColor;
layout(std140, binding = 0) uniform buf {
    mat4 qt_Matrix;
    float qt_Opacity;
    vec2 delta;
    vec2 extent;
    float cornerRadius;
};
layout(binding = 1) uniform sampler2D source;
void main() {
    vec4 color = texture(source, qt_TexCoord0) * 0.2270270270;
    color += texture(source, qt_TexCoord0 + delta * 1.3846153846) * 0.3162162162;
    color += texture(source, qt_TexCoord0 - delta * 1.3846153846) * 0.3162162162;
    color += texture(source, qt_TexCoord0 + delta * 3.2307692308) * 0.0702702703;
    color += texture(source, qt_TexCoord0 - delta * 3.2307692308) * 0.0702702703;
    vec2 p = qt_TexCoord0 * extent;
    float mask = 1.0;
    if (cornerRadius > 0.0 && p.x > extent.x - cornerRadius) {
        float y = min(p.y, extent.y - p.y);
        if (y < cornerRadius) {
            float distance = length(vec2(p.x - extent.x + cornerRadius, y - cornerRadius)) - cornerRadius;
            mask = 1.0 - smoothstep(-fwidth(distance), fwidth(distance), distance);
        }
    }
    fragColor = color * qt_Opacity * mask;
}
