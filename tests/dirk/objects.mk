# dirk's objects: DXVK's D3D11/DXGI front end, dxbc, spirv, util and wsi, and
# the dirk backend (src/dxvk on gallium), for linking with a packaged wink.
#
#   make -f tests/dirk/objects.mk O=<obj-dir> MESA=<wink Mesa tree> \
#     MESA_BUILD=<its meson build dir> WEBGPU_INC=<emdawnwebgpu include dir> \
#     [GLSLANG=glslangValidator] [EMSDK=~/emsdk] -j
D       := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))../../src)
EMSDK   ?= $(HOME)/emsdk
EMCC    := $(EMSDK)/upstream/emscripten/emcc
EMXX    := $(EMSDK)/upstream/emscripten/em++
GLSLANG ?= glslangValidator
G       := $(O)/gen

DXVK_FLAGS := -std=c++17 -O2 -pthread -fwasm-exceptions -DNOMINMAX -D_WIN32_WINNT=0xa00 -DDXVK_WSI_DIRK \
  -I$(D)/../include -I$(D)/../include/native -I$(D)/../include/native/windows \
  -I$(D)/../include/native/directx -I$(D)/../include/vulkan/include -I$(D)/../include/spirv/include -I$(G)
MESA_FLAGS := -O2 -pthread -fwasm-exceptions -sSUPPORT_LONGJMP=wasm -DNDEBUG -D_GNU_SOURCE \
  -DMESA_DEBUG=0 -DHAVE_PTHREAD -DUSE_GCC_ATOMIC_BUILTINS -DHAVE_ENDIAN_H -DHAVE_STRUCT_TIMESPEC \
  -DHAVE___BUILTIN_EXPECT -DHAVE___BUILTIN_CLZ -DHAVE___BUILTIN_CLZLL -DHAVE___BUILTIN_CTZ \
  -DHAVE___BUILTIN_FFS -DHAVE___BUILTIN_FFSLL -DHAVE___BUILTIN_POPCOUNT -DHAVE___BUILTIN_POPCOUNTLL \
  -DHAVE___BUILTIN_UNREACHABLE -DHAVE_FUNC_ATTRIBUTE_CONST -DHAVE_FUNC_ATTRIBUTE_PURE \
  -DHAVE_FUNC_ATTRIBUTE_UNUSED -DHAVE_FUNC_ATTRIBUTE_WARN_UNUSED_RESULT -DHAVE_FUNC_ATTRIBUTE_FORMAT \
  -DHAVE_FUNC_ATTRIBUTE_PACKED -DHAVE_FUNC_ATTRIBUTE_RETURNS_NONNULL -DHAVE_FUNC_ATTRIBUTE_NORETURN \
  -DHAVE_FUNC_ATTRIBUTE_VISIBILITY -D__STDC_CONSTANT_MACROS -D__STDC_FORMAT_MACROS -D__STDC_LIMIT_MACROS \
  -I$(WEBGPU_INC) -I$(MESA)/src/gallium/include -I$(MESA_BUILD)/src/gallium/auxiliary \
  -I$(MESA)/src/gallium/auxiliary -I$(MESA_BUILD)/include -I$(MESA)/include -I$(MESA_BUILD)/src \
  -I$(MESA)/src -I$(MESA_BUILD)/src/compiler/nir -I$(MESA)/src/compiler/nir -I$(MESA_BUILD)/src/compiler \
  -I$(MESA)/src/compiler -I$(MESA_BUILD)/src/compiler/spirv -I$(MESA_BUILD)/src/util \
  -I$(MESA_BUILD)/src/util/format -I$(MESA)/src/gallium/drivers/gex_wgpu -Wno-unused-function

FE  := $(wildcard $(D)/d3d11/*.cpp) $(filter-out %/d3d10_core.cpp,$(wildcard $(D)/d3d10/*.cpp)) \
       $(wildcard $(D)/dxgi/*.cpp) $(wildcard $(D)/dxbc/*.cpp) $(wildcard $(D)/spirv/*.cpp) \
       $(wildcard $(D)/util/*.cpp) $(wildcard $(D)/util/*/*.cpp) \
       $(D)/wsi/wsi_platform.cpp $(wildcard $(D)/wsi/dirk/*.cpp) \
       $(D)/vulkan/vulkan_loader.cpp $(D)/vulkan/vulkan_names.cpp \
       $(filter-out $(D)/dxvk/dirk_%,$(wildcard $(D)/dxvk/*.cpp))
BK  := $(wildcard $(D)/dxvk/dirk_*.cpp)
CS  := $(D)/util/sha1/sha1.c
NIR := $(D)/dirk/dirk_nir.c
name = $(O)/$(subst /,_,$(patsubst $(D)/%,%,$(1))).o
GEN := $(G)/buildenv.h $(G)/version.h $(G)/d3d11_video_blit_frag.h $(G)/d3d11_video_blit_vert.h

all: $(foreach f,$(FE) $(BK) $(CS) $(NIR),$(call name,$(f)))

$(G)/buildenv.h:
	@mkdir -p $(G)
	printf '#define DXVK_TARGET "wasm32"\n#define DXVK_COMPILER "em++"\n#define DXVK_COMPILER_ARGS ""\n' > $@
$(G)/version.h:
	@mkdir -p $(G)
	printf '#define DXVK_VERSION "v2.6.2-dirk"\n' > $@
$(G)/d3d11_video_blit_%.h: $(D)/d3d11/shaders/d3d11_video_blit_%.*
	@mkdir -p $(G)
	$(GLSLANG) --quiet --target-env vulkan1.3 --vn d3d11_video_blit_$* -o $@ $<

define fe
$(call name,$(1)): $(1) $(GEN)
	@cd $(D) && $(EMXX) $(DXVK_FLAGS) -c $(1) -o $$@
endef
define bk
$(call name,$(1)): $(1) $(GEN)
	@cd $(D) && $(EMXX) $(DXVK_FLAGS) $(MESA_FLAGS) -c $(1) -o $$@
endef
define cc
$(call name,$(1)): $(1)
	@cd $(D) && $(EMCC) -O2 -pthread -c $(1) -o $$@
endef
define nir
$(call name,$(1)): $(1)
	@cd $(D) && $(EMCC) -std=c11 $(MESA_FLAGS) -c $(1) -o $$@
endef
$(foreach f,$(FE),$(eval $(call fe,$(f))))
$(foreach f,$(BK),$(eval $(call bk,$(f))))
$(foreach f,$(CS),$(eval $(call cc,$(f))))
$(foreach f,$(NIR),$(eval $(call nir,$(f))))
