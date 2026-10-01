#pragma once

// WGSL for the web port's renderer (webgpu_renderer.cpp). kGeShader follows
// shaders/ge.vert and shaders/ge.frag, which the Vulkan renderer uses: keep the
// two in step. Differences from them:
// - One uniform block per draw (DrawBlock in webgpu_renderer.cpp) holds what
//   the push constants, the environment and the object blocks hold there.
// - WebGPU's clip space has y up, and its viewport must lie inside the target,
//   with a positive size and its near depth before its far one. So every draw
//   gets the whole target as its viewport, and draw.fold maps x and y to where
//   the GE's viewport puts them; a depth range the wrong way round is set the
//   right way round and draw.extra.x mirrors depth to match.
// - The texture is sampled on every draw (a white one when there is none), as
//   WGSL wants samples in uniform control flow; sharp bilinear is not used.
// - The alpha test always runs; with function 0 it passes.

namespace mhp3rd::gpu::wgsl {

inline constexpr const char *kGeShader = R"(
struct Draw {
    transform: mat4x4f,
    viewport: vec4f,        // xy: target size in PSP pixels, z: through, w: 1 fog + 2 lighting
    texture_params: vec4f,  // x: texture enabled, y: texture function, z: alpha ref, w: alpha func
    uv_transform: vec4f,    // xy: scale, zw: offset
    view_z: vec4f,          // row of view * world that gives view-space z
    fold: vec4f,            // xy: scale, zw: offset, from the GE viewport to the whole target
    extra: vec4f,           // x: 1 mirrors depth, y: 1 reads the texture's alpha as 1 (a 5650 render target)
    ambient: vec4f,
    fog: vec4f,             // x: end, y: scale
    fog_color: vec4f,
    light_position: array<vec4f, 4>,     // xyz; w: enabled
    light_direction: array<vec4f, 4>,    // xyz; w: type (0 directional, 1 point, 2 spot)
    light_attenuation: array<vec4f, 4>,  // xyz: constant, linear, quadratic; w: kind
    light_spot: array<vec4f, 4>,         // x: exponent, y: cutoff
    light_ambient: array<vec4f, 4>,
    light_diffuse: array<vec4f, 4>,
    light_specular: array<vec4f, 4>,
    world: mat4x4f,
    flags: vec4f,              // y: vertex has a colour, w: material update mask
    emissive: vec4f,           // rgb; w: specular power
    material_ambient: vec4f,
    material_diffuse: vec4f,   // rgb; w: 1 keeps specular apart
    material_specular: vec4f,  // rgb; w: reverse normals
}

@group(0) @binding(0) var guest_texture: texture_2d<f32>;
@group(0) @binding(1) var guest_sampler: sampler;
@group(1) @binding(0) var<uniform> draw: Draw;

struct VertexIn {
    @location(0) position: vec4f,
    @location(1) texcoord: vec2f,
    @location(2) color: vec4f,
    @location(3) normal: vec3f,
}

struct VertexOut {
    @builtin(position) position: vec4f,
    @location(0) texcoord: vec2f,
    @location(1) color: vec4f,
    @location(2) specular: vec3f,
    @location(3) fog: f32,
    @location(4) @interpolate(flat) uv_rect: vec4f,
}

struct Lit {
    color: vec4f,
    specular: vec3f,
}

fn light_vertex(v: VertexIn) -> Lit {
    let mask = i32(draw.flags.w + 0.5);
    let has_color = draw.flags.y > 0.5;
    let ambient_material = select(draw.material_ambient, v.color, has_color && (mask & 1) != 0);
    let diffuse_material = select(draw.material_diffuse.rgb, v.color.rgb, has_color && (mask & 2) != 0);
    let specular_material = select(draw.material_specular.rgb, v.color.rgb, has_color && (mask & 4) != 0);
    let power = draw.emissive.w;

    let world_position = (draw.world * vec4f(v.position.xyz, 1.0)).xyz;
    let world3 = mat3x3f(draw.world[0].xyz, draw.world[1].xyz, draw.world[2].xyz);
    var normal = world3 * v.normal;
    let length_squared = dot(normal, normal);
    normal = select(vec3f(0.0, 0.0, 1.0), normal * inverseSqrt(length_squared), length_squared > 0.0);
    if (draw.material_specular.w > 0.5) { normal = -normal; }

    var sum = draw.emissive.rgb + draw.ambient.rgb * ambient_material.rgb;
    var specular = vec3f(0.0);
    for (var i = 0; i < 4; i++) {
        if (draw.light_position[i].w < 0.5) { continue; }
        let kind_of = i32(draw.light_direction[i].w + 0.5);
        let kind = i32(draw.light_attenuation[i].w + 0.5);
        var to_light = draw.light_position[i].xyz;
        var scale = 1.0;
        if (kind_of != 0) {
            to_light -= world_position;
            let distance = length(to_light);
            let k = draw.light_attenuation[i].xyz;
            scale = clamp(1.0 / max(k.x + k.y * distance + k.z * distance * distance, 1e-20), 0.0, 1.0);
        }
        to_light = select(vec3f(0.0, 0.0, 1.0), normalize(to_light), dot(to_light, to_light) > 0.0);
        if (kind_of == 2) {
            var axis = draw.light_direction[i].xyz;
            axis = select(vec3f(0.0, 0.0, 1.0), normalize(axis), dot(axis, axis) > 0.0);
            let angle = dot(axis, -to_light);
            scale *= select(0.0, pow(max(angle, 0.0), draw.light_spot[i].x), angle >= draw.light_spot[i].y);
        }
        let n_dot_l = dot(normal, to_light);
        var diffuse = max(n_dot_l, 0.0);
        if (kind == 2) { diffuse = pow(diffuse, power); }
        sum += (draw.light_ambient[i].rgb * ambient_material.rgb +
                draw.light_diffuse[i].rgb * diffuse_material * diffuse) * scale;
        if (kind == 1 && n_dot_l >= 0.0) {
            let half_vector = normalize(to_light + vec3f(0.0, 0.0, 1.0));
            specular += draw.light_specular[i].rgb * specular_material *
                        pow(max(dot(normal, half_vector), 0.0), power) * scale;
        }
    }
    let alpha = draw.ambient.a * ambient_material.a;
    var out: Lit;
    if (draw.material_diffuse.w > 0.5) {
        out.specular = clamp(specular, vec3f(0.0), vec3f(1.0));
    } else {
        sum += specular;
        out.specular = vec3f(0.0);
    }
    out.color = clamp(vec4f(sum, alpha), vec4f(0.0), vec4f(1.0));
    return out;
}

@vertex fn vertex_main(v: VertexIn) -> VertexOut {
    var out: VertexOut;
    out.texcoord = v.texcoord * draw.uv_transform.xy + draw.uv_transform.zw;
    out.color = v.color;
    out.specular = vec3f(0.0);
    out.fog = 1.0;
    out.uv_rect = vec4f(-1e30, -1e30, 1e30, 1e30);
    var clip: vec4f;
    if (draw.viewport.z > 0.5) {
        let rect = vec4f(v.normal.xy, v.normal.z, v.position.w);
        out.uv_rect = vec4f(rect.xy * draw.uv_transform.xy + draw.uv_transform.zw,
                            rect.zw * draw.uv_transform.xy + draw.uv_transform.zw);
        let ndc = vec2f(v.position.x / draw.viewport.x, v.position.y / draw.viewport.y) * 2.0 - 1.0;
        clip = vec4f(ndc, clamp(v.position.z / 65535.0, 0.0, 1.0), 1.0);
    } else {
        let enables = i32(draw.viewport.w + 0.5);
        if ((enables & 2) != 0) {
            let lit = light_vertex(v);
            out.color = lit.color;
            out.specular = lit.specular;
        }
        if ((enables & 1) != 0) {
            out.fog = (dot(draw.view_z, vec4f(v.position.xyz, 1.0)) + draw.fog.x) * draw.fog.y;
        }
        clip = draw.transform * vec4f(v.position.xyz, 1.0);
        clip.z = (clip.z + clip.w) * 0.5;
    }
    clip.x = clip.x * draw.fold.x + draw.fold.z * clip.w;
    clip.y = clip.y * draw.fold.y + draw.fold.w * clip.w;
    if (draw.extra.x > 0.5) { clip.z = clip.w - clip.z; }
    out.position = clip;
    return out;
}

@fragment fn fragment_main(f: VertexOut) -> @location(0) vec4f {
    var texel = textureSample(guest_texture, guest_sampler, clamp(f.texcoord, f.uv_rect.xy, f.uv_rect.zw));
    if (draw.extra.y > 0.5) { texel.a = 1.0; }
    var color = f.color;
    if (draw.texture_params.x > 0.5) {
        let function = i32(draw.texture_params.y + 0.5);
        if (function == 0) {
            color *= texel;
        } else if (function == 1) {
            color = vec4f(mix(color.rgb, texel.rgb, texel.a), color.a);
        } else if (function == 2) {
            color = vec4f(mix(color.rgb, texel.rgb, texel.rgb), color.a * texel.a);
        } else {
            color = texel;
        }
    }
    color = vec4f(min(color.rgb + f.specular, vec3f(1.0)), color.a);
    if ((i32(draw.viewport.w + 0.5) & 1) != 0) {
        color = vec4f(mix(draw.fog_color.rgb, color.rgb, clamp(f.fog, 0.0, 1.0)), color.a);
    }
    let alpha_function = i32(draw.texture_params.w + 0.5);
    let reference = draw.texture_params.z / 255.0;
    let alpha = color.a;
    var passed = true;
    if (alpha_function == 1) { passed = false; }
    else if (alpha_function == 2) { passed = abs(alpha - reference) < 0.002; }
    else if (alpha_function == 3) { passed = abs(alpha - reference) >= 0.002; }
    else if (alpha_function == 4) { passed = alpha < reference; }
    else if (alpha_function == 5) { passed = alpha <= reference; }
    else if (alpha_function == 6) { passed = alpha > reference; }
    else if (alpha_function == 7) { passed = alpha >= reference; }
    if (!passed) { discard; }
    return color;
}
)";

// Draws a game frame (a render target) into the rectangle the viewport gives.
inline constexpr const char *kBlitShader = R"(
@group(0) @binding(0) var frame: texture_2d<f32>;
@group(0) @binding(1) var frame_sampler: sampler;

struct Out {
    @builtin(position) position: vec4f,
    @location(0) uv: vec2f,
}

@vertex fn vertex_main(@builtin(vertex_index) index: u32) -> Out {
    // One triangle that covers the viewport.
    let corner = vec2f(f32((index << 1u) & 2u), f32(index & 2u));
    var out: Out;
    out.position = vec4f(corner * 2.0 - 1.0, 0.0, 1.0);
    out.uv = vec2f(corner.x, 1.0 - corner.y);
    return out;
}

@fragment fn fragment_main(f: Out) -> @location(0) vec4f {
    return vec4f(textureSample(frame, frame_sampler, f.uv).rgb, 1.0);
}
)";

} // namespace mhp3rd::gpu::wgsl
