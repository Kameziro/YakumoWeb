// WebGPU implementation of gpu::VulkanRenderer for the web port (Emscripten).
//
// The class keeps its name: the rest of the port (kernel, HLE, interface) talks
// to "the renderer" through vulkan_renderer.hpp, which exposes no Vulkan types,
// and the web build compiles this file in place of vulkan_renderer.cpp.
//
// It draws the game's picture as the Vulkan renderer does, more simply: each
// guest framebuffer the game draws into is a render target at the internal
// scale, every draw becomes a triangle list with its own uniform block
// (shaders in webgpu_shaders.hpp), and a flip shows the flipped-to target in
// the canvas with the interface (Dear ImGui) over it. Each present gives
// control back to the browser until its next animation frame.
//
// The keyboard and the first gamepad reach the game through the player's
// bindings, as in the Vulkan renderer, and mouse buttons. A frame the game
// uploads itself (a movie) is shown in place of its target. Not implemented
// yet: touch and mouse motion for the game, the lead-in of L, more than one
// gamepad; points and lines; render targets sampled as textures, write back
// to guest memory; capture; frame interpolation; texture packs and the
// sharper interface textures. Those members keep neutral values and do
// nothing.

#include "gpu/vulkan_renderer.hpp"

#include "gpu/game_hud.hpp"
#include "gpu/texture_decode.hpp"
#include "gpu/webgpu_shaders.hpp"
#include "input/bindings.hpp"
#include "input/chords.hpp"
#include "install/user_data.hpp"
#include "settings/settings.hpp"

#include <SDL3/SDL.h>
#include <emscripten/emscripten.h>
#include <emscripten/html5.h>
#include <webgpu/webgpu.h>

#include "imgui.h"
#include "backends/imgui_impl_wgpu.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <iostream>
#include <map>
#include <span>
#include <unordered_map>
#include <utility>
#include <vector>

