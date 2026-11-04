// spartanport: the game window on iOS.
//
// The desktop shim opens an SDL3 window; on iOS the app already has a UIKit
// scene, so the video output gets a CAMetalLayer on a view of its own and
// reports WindowSystemType::Metal. shadPS4's vk_platform.cpp already creates a
// VkSurfaceKHR from a CAMetalLayer with vkCreateMetalSurfaceEXT (the macOS
// path), so no new Vulkan code is needed here.
//
// The layer is created on the main thread; the GPU library calls this from its
// window thread, so the constructor waits for the main queue to finish.
//
// Input: the in-game overlay and the guest pad still come from the shim's own
// paths; touch controls are not wired here yet.

#include "sdl_window.h"

#import <UIKit/UIKit.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

#include <atomic>
#include <cstdio>
#include <mutex>
#include <string>
#include <unordered_map>

extern "C" float sp_ios_display_scale(void) {
    CGFloat scale = UIScreen.mainScreen.scale;
    return scale > 0 ? (float)scale : 1.0f;
}

extern "C" int sp_ios_display_refresh_hz(void) {
    return (int)UIScreen.mainScreen.maximumFramesPerSecond;
}

namespace Frontend {
namespace {
struct IOSWindow {
    UIWindow* window = nil;
    UIViewController* controller = nil;
    UIView* view = nil;
    CAMetalLayer* layer = nil;
    bool open = true;
};

std::mutex g_lock;
std::unordered_map<const WindowSDL*, IOSWindow*> g_windows;

UIWindowScene* ActiveScene() {
    for (UIScene* scene in UIApplication.sharedApplication.connectedScenes) {
        if (![scene isKindOfClass:[UIWindowScene class]]) continue;
        if (scene.activationState == UISceneActivationStateForegroundActive) {
            return (UIWindowScene*)scene;
        }
    }
    for (UIScene* scene in UIApplication.sharedApplication.connectedScenes) {
        if ([scene isKindOfClass:[UIWindowScene class]]) return (UIWindowScene*)scene;
    }
    return nil;
}

void RunOnMain(dispatch_block_t block) {
    if ([NSThread isMainThread]) {
        block();
    } else {
        dispatch_sync(dispatch_get_main_queue(), block);
    }
}

void UpdateDrawable(IOSWindow* w, std::atomic<s32>& width, std::atomic<s32>& height) {
    if (!w || !w->view || !w->layer) return;
    CGRect bounds = w->view.bounds;
    CGFloat scale = w->view.window.screen.scale > 0 ? w->view.window.screen.scale : UIScreen.mainScreen.scale;
    if (scale <= 0) scale = 1.0;
    s32 wpx = (s32)(bounds.size.width * scale + 0.5);
    s32 hpx = (s32)(bounds.size.height * scale + 0.5);
    if (wpx <= 0 || hpx <= 0) return;
    width.store(wpx, std::memory_order_relaxed);
    height.store(hpx, std::memory_order_relaxed);
    w->layer.contentsScale = scale;
    w->layer.drawableSize = CGSizeMake(wpx, hpx);
}

} // namespace

WindowSDL::WindowSDL(s32 width_, s32 height_, const char* title) {
    (void)title;
    width.store(width_, std::memory_order_relaxed);
    height.store(height_, std::memory_order_relaxed);

    __block IOSWindow* created = nullptr;
    RunOnMain(^{
        UIWindowScene* scene = ActiveScene();
        if (!scene) {
            std::fprintf(stderr, "VideoOut: no UIWindowScene yet; the Metal layer cannot be created\n");
            return;
        }
        IOSWindow* w = new IOSWindow();
        w->window = [[UIWindow alloc] initWithWindowScene:scene];
        w->controller = [[UIViewController alloc] init];
        CGRect frame = scene.screen.bounds;
        w->view = [[UIView alloc] initWithFrame:frame];
        w->view.backgroundColor = [UIColor blackColor];
        w->view.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;

        w->layer = [CAMetalLayer layer];
        w->layer.device = MTLCreateSystemDefaultDevice();
        w->layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
        // MoltenVK wants to own the drawable; a framebuffer-only layer forbids
        // the storage modes some formats need.
        w->layer.framebufferOnly = NO;
        w->layer.opaque = YES;
        w->layer.contentsScale = scene.screen.scale;
        w->layer.frame = w->view.bounds;
        w->layer.drawableSize = CGSizeMake(frame.size.width * scene.screen.scale,
                                           frame.size.height * scene.screen.scale);
        [w->view.layer addSublayer:w->layer];

        w->controller.view = w->view;
        w->window.rootViewController = w->controller;
        w->window.backgroundColor = [UIColor blackColor];
        w->window.hidden = NO;
        [w->window makeKeyAndVisible];

        std::lock_guard lock(g_lock);
        g_windows[this] = w;
        created = w;
    });

    if (created) {
        window_info.type = WindowSystemType::Metal;
        window_info.display_connection = nullptr;
        window_info.render_surface = (__bridge void*)created->layer;
        window_info.render_surface_scale = (float)created->layer.contentsScale;
        UpdateDrawable(created, width, height);
    } else {
        is_open.store(false, std::memory_order_relaxed);
    }
}

WindowSDL::~WindowSDL() {
    IOSWindow* w = nullptr;
    {
        std::lock_guard lock(g_lock);
        auto found = g_windows.find(this);
        if (found != g_windows.end()) {
            w = found->second;
            g_windows.erase(found);
        }
    }
    if (!w) return;
    RunOnMain(^{
        [w->window setHidden:YES];
        w->window.rootViewController = nil;
        [w->layer removeFromSuperlayer];
        w->layer = nil;
        w->view = nil;
        w->controller = nil;
        w->window = nil;
    });
    delete w;
}

bool WindowSDL::IsMinimized() const {
    return is_minimized.load(std::memory_order_relaxed);
}

bool WindowSDL::PollEvents() {
    IOSWindow* w = nullptr;
    {
        std::lock_guard lock(g_lock);
        auto found = g_windows.find(this);
        if (found != g_windows.end()) w = found->second;
    }
    if (!w) {
        is_open.store(false, std::memory_order_relaxed);
        return false;
    }

    __block bool open = true;
    __block IOSWindow* blockWindow = w;
    RunOnMain(^{
        open = blockWindow->window != nil && !blockWindow->window.hidden;
        UpdateDrawable(blockWindow, width, height);
    });
    // The overlay's touch layer, when it exists, sits on top of the Metal view;
    // nothing else consumes events here yet.
    is_open.store(open, std::memory_order_relaxed);
    return open;
}

void WindowSDL::BeginTextInput(const std::string& initial, const std::string& prompt) {
    (void)initial;
    (void)prompt;
}

int WindowSDL::PollTextInput(std::string& text) {
    (void)text;
    return 2; // cancelled: iOS text entry needs a UITextField overlay
}

void WindowSDL::ApplyDisplayMode(int mode) {
    (void)mode; // fullscreen is the only mode on iOS
}

void ToggleMousePause() {
    // No mouse on iOS; the touch overlay owns this in a later pass.
}

} // namespace Frontend
