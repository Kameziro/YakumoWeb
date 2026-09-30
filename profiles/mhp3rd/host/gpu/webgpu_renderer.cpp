// WebGPU implementation of gpu::VulkanRenderer for the web port (Emscripten).
//
// The class keeps its name: the rest of the port (kernel, HLE, interface) talks
// to "the renderer" through vulkan_renderer.hpp, which exposes no Vulkan types,
// and the web build compiles this file in place of vulkan_renderer.cpp.
//
// So far this draws the port's own interface (Dear ImGui) into the page's
// canvas and gives control back to the browser once per presented frame. The
// GE (the game's picture), the pad, touch, capture and frame interpolation are
// not implemented yet: those members keep neutral values and do nothing.

#include "gpu/vulkan_renderer.hpp"

#include "install/user_data.hpp"
#include "settings/settings.hpp"

#include <SDL3/SDL.h>
#include <emscripten/emscripten.h>
#include <webgpu/webgpu.h>

#include "imgui.h"
#include "backends/imgui_impl_wgpu.h"

#include <algorithm>
#include <cstring>
#include <iostream>
#include <utility>

namespace mhp3rd::gpu {

namespace {

constexpr std::uint32_t kPspWidth = 480u;
constexpr std::uint32_t kPspHeight = 272u;
constexpr const char *kCanvasSelector = "#canvas";

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

    bool request_device(std::string &error);
    void configure_surface();
    void draw(float red, float green, float blue);
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

// One window image: a plain background with the interface over it.
void VulkanRenderer::Impl::draw(float red, float green, float blue) {
    if (!configured) configure_surface();
    if (!configured) return;
    WGPUSurfaceTexture image = WGPU_SURFACE_TEXTURE_INIT;
    wgpuSurfaceGetCurrentTexture(surface, &image);
    if (ImGui_ImplWGPU_IsSurfaceStatusError(image.status)) {
        std::cerr << "[webgpu] cannot get the canvas image (status " << static_cast<int>(image.status) << ")\n";
        configured = false;
        return;
    }
    WGPUTextureView target = wgpuTextureCreateView(image.texture, nullptr);

    WGPURenderPassColorAttachment color = WGPU_RENDER_PASS_COLOR_ATTACHMENT_INIT;
    color.view = target;
    color.loadOp = WGPULoadOp_Clear;
    color.storeOp = WGPUStoreOp_Store;
    color.clearValue = WGPUColor{red, green, blue, 1.0};
    WGPURenderPassDescriptor pass_desc = WGPU_RENDER_PASS_DESCRIPTOR_INIT;
    pass_desc.colorAttachmentCount = 1;
    pass_desc.colorAttachments = &color;

    WGPUCommandEncoder encoder = wgpuDeviceCreateCommandEncoder(device, nullptr);
    WGPURenderPassEncoder pass = wgpuCommandEncoderBeginRenderPass(encoder, &pass_desc);
    if (ui_ready && ui_draw_data != nullptr) ImGui_ImplWGPU_RenderDrawData(ui_draw_data, pass);
    wgpuRenderPassEncoderEnd(pass);
    WGPUCommandBuffer commands = wgpuCommandEncoderFinish(encoder, nullptr);
    wgpuQueueSubmit(queue, 1, &commands);

    wgpuCommandBufferRelease(commands);
    wgpuRenderPassEncoderRelease(pass);
    wgpuCommandEncoderRelease(encoder);
    wgpuTextureViewRelease(target);
    wgpuTextureRelease(image.texture);
    ui_draw_data = nullptr;
    ++frames;
    // The browser shows the canvas when this turn ends, not at a present call.
    wait_for_animation_frame();
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
                                   static_cast<int>(kPspHeight * scale), SDL_WINDOW_RESIZABLE);
    if (impl.window == nullptr) {
        error = std::string("SDL_CreateWindow failed: ") + SDL_GetError();
        return false;
    }
    if (!impl.request_device(error)) return false;
    impl.configure_surface();
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
std::uint64_t VulkanRenderer::draws_submitted() const noexcept { return 0u; }
CameraReading VulkanRenderer::camera() const noexcept { return {}; }

bool VulkanRenderer::pump_events() {
    if (!impl_ || impl_->window == nullptr) return false;
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        if (event.type == SDL_EVENT_QUIT || event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) impl_->quit = true;
        if (event.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED) impl_->configured = false;
        if (impl_->event_hook && impl_->event_hook(event)) continue;
    }
    return !impl_->quit;
}

// Input for the game: not ported yet.
PadState VulkanRenderer::pad() const noexcept { return {}; }
void VulkanRenderer::sample_pad() {}
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
void VulkanRenderer::set_game_input(bool) {}
void VulkanRenderer::set_free_camera(bool) {}
FreeCameraControls VulkanRenderer::take_free_camera_controls() { return {}; }
bool VulkanRenderer::take_screenshot_request() noexcept { return false; }
bool VulkanRenderer::frame_step_held() const noexcept { return false; }
bool VulkanRenderer::take_hide_hud_toggle() noexcept { return false; }
bool VulkanRenderer::take_lock_on_press() noexcept { return false; }
bool VulkanRenderer::window_capture_pending() const noexcept { return false; }

// The GE: not ported yet. The game runs, and each flip shows a plain frame.
void VulkanRenderer::begin_frame() {}
void VulkanRenderer::begin_display_list() {}
bool VulkanRenderer::gpu_decode() const { return false; }
bool VulkanRenderer::check_gpu_decode() const { return false; }
void VulkanRenderer::submit(const DrawCall &, const GuestMemory &) {}
void VulkanRenderer::write_back_frame(GuestMemory &) {}
void VulkanRenderer::read_back_framebuffer(std::uint32_t, GuestMemory &) {}
bool VulkanRenderer::present(std::uint32_t, std::optional<std::chrono::steady_clock::time_point>) {
    if (!impl_ || !impl_->ready) return false;
    impl_->draw(0.05f, 0.05f, 0.08f);
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
void VulkanRenderer::upload_frame(std::uint32_t, const std::uint8_t *, std::uint32_t, std::uint32_t, std::uint32_t) {}

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
std::array<std::uint32_t, 2> VulkanRenderer::target_size() const noexcept { return {kPspWidth, kPspHeight}; }

SDL_Window *VulkanRenderer::window() const noexcept { return impl_ ? impl_->window : nullptr; }
std::string VulkanRenderer::device_name() const { return impl_ ? impl_->device_name : std::string{}; }
std::string VulkanRenderer::device_summary() const { return device_name(); }
std::string VulkanRenderer::gpu_problem() const { return {}; }
std::string VulkanRenderer::gpu_compat_status() const { return "Off"; }
SDL_Gamepad *VulkanRenderer::gamepad() const noexcept { return nullptr; }

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

void VulkanRenderer::present_ui(bool) {
    if (impl_ && impl_->ready) impl_->draw(0.05f, 0.05f, 0.08f);
}

} // namespace mhp3rd::gpu