namespace mhp3rd::gpu {

namespace {

constexpr std::uint32_t kPspWidth = 480u;
constexpr std::uint32_t kPspHeight = 272u;
constexpr const char *kCanvasSelector = "#canvas";
constexpr const char *kScreenSelector = "#screen";

WGPUStringView view_of(const char *text) { return WGPUStringView{text, std::strlen(text)}; }

std::string to_string(WGPUStringView text) {
    if (text.data == nullptr) return {};
    return text.length == WGPU_STRLEN ? std::string(text.data) : std::string(text.data, text.length);
}

// Gives the browser its turn until the next animation frame: the canvas shows
// what was submitted, and input events are delivered. The wasm stack is
// suspended meanwhile (JSPI), so the port's blocking loops need no rewrite.
EM_ASYNC_JS(void, wait_for_animation_frame, (), {
    await new Promise((resolve) => requestAnimationFrame(() => resolve()));
});

constexpr WGPUTextureFormat kTargetFormat = WGPUTextureFormat_RGBA8Unorm;
constexpr WGPUTextureFormat kDepthFormat = WGPUTextureFormat_Depth24Plus;
// A frame's geometry and uniform blocks; a frame that needs more is submitted
// in parts.
constexpr std::uint64_t kVertexBufferBytes = 32ull << 20u;
constexpr std::uint64_t kUniformStride = 1024u;  // DrawBlock, rounded up to the offset alignment
constexpr std::uint64_t kUniformBufferBytes = 8ull << 20u;
constexpr std::size_t kMaxCachedTextures = 1024u;

// The vertex the shaders read; as GpuVertex in vulkan_renderer.cpp.
struct GpuVertex {
    float x{}, y{}, z{}, w{1.0f};
    float u{}, v{};
    std::uint32_t color{};
    float nx{}, ny{}, nz{};
};
static_assert(sizeof(GpuVertex) == 40u);

// The Draw uniform block of kGeShader.
struct DrawBlock {
    std::array<float, 16> transform{};
    std::array<float, 4> viewport{};
    std::array<float, 4> texture_params{};
    std::array<float, 4> uv_transform{1.0f, 1.0f, 0.0f, 0.0f};
    std::array<float, 4> view_z{};
    std::array<float, 4> fold{1.0f, -1.0f, 0.0f, 0.0f};
    std::array<float, 4> extra{};
    std::array<float, 4> ambient{};
    std::array<float, 4> fog{};
    std::array<float, 4> fog_color{};
    std::array<std::array<float, 4>, 4> light_position{};
    std::array<std::array<float, 4>, 4> light_direction{};
    std::array<std::array<float, 4>, 4> light_attenuation{};
    std::array<std::array<float, 4>, 4> light_spot{};
    std::array<std::array<float, 4>, 4> light_ambient{};
    std::array<std::array<float, 4>, 4> light_diffuse{};
    std::array<std::array<float, 4>, 4> light_specular{};
    std::array<float, 16> world{};
    std::array<float, 4> flags{};
    std::array<float, 4> emissive{};
    std::array<float, 4> material_ambient{};
    std::array<float, 4> material_diffuse{};
    std::array<float, 4> material_specular{};
};
static_assert(sizeof(DrawBlock) == 800u && sizeof(DrawBlock) <= kUniformStride);

std::array<float, 4> unpack_color(std::uint32_t color, float alpha = 1.0f) {
    return {static_cast<float>(color & 0xFFu) / 255.0f, static_cast<float>((color >> 8u) & 0xFFu) / 255.0f,
            static_cast<float>((color >> 16u) & 0xFFu) / 255.0f, alpha};
}

std::array<float, 16> multiply(const std::array<float, 16> &a, const std::array<float, 16> &b) {
    std::array<float, 16> result{};
    for (std::uint32_t column = 0; column < 4u; ++column)
        for (std::uint32_t row = 0; row < 4u; ++row) {
            float sum = 0.0f;
            for (std::uint32_t k = 0; k < 4u; ++k) sum += a[k * 4u + row] * b[column * 4u + k];
            result[column * 4u + row] = sum;
        }
    return result;
}

// Blend state, as in vulkan_renderer.cpp: GU_FIX takes its factor from a
// colour register, and a single blend constant serves both sides.
constexpr std::uint32_t kFactorFixed = 10u;
constexpr std::uint32_t kFactorOne = 16u;
constexpr std::uint32_t kFactorZero = 17u;
constexpr std::uint32_t kFactorInverseConstant = 18u;

std::uint32_t resolve_fixed_factor(std::uint32_t factor, std::uint32_t color) {
    if (factor != kFactorFixed) return factor;
    const std::uint32_t rgb = color & 0x00FFFFFFu;
    if (rgb == 0x00FFFFFFu) return kFactorOne;
    if (rgb == 0u) return kFactorZero;
    return kFactorFixed;
}

WGPUBlendFactor to_blend_factor(std::uint32_t factor, bool source) {
    switch (factor) {
    // Factor 0 names the other pixel: the source side scales by the
    // destination colour and the destination side by the source colour.
    case 0u: return source ? WGPUBlendFactor_Dst : WGPUBlendFactor_Src;
    case 1u: return source ? WGPUBlendFactor_OneMinusDst : WGPUBlendFactor_OneMinusSrc;
    case 2u: return WGPUBlendFactor_SrcAlpha;
    case 3u: return WGPUBlendFactor_OneMinusSrcAlpha;
    case 4u: return WGPUBlendFactor_DstAlpha;
    case 5u: return WGPUBlendFactor_OneMinusDstAlpha;
    case 6u: return WGPUBlendFactor_SrcAlpha;
    case 7u: return WGPUBlendFactor_OneMinusSrcAlpha;
    case 8u: return WGPUBlendFactor_DstAlpha;
    case 9u: return WGPUBlendFactor_OneMinusDstAlpha;
    case kFactorFixed: return WGPUBlendFactor_Constant;
    case kFactorOne: return WGPUBlendFactor_One;
    case kFactorZero: return WGPUBlendFactor_Zero;
    case kFactorInverseConstant: return WGPUBlendFactor_OneMinusConstant;
    default: return source ? WGPUBlendFactor_One : WGPUBlendFactor_Zero;
    }
}

WGPUBlendOperation to_blend_operation(std::uint32_t equation) {
    switch (equation) {
    case 1u: return WGPUBlendOperation_Subtract;
    case 2u: return WGPUBlendOperation_ReverseSubtract;
    case 3u: return WGPUBlendOperation_Min;
    case 4u: return WGPUBlendOperation_Max;
    default: return WGPUBlendOperation_Add;
    }
}

WGPUCompareFunction to_compare(std::uint32_t function) {
    switch (function) {
    case 0u: return WGPUCompareFunction_Never;
    case 1u: return WGPUCompareFunction_Always;
    case 2u: return WGPUCompareFunction_Equal;
    case 3u: return WGPUCompareFunction_NotEqual;
    case 4u: return WGPUCompareFunction_Less;
    case 5u: return WGPUCompareFunction_LessEqual;
    case 6u: return WGPUCompareFunction_Greater;
    default: return WGPUCompareFunction_GreaterEqual;
    }
}

// The GE state a pipeline is made for.
struct PipelineKey {
    bool blend{};
    std::uint32_t source_factor{};
    std::uint32_t destination_factor{};
    std::uint32_t equation{};
    bool depth_test{};
    bool depth_write{};
    std::uint32_t depth_function{};
    bool cull{};
    bool cull_clockwise{};
    std::uint32_t color_mask{WGPUColorWriteMask_All};
    auto operator<=>(const PipelineKey &) const = default;
};

// A through-mode tile's texel range, which the fragment shader clamps to; see
// clamp_through_quad() in vulkan_renderer.cpp.
constexpr float kNoClamp = 1e30f;

void set_uv_rect(GpuVertex &vertex, float u_min, float v_min, float u_max, float v_max) {
    vertex.nx = u_min;
    vertex.ny = v_min;
    vertex.nz = u_max;
    vertex.w = v_max;
}

void clamp_through_quad(std::vector<GpuVertex> &vertices, std::size_t first, float texture_width,
                        float texture_height) {
    if (first + 6u > vertices.size()) return;
    float x0 = vertices[first].x, x1 = x0, y0 = vertices[first].y, y1 = y0;
    for (std::size_t i = first; i < first + 6u; ++i) {
        x0 = std::min(x0, vertices[i].x);
        x1 = std::max(x1, vertices[i].x);
        y0 = std::min(y0, vertices[i].y);
        y1 = std::max(y1, vertices[i].y);
    }
    if (x1 - x0 < 1.0f || y1 - y0 < 1.0f) return;
    float u_at_x0 = 0.0f, u_at_x1 = 0.0f, v_at_y0 = 0.0f, v_at_y1 = 0.0f;
    bool seen[4]{};
    for (std::size_t i = first; i < first + 6u; ++i) {
        const GpuVertex &vertex = vertices[i];
        const bool left = vertex.x == x0, top = vertex.y == y0;
        if (!left && vertex.x != x1) return;
        if (!top && vertex.y != y1) return;
        float &u = left ? u_at_x0 : u_at_x1;
        float &v = top ? v_at_y0 : v_at_y1;
        bool &u_seen = seen[left ? 0 : 1];
        bool &v_seen = seen[top ? 2 : 3];
        if (u_seen && u != vertex.u) return;
        if (v_seen && v != vertex.v) return;
        u = vertex.u;
        v = vertex.v;
        u_seen = v_seen = true;
    }
    const float u_min = std::min(u_at_x0, u_at_x1), u_max = std::max(u_at_x0, u_at_x1);
    const float v_min = std::min(v_at_y0, v_at_y1), v_max = std::max(v_at_y0, v_at_y1);
    if (u_min < 0.0f || v_min < 0.0f || u_max > texture_width || v_max > texture_height) return;
    const float inset_u = 0.5f * (u_max - u_min) / (x1 - x0);
    const float inset_v = 0.5f * (v_max - v_min) / (y1 - y0);
    for (std::size_t i = first; i < first + 6u; ++i)
        set_uv_rect(vertices[i], u_min + inset_u, v_min + inset_v, u_max - inset_u, v_max - inset_v);
}

// The gamepad, as vulkan_renderer.cpp reads it (pad_tuning, pad_input_held
// and read_gamepad there).
struct PadTuning {
    float dead_zone{0.15f};
    float trigger{0.25f};
    float right_stick{0.5f};
    settings::RightStick right_stick_mode{settings::RightStick::Camera};
    bool invert_x{};
    bool invert_y{};
    bool confirm_south{};
    bool swap_sticks{};
};

PadTuning pad_tuning() {
    const settings::Settings &player = settings::current();
    PadTuning value{};
    value.dead_zone = player.dead_zone;
    value.trigger = player.trigger;
    value.right_stick = player.right_stick_zone;
    value.right_stick_mode = player.right_stick;
    value.invert_x = player.invert_camera_x;
    value.invert_y = player.invert_camera_y;
    value.confirm_south = player.confirm_south;
    value.swap_sticks = player.controls.swap_sticks;
    return value;
}

bool pad_input_held(SDL_Gamepad *device, input::Binding binding, const PadTuning &tuning) {
    int input = input::pad_input_of(binding);
    if (input < 0) return false;
    if (tuning.confirm_south) {
        if (input == static_cast<int>(input::PadInput::South)) input = static_cast<int>(input::PadInput::East);
        else if (input == static_cast<int>(input::PadInput::East)) input = static_cast<int>(input::PadInput::South);
    }
    if (input == static_cast<int>(input::PadInput::LeftTrigger) ||
        input == static_cast<int>(input::PadInput::RightTrigger)) {
        const SDL_GamepadAxis axis = input == static_cast<int>(input::PadInput::LeftTrigger)
                                         ? SDL_GAMEPAD_AXIS_LEFT_TRIGGER
                                         : SDL_GAMEPAD_AXIS_RIGHT_TRIGGER;
        return static_cast<float>(SDL_GetGamepadAxis(device, axis)) / 32767.0f > tuning.trigger;
    }
    return input < static_cast<int>(input::PadInput::ButtonCount) &&
           SDL_GetGamepadButton(device, static_cast<SDL_GamepadButton>(input));
}

void read_gamepad(SDL_Gamepad *device, const input::PadState &mapped, PadState &pad, int &analog_x,
                  int &analog_y) {
    const PadTuning tuning = pad_tuning();
    std::uint32_t &buttons = pad.buttons;
    buttons |= mapped.buttons;
    pad.fast_forward = pad.fast_forward || mapped.fast_forward;
    analog_x += mapped.stick_x;
    analog_y += mapped.stick_y;
    const auto axis = [&](SDL_GamepadAxis id) {
        return std::clamp(static_cast<float>(SDL_GetGamepadAxis(device, id)) / 32767.0f, -1.0f, 1.0f);
    };
    const SDL_GamepadAxis camera_x_axis = tuning.swap_sticks ? SDL_GAMEPAD_AXIS_LEFTX : SDL_GAMEPAD_AXIS_RIGHTX;
    const SDL_GamepadAxis camera_y_axis = tuning.swap_sticks ? SDL_GAMEPAD_AXIS_LEFTY : SDL_GAMEPAD_AXIS_RIGHTY;
    const SDL_GamepadAxis move_x_axis = tuning.swap_sticks ? SDL_GAMEPAD_AXIS_RIGHTX : SDL_GAMEPAD_AXIS_LEFTX;
    const SDL_GamepadAxis move_y_axis = tuning.swap_sticks ? SDL_GAMEPAD_AXIS_RIGHTY : SDL_GAMEPAD_AXIS_LEFTY;
    const float right_x = std::clamp(axis(camera_x_axis) + static_cast<float>(mapped.camera_x) / 127.0f, -1.0f, 1.0f);
    const float right_y = std::clamp(axis(camera_y_axis) + static_cast<float>(mapped.camera_y) / 127.0f, -1.0f, 1.0f);
    if (tuning.right_stick_mode == settings::RightStick::DPad) {
        if (right_x < -tuning.right_stick) buttons |= 0x0080u;
        if (right_x > tuning.right_stick) buttons |= 0x0020u;
        if (right_y < -tuning.right_stick) buttons |= 0x0010u;
        if (right_y > tuning.right_stick) buttons |= 0x0040u;
    }
    const auto deflect = [&](float x, float y, std::uint8_t &out_x, std::uint8_t &out_y) {
        const float length = std::sqrt(x * x + y * y);
        if (length <= tuning.dead_zone) return;
        const float scale = std::min((length - tuning.dead_zone) / (1.0f - tuning.dead_zone), 1.0f) / length;
        out_x = static_cast<std::uint8_t>(std::clamp(0x80 + static_cast<int>(x * scale * 127.0f), 0, 255));
        out_y = static_cast<std::uint8_t>(std::clamp(0x80 + static_cast<int>(y * scale * 127.0f), 0, 255));
    };
    if (tuning.right_stick_mode == settings::RightStick::Camera)
        deflect(tuning.invert_x ? -right_x : right_x, tuning.invert_y ? -right_y : right_y, pad.right_x, pad.right_y);
    std::uint8_t nub_x = 0x80u;
    std::uint8_t nub_y = 0x80u;
    deflect(axis(move_x_axis), axis(move_y_axis), nub_x, nub_y);
    analog_x += static_cast<int>(nub_x) - 0x80;
    analog_y += static_cast<int>(nub_y) - 0x80;
}

} // namespace

struct VulkanRenderer::Impl {
    RendererConfig config;
    SDL_Window *window{};
    WGPUInstance instance{};
    WGPUAdapter adapter{};
    WGPUDevice device{};
    WGPUQueue queue{};
    WGPUSurface surface{};
    WGPUTextureFormat format{WGPUTextureFormat_Undefined};
    std::uint32_t width{};
    std::uint32_t height{};
    bool configured{};
    bool ready{};
    bool quit{};
    bool ui_ready{};
    ImDrawData *ui_draw_data{};
    std::function<bool(const SDL_Event &)> event_hook;
    std::uint64_t frames{};
    std::string device_name;
    input::touch::Controls touch;
    input::touch::ActionControls action_touch;

    // Input for the game.
    SDL_Gamepad *gamepad{};
    SDL_JoystickID gamepad_id{};
    input::Resolver keys_resolver;
    input::Resolver pad_resolver;
    input::PadState typed{};
    input::PadState mapped{};
    PadState pad{};
    bool game_input{true};
    bool free_camera{};
    bool suppress_held{};
    std::uint32_t suppressed_buttons{};
    std::uint32_t mouse_buttons{};  // bit n: SDL mouse button n held over the game
    void open_gamepad(SDL_JoystickID id);
    void close_gamepad(SDL_JoystickID id);
    void sample_pad();

    // The game's picture.
    struct Target {
        WGPUTexture color{};
        WGPUTextureView color_view{};
        WGPUTexture depth{};
        WGPUTextureView depth_view{};
        WGPUBindGroup sample_group{};  // for showing it
        bool cleared{};                // the first pass into it clears it
    };
    struct Texture {
        WGPUTexture texture{};
        WGPUTextureView view{};
        WGPUBindGroup group{};
        std::uint64_t last_used{};
    };
    std::uint32_t target_width{kPspWidth * 2u};
    std::uint32_t target_height{kPspHeight * 2u};
    std::map<std::uint32_t, Target> targets;
    std::unordered_map<std::uint64_t, Texture> textures;
    Texture white;
    std::map<PipelineKey, WGPURenderPipeline> pipelines;
    WGPUShaderModule ge_module{};
    WGPUShaderModule blit_module{};
    WGPUBindGroupLayout texture_layout{};
    WGPUBindGroupLayout draw_layout{};
    WGPUPipelineLayout ge_layout{};
    WGPUPipelineLayout blit_layout{};
    WGPURenderPipeline blit_pipeline{};
    WGPUSampler sampler{};
    WGPUBuffer vertex_buffer{};
    WGPUBuffer uniform_buffer{};
    WGPUBindGroup draw_group{};
    bool ge_ready{};

