#pragma once

#include "dxvk_image.h"
#include "dxvk_limits.h"

namespace dxvk {

  struct DxvkAttachment {
    Rc<DxvkImageView> view    = nullptr;
    VkImageLayout     layout  = VK_IMAGE_LAYOUT_UNDEFINED;
  };

  struct DxvkRenderTargets {
    DxvkAttachment depth;
    DxvkAttachment color[MaxNumRenderTargets];
  };

}
