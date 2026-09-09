#version 330 core

// Infinite ground grid fragment shader (XZ plane at Y=0).
// Computes world-space position via ray-plane intersection.
// Anti-aliased grid lines using fwidth().
// Major grid every 10 units, minor grid every 1 unit.
// Axis highlights: X in red, Z in blue.

in vec3 v_nearPoint;
in vec3 v_farPoint;

uniform mat4 u_viewProjection;
uniform float u_near;
uniform float u_far;

out vec4 fragColor;

// Compute depth value for the fragment (for depth buffer writes)
float computeDepth(vec3 pos) {
    vec4 clipPos = u_viewProjection * vec4(pos, 1.0);
    return (clipPos.z / clipPos.w) * 0.5 + 0.5; // Map to [0,1]
}

// Compute linear depth for fading
float computeLinearDepth(vec3 pos) {
    vec4 clipPos = u_viewProjection * vec4(pos, 1.0);
    float clipDepth = (clipPos.z / clipPos.w) * 2.0 - 1.0; // NDC
    float linearDepth = (2.0 * u_near * u_far) / (u_far + u_near - clipDepth * (u_far - u_near));
    return linearDepth / u_far; // Normalize to [0,1]
}

// Draw grid lines at a given scale
vec4 grid(vec3 fragPos3D, float scale, vec4 lineColor) {
    vec2 coord = fragPos3D.xz * scale;
    vec2 derivative = fwidth(coord);
    vec2 grid = abs(fract(coord - 0.5) - 0.5) / derivative;
    float line = min(grid.x, grid.y);
    float minimumz = min(derivative.y, 1.0);
    float minimumx = min(derivative.x, 1.0);

    vec4 color = lineColor;
    color.a = 1.0 - min(line, 1.0);

    // X-axis highlight (red) - where Z is near 0
    if (fragPos3D.z > -0.5 * minimumz && fragPos3D.z < 0.5 * minimumz) {
        color = vec4(0.8, 0.2, 0.2, 1.0);
    }
    // Z-axis highlight (blue) - where X is near 0
    if (fragPos3D.x > -0.5 * minimumx && fragPos3D.x < 0.5 * minimumx) {
        color = vec4(0.2, 0.2, 0.8, 1.0);
    }

    return color;
}

void main() {
    // Ray-plane intersection: find t where ray hits Y=0 plane
    float t = -v_nearPoint.y / (v_farPoint.y - v_nearPoint.y);

    // Discard if intersection is behind the camera
    if (t < 0.0) discard;

    // World-space position on the XZ plane
    vec3 fragPos3D = v_nearPoint + t * (v_farPoint - v_nearPoint);

    // Compute depth for depth buffer
    gl_FragDepth = computeDepth(fragPos3D);

    // Depth-based fading
    float linearDepth = computeLinearDepth(fragPos3D);
    float fade = max(0.0, 1.0 - linearDepth);

    // Minor grid (every 1 unit) - subtle gray
    vec4 minorColor = grid(fragPos3D, 1.0, vec4(0.4, 0.4, 0.4, 1.0));
    // Major grid (every 10 units) - slightly brighter
    vec4 majorColor = grid(fragPos3D, 0.1, vec4(0.6, 0.6, 0.6, 1.0));

    // Combine: major grid overrides minor where both are visible
    vec4 color = minorColor;
    if (majorColor.a > minorColor.a) {
        color = majorColor;
    }

    // Apply distance fade
    color.a *= fade;

    // Discard fully transparent fragments
    if (color.a < 0.001) discard;

    fragColor = color;
}