    // The frame being recorded.
    WGPUCommandEncoder encoder{};
    WGPURenderPassEncoder pass{};
    std::uint32_t pass_target{};
    std::vector<std::uint8_t> vertex_bytes;
    std::vector<std::uint8_t> uniform_bytes;
    std::vector<GpuVertex> scratch;
    std::uint32_t shown_target{};
    bool shown_valid{};
    // A frame the game wrote itself (the movie player, upload_frame): shown
    // instead of the target at movie_address until the GE draws there again.
    Texture movie;
    std::uint32_t movie_width{};
    std::uint32_t movie_height{};
    std::uint32_t movie_address{};
    bool movie_valid{};
    std::uint64_t draws{};
    std::uint64_t texture_clock{};
    std::vector<std::uint32_t> decoded;

    bool request_device(std::string &error);
    void configure_surface();
    bool create_ge_resources(std::string &error);
    WGPURenderPipeline pipeline_for(const PipelineKey &key);
    Target *target_for(std::uint32_t address);
    Texture make_texture(std::uint32_t width, std::uint32_t height, const std::uint32_t *pixels);
    WGPUBindGroup texture_group(const GuestMemory &memory, const TextureState &state);
    void release_texture(Texture &texture);
    void begin_recording();
    void end_pass();
    void begin_pass(std::uint32_t address);
    // Uploads the frame's geometry and blocks, and submits what was recorded.
    void flush();
    // Ends the frame: shows the target at `address` (when `show_game`) with
    // the interface over it, and waits for the browser's next frame.
    void finish_frame(bool show_game);
    void draw(const DrawCall &call, const GuestMemory &memory);
};

bool VulkanRenderer::Impl::request_device(std::string &error) {
    WGPUInstanceDescriptor instance_desc = WGPU_INSTANCE_DESCRIPTOR_INIT;
    instance = wgpuCreateInstance(&instance_desc);
    if (instance == nullptr) {
        error = "wgpuCreateInstance failed: this browser has no WebGPU";
        return false;
    }

    // The requests complete from the browser's event loop; wait for them by
    // giving it turns, as a frame does.
    struct Pending {
        bool done{};
        std::string message;
        WGPUAdapter adapter{};
        WGPUDevice device{};
    } pending;

    WGPURequestAdapterOptions adapter_options = WGPU_REQUEST_ADAPTER_OPTIONS_INIT;
    adapter_options.powerPreference = WGPUPowerPreference_HighPerformance;
    WGPURequestAdapterCallbackInfo adapter_callback = WGPU_REQUEST_ADAPTER_CALLBACK_INFO_INIT;
    adapter_callback.mode = WGPUCallbackMode_AllowSpontaneous;
    adapter_callback.callback = [](WGPURequestAdapterStatus status, WGPUAdapter result, WGPUStringView message,
                                   void *userdata, void *) {
        auto &p = *static_cast<Pending *>(userdata);
        if (status == WGPURequestAdapterStatus_Success) p.adapter = result;
        else p.message = to_string(message);
        p.done = true;
    };
    adapter_callback.userdata1 = &pending;
    wgpuInstanceRequestAdapter(instance, &adapter_options, adapter_callback);
    while (!pending.done) emscripten_sleep(1);
    if (pending.adapter == nullptr) {
        error = "no WebGPU adapter: " + pending.message;
        return false;
    }
    adapter = pending.adapter;

    WGPUAdapterInfo info = WGPU_ADAPTER_INFO_INIT;
    if (wgpuAdapterGetInfo(adapter, &info) == WGPUStatus_Success) {
        device_name = to_string(info.description);
        if (device_name.empty()) device_name = to_string(info.device);
        if (device_name.empty()) device_name = to_string(info.vendor);
        wgpuAdapterInfoFreeMembers(info);
    }
    if (device_name.empty()) device_name = "WebGPU";

    pending.done = false;
    WGPUDeviceDescriptor device_desc = WGPU_DEVICE_DESCRIPTOR_INIT;
    device_desc.label = view_of("Yakumo");
    device_desc.uncapturedErrorCallbackInfo.callback = [](WGPUDevice const *, WGPUErrorType type,
                                                          WGPUStringView message, void *, void *) {
        std::cerr << "[webgpu] error " << static_cast<int>(type) << ": " << to_string(message) << "\n";
    };
    WGPURequestDeviceCallbackInfo device_callback = WGPU_REQUEST_DEVICE_CALLBACK_INFO_INIT;
    device_callback.mode = WGPUCallbackMode_AllowSpontaneous;
    device_callback.callback = [](WGPURequestDeviceStatus status, WGPUDevice result, WGPUStringView message,
                                  void *userdata, void *) {
        auto &p = *static_cast<Pending *>(userdata);
        if (status == WGPURequestDeviceStatus_Success) p.device = result;
        else p.message = to_string(message);
        p.done = true;
    };
    device_callback.userdata1 = &pending;
    wgpuAdapterRequestDevice(adapter, &device_desc, device_callback);
    while (!pending.done) emscripten_sleep(1);
    if (pending.device == nullptr) {
        error = "no WebGPU device: " + pending.message;
        return false;
    }
    device = pending.device;
    queue = wgpuDeviceGetQueue(device);

    WGPUEmscriptenSurfaceSourceCanvasHTMLSelector canvas = WGPU_EMSCRIPTEN_SURFACE_SOURCE_CANVAS_HTML_SELECTOR_INIT;
    canvas.selector = view_of(kCanvasSelector);
    WGPUSurfaceDescriptor surface_desc = WGPU_SURFACE_DESCRIPTOR_INIT;
    surface_desc.nextInChain = &canvas.chain;
    surface = wgpuInstanceCreateSurface(instance, &surface_desc);
    if (surface == nullptr) {
        error = "cannot create a WebGPU surface on the canvas";
        return false;
    }
    WGPUSurfaceCapabilities caps = WGPU_SURFACE_CAPABILITIES_INIT;
    if (wgpuSurfaceGetCapabilities(surface, adapter, &caps) == WGPUStatus_Success && caps.formatCount > 0u)
        format = caps.formats[0];
    wgpuSurfaceCapabilitiesFreeMembers(caps);
    if (format == WGPUTextureFormat_Undefined) format = WGPUTextureFormat_BGRA8Unorm;
    return true;
}

void VulkanRenderer::Impl::configure_surface() {
    int w = 0, h = 0;
    SDL_GetWindowSizeInPixels(window, &w, &h);
    if (w <= 0 || h <= 0) return;
    width = static_cast<std::uint32_t>(w);
    height = static_cast<std::uint32_t>(h);
    WGPUSurfaceConfiguration surface_config = WGPU_SURFACE_CONFIGURATION_INIT;
    surface_config.device = device;
    surface_config.format = format;
    surface_config.usage = WGPUTextureUsage_RenderAttachment;
    surface_config.width = width;
    surface_config.height = height;
    surface_config.alphaMode = WGPUCompositeAlphaMode_Opaque;
    surface_config.presentMode = WGPUPresentMode_Fifo;
    wgpuSurfaceConfigure(surface, &surface_config);
    configured = true;
}

static WGPUShaderModule create_module(WGPUDevice device, const char *code) {
    WGPUShaderSourceWGSL source = WGPU_SHADER_SOURCE_WGSL_INIT;
    source.code = view_of(code);
    WGPUShaderModuleDescriptor desc = WGPU_SHADER_MODULE_DESCRIPTOR_INIT;
    desc.nextInChain = &source.chain;
    return wgpuDeviceCreateShaderModule(device, &desc);
}

bool VulkanRenderer::Impl::create_ge_resources(std::string &error) {
    const std::uint32_t scale = std::clamp<std::uint32_t>(settings::current().internal_scale, 1u, 4u);
    target_width = kPspWidth * scale;
    target_height = kPspHeight * scale;

    ge_module = create_module(device, wgsl::kGeShader);
    blit_module = create_module(device, wgsl::kBlitShader);
    if (ge_module == nullptr || blit_module == nullptr) {
        error = "cannot compile the WGSL shaders";
        return false;
    }

    std::array<WGPUBindGroupLayoutEntry, 2> texture_entries{WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT,
                                                            WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT};
    texture_entries[0].binding = 0;
    texture_entries[0].visibility = WGPUShaderStage_Fragment;
    texture_entries[0].texture.sampleType = WGPUTextureSampleType_Float;
    texture_entries[0].texture.viewDimension = WGPUTextureViewDimension_2D;
    texture_entries[1].binding = 1;
    texture_entries[1].visibility = WGPUShaderStage_Fragment;
    texture_entries[1].sampler.type = WGPUSamplerBindingType_Filtering;
    WGPUBindGroupLayoutDescriptor texture_layout_desc = WGPU_BIND_GROUP_LAYOUT_DESCRIPTOR_INIT;
    texture_layout_desc.entryCount = texture_entries.size();
    texture_layout_desc.entries = texture_entries.data();
    texture_layout = wgpuDeviceCreateBindGroupLayout(device, &texture_layout_desc);

    WGPUBindGroupLayoutEntry draw_entry = WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT;
    draw_entry.binding = 0;
    draw_entry.visibility = WGPUShaderStage_Vertex | WGPUShaderStage_Fragment;
    draw_entry.buffer.type = WGPUBufferBindingType_Uniform;
    draw_entry.buffer.hasDynamicOffset = true;
    draw_entry.buffer.minBindingSize = sizeof(DrawBlock);
    WGPUBindGroupLayoutDescriptor draw_layout_desc = WGPU_BIND_GROUP_LAYOUT_DESCRIPTOR_INIT;
    draw_layout_desc.entryCount = 1;
    draw_layout_desc.entries = &draw_entry;
    draw_layout = wgpuDeviceCreateBindGroupLayout(device, &draw_layout_desc);

    const std::array<WGPUBindGroupLayout, 2> ge_groups{texture_layout, draw_layout};
    WGPUPipelineLayoutDescriptor ge_layout_desc = WGPU_PIPELINE_LAYOUT_DESCRIPTOR_INIT;
    ge_layout_desc.bindGroupLayoutCount = ge_groups.size();
    ge_layout_desc.bindGroupLayouts = ge_groups.data();
    ge_layout = wgpuDeviceCreatePipelineLayout(device, &ge_layout_desc);
    WGPUPipelineLayoutDescriptor blit_layout_desc = WGPU_PIPELINE_LAYOUT_DESCRIPTOR_INIT;
    blit_layout_desc.bindGroupLayoutCount = 1;
    blit_layout_desc.bindGroupLayouts = &texture_layout;
    blit_layout = wgpuDeviceCreatePipelineLayout(device, &blit_layout_desc);

    // As in the Vulkan renderer: one linear sampler that repeats.
    WGPUSamplerDescriptor sampler_desc = WGPU_SAMPLER_DESCRIPTOR_INIT;
    sampler_desc.addressModeU = WGPUAddressMode_Repeat;
    sampler_desc.addressModeV = WGPUAddressMode_Repeat;
    sampler_desc.magFilter = WGPUFilterMode_Linear;
    sampler_desc.minFilter = WGPUFilterMode_Linear;
    sampler = wgpuDeviceCreateSampler(device, &sampler_desc);

    WGPUBufferDescriptor vertex_desc = WGPU_BUFFER_DESCRIPTOR_INIT;
    vertex_desc.usage = WGPUBufferUsage_Vertex | WGPUBufferUsage_CopyDst;
    vertex_desc.size = kVertexBufferBytes;
    vertex_buffer = wgpuDeviceCreateBuffer(device, &vertex_desc);
    WGPUBufferDescriptor uniform_desc = WGPU_BUFFER_DESCRIPTOR_INIT;
    uniform_desc.usage = WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst;
    uniform_desc.size = kUniformBufferBytes;
    uniform_buffer = wgpuDeviceCreateBuffer(device, &uniform_desc);
    WGPUBindGroupEntry draw_group_entry = WGPU_BIND_GROUP_ENTRY_INIT;
    draw_group_entry.binding = 0;
    draw_group_entry.buffer = uniform_buffer;
    draw_group_entry.size = sizeof(DrawBlock);
    WGPUBindGroupDescriptor draw_group_desc = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
    draw_group_desc.layout = draw_layout;
    draw_group_desc.entryCount = 1;
    draw_group_desc.entries = &draw_group_entry;
    draw_group = wgpuDeviceCreateBindGroup(device, &draw_group_desc);

    const std::uint32_t white_texel = 0xFFFFFFFFu;
    white = make_texture(1u, 1u, &white_texel);

    WGPUColorTargetState blit_target = WGPU_COLOR_TARGET_STATE_INIT;
    blit_target.format = format;
    WGPUFragmentState blit_fragment = WGPU_FRAGMENT_STATE_INIT;
    blit_fragment.module = blit_module;
    blit_fragment.entryPoint = view_of("fragment_main");
    blit_fragment.targetCount = 1;
    blit_fragment.targets = &blit_target;
    WGPURenderPipelineDescriptor blit_desc = WGPU_RENDER_PIPELINE_DESCRIPTOR_INIT;
    blit_desc.layout = blit_layout;
    blit_desc.vertex.module = blit_module;
    blit_desc.vertex.entryPoint = view_of("vertex_main");
    blit_desc.fragment = &blit_fragment;
    blit_pipeline = wgpuDeviceCreateRenderPipeline(device, &blit_desc);

    vertex_bytes.reserve(4u << 20u);
    uniform_bytes.reserve(1u << 20u);
    ge_ready = blit_pipeline != nullptr && white.group != nullptr;
    if (!ge_ready) error = "cannot create the GE resources";
    return ge_ready;
}

WGPURenderPipeline VulkanRenderer::Impl::pipeline_for(const PipelineKey &key) {
    if (const auto found = pipelines.find(key); found != pipelines.end()) return found->second;

    const std::array<WGPUVertexAttribute, 4> attributes{{
        {nullptr, WGPUVertexFormat_Float32x4, offsetof(GpuVertex, x), 0},
        {nullptr, WGPUVertexFormat_Float32x2, offsetof(GpuVertex, u), 1},
        {nullptr, WGPUVertexFormat_Unorm8x4, offsetof(GpuVertex, color), 2},
        {nullptr, WGPUVertexFormat_Float32x3, offsetof(GpuVertex, nx), 3},
    }};
    WGPUVertexBufferLayout vertex_layout = WGPU_VERTEX_BUFFER_LAYOUT_INIT;
    vertex_layout.stepMode = WGPUVertexStepMode_Vertex;
    vertex_layout.arrayStride = sizeof(GpuVertex);
    vertex_layout.attributeCount = attributes.size();
    vertex_layout.attributes = attributes.data();

    WGPUBlendState blend = WGPU_BLEND_STATE_INIT;
    blend.color.operation = to_blend_operation(key.equation);
    blend.color.srcFactor = to_blend_factor(key.source_factor, true);
    blend.color.dstFactor = to_blend_factor(key.destination_factor, false);
    // WebGPU takes min and max only with factors of one; the GE ignores its
    // factors for them.
    if (blend.color.operation == WGPUBlendOperation_Min || blend.color.operation == WGPUBlendOperation_Max) {
        blend.color.srcFactor = WGPUBlendFactor_One;
        blend.color.dstFactor = WGPUBlendFactor_One;
    }
    blend.alpha.operation = WGPUBlendOperation_Add;
    blend.alpha.srcFactor = WGPUBlendFactor_One;
    blend.alpha.dstFactor = WGPUBlendFactor_Zero;
    WGPUColorTargetState color_target = WGPU_COLOR_TARGET_STATE_INIT;
    color_target.format = kTargetFormat;
    color_target.blend = key.blend ? &blend : nullptr;
    color_target.writeMask = key.color_mask;
    WGPUFragmentState fragment = WGPU_FRAGMENT_STATE_INIT;
    fragment.module = ge_module;
    fragment.entryPoint = view_of("fragment_main");
    fragment.targetCount = 1;
    fragment.targets = &color_target;

    // Without a depth test the GE writes no depth either.
    WGPUDepthStencilState depth = WGPU_DEPTH_STENCIL_STATE_INIT;
    depth.format = kDepthFormat;
    depth.depthWriteEnabled = key.depth_test && key.depth_write ? WGPUOptionalBool_True : WGPUOptionalBool_False;
    depth.depthCompare = key.depth_test ? to_compare(key.depth_function) : WGPUCompareFunction_Always;

    WGPURenderPipelineDescriptor desc = WGPU_RENDER_PIPELINE_DESCRIPTOR_INIT;
    desc.layout = ge_layout;
    desc.vertex.module = ge_module;
    desc.vertex.entryPoint = view_of("vertex_main");
    desc.vertex.bufferCount = 1;
    desc.vertex.buffers = &vertex_layout;
    desc.primitive.topology = WGPUPrimitiveTopology_TriangleList;
    desc.primitive.cullMode = key.cull ? WGPUCullMode_Back : WGPUCullMode_None;
    static const bool flip_cull = std::getenv("MHP3RD_WEB_FLIP_CULL") != nullptr;
    desc.primitive.frontFace =
        key.cull_clockwise != flip_cull ? WGPUFrontFace_CW : WGPUFrontFace_CCW;
    desc.depthStencil = &depth;
    desc.fragment = &fragment;
    WGPURenderPipeline pipeline = wgpuDeviceCreateRenderPipeline(device, &desc);
    pipelines.emplace(key, pipeline);
    return pipeline;
}

VulkanRenderer::Impl::Target *VulkanRenderer::Impl::target_for(std::uint32_t address) {
    if (const auto found = targets.find(address); found != targets.end()) return &found->second;
    Target target;
    WGPUTextureDescriptor color_desc = WGPU_TEXTURE_DESCRIPTOR_INIT;
    color_desc.usage = WGPUTextureUsage_RenderAttachment | WGPUTextureUsage_TextureBinding;
    color_desc.size = {target_width, target_height, 1u};
    color_desc.format = kTargetFormat;
    target.color = wgpuDeviceCreateTexture(device, &color_desc);
    target.color_view = wgpuTextureCreateView(target.color, nullptr);
    WGPUTextureDescriptor depth_desc = color_desc;
    depth_desc.usage = WGPUTextureUsage_RenderAttachment;
    depth_desc.format = kDepthFormat;
    target.depth = wgpuDeviceCreateTexture(device, &depth_desc);
    target.depth_view = wgpuTextureCreateView(target.depth, nullptr);
    std::array<WGPUBindGroupEntry, 2> entries{WGPU_BIND_GROUP_ENTRY_INIT, WGPU_BIND_GROUP_ENTRY_INIT};
    entries[0].binding = 0;
    entries[0].textureView = target.color_view;
    entries[1].binding = 1;
    entries[1].sampler = sampler;
    WGPUBindGroupDescriptor group_desc = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
    group_desc.layout = texture_layout;
    group_desc.entryCount = entries.size();
    group_desc.entries = entries.data();
    target.sample_group = wgpuDeviceCreateBindGroup(device, &group_desc);
    return &targets.emplace(address, target).first->second;
}

VulkanRenderer::Impl::Texture VulkanRenderer::Impl::make_texture(std::uint32_t width, std::uint32_t height,
                                                                 const std::uint32_t *pixels) {
    Texture texture;
    WGPUTextureDescriptor desc = WGPU_TEXTURE_DESCRIPTOR_INIT;
    desc.usage = WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopyDst;
    desc.size = {width, height, 1u};
    desc.format = WGPUTextureFormat_RGBA8Unorm;
    texture.texture = wgpuDeviceCreateTexture(device, &desc);
    texture.view = wgpuTextureCreateView(texture.texture, nullptr);
    WGPUTexelCopyTextureInfo destination = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
    destination.texture = texture.texture;
    WGPUTexelCopyBufferLayout layout = WGPU_TEXEL_COPY_BUFFER_LAYOUT_INIT;
    layout.bytesPerRow = width * 4u;
    layout.rowsPerImage = height;
    const WGPUExtent3D size{width, height, 1u};
    wgpuQueueWriteTexture(queue, &destination, pixels, static_cast<std::size_t>(width) * height * 4u, &layout, &size);
    std::array<WGPUBindGroupEntry, 2> entries{WGPU_BIND_GROUP_ENTRY_INIT, WGPU_BIND_GROUP_ENTRY_INIT};
    entries[0].binding = 0;
    entries[0].textureView = texture.view;
    entries[1].binding = 1;
    entries[1].sampler = sampler;
    WGPUBindGroupDescriptor group_desc = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
    group_desc.layout = texture_layout;
    group_desc.entryCount = entries.size();
    group_desc.entries = entries.data();
    texture.group = wgpuDeviceCreateBindGroup(device, &group_desc);
    texture.last_used = ++texture_clock;
    return texture;
}

void VulkanRenderer::Impl::release_texture(Texture &texture) {
    if (texture.group != nullptr) wgpuBindGroupRelease(texture.group);
    if (texture.view != nullptr) wgpuTextureViewRelease(texture.view);
    if (texture.texture != nullptr) {
        wgpuTextureDestroy(texture.texture);
        wgpuTextureRelease(texture.texture);
    }
    texture = Texture{};
}

// The game's texture, decoded once per distinct content (texture_key).
WGPUBindGroup VulkanRenderer::Impl::texture_group(const GuestMemory &memory, const TextureState &state) {
    const std::uint64_t key = texture_key(memory, state);
    if (const auto found = textures.find(key); found != textures.end()) {
        found->second.last_used = ++texture_clock;
        return found->second.group;
    }
    if (!decode_texture(memory, state, decoded)) return white.group;
    Texture texture = make_texture(state.width, state.height, decoded.data());
    textures.emplace(key, texture);
    return texture.group;
}

void VulkanRenderer::Impl::begin_recording() {
    if (encoder != nullptr) return;
    // Textures unused for longest go between frames, never while a frame
    // that may sample them is being recorded.
    while (textures.size() > kMaxCachedTextures) {
        auto oldest = textures.begin();
        for (auto it = textures.begin(); it != textures.end(); ++it)
            if (it->second.last_used < oldest->second.last_used) oldest = it;
        release_texture(oldest->second);
        textures.erase(oldest);
    }
    encoder = wgpuDeviceCreateCommandEncoder(device, nullptr);
}

void VulkanRenderer::Impl::end_pass() {
    if (pass == nullptr) return;
    wgpuRenderPassEncoderEnd(pass);
    wgpuRenderPassEncoderRelease(pass);
    pass = nullptr;
}

void VulkanRenderer::Impl::begin_pass(std::uint32_t address) {
    end_pass();
    begin_recording();
    Target *target = target_for(address);
    WGPURenderPassColorAttachment color = WGPU_RENDER_PASS_COLOR_ATTACHMENT_INIT;
    color.view = target->color_view;
    color.loadOp = target->cleared ? WGPULoadOp_Load : WGPULoadOp_Clear;
    color.storeOp = WGPUStoreOp_Store;
    color.clearValue = WGPUColor{0.0, 0.0, 0.0, 1.0};
    WGPURenderPassDepthStencilAttachment depth = WGPU_RENDER_PASS_DEPTH_STENCIL_ATTACHMENT_INIT;
    depth.view = target->depth_view;
    depth.depthLoadOp = target->cleared ? WGPULoadOp_Load : WGPULoadOp_Clear;
    depth.depthStoreOp = WGPUStoreOp_Store;
    depth.depthClearValue = 0.0f;
    WGPURenderPassDescriptor desc = WGPU_RENDER_PASS_DESCRIPTOR_INIT;
    desc.colorAttachmentCount = 1;
    desc.colorAttachments = &color;
    desc.depthStencilAttachment = &depth;
    pass = wgpuCommandEncoderBeginRenderPass(encoder, &desc);
    target->cleared = true;
    pass_target = address;
}

void VulkanRenderer::Impl::flush() {
    end_pass();
    if (encoder == nullptr) return;
    if (!vertex_bytes.empty()) wgpuQueueWriteBuffer(queue, vertex_buffer, 0, vertex_bytes.data(), vertex_bytes.size());
    if (!uniform_bytes.empty())
        wgpuQueueWriteBuffer(queue, uniform_buffer, 0, uniform_bytes.data(), uniform_bytes.size());
    WGPUCommandBuffer commands = wgpuCommandEncoderFinish(encoder, nullptr);
    wgpuQueueSubmit(queue, 1, &commands);
    wgpuCommandBufferRelease(commands);
    wgpuCommandEncoderRelease(encoder);
    encoder = nullptr;
    vertex_bytes.clear();
    uniform_bytes.clear();
}

void VulkanRenderer::Impl::finish_frame(bool show_game) {
    end_pass();
    // The window follows the page's area for it (#screen in web/shell.html),
    // which changes with the browser window and the log below it.
    double area_width = 0.0, area_height = 0.0;
    if (emscripten_get_element_css_size(kScreenSelector, &area_width, &area_height) == EMSCRIPTEN_RESULT_SUCCESS &&
        area_width >= 1.0 && area_height >= 1.0) {
        int window_width = 0, window_height = 0;
        SDL_GetWindowSize(window, &window_width, &window_height);
        const int want_width = static_cast<int>(area_width), want_height = static_cast<int>(area_height);
        if (want_width != window_width || want_height != window_height)
            SDL_SetWindowSize(window, want_width, want_height);
    }
    int pixel_width = 0, pixel_height = 0;
    SDL_GetWindowSizeInPixels(window, &pixel_width, &pixel_height);
    if (static_cast<std::uint32_t>(pixel_width) != width || static_cast<std::uint32_t>(pixel_height) != height)
        configured = false;
    if (!configured) configure_surface();
    if (!configured) {
        flush();
        return;
    }
    WGPUSurfaceTexture image = WGPU_SURFACE_TEXTURE_INIT;
    wgpuSurfaceGetCurrentTexture(surface, &image);
    if (ImGui_ImplWGPU_IsSurfaceStatusError(image.status)) {
        std::cerr << "[webgpu] cannot get the canvas image (status " << static_cast<int>(image.status) << ")\n";
        configured = false;
        flush();
        return;
    }
    WGPUTextureView canvas = wgpuTextureCreateView(image.texture, nullptr);
    begin_recording();
    WGPURenderPassColorAttachment color = WGPU_RENDER_PASS_COLOR_ATTACHMENT_INIT;
    color.view = canvas;
    color.loadOp = WGPULoadOp_Clear;
    color.storeOp = WGPUStoreOp_Store;
    color.clearValue = WGPUColor{0.0, 0.0, 0.0, 1.0};
    WGPURenderPassDescriptor desc = WGPU_RENDER_PASS_DESCRIPTOR_INIT;
    desc.colorAttachmentCount = 1;
    desc.colorAttachments = &color;
    WGPURenderPassEncoder canvas_pass = wgpuCommandEncoderBeginRenderPass(encoder, &desc);
    const auto shown = show_game && shown_valid ? targets.find(shown_target) : targets.end();
    const bool movie_shown = show_game && shown_valid && movie_valid && movie_address == shown_target &&
                             movie.group != nullptr;
    if ((shown != targets.end() || movie_shown) && ge_ready) {
        // The PSP's shape, as large as the canvas allows, centred.
        const float scale = std::min(static_cast<float>(width) / kPspWidth, static_cast<float>(height) / kPspHeight);
        const float w = std::floor(kPspWidth * scale), h = std::floor(kPspHeight * scale);
        wgpuRenderPassEncoderSetViewport(canvas_pass, std::floor((width - w) * 0.5f), std::floor((height - h) * 0.5f),
                                         w, h, 0.0f, 1.0f);
        wgpuRenderPassEncoderSetPipeline(canvas_pass, blit_pipeline);
        wgpuRenderPassEncoderSetBindGroup(canvas_pass, 0, movie_shown ? movie.group : shown->second.sample_group, 0,
                                          nullptr);
        wgpuRenderPassEncoderDraw(canvas_pass, 3, 1, 0, 0);
        wgpuRenderPassEncoderSetViewport(canvas_pass, 0.0f, 0.0f, static_cast<float>(width),
                                         static_cast<float>(height), 0.0f, 1.0f);
    }
    if (ui_ready && ui_draw_data != nullptr) ImGui_ImplWGPU_RenderDrawData(ui_draw_data, canvas_pass);
    wgpuRenderPassEncoderEnd(canvas_pass);
    wgpuRenderPassEncoderRelease(canvas_pass);
    flush();
    wgpuTextureViewRelease(canvas);
    wgpuTextureRelease(image.texture);
    ui_draw_data = nullptr;
    ++frames;
    // The browser shows the canvas when this turn ends, not at a present call.
    wait_for_animation_frame();
}

// One GE draw, recorded as vulkan_renderer.cpp's submit() records it, minus
// what this renderer leaves out (see the top of the file).
void VulkanRenderer::Impl::draw(const DrawCall &call, const GuestMemory &memory) {
    if (call.vertices.empty()) return;
    if (call.through && hud::hides(call.command_address, call.call_return)) return;

    const bool lit = call.lighting_enabled && !call.through && !call.clear_mode;
    const bool use_material_color = !call.has_vertex_color && !call.lighting_enabled;
    const auto push_vertex = [&](const Vertex &vertex) {
        GpuVertex out{};
        out.x = vertex.position[0];
        out.y = vertex.position[1];
        out.z = vertex.position[2];
        out.u = vertex.texcoord[0];
        out.v = vertex.texcoord[1];
        out.color = use_material_color ? call.material_color : vertex.color;
        out.nx = vertex.normal[0];
        out.ny = vertex.normal[1];
        out.nz = vertex.normal[2];
        scratch.push_back(out);
    };
    const auto vertex_at = [&](std::size_t index) -> const Vertex & {
        if (!call.indices.empty())
            return call.vertices[std::min<std::size_t>(call.indices[index], call.vertices.size() - 1u)];
        return call.vertices[std::min(index, call.vertices.size() - 1u)];
    };
    const std::size_t count = call.indices.empty() ? call.vertices.size() : call.indices.size();

    // Everything becomes a triangle list; sprites expand to two triangles.
    scratch.clear();
    switch (call.primitive) {
    case PrimitiveType::Triangles:
        for (std::size_t i = 0; i + 2u < count; i += 3u) {
            push_vertex(vertex_at(i));
            push_vertex(vertex_at(i + 1u));
            push_vertex(vertex_at(i + 2u));
        }
        break;
    case PrimitiveType::TriangleStrip:
        for (std::size_t i = 0; i + 2u < count; ++i) {
            const bool odd = (i & 1u) != 0u;
            push_vertex(vertex_at(i));
            push_vertex(vertex_at(odd ? i + 2u : i + 1u));
            push_vertex(vertex_at(odd ? i + 1u : i + 2u));
        }
        break;
    case PrimitiveType::TriangleFan:
        for (std::size_t i = 1u; i + 1u < count; ++i) {
            push_vertex(vertex_at(0));
            push_vertex(vertex_at(i));
            push_vertex(vertex_at(i + 1u));
        }
        break;
    case PrimitiveType::Sprites:
        for (std::size_t i = 0; i + 1u < count; i += 2u) {
            Vertex a = vertex_at(i);
            const Vertex &b = vertex_at(i + 1u);
            a.position[2] = b.position[2];
            a.color = b.color;
            a.normal = b.normal;
            Vertex top_right = b;
            top_right.position[1] = a.position[1];
            top_right.texcoord[1] = a.texcoord[1];
            Vertex bottom_left = a;
            bottom_left.position[1] = b.position[1];
            bottom_left.texcoord[1] = b.texcoord[1];
            push_vertex(a);
            push_vertex(top_right);
            push_vertex(b);
            push_vertex(a);
            push_vertex(b);
            push_vertex(bottom_left);
        }
        break;
    default:
        return;  // points and lines are not drawn yet
    }
    if (scratch.empty()) return;
    if (call.through) {
        for (GpuVertex &vertex : scratch) set_uv_rect(vertex, -kNoClamp, -kNoClamp, kNoClamp, kNoClamp);
        const bool quads = call.primitive == PrimitiveType::Sprites ||
                           ((call.primitive == PrimitiveType::TriangleStrip ||
                             call.primitive == PrimitiveType::TriangleFan) &&
                            count == 4u);
        if (call.texture.enabled && !call.clear_mode && quads)
            for (std::size_t first = 0; first + 6u <= scratch.size(); first += 6u)
                clamp_through_quad(scratch, first, static_cast<float>(call.texture.width),
                                   static_cast<float>(call.texture.height));
    }

    // A frame that outgrows the buffers is submitted in parts.
    const std::size_t vertex_size = scratch.size() * sizeof(GpuVertex);
    if (vertex_size > kVertexBufferBytes) return;
    if (vertex_bytes.size() + vertex_size > kVertexBufferBytes ||
        uniform_bytes.size() + kUniformStride > kUniformBufferBytes) {
        const std::uint32_t resume = pass_target;
        const bool had_pass = pass != nullptr;
        flush();
        if (had_pass) begin_pass(resume);
    }

    PipelineKey key{};
    if (call.clear_mode) {
        key.depth_test = true;
        key.depth_function = 1u;  // always
        key.depth_write = (call.clear_flags & 4u) != 0u;
        key.color_mask = ((call.clear_flags & 1u) != 0u
                              ? WGPUColorWriteMask_Red | WGPUColorWriteMask_Green | WGPUColorWriteMask_Blue
                              : 0u) |
                         ((call.clear_flags & 2u) != 0u ? WGPUColorWriteMask_Alpha : 0u);
    } else {
        key.blend = call.blend.enabled;
        key.source_factor = resolve_fixed_factor(call.blend.source_factor, call.blend.fixed_source);
        key.destination_factor = resolve_fixed_factor(call.blend.destination_factor, call.blend.fixed_destination);
        if (key.source_factor == kFactorFixed && key.destination_factor == kFactorFixed &&
            ((call.blend.fixed_source + call.blend.fixed_destination) & 0x00FFFFFFu) == 0x00FFFFFFu)
            key.destination_factor = kFactorInverseConstant;
        key.equation = call.blend.equation;
        key.depth_test = call.depth.test_enabled && !call.through;
        key.depth_write = call.depth.write_enabled;
        key.depth_function = call.depth.function;
        key.cull = call.culling_enabled && !call.through && call.primitive != PrimitiveType::Sprites;
        key.cull_clockwise = call.cull_clockwise;
    }
    WGPURenderPipeline pipeline = pipeline_for(key);
    if (pipeline == nullptr) return;

    const bool fogged = call.fog.enabled && !call.through && !call.clear_mode;
    const auto view_world = multiply(call.view, call.world);
    DrawBlock block{};
    block.transform = multiply(call.projection, view_world);
    block.viewport = {static_cast<float>(kPspWidth), static_cast<float>(kPspHeight), call.through ? 1.0f : 0.0f,
                      (fogged ? 1.0f : 0.0f) + (lit ? 2.0f : 0.0f)};
    block.view_z = {view_world[2], view_world[6], view_world[10], view_world[14]};
    block.texture_params = {call.texture.enabled ? 1.0f : 0.0f, static_cast<float>(call.texture.function),
                            static_cast<float>(call.alpha_test.enabled ? call.alpha_test.reference : 0u),
                            static_cast<float>(call.alpha_test.enabled ? call.alpha_test.function : 0u)};
    block.uv_transform = call.through && call.texture.width != 0u
                             ? std::array<float, 4>{1.0f / static_cast<float>(call.texture.width),
                                                    1.0f / static_cast<float>(call.texture.height), 0.0f, 0.0f}
                             : std::array<float, 4>{call.texture.scale_u, call.texture.scale_v, call.texture.offset_u,
                                                    call.texture.offset_v};
    if (call.clear_mode) block.texture_params = {0.0f, 0.0f, 0.0f, 0.0f};
    if (lit || fogged) {
        const LightingState &state = call.lighting;
        block.ambient = unpack_color(state.ambient_color, static_cast<float>(state.ambient_alpha) / 255.0f);
        block.fog = {call.fog.end, call.fog.scale, 0.0f, 0.0f};
        block.fog_color = unpack_color(call.fog.color);
        for (std::size_t i = 0; i < state.lights.size(); ++i) {
            const LightState &light = state.lights[i];
            block.light_position[i] = {light.position[0], light.position[1], light.position[2],
                                       light.enabled ? 1.0f : 0.0f};
            block.light_direction[i] = {light.direction[0], light.direction[1], light.direction[2],
                                        static_cast<float>(light.type)};
            block.light_attenuation[i] = {light.attenuation[0], light.attenuation[1], light.attenuation[2],
                                          static_cast<float>(light.kind)};
            block.light_spot[i] = {light.spot_exponent, light.spot_cutoff, 0.0f, 0.0f};
            block.light_ambient[i] = unpack_color(light.ambient);
            block.light_diffuse[i] = unpack_color(light.diffuse);
            block.light_specular[i] = unpack_color(light.specular);
        }
    }
    if (lit) {
        const LightingState &state = call.lighting;
        block.world = call.world;
        block.flags = {1.0f, call.has_vertex_color ? 1.0f : 0.0f, 0.0f, static_cast<float>(state.material_update)};
        block.emissive = unpack_color(state.material_emissive, state.specular_power);
        block.material_ambient =
            unpack_color(call.material_color, static_cast<float>(call.material_color >> 24u) / 255.0f);
        block.material_diffuse = unpack_color(state.material_diffuse, static_cast<float>(state.mode));
        block.material_specular = unpack_color(state.material_specular, state.reverse_normals ? 1.0f : 0.0f);
    }

    // The GE viewport, in target pixels, as the Vulkan renderer sets it; the
    // shader folds it into the whole target (see webgpu_shaders.hpp).
    const float scale_x = static_cast<float>(target_width) / kPspWidth;
    const float scale_y = static_cast<float>(target_height) / kPspHeight;
    const float full_w = static_cast<float>(target_width), full_h = static_cast<float>(target_height);
    float vx = 0.0f, vy = 0.0f, vw = full_w, vh = full_h, near_depth = 0.0f, far_depth = 1.0f;
    if (!call.through && call.viewport.x_scale != 0.0f && call.viewport.y_scale != 0.0f) {
        const ViewportState &vp = call.viewport;
        vx = (vp.x_offset - vp.offset_x - vp.x_scale) * scale_x;
        vy = (vp.y_offset - vp.offset_y - vp.y_scale) * scale_y;
        vw = 2.0f * vp.x_scale * scale_x;
        vh = 2.0f * vp.y_scale * scale_y;
        if (vp.z_scale != 0.0f) {
            near_depth = std::clamp((vp.z_offset - vp.z_scale) / 65535.0f, 0.0f, 1.0f);
            far_depth = std::clamp((vp.z_offset + vp.z_scale) / 65535.0f, 0.0f, 1.0f);
        }
    }
    block.fold = {vw / full_w, -vh / full_h, (2.0f * vx + vw) / full_w - 1.0f, 1.0f - (2.0f * vy + vh) / full_h};
    if (near_depth > far_depth) {
        std::swap(near_depth, far_depth);
        block.extra[0] = 1.0f;
    }

    const std::uint32_t constant_color =
        key.source_factor == kFactorFixed ? call.blend.fixed_source : call.blend.fixed_destination;
    const WGPUColor blend_constant{static_cast<double>(constant_color & 0xFFu) / 255.0,
                                   static_cast<double>((constant_color >> 8u) & 0xFFu) / 255.0,
                                   static_cast<double>((constant_color >> 16u) & 0xFFu) / 255.0, 1.0};

    const auto clamp_axis = [](std::uint32_t value, std::uint32_t limit) { return std::min(value, limit); };
    const std::uint32_t sx1 = clamp_axis(call.viewport.scissor_x1, kPspWidth - 1u);
    const std::uint32_t sy1 = clamp_axis(call.viewport.scissor_y1, kPspHeight - 1u);
    const std::uint32_t sx2 = clamp_axis(std::max(call.viewport.scissor_x2, sx1), kPspWidth - 1u);
    const std::uint32_t sy2 = clamp_axis(std::max(call.viewport.scissor_y2, sy1), kPspHeight - 1u);
    const auto to_pixels = [](float edge, float scale, std::uint32_t size) {
        return static_cast<std::uint32_t>(std::clamp(std::lround(edge * scale), 0l, static_cast<long>(size)));
    };
    const std::uint32_t left = to_pixels(static_cast<float>(sx1), scale_x, target_width);
    const std::uint32_t top = to_pixels(static_cast<float>(sy1), scale_y, target_height);
    const std::uint32_t right = std::max(left, to_pixels(static_cast<float>(sx2 + 1u), scale_x, target_width));
    const std::uint32_t bottom = std::max(top, to_pixels(static_cast<float>(sy2 + 1u), scale_y, target_height));
    if (right == left || bottom == top) return;

    WGPUBindGroup texture = white.group;
    if (call.texture.enabled && !call.clear_mode) texture = texture_group(memory, call.texture);

    if (pass == nullptr || pass_target != call.target.color_address) begin_pass(call.target.color_address);
    if (movie_valid && movie_address == call.target.color_address) movie_valid = false;

    const std::uint64_t vertex_offset = vertex_bytes.size();
    vertex_bytes.resize(vertex_offset + vertex_size);
    std::memcpy(vertex_bytes.data() + vertex_offset, scratch.data(), vertex_size);
    const std::uint64_t uniform_offset = uniform_bytes.size();
    uniform_bytes.resize(uniform_offset + kUniformStride);
    std::memcpy(uniform_bytes.data() + uniform_offset, &block, sizeof(block));

    wgpuRenderPassEncoderSetPipeline(pass, pipeline);
    wgpuRenderPassEncoderSetViewport(pass, 0.0f, 0.0f, full_w, full_h, near_depth, far_depth);
    wgpuRenderPassEncoderSetScissorRect(pass, left, top, right - left, bottom - top);
    wgpuRenderPassEncoderSetBlendConstant(pass, &blend_constant);
    wgpuRenderPassEncoderSetBindGroup(pass, 0, texture, 0, nullptr);
    const auto dynamic_offset = static_cast<std::uint32_t>(uniform_offset);
    wgpuRenderPassEncoderSetBindGroup(pass, 1, draw_group, 1, &dynamic_offset);
    wgpuRenderPassEncoderSetVertexBuffer(pass, 0, vertex_buffer, vertex_offset, vertex_size);
    wgpuRenderPassEncoderDraw(pass, static_cast<std::uint32_t>(scratch.size()), 1, 0, 0);
    ++draws;
}

VulkanRenderer::VulkanRenderer() : impl_(std::make_unique<Impl>()) {}
VulkanRenderer::~VulkanRenderer() { shutdown(); }

bool VulkanRenderer::initialize(const RendererConfig &config, std::string &error) {
    Impl &impl = *impl_;
    impl.config = config;
    SDL_SetHint(SDL_HINT_EMSCRIPTEN_CANVAS_SELECTOR, kCanvasSelector);
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        error = std::string("SDL_Init failed: ") + SDL_GetError();
        return false;
    }
    if (!SDL_InitSubSystem(SDL_INIT_GAMEPAD)) std::cout << "[pad] no gamepad support: " << SDL_GetError() << "\n";
    const std::uint32_t scale = std::clamp<std::uint32_t>(settings::current().window_scale, 1u, settings::kMaxWindowScale);
    impl.window = SDL_CreateWindow(config.title.c_str(), static_cast<int>(kPspWidth * scale),
                                   static_cast<int>(kPspHeight * scale),
                                   SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    if (impl.window == nullptr) {
        error = std::string("SDL_CreateWindow failed: ") + SDL_GetError();
        return false;
    }
    if (!impl.request_device(error)) return false;
    impl.configure_surface();
    if (!impl.create_ge_resources(error)) return false;
    impl.ready = true;
    std::cout << "[webgpu] " << impl.device_name << ", canvas " << impl.width << "x" << impl.height << "\n";
    return true;
}

void VulkanRenderer::shutdown() {
    if (!impl_) return;
    Impl &impl = *impl_;
    shutdown_ui();
    if (impl.surface != nullptr) wgpuSurfaceRelease(impl.surface);
    if (impl.queue != nullptr) wgpuQueueRelease(impl.queue);
    if (impl.device != nullptr) wgpuDeviceRelease(impl.device);
    if (impl.adapter != nullptr) wgpuAdapterRelease(impl.adapter);
    if (impl.instance != nullptr) wgpuInstanceRelease(impl.instance);
    impl.surface = nullptr;
    impl.queue = nullptr;
    impl.device = nullptr;
    impl.adapter = nullptr;
    impl.instance = nullptr;
    if (impl.window != nullptr) SDL_DestroyWindow(impl.window);
    impl.window = nullptr;
    impl.ready = false;
}

bool VulkanRenderer::available() const noexcept { return impl_ && impl_->ready; }
bool VulkanRenderer::quit_requested() const noexcept { return impl_ && impl_->quit; }
std::uint64_t VulkanRenderer::frames_presented() const noexcept { return impl_ ? impl_->frames : 0u; }
std::uint64_t VulkanRenderer::draws_submitted() const noexcept { return impl_ ? impl_->draws : 0u; }
CameraReading VulkanRenderer::camera() const noexcept { return {}; }

void VulkanRenderer::Impl::open_gamepad(SDL_JoystickID id) {
    if (gamepad != nullptr) return;
    gamepad = SDL_OpenGamepad(id);
    if (gamepad == nullptr) return;
    gamepad_id = id;
    const char *name = SDL_GetGamepadName(gamepad);
    std::cout << "[pad] " << (name != nullptr ? name : "gamepad") << " drives the game\n";
}

void VulkanRenderer::Impl::close_gamepad(SDL_JoystickID id) {
    if (gamepad == nullptr || id != gamepad_id) return;
    SDL_CloseGamepad(gamepad);
    gamepad = nullptr;
    gamepad_id = 0;
    pad_resolver.reset();
    mapped = {};
    int count = 0;
    if (SDL_JoystickID *ids = SDL_GetGamepads(&count)) {
        for (int i = 0; i < count && gamepad == nullptr; ++i) open_gamepad(ids[i]);
        SDL_free(ids);
    }
}

// The keyboard and the gamepad to the PSP pad, through the player's bindings,
// as the Vulkan renderer's sample_pad() does. The page has the keys while it
// has focus, so there is no window focus to check.
void VulkanRenderer::Impl::sample_pad() {
    const settings::Settings &player = settings::current();
    const std::uint64_t now = SDL_GetTicks();
    const bool *keys = SDL_GetKeyboardState(nullptr);
    typed = keys_resolver.update(
        input::Table{player.controls.keys, player.controls.combos, false},
        [&](input::Binding binding) {
            if (const int button = input::mouse_button_of(binding)) return (mouse_buttons & (1u << button)) != 0u;
            const int position = input::key_position(binding);
            return position >= 0 && position < SDL_SCANCODE_COUNT && keys[position];
        },
        now, player.chord_window);
    if (gamepad != nullptr) {
        static const input::Chord kMenu =
            input::chord(input::pad(input::PadInput::LeftStick), input::pad(input::PadInput::RightStick));
        const PadTuning tuning = pad_tuning();
        mapped = pad_resolver.update(
            input::Table{player.controls.pad, player.controls.combos, true},
            [&](input::Binding binding) { return pad_input_held(gamepad, binding, tuning); }, now,
            player.chord_window, std::span<const input::Chord>(&kMenu, 1u));
    }
    if (!game_input || free_camera) {
        pad = PadState{};
        return;
    }
    PadState next{};
    next.buttons = typed.buttons;
    next.fast_forward = typed.fast_forward;
    int analog_x = typed.stick_x;
    int analog_y = typed.stick_y;
    if (player.right_stick == settings::RightStick::Camera) {
        next.right_x = static_cast<std::uint8_t>(0x80 + typed.camera_x);
        next.right_y = static_cast<std::uint8_t>(0x80 + typed.camera_y);
    } else if (player.right_stick == settings::RightStick::DPad) {
        if (typed.camera_x < 0) next.buttons |= 0x0080u;
        if (typed.camera_x > 0) next.buttons |= 0x0020u;
        if (typed.camera_y < 0) next.buttons |= 0x0010u;
        if (typed.camera_y > 0) next.buttons |= 0x0040u;
    }
    if (gamepad != nullptr) read_gamepad(gamepad, mapped, next, analog_x, analog_y);
    next.analog_x = static_cast<std::uint8_t>(std::clamp(0x80 + analog_x, 0, 255));
    next.analog_y = static_cast<std::uint8_t>(std::clamp(0x80 + analog_y, 0, 255));
    // Buttons held when the game got its input back stay hidden from it until
    // they are released.
    if (suppress_held) {
        suppressed_buttons = next.buttons;
        suppress_held = false;
    }
    suppressed_buttons &= next.buttons;
    next.buttons &= ~suppressed_buttons;
    pad = next;
}

bool VulkanRenderer::pump_events() {
    if (!impl_ || impl_->window == nullptr) return false;
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        if (event.type == SDL_EVENT_QUIT || event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) impl_->quit = true;
        if (event.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED) impl_->configured = false;
        if (event.type == SDL_EVENT_GAMEPAD_ADDED) impl_->open_gamepad(event.gdevice.which);
        if (event.type == SDL_EVENT_GAMEPAD_REMOVED) impl_->close_gamepad(event.gdevice.which);
        // Mouse buttons press what the bindings put on them (the attacks by
        // default) while the game has the input. Releases always count.
        if (event.type == SDL_EVENT_MOUSE_BUTTON_UP && event.button.button < 32u)
            impl_->mouse_buttons &= ~(1u << event.button.button);
        if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN && event.button.button < 32u && impl_->game_input &&
            event.button.which != SDL_TOUCH_MOUSEID)
            impl_->mouse_buttons |= 1u << event.button.button;
        if (impl_->event_hook && impl_->event_hook(event)) continue;
    }
    impl_->sample_pad();
    return !impl_->quit;
}

