#if defined(DXVK_WSI_DIRK)

#include "wsi_platform_dirk.h"
#include "../wsi_edid.h"

#include "../../dirk/dirk.h"
#include "../../util/util_string.h"

namespace dxvk::wsi {

  // The one display.
  static const HMONITOR DirkMonitor = reinterpret_cast<HMONITOR>(uintptr_t(1));

  static WsiMode hostMode() {
    uint32_t w = 1920, h = 1080, mhz = 60000;
    const dirk_host* host = dirk_get_host();
    if (host && host->display_mode)
      host->display_mode(&w, &h, &mhz);

    WsiMode mode = { };
    mode.width        = w;
    mode.height       = h;
    mode.refreshRate  = WsiRational{ mhz, 1000 };
    mode.bitsPerPixel = 32;
    mode.interlaced   = false;
    return mode;
  }

  std::vector<const char *> DirkWsiDriver::getInstanceExtensions() {
    return { };
  }

  HMONITOR DirkWsiDriver::getDefaultMonitor() {
    return DirkMonitor;
  }

  HMONITOR DirkWsiDriver::enumMonitors(uint32_t index) {
    return index == 0 ? DirkMonitor : nullptr;
  }

  HMONITOR DirkWsiDriver::enumMonitors(const LUID *adapterLUID[], uint32_t numLUIDs, uint32_t index) {
    return enumMonitors(index);
  }

  bool DirkWsiDriver::getDisplayName(HMONITOR hMonitor, WCHAR (&Name)[32]) {
    if (hMonitor != DirkMonitor)
      return false;
    std::wstring name = L"\\\\.\\DISPLAY1";
    std::memset(Name, 0, sizeof(Name));
    for (size_t i = 0; i < name.size() && i < 31; i++)
      Name[i] = WCHAR(name[i]);
    return true;
  }

  bool DirkWsiDriver::getDesktopCoordinates(HMONITOR hMonitor, RECT* pRect) {
    if (hMonitor != DirkMonitor)
      return false;
    WsiMode mode = hostMode();
    pRect->left   = 0;
    pRect->top    = 0;
    pRect->right  = LONG(mode.width);
    pRect->bottom = LONG(mode.height);
    return true;
  }

  bool DirkWsiDriver::getDisplayMode(HMONITOR hMonitor, uint32_t modeNumber, WsiMode* pMode) {
    if (hMonitor != DirkMonitor || modeNumber != 0)
      return false;
    *pMode = hostMode();
    return true;
  }

  bool DirkWsiDriver::getCurrentDisplayMode(HMONITOR hMonitor, WsiMode* pMode) {
    return getDisplayMode(hMonitor, 0, pMode);
  }

  bool DirkWsiDriver::getDesktopDisplayMode(HMONITOR hMonitor, WsiMode* pMode) {
    return getDisplayMode(hMonitor, 0, pMode);
  }

  WsiEdidData DirkWsiDriver::getMonitorEdid(HMONITOR hMonitor) {
    return { };
  }

  void DirkWsiDriver::getWindowSize(HWND hWindow, uint32_t* pWidth, uint32_t* pHeight) {
    uint32_t w = 0, h = 0;
    const dirk_host* host = dirk_get_host();
    if (host && host->window_size)
      host->window_size(hWindow, &w, &h);
    if (pWidth)
      *pWidth = w;
    if (pHeight)
      *pHeight = h;
  }

  void DirkWsiDriver::resizeWindow(HWND hWindow, DxvkWindowState* pState, uint32_t width, uint32_t height) { }

  bool DirkWsiDriver::setWindowMode(HMONITOR hMonitor, HWND hWindow, DxvkWindowState* pState, const WsiMode& mode) {
    return true;
  }

  bool DirkWsiDriver::enterFullscreenMode(HMONITOR hMonitor, HWND hWindow, DxvkWindowState* pState, bool modeSwitch) {
    return true;
  }

  bool DirkWsiDriver::leaveFullscreenMode(HWND hWindow, DxvkWindowState* pState, bool restoreCoordinates) {
    return true;
  }

  bool DirkWsiDriver::restoreDisplayMode() {
    return true;
  }

  HMONITOR DirkWsiDriver::getWindowMonitor(HWND hWindow) {
    return DirkMonitor;
  }

  bool DirkWsiDriver::isWindow(HWND hWindow) {
    uint32_t w = 0, h = 0;
    const dirk_host* host = dirk_get_host();
    return host && host->window_size && host->window_size(hWindow, &w, &h);
  }

  bool DirkWsiDriver::isMinimized(HWND hWindow) {
    return false;
  }

  bool DirkWsiDriver::isOccluded(HWND hWindow) {
    return false;
  }

  void DirkWsiDriver::updateFullscreenWindow(HMONITOR hMonitor, HWND hWindow, bool forceTopmost) { }

  VkResult DirkWsiDriver::createSurface(
          HWND                hWindow,
          PFN_vkGetInstanceProcAddr pfnVkGetInstanceProcAddr,
          VkInstance          instance,
          VkSurfaceKHR*       pSurface) {
    // The surface is the window: the context presents to it by handle.
    *pSurface = VkSurfaceKHR(reinterpret_cast<uintptr_t>(hWindow));
    return VK_SUCCESS;
  }

  static bool createDirkWsiDriver(WsiDriver **driver) {
    *driver = new DirkWsiDriver();
    return true;
  }

  // The host describes no display by EDID.
  std::optional<WsiDisplayMetadata> parseColorimetryInfo(const WsiEdidData& edidData) {
    return std::nullopt;
  }

  WsiBootstrap DirkWSI = {
    "dirk",
    createDirkWsiDriver
  };

}

#endif
