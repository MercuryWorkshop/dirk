#pragma once

#include "../../vulkan/vulkan_loader.h"

#include "../wsi_platform.h"

namespace dxvk::wsi {

  /**
   * \brief The host's windows
   *
   * One display, of the size the host reports, and windows that are the
   * host's handles. Nothing here goes fullscreen: a window is the size the
   * host gave it, and a mode change only changes what DXGI reports.
   */
  class DirkWsiDriver : public WsiDriver {

  public:

    std::vector<const char *> getInstanceExtensions();

    HMONITOR getDefaultMonitor();

    HMONITOR enumMonitors(uint32_t index);

    HMONITOR enumMonitors(const LUID *adapterLUID[], uint32_t numLUIDs, uint32_t index);

    bool getDisplayName(
            HMONITOR         hMonitor,
            WCHAR            (&Name)[32]);

    bool getDesktopCoordinates(
            HMONITOR         hMonitor,
            RECT*            pRect);

    bool getDisplayMode(
            HMONITOR         hMonitor,
            uint32_t         modeNumber,
            WsiMode*         pMode);

    bool getCurrentDisplayMode(
            HMONITOR         hMonitor,
            WsiMode*         pMode);

    bool getDesktopDisplayMode(
            HMONITOR         hMonitor,
            WsiMode*         pMode);

    WsiEdidData getMonitorEdid(HMONITOR hMonitor);

    void getWindowSize(
            HWND      hWindow,
            uint32_t* pWidth,
            uint32_t* pHeight);

    void resizeWindow(
            HWND             hWindow,
            DxvkWindowState* pState,
            uint32_t         width,
            uint32_t         height);

    bool setWindowMode(
            HMONITOR         hMonitor,
            HWND             hWindow,
            DxvkWindowState* pState,
      const WsiMode&         mode);

    bool enterFullscreenMode(
            HMONITOR         hMonitor,
            HWND             hWindow,
            DxvkWindowState* pState,
            bool             modeSwitch);

    bool leaveFullscreenMode(
            HWND             hWindow,
            DxvkWindowState* pState,
            bool             restoreCoordinates);

    bool restoreDisplayMode();

    HMONITOR getWindowMonitor(HWND hWindow);

    bool isWindow(HWND hWindow);

    bool isMinimized(HWND hWindow);

    bool isOccluded(HWND hWindow);

    void updateFullscreenWindow(
            HMONITOR hMonitor,
            HWND     hWindow,
            bool     forceTopmost);

    VkResult createSurface(
            HWND                hWindow,
            PFN_vkGetInstanceProcAddr pfnVkGetInstanceProcAddr,
            VkInstance          instance,
            VkSurfaceKHR*       pSurface);

  };

}