PadState VulkanRenderer::pad() const noexcept { return impl_ ? impl_->pad : PadState{}; }
void VulkanRenderer::sample_pad() {
    if (!impl_ || impl_->window == nullptr) return;
    SDL_PumpEvents();
    impl_->sample_pad();
}
MouseMotion VulkanRenderer::take_mouse_motion() noexcept { return {}; }
bool VulkanRenderer::touch_controls_visible() const noexcept { return false; }
const input::touch::Controls &VulkanRenderer::touch_controls() const { return impl_->touch; }
const input::touch::ActionControls &VulkanRenderer::action_touch_controls() const { return impl_->action_touch; }
MouseMotion VulkanRenderer::take_touch_motion() noexcept { return {}; }
bool VulkanRenderer::take_touch_menu() noexcept { return false; }
bool VulkanRenderer::mouse_captured() const noexcept { return false; }
void VulkanRenderer::set_pointer_free(bool) {}
void VulkanRenderer::set_scripted_key(int, bool) {}
void VulkanRenderer::set_scripted_input(bool) {}
void VulkanRenderer::request_quit() noexcept {
    if (impl_) impl_->quit = true;
}
void VulkanRenderer::hold_frame(bool) {}
void VulkanRenderer::set_event_hook(std::function<bool(const SDL_Event &)> hook) {
    if (impl_) impl_->event_hook = std::move(hook);
}
void VulkanRenderer::set_game_input(bool enabled) {
    if (!impl_) return;
    if (enabled && !impl_->game_input) impl_->suppress_held = true;
    impl_->game_input = enabled;
}
void VulkanRenderer::set_free_camera(bool flying) {
    if (!impl_ || impl_->free_camera == flying) return;
    if (!flying) impl_->suppress_held = true;
    impl_->free_camera = flying;
}
FreeCameraControls VulkanRenderer::take_free_camera_controls() { return {}; }
bool VulkanRenderer::take_screenshot_request() noexcept { return false; }
bool VulkanRenderer::frame_step_held() const noexcept { return false; }
bool VulkanRenderer::take_hide_hud_toggle() noexcept { return false; }
bool VulkanRenderer::take_lock_on_press() noexcept { return false; }
bool VulkanRenderer::window_capture_pending() const noexcept { return false; }

