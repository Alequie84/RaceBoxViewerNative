#include "mac_app.hpp"

#include <imgui.h>
#include <implot.h>
#include <backends/imgui_impl_metal.h>
#include <backends/imgui_impl_sdl3.h>

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#import <Cocoa/Cocoa.h>
#import <Metal/Metal.h>
#import <QuartzCore/QuartzCore.h>

#include <filesystem>

int main(int argc, char** argv) {
    @autoreleasepool {
        if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) return 1;

        SDL_Window* window = SDL_CreateWindow(
            "RaceBox Telemetry Viewer", 1500, 920,
            SDL_WINDOW_METAL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
        if (!window) {
            SDL_Quit();
            return 1;
        }

        SDL_MetalView metal_view = SDL_Metal_CreateView(window);
        CAMetalLayer* layer = (__bridge CAMetalLayer*)SDL_Metal_GetLayer(metal_view);
        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        id<MTLCommandQueue> command_queue = [device newCommandQueue];
        layer.device = device;
        layer.pixelFormat = MTLPixelFormatBGRA8Unorm;

        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImPlot::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
        io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
        ImGui::StyleColorsDark();
        ImGui::GetStyle().ScaleAllSizes(1.15F);
        ImGui_ImplSDL3_InitForMetal(window);
        ImGui_ImplMetal_Init(device);

        const auto resource_path = [[[NSBundle mainBundle] resourcePath] UTF8String];
        racebox::app::mac::MacApp app(resource_path ? std::filesystem::path(resource_path)
                                                    : std::filesystem::current_path());
        if (argc > 1) {
            std::vector<std::filesystem::path> paths;
            for (int i = 1; i < argc; ++i) paths.emplace_back(argv[i]);
            app.load_paths(paths);
        }

        bool done = false;
        while (!done) {
            SDL_Event event;
            while (SDL_PollEvent(&event)) {
                ImGui_ImplSDL3_ProcessEvent(&event);
                if (event.type == SDL_EVENT_QUIT ||
                    (event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED && event.window.windowID == SDL_GetWindowID(window))) {
                    done = true;
                } else if (event.type == SDL_EVENT_DROP_BEGIN) {
                    app.begin_drop();
                } else if (event.type == SDL_EVENT_DROP_FILE && event.drop.data) {
                    app.add_dropped_file(std::filesystem::path(event.drop.data));
                } else if (event.type == SDL_EVENT_DROP_COMPLETE) {
                    app.finish_drop();
                }
            }
            if (SDL_GetWindowFlags(window) & SDL_WINDOW_MINIMIZED) {
                SDL_Delay(10);
                continue;
            }

            int width = 0;
            int height = 0;
            SDL_GetWindowSizeInPixels(window, &width, &height);
            layer.drawableSize = CGSizeMake(width, height);
            id<CAMetalDrawable> drawable = [layer nextDrawable];
            if (!drawable) continue;

            MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
            pass.colorAttachments[0].texture = drawable.texture;
            pass.colorAttachments[0].loadAction = MTLLoadActionClear;
            pass.colorAttachments[0].storeAction = MTLStoreActionStore;
            pass.colorAttachments[0].clearColor = MTLClearColorMake(0.035, 0.047, 0.065, 1.0);

            ImGui_ImplMetal_NewFrame(pass);
            ImGui_ImplSDL3_NewFrame();
            ImGui::NewFrame();
            app.draw();
            ImGui::Render();

            id<MTLCommandBuffer> command_buffer = [command_queue commandBuffer];
            id<MTLRenderCommandEncoder> encoder = [command_buffer renderCommandEncoderWithDescriptor:pass];
            [encoder pushDebugGroup:@"RaceBox ImGui"];
            ImGui_ImplMetal_RenderDrawData(ImGui::GetDrawData(), command_buffer, encoder);
            [encoder popDebugGroup];
            [encoder endEncoding];
            [command_buffer presentDrawable:drawable];
            [command_buffer commit];
        }

        ImGui_ImplMetal_Shutdown();
        ImGui_ImplSDL3_Shutdown();
        ImPlot::DestroyContext();
        ImGui::DestroyContext();
        SDL_Metal_DestroyView(metal_view);
        SDL_DestroyWindow(window);
        SDL_Quit();
    }
    return 0;
}
