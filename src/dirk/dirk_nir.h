/*
 * dirk: DXVK's SPIR-V to a gallium shader.
 *
 * DXVK's shaders name every resource by its DXVK binding (descriptor set 0,
 * binding = the front end's resource slot). Gallium names them per stage,
 * per kind, from zero, and textures and samplers come in pairs (units). The
 * layout this returns says, for each gallium index, which DXVK binding feeds
 * it; the context binds by that.
 */
#ifndef DIRK_NIR_H
#define DIRK_NIR_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct pipe_context;

#define DIRK_MAX_UBOS      16
#define DIRK_MAX_UNITS     16
#define DIRK_MAX_IMAGES    16
#define DIRK_MAX_SSBOS     24
#define DIRK_NO_BINDING    0xffffffffu

/* The stages, numbered as gallium numbers them. */
enum dirk_stage {
   DIRK_STAGE_VERTEX    = 0,
   DIRK_STAGE_TESS_CTRL = 1,
   DIRK_STAGE_TESS_EVAL = 2,
   DIRK_STAGE_GEOMETRY  = 3,
   DIRK_STAGE_FRAGMENT  = 4,
   DIRK_STAGE_COMPUTE   = 5,
};

/* A texture unit: the texture and the sampler a shader reads it with.
 * sampler is DIRK_NO_BINDING for a texture only fetched or queried. */
struct dirk_unit {
   uint32_t texture;
   uint32_t sampler;
};

struct dirk_shader_layout {
   uint32_t ubo_count;
   uint32_t ubos[DIRK_MAX_UBOS];

   uint32_t unit_count;
   struct dirk_unit units[DIRK_MAX_UNITS];

   uint32_t image_count;
   uint32_t images[DIRK_MAX_IMAGES];

   uint32_t ssbo_count;
   uint32_t ssbos[DIRK_MAX_SSBOS];

   /* The constant buffer push constants are read from, or DIRK_NO_BINDING. */
   uint32_t push_ubo;

   /* Vertex shader: the input locations it reads. */
   uint32_t inputs;

   /* Compute shader: its workgroup size. */
   uint32_t workgroup[3];
};

struct dirk_spec_constant {
   uint32_t id;
   uint32_t value;
};

/* Compiles and creates the gallium shader (create_*_state). NULL on failure,
 * with the reason in why. */
void *dirk_nir_compile(struct pipe_context *pipe, enum dirk_stage stage,
                       const uint32_t *words, size_t word_count,
                       const struct dirk_spec_constant *spec,
                       unsigned spec_count,
                       struct dirk_shader_layout *layout,
                       char *why, size_t why_len);

void dirk_nir_delete(struct pipe_context *pipe, enum dirk_stage stage,
                     void *shader);

#ifdef __cplusplus
}
#endif

#endif