void VulkanRenderer::begin_frame() {
    if (impl_ && impl_->ready) impl_->begin_recording();
}
void VulkanRenderer::begin_display_list() {}
bool VulkanRenderer::gpu_decode() const { return false; }
bool VulkanRenderer::check_gpu_decode() const { return false; }
void VulkanRenderer::submit(const DrawCall &call, const GuestMemory &memory) {
    if (impl_ && impl_->ready && impl_->ge_ready) impl_->draw(call, memory);
}
void VulkanRenderer::write_back_frame(GuestMemory &) {}
void VulkanRenderer::read_back_framebuffer(std::uint32_t, GuestMemory &) {}
bool VulkanRenderer::present(std::uint32_t display_address, std::optional<std::chrono::steady_clock::time_point>) {
    if (!impl_ || !impl_->ready) return false;
    impl_->shown_target = display_address;
    impl_->shown_valid = true;
    impl_->finish_frame(true);
    return true;
}
void VulkanRenderer::present_due() {}
// The kernel's idle hook: it is about to sleep until `wake` to keep the game
// at PSP speed. On the page's thread a sleep would spin and freeze the tab, so
// give the browser the time instead; the sleep that follows finds it passed.
void VulkanRenderer::present_until(std::chrono::steady_clock::time_point wake) {
    const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(wake - std::chrono::steady_clock::now());
    if (left.count() >= 1) emscripten_sleep(static_cast<unsigned>(left.count()));
}
void VulkanRenderer::pause_interpolation() {}
void VulkanRenderer::set_still(bool) {}
void VulkanRenderer::set_fast_forward(bool) {}
void VulkanRenderer::set_frame_rate(settings::FrameRate) {}
void VulkanRenderer::set_frame_rate_auto(bool) {}
float VulkanRenderer::display_refresh() const noexcept { return 0.0f; }
double VulkanRenderer::frame_rate_now() const noexcept { return 30.0; }
void VulkanRenderer::upload_frame(std::uint32_t display_address, const std::uint8_t *pixels, std::uint32_t width,
                                  std::uint32_t height, std::uint32_t stride) {
    if (!impl_ || !impl_->ge_ready || pixels == nullptr || width == 0u || height == 0u || stride < width) return;
    Impl &impl = *impl_;
    if (impl.movie.texture == nullptr || impl.movie_width != width || impl.movie_height != height) {
        impl.release_texture(impl.movie);
        std::vector<std::uint32_t> black(static_cast<std::size_t>(width) * height, 0xFF000000u);
        impl.movie = impl.make_texture(width, height, black.data());
        impl.movie_width = width;
        impl.movie_height = height;
    }
    WGPUTexelCopyTextureInfo destination = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
    destination.texture = impl.movie.texture;
    WGPUTexelCopyBufferLayout layout = WGPU_TEXEL_COPY_BUFFER_LAYOUT_INIT;
    layout.bytesPerRow = stride * 4u;
    layout.rowsPerImage = height;
    const WGPUExtent3D size{width, height, 1u};
    const std::size_t bytes = static_cast<std::size_t>(stride) * 4u * (height - 1u) + static_cast<std::size_t>(width) * 4u;
    wgpuQueueWriteTexture(impl.queue, &destination, pixels, bytes, &layout, &size);
    impl.movie_address = display_address;
    impl.movie_valid = true;
}

