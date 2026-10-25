# dirk's objects: DXVK's D3D11/DXGI front end, dxbc, spirv, util and wsi, and
# the dirk backend (src/dxvk on gallium), for linking with a packaged wink.
#
#   make -f tests/dirk/objects.mk O=<obj-dir> MESA=<wink Mesa tree> \
#     MESA_BUILD=<its meson build dir> WEBGPU_INC=<emdawnwebgpu include dir> \
#     [GLSLANG=glslangValidator] [EMSDK=~/emsdk] [WINE_INCLUDE=<Wine's include dir>] \
#     [THREADS=0] [PKG=<dir>] -j
#
# THREADS=0: for a module without pthreads. PKG: also a package of them,
# PKG/lib/libdirk.a and PKG/include, for a module that links dirk in (gex's
# Mesa, -Dgex-dirk-dir); the wire's host side is in it.
# Without GLSLANG, the video blit shaders are the ones in src/dirk/gen.
#
# With WINE_INCLUDE, `wire-gen` writes the wire's generated sources
# (src/dirk/wire/gen) anew from Wine's D3D11 and DXGI IDL.
D       := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))../../src)
EMSDK   ?= $(HOME)/emsdk
EMCC    := $(EMSDK)/upstream/emscripten/emcc
EMXX    := $(EMSDK)/upstream/emscripten/em++
GLSLANG ?=
THREADS ?= 1
PTHREAD := $(if $(filter 0,$(THREADS)),,-pthread)
G       := $(O)/gen

DXVK_FLAGS := -std=c++17 -O2 $(PTHREAD) -fwasm-exceptions -DNOMINMAX -D_WIN32_WINNT=0xa00 -DDXVK_WSI_DIRK \
  -I$(D)/../include -I$(D)/../include/native -I$(D)/../include/native/windows \
  -I$(D)/../include/native/directx -I$(D)/../include/vulkan/include -I$(D)/../include/spirv/include -I$(G)
MESA_FLAGS := -O2 $(PTHREAD) -fwasm-exceptions -sSUPPORT_LONGJMP=wasm -DNDEBUG -D_GNU_SOURCE \
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

# The wire's host side. Its generated sources are kept in src/dirk/wire/gen;
# WINE_INCLUDE writes them anew from Wine's IDL first (make ... wire-gen).
W     := $(D)/dirk/wire
WG    := $(W)/gen
WFLAGS := -I$(W) -I$(WG)
WIRE_OBJS := $(O)/dirk_wire_host.o $(O)/dirk_host.o
all: $(WIRE_OBJS)
wire-gen:
	python3 $(W)/gen.py $(WINE_INCLUDE) $(WG)
$(O)/dirk_wire_host.o: $(WG)/dirk_wire_host.cpp $(WG)/dirk_wire_ops.h $(W)/dirk_host.h $(W)/dirk_wire.h
	@cd $(D) && $(EMXX) $(DXVK_FLAGS) $(WFLAGS) -c $< -o $@
$(O)/dirk_host.o: $(W)/dirk_host.cpp $(WG)/dirk_wire_ops.h $(W)/dirk_host.h $(W)/dirk_wire.h $(W)/dirk_format.h
	@cd $(D) && $(EMXX) $(DXVK_FLAGS) $(WFLAGS) -c $< -o $@

$(G)/buildenv.h:
	@mkdir -p $(G)
	printf '#define DXVK_TARGET "wasm32"\n#define DXVK_COMPILER "em++"\n#define DXVK_COMPILER_ARGS ""\n' > $@
$(G)/version.h:
	@mkdir -p $(G)
	printf '#define DXVK_VERSION "v2.6.2-dirk"\n' > $@
$(G)/d3d11_video_blit_%.h: $(D)/d3d11/shaders/d3d11_video_blit_%.*
	@mkdir -p $(G)
	$(if $(GLSLANG),$(GLSLANG) --quiet --target-env vulkan1.3 --vn d3d11_video_blit_$* -o $@ $<,cp $(D)/dirk/gen/d3d11_video_blit_$*.h $@)

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
	@cd $(D) && $(EMCC) -O2 $(PTHREAD) -c $(1) -o $$@
endef
define nir
$(call name,$(1)): $(1)
	@cd $(D) && $(EMCC) -std=c11 $(MESA_FLAGS) -c $(1) -o $$@
endef
$(foreach f,$(FE),$(eval $(call fe,$(f))))
$(foreach f,$(BK),$(eval $(call bk,$(f))))
$(foreach f,$(CS),$(eval $(call cc,$(f))))
$(foreach f,$(NIR),$(eval $(call nir,$(f))))

ifneq ($(PKG),)
EMAR := $(EMSDK)/upstream/emscripten/emar
OBJS := $(foreach f,$(FE) $(BK) $(CS) $(NIR),$(call name,$(f))) $(WIRE_OBJS)
all: $(PKG)/lib/libdirk.a $(PKG)/include/dirk/dirk.h $(PKG)/include/dirk_wire_host.h
$(PKG)/lib/libdirk.a: $(OBJS)
	@mkdir -p $(dir $@)
	rm -f $@ && $(EMAR) rcs $@ $(OBJS)
$(PKG)/include/dirk/dirk.h: $(D)/dirk/dirk.h
	@mkdir -p $(dir $@)
	cp $< $@
$(PKG)/include/dirk_wire_host.h: $(D)/dirk/wire/dirk_wire_host.h
	@mkdir -p $(dir $@)
	cp $< $@
endif
