#include <cstring>
#include <vector>

#include "dxvk_limits.h"
#include "dxvk_pipelayout.h"

// dirk: the binding lists only; there are no Vulkan layout objects.
namespace dxvk {
  
  uint32_t DxvkBindingInfo::computeSetIndex() const {
    if (stage == VK_SHADER_STAGE_COMPUTE_BIT) {
      // Use one single set for compute shaders
      return DxvkDescriptorSets::CsAll;
    } else if (stage == VK_SHADER_STAGE_FRAGMENT_BIT) {
      // For fragment shaders, create a separate set for UBOs
      return uboSet
        ? DxvkDescriptorSets::FsBuffers
        : DxvkDescriptorSets::FsViews;
    } else {
      // Put all vertex shader resources into the last set.
      // Vertex shader UBOs are usually updated every draw,
      // and other resource types are rarely used.
      return DxvkDescriptorSets::VsAll;
    }
  }


  uint32_t DxvkBindingInfo::value() const {
    return (uint32_t(descriptorType) << 24) | resourceBinding;
  }


  bool DxvkBindingInfo::eq(const DxvkBindingInfo& other) const {
    return descriptorType  == other.descriptorType
        && resourceBinding == other.resourceBinding
        && viewType        == other.viewType
        && stage           == other.stage
        && access          == other.access
        && accessOp        == other.accessOp
        && uboSet          == other.uboSet
        && isMultisampled  == other.isMultisampled;
  }


  size_t DxvkBindingInfo::hash() const {
    DxvkHashState hash;
    hash.add(uint32_t(descriptorType));
    hash.add(resourceBinding);
    hash.add(uint32_t(viewType));
    hash.add(uint32_t(stage));
    hash.add(access);
    hash.add(uint32_t(accessOp));
    hash.add(uint32_t(uboSet));
    hash.add(uint32_t(isMultisampled));
    return hash;
  }


  DxvkBindingList::DxvkBindingList() {

  }


  DxvkBindingList::~DxvkBindingList() {

  }


  void DxvkBindingList::addBinding(const DxvkBindingInfo& binding) {
    auto iter = m_bindings.begin();

    while (iter != m_bindings.end() && iter->value() <= binding.value())
      iter++;

    m_bindings.insert(iter, binding);
  }


  void DxvkBindingList::merge(const DxvkBindingList& list) {
    for (const auto& binding : list.m_bindings)
      addBinding(binding);
  }


  bool DxvkBindingList::eq(const DxvkBindingList& other) const {
    if (getBindingCount() != other.getBindingCount())
      return false;

    for (uint32_t i = 0; i < getBindingCount(); i++) {
      if (!getBinding(i).eq(other.getBinding(i)))
        return false;
    }

    return true;
  }


  size_t DxvkBindingList::hash() const {
    DxvkHashState hash;

    for (const auto& binding : m_bindings)
      hash.add(binding.hash());

    return hash;
  }



  DxvkBindingLayout::DxvkBindingLayout(VkShaderStageFlags stages)
  : m_pushConst { 0, 0, 0 }, m_pushConstStages(0), m_stages(stages), m_hazards(0u) {

  }


  DxvkBindingLayout::~DxvkBindingLayout() {

  }


  uint32_t DxvkBindingLayout::getSetMask() const {
    uint32_t mask = 0;

    if (m_stages & VK_SHADER_STAGE_COMPUTE_BIT)
      mask |= (1u << DxvkDescriptorSets::CsAll);

    if (m_stages & VK_SHADER_STAGE_FRAGMENT_BIT) {
      mask |= (1u << DxvkDescriptorSets::FsViews)
           |  (1u << DxvkDescriptorSets::FsBuffers);
    }

    if (m_stages & VK_SHADER_STAGE_VERTEX_BIT)
      mask |= (1u << DxvkDescriptorSets::VsAll);

    return mask;
  }


  void DxvkBindingLayout::addBinding(const DxvkBindingInfo& binding) {
    uint32_t set = binding.computeSetIndex();
    m_bindings[set].addBinding(binding);

    if ((binding.access & VK_ACCESS_2_SHADER_WRITE_BIT) && binding.accessOp == DxvkAccessOp::None)
      m_hazards |= 1u << set;
  }


  void DxvkBindingLayout::addPushConstantRange(VkPushConstantRange range) {
    uint32_t oldEnd = m_pushConst.offset + m_pushConst.size;
    uint32_t newEnd = range.offset + range.size;

    m_pushConst.stageFlags |= range.stageFlags;
    m_pushConst.offset = std::min(m_pushConst.offset, range.offset);
    m_pushConst.size = std::max(oldEnd, newEnd) - m_pushConst.offset;
  }


  void DxvkBindingLayout::addPushConstantStage(VkShaderStageFlagBits stage) {
    m_pushConstStages |= stage;
  }


  void DxvkBindingLayout::merge(const DxvkBindingLayout& layout) {
    for (uint32_t i = 0; i < layout.m_bindings.size(); i++)
      m_bindings[i].merge(layout.m_bindings[i]);

    addPushConstantRange(layout.m_pushConst);
    m_pushConstStages |= layout.m_pushConstStages;

    m_hazards |= layout.m_hazards;
  }


  bool DxvkBindingLayout::eq(const DxvkBindingLayout& other) const {
    if (m_stages != other.m_stages)
      return false;

    for (uint32_t i = 0; i < m_bindings.size(); i++) {
      if (!m_bindings[i].eq(other.m_bindings[i]))
        return false;
    }

    if (m_pushConstStages != other.m_pushConstStages)
      return false;

    if (m_pushConst.stageFlags != other.m_pushConst.stageFlags
     || m_pushConst.offset     != other.m_pushConst.offset
     || m_pushConst.size       != other.m_pushConst.size)
      return false;

    return true;
  }


  size_t DxvkBindingLayout::hash() const {
    DxvkHashState hash;
    hash.add(m_stages);

    for (uint32_t i = 0; i < m_bindings.size(); i++)
      hash.add(m_bindings[i].hash());

    hash.add(m_pushConstStages);
    hash.add(m_pushConst.stageFlags);
    hash.add(m_pushConst.offset);
    hash.add(m_pushConst.size);
    return hash;
  }

}