bool VulkanRenderer::capture_frame(const std::filesystem::path &) { return false; }
bool VulkanRenderer::read_frame(std::vector<std::uint8_t> &, std::uint32_t &, std::uint32_t &) { return false; }
void VulkanRenderer::capture_window(const std::filesystem::path &) {}

void VulkanRenderer::set_internal_scale(std::uint32_t) {}
void VulkanRenderer::set_window_scale(std::uint32_t) {}
void VulkanRenderer::set_fullscreen(bool) {}
void VulkanRenderer::set_present_mode(settings::PresentMode) {}
bool VulkanRenderer::supports_present_mode(settings::PresentMode) const { return false; }
void VulkanRenderer::set_aspect(settings::Aspect) {}
void VulkanRenderer::set_sharp_screen(bool) {}
void VulkanRenderer::set_sharp_textures(bool) {}
void VulkanRenderer::set_texture_pack(bool) {}
std::string VulkanRenderer::texture_pack_status() const { return "Not available on the web"; }
void VulkanRenderer::reload_texture_pack() {}
void VulkanRenderer::hold_texture_pack(bool) {}
bool VulkanRenderer::texture_pack_held() const { return true; }
std::string VulkanRenderer::texture_pack_folder() const { return {}; }
std::filesystem::path VulkanRenderer::textures_root() { return install::user_data_directory() / "textures"; }
void VulkanRenderer::set_perf_overlay(bool) {}

