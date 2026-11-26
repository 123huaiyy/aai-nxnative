#import <UIKit/UIKit.h>
#include <SDL3/SDL.h>
#include <cstdio>

void RequestGameLandscape(SDL_Window* window) {
    const SDL_PropertiesID properties = SDL_GetWindowProperties(window);
    UIWindow* nativeWindow = (__bridge UIWindow*)SDL_GetPointerProperty(
        properties, SDL_PROP_WINDOW_UIKIT_WINDOW_POINTER, nullptr);
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
