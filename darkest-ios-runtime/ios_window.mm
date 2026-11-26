#import <UIKit/UIKit.h>
#include <SDL3/SDL.h>
#include <cstdio>

namespace {
void RequestLandscape(UIWindow* nativeWindow) {
    UIWindowScene* scene = nativeWindow.windowScene;
    UIViewController* controller = nativeWindow.rootViewController;
    if (!scene || !controller) {
        std::fputs("Darkest Dungeon: SDL window has no active UIKit scene yet.\n", stderr);
        return;
    }

    [controller setNeedsUpdateOfSupportedInterfaceOrientations];
    if (UIInterfaceOrientationIsLandscape(scene.interfaceOrientation)) return;
    auto* preferences = [[UIWindowSceneGeometryPreferencesIOS alloc]
        initWithInterfaceOrientations:UIInterfaceOrientationMaskLandscape];
    [scene requestGeometryUpdateWithPreferences:preferences errorHandler:^(NSError* error) {
        std::fprintf(stderr, "Darkest Dungeon: landscape request failed: %s\n",
                     error.localizedDescription.UTF8String);
    }];
#if !__has_feature(objc_arc)
    [preferences release];
#endif
}
}  // namespace

extern "C" void darkest_request_launch_landscape() {
    for (UIScene* scene in UIApplication.sharedApplication.connectedScenes) {
        if (![scene isKindOfClass:UIWindowScene.class] ||
            scene.activationState == UISceneActivationStateBackground ||
            scene.activationState == UISceneActivationStateUnattached) continue;
        UIWindow* window = ((UIWindowScene*)scene).keyWindow;
        if (!window) continue;
        RequestLandscape(window);
        return;
    }
}

void RequestGameLandscape(SDL_Window* window) {
    const SDL_PropertiesID properties = SDL_GetWindowProperties(window);
    RequestLandscape((__bridge UIWindow*)SDL_GetPointerProperty(
        properties, SDL_PROP_WINDOW_UIKIT_WINDOW_POINTER, nullptr));
}