float VulkanRenderer::game_aspect() const noexcept { return static_cast<float>(kPspWidth) / kPspHeight; }
std::array<float, 4> VulkanRenderer::game_picture() const noexcept { return {0.0f, 0.0f, 1.0f, 1.0f}; }
std::array<std::uint32_t, 2> VulkanRenderer::target_size() const noexcept {
    return impl_ ? std::array<std::uint32_t, 2>{impl_->target_width, impl_->target_height}
                 : std::array<std::uint32_t, 2>{kPspWidth, kPspHeight};
}

SDL_Window *VulkanRenderer::window() const noexcept { return impl_ ? impl_->window : nullptr; }
std::string VulkanRenderer::device_name() const { return impl_ ? impl_->device_name : std::string{}; }
std::string VulkanRenderer::device_summary() const { return device_name(); }
std::string VulkanRenderer::gpu_problem() const { return {}; }
std::string VulkanRenderer::gpu_compat_status() const { return "Off"; }
SDL_Gamepad *VulkanRenderer::gamepad() const noexcept { return impl_ ? impl_->gamepad : nullptr; }

bool VulkanRenderer::initialize_ui(std::string &error) {
    if (!impl_ || !impl_->ready) {
        error = "no WebGPU device";
        return false;
    }
    ImGui_ImplWGPU_InitInfo info;
    info.Device = impl_->device;
    info.RenderTargetFormat = impl_->format;
    if (!ImGui_ImplWGPU_Init(&info)) {
        error = "ImGui_ImplWGPU_Init failed";
        return false;
    }
    impl_->ui_ready = true;
    return true;
}

void VulkanRenderer::shutdown_ui() {
    if (!impl_ || !impl_->ui_ready) return;
    ImGui_ImplWGPU_Shutdown();
    impl_->ui_ready = false;
}

void VulkanRenderer::begin_ui_frame() {
    if (impl_ && impl_->ui_ready) ImGui_ImplWGPU_NewFrame();
}

void VulkanRenderer::set_ui_draw_data(ImDrawData *draw_data) {
    if (impl_) impl_->ui_draw_data = draw_data;
}

void VulkanRenderer::present_ui(bool show_game) {
    if (impl_ && impl_->ready) impl_->finish_frame(show_game);
}

} // namespace mhp3rd::gpu
