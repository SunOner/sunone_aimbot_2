#include "overlay/window_geometry.h"

#include <iostream>
#include <stdexcept>

using namespace OverlayWindowGeometry;

namespace
{
void require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

bool equal(Rect a, Rect b)
{
    return a.x == b.x && a.y == b.y && a.width == b.width && a.height == b.height;
}

void requireInside(Rect window, Rect work)
{
    require(window.width > 0 && window.height > 0, "Window has an invalid size");
    require(window.x >= work.x && window.y >= work.y &&
        window.x + window.width <= work.x + work.width &&
        window.y + window.height <= work.y + work.height,
        "Window extends beyond the monitor work area");
}

void testSavedWindowRemainsStable()
{
    const Rect saved{80, 60, 1100, 700}, work{0, 0, 1920, 1040};
    require(equal(fit(saved, work, 96), saved), "An already visible saved window changed geometry");
    require(equal(fit(saved, work, 0), saved), "Missing DPI did not use the normal fallback");
}

void testStartupFitsShortAndPortraitScreens()
{
    const auto shortScreen = fit({0, 0, 1180, 740}, {0, 0, 1280, 680}, 96, true);
    require(equal(shortScreen, {50, 10, 1180, 660}), "Startup did not fit and center the menu above the taskbar");
    const auto portrait = fit({0, 0, 1180, 740}, {0, 0, 720, 1280}, 144, true);
    require(equal(portrait, {15, 270, 690, 740}), "Portrait screen placement was incorrect");
}

void testHighDpiMinimumCannotExceedMonitor()
{
    const Rect small{0, 0, 480, 272};
    const auto window = fit({0, 0, 1180, 740}, small, 192, true);
    require(equal(window, {20, 20, 440, 232}), "Preferred minimum defeated the available screen size");
    requireInside(window, small);
    const auto bounds = limits(small, 192);
    require(bounds.minWidth <= bounds.available.width && bounds.minHeight <= bounds.available.height,
        "Native resize limits would force the window off screen");
}

void testNegativeMonitorCoordinatesAndTaskbarEdges()
{
    const Rect secondary{-1280, -80, 1280, 984};
    const auto window = fit({-1400, -180, 1180, 740}, secondary, 144);
    require(equal(window, {-1265, -65, 1180, 740}), "Secondary monitor offsets were ignored");
    requireInside(window, secondary);
    const Rect leftTaskbar{48, 0, 1872, 1080};
    const auto besideTaskbar = fit({0, 0, 1180, 740}, leftTaskbar, 96);
    require(besideTaskbar.x == 58 && besideTaskbar.y == 10, "Window overlaps a taskbar on the left edge");
}

void testRestoringDisconnectedMonitorAndChangedResolution()
{
    const Rect work{0, 40, 1024, 728};
    const auto recovered = fit({30000, -30000, 3840, 2160}, work, 144, true);
    requireInside(recovered, work);
    require(equal(recovered, {15, 55, 994, 698}), "Stale saved geometry was not recovered into the new work area");
    const auto repeated = fit(recovered, work, 144);
    require(equal(repeated, recovered), "Repeated reopen or display notifications caused geometry drift");
}

void testAllSizeAndDpiCombinationsStayInside()
{
    for (int width : {1, 320, 640, 1280, 1920, 3840})
        for (int height : {1, 200, 480, 720, 1080, 2160})
            for (unsigned dpi : {96u, 120u, 144u, 192u, 288u})
            {
                const Rect work{-640, 32, width, height};
                for (Rect desired : {Rect{0, 0, 1180, 740}, Rect{-10000, 10000, 3840, 2160}, Rect{0, 0, -1, 0}})
                {
                    const auto window = fit(desired, work, dpi);
                    requireInside(window, work);
                    require(equal(fit(window, work, dpi), window), "Fitting a window twice changed its geometry");
                }
            }
}

void testPresentationModesRestoreWithoutDrift()
{
    const Rect saved{80,60,1100,700},work{0,40,1920,1000},screen{0,0,1920,1080};
    Presentation state;
    for(int i=0;i<20;++i)
    {
        state.toggleMaximized(saved);
        require(state.mode==Mode::Maximized&&equal(state.bounds(work,screen,96),work),"Maximize did not use the exact taskbar-safe work area");
        state.toggleFullscreen(work);
        require(state.mode==Mode::Fullscreen&&equal(state.bounds(work,screen,96),screen),"Fullscreen was clamped to the taskbar or inset margins");
        state.toggleFullscreen(screen);
        require(state.mode==Mode::Maximized&&equal(state.restored,saved),"Fullscreen forgot maximized mode or overwrote restore geometry");
        state.toggleMaximized(work);
        require(state.mode==Mode::Windowed&&equal(state.bounds(work,screen,96),saved),"Repeated mode switches drifted the saved window");
    }
    state.toggleFullscreen(saved);state.toggleFullscreen(screen);
    require(state.mode==Mode::Windowed&&equal(state.bounds(work,screen,96),saved),"Windowed fullscreen did not restore the normal window");
    state.toggleFullscreen(saved);state.toggleMaximized(screen);
    require(state.mode==Mode::Maximized&&equal(state.restored,saved),"Switching directly from fullscreen to maximize lost the original window");
    state.toggleMaximized(work);
    require(equal(state.bounds(work,screen,96),saved),"Restoring after a direct mode change lost the normal window");
}

void testPresentationFollowsMonitorChanges()
{
    const Rect saved{-1500,-500,1100,740},work{-1920,-1040,1880,1040},screen{-1920,-1080,1920,1080};
    Presentation state;state.toggleMaximized(saved);
    require(equal(state.bounds(work,screen,144),work),"Maximize ignored negative monitor coordinates or a side taskbar");
    state.toggleFullscreen(work);
    require(equal(state.bounds(work,screen,192),screen),"Fullscreen used the primary monitor or DPI inset");
    const Rect changedWork{48,0,976,728},changedScreen{0,0,1024,768};
    require(equal(state.bounds(changedWork,changedScreen,144),changedScreen),"Fullscreen did not follow a display change");
    state.toggleFullscreen(changedScreen);
    require(equal(state.bounds(changedWork,changedScreen,144),changedWork),"Maximize did not follow a work area change");
    state.toggleMaximized(changedWork);
    requireInside(state.bounds(changedWork,changedScreen,144),changedWork);
    require(equal(state.restored,saved),"Monitor recovery destroyed the original restore rectangle");
}
}

int main()
{
    try
    {
        testSavedWindowRemainsStable();
        testStartupFitsShortAndPortraitScreens();
        testHighDpiMinimumCannotExceedMonitor();
        testNegativeMonitorCoordinatesAndTaskbarEdges();
        testRestoringDisconnectedMonitorAndChangedResolution();
        testAllSizeAndDpiCombinationsStayInside();
        testPresentationModesRestoreWithoutDrift();
        testPresentationFollowsMonitorChanges();
        std::cout << "Overlay window geometry regressions passed\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
