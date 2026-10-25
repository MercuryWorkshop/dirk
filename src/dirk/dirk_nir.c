/*
 * dirk: DXVK's SPIR-V to a gallium shader.
 *
 * spirv_to_nir in its Vulkan environment, then the NIR a gallium driver is
 * handed by st/mesa: constant and storage buffers as load_ubo/load_ssbo by
 * block index, with their variables' driver_location the block; combined
 * samplers by unit; images by unit; I/O variables by location.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nir.h"
#include "nir_builder.h"
#include "spirv/nir_spirv.h"
#include "spirv_info.h"

#include "pipe/p_context.h"
#include "pipe/p_screen.h"
#include "pipe/p_state.h"
#include "util/u_memory.h"

#include "dirk_nir.h"

struct dirk_lower {
   struct dirk_shader_layout *layout;
   bool failed;
   char why[160];
};

static void
dirk_fail(struct dirk_lower *s, const char *what, unsigned value)
{
   if (!s->failed)
      snprintf(s->why, sizeof(s->why), "%s (%u)", what, value);
   s->failed = true;
}

static unsigned
dirk_dense(uint32_t *list, uint32_t *count, unsigned max, uint32_t binding,
           struct dirk_lower *s, const char *what)
{
   for (unsigned i = 0; i < *count; i++)
      if (list[i] == binding)
         return i;
   if (*count >= max) {
      dirk_fail(s, what, binding);
      return 0;
   }
   list[*count] = binding;
   return (*count)++;
}

/* vulkan_resource_index(set 0, binding) becomes the block index the layout
 * gives the binding; load_vulkan_descriptor passes it through. */
static bool
dirk_lower_resource_index(nir_builder *b, nir_intrinsic_instr *intr, void *data)
{
   struct dirk_lower *s = data;
   struct dirk_shader_layout *l = s->layout;

   if (intr->intrinsic == nir_intrinsic_load_vulkan_descriptor) {
      b->cursor = nir_before_instr(&intr->instr);
      nir_def_replace(&intr->def, intr->src[0].ssa);
      return true;
   }

   if (intr->intrinsic != nir_intrinsic_vulkan_resource_index)
      return false;

   const unsigned binding = nir_intrinsic_binding(intr);
   /* A constant buffer or a storage buffer, by the variable that has the
    * binding: the descriptor type the intrinsic carries is not reliable. */
   bool ubo = false;
   nir_foreach_variable_with_modes(var, b->shader, nir_var_mem_ubo) {
      if (var->data.binding == binding)
         ubo = true;
   }
   unsigned index;
   if (ubo)
      index = dirk_dense(l->ubos, &l->ubo_count, DIRK_MAX_UBOS, binding, s,
                         "too many constant buffers");
   else
      index = dirk_dense(l->ssbos, &l->ssbo_count, DIRK_MAX_SSBOS, binding, s,
                         "too many storage buffers");

   b->cursor = nir_before_instr(&intr->instr);
   nir_def *array = intr->src[0].ssa;
   nir_def *block = nir_iadd_imm(b, array, index);
   nir_def_replace(&intr->def, nir_vec2(b, block, nir_imm_int(b, 0)));
   return true;
}

/* load_push_constant becomes a load from the constant buffer after the
 * shader's own, which the context fills with the push constants. */
static bool
dirk_lower_push_constants(nir_builder *b, nir_intrinsic_instr *intr, void *data)
{
   struct dirk_lower *s = data;
   if (intr->intrinsic != nir_intrinsic_load_push_constant)
      return false;

   b->cursor = nir_before_instr(&intr->instr);
   nir_def *offset = nir_iadd_imm(b, intr->src[0].ssa, nir_intrinsic_base(intr));
   nir_def *load = nir_load_ubo(b, intr->def.num_components, intr->def.bit_size,
                                nir_imm_int(b, s->layout->push_ubo), offset,
                                .align_mul = 4, .align_offset = 0,
                                .range_base = 0, .range = ~0u);
   nir_def_replace(&intr->def, load);
   return true;
}

/* DXVK makes discard a demote (SPV_EXT_demote_to_helper_invocation), which
 * Tint does not take. WGSL's discard is itself a demote -- the invocation
 * stays as a helper, so derivatives keep working -- and it is what a
 * terminate (OpKill) becomes there, so the semantics are kept. */
static bool
dirk_lower_demote(nir_builder *b, nir_intrinsic_instr *intr, void *data)
{
   (void)data;
   if (intr->intrinsic != nir_intrinsic_demote && intr->intrinsic != nir_intrinsic_demote_if)
      return false;
   b->cursor = nir_before_instr(&intr->instr);
   if (intr->intrinsic == nir_intrinsic_demote)
      nir_terminate(b);
   else
      nir_terminate_if(b, intr->src[0].ssa);
   nir_instr_remove(&intr->instr);
   return true;
}

static bool
dirk_shader_has_push_constants(nir_shader *nir)
{
   nir_foreach_function_impl(impl, nir) {
      nir_foreach_block(block, impl) {
         nir_foreach_instr(instr, block) {
            if (instr->type == nir_instr_type_intrinsic &&
                nir_instr_as_intrinsic(instr)->intrinsic ==
                   nir_intrinsic_load_push_constant)
               return true;
         }
      }
   }
   return false;
}

/* Units: one combined sampler variable for each texture and sampler pair
 * the shader reads together. */
struct dirk_unit_vars {
   nir_variable *var[DIRK_MAX_UNITS];
   bool shadow[DIRK_MAX_UNITS];
};

static nir_variable *
dirk_deref_var(nir_tex_instr *tex, nir_tex_src_type type)
{
   const int i = nir_tex_instr_src_index(tex, type);
   if (i < 0)
      return NULL;
   nir_deref_instr *deref = nir_src_as_deref(tex->src[i].src);
   return deref ? nir_deref_instr_get_variable(deref) : NULL;
}

static unsigned
dirk_unit(nir_shader *nir, struct dirk_lower *s, struct dirk_unit_vars *vars,
          nir_variable *texture, uint32_t sampler, bool shadow,
          bool any_sampler)
{
   struct dirk_shader_layout *l = s->layout;
   const uint32_t binding = texture->data.binding;

   for (unsigned i = 0; i < l->unit_count; i++) {
      if (l->units[i].texture != binding || vars->shadow[i] != shadow)
         continue;
      if (any_sampler || l->units[i].sampler == sampler)
         return i;
   }
   if (l->unit_count >= DIRK_MAX_UNITS) {
      dirk_fail(s, "too many texture units", binding);
      return 0;
   }

   const unsigned unit = l->unit_count++;
   l->units[unit].texture = binding;
   l->units[unit].sampler = sampler;

   const struct glsl_type *ttype = glsl_without_array(texture->type);
   const struct glsl_type *type = glsl_sampler_type(
      glsl_get_sampler_dim(ttype), shadow, glsl_sampler_type_is_array(ttype),
      glsl_get_sampler_result_type(ttype));
   char name[32];
   snprintf(name, sizeof(name), "dirk_unit%u", unit);
   nir_variable *var = nir_variable_create(nir, nir_var_uniform, type, name);
   var->data.descriptor_set = 0;
   var->data.binding = unit;
   var->data.driver_location = unit;
   vars->var[unit] = var;
   vars->shadow[unit] = shadow;
   return unit;
}

static void
dirk_lower_units(nir_shader *nir, struct dirk_lower *s)
{
   struct dirk_unit_vars vars;
   memset(&vars, 0, sizeof(vars));

   /* Sampled reads first, so a fetch or a query can take a unit one of
    * them already has rather than a unit of its own. */
   for (unsigned pass = 0; pass < 2; pass++) {
      nir_foreach_function_impl(impl, nir) {
         nir_builder b = nir_builder_create(impl);
         nir_foreach_block(block, impl) {
            nir_foreach_instr(instr, block) {
               if (instr->type != nir_instr_type_tex)
                  continue;
               nir_tex_instr *tex = nir_instr_as_tex(instr);
               nir_variable *texture =
                  dirk_deref_var(tex, nir_tex_src_texture_deref);
               nir_variable *sampler =
                  dirk_deref_var(tex, nir_tex_src_sampler_deref);
               if (!texture)
                  continue;
               if (texture->data.mode != nir_var_uniform ||
                   (pass == 0) != (sampler != NULL))
                  continue;
               if (texture->name && !strncmp(texture->name, "dirk_unit", 9))
                  continue;

               const unsigned unit = sampler
                  ? dirk_unit(nir, s, &vars, texture, sampler->data.binding,
                              tex->is_shadow, false)
                  : dirk_unit(nir, s, &vars, texture, DIRK_NO_BINDING,
                              false, true);
               if (s->failed)
                  return;

               b.cursor = nir_before_instr(&tex->instr);
               nir_deref_instr *deref = nir_build_deref_var(&b, vars.var[unit]);
               const int ti = nir_tex_instr_src_index(tex, nir_tex_src_texture_deref);
               nir_src_rewrite(&tex->src[ti].src, &deref->def);
               const int si = nir_tex_instr_src_index(tex, nir_tex_src_sampler_deref);
               if (si >= 0)
                  nir_src_rewrite(&tex->src[si].src, &deref->def);
               else
                  nir_tex_instr_add_src(tex, nir_tex_src_sampler_deref, &deref->def);
               tex->texture_index = unit;
               tex->sampler_index = unit;
            }
         }
         nir_progress(true, impl, nir_metadata_control_flow);
      }
   }

   /* The bare textures and samplers are no longer read. */
   nir_foreach_variable_with_modes_safe(var, nir, nir_var_uniform) {
      const struct glsl_type *type = glsl_without_array(var->type);
      if ((glsl_type_is_texture(type) || glsl_type_is_bare_sampler(type)) &&
          !(var->name && !strncmp(var->name, "dirk_unit", 9)))
         exec_node_remove(&var->node);
   }
}

static void
dirk_lower_images(nir_shader *nir, struct dirk_lower *s)
{
   struct dirk_shader_layout *l = s->layout;
   nir_foreach_variable_with_modes(var, nir, nir_var_image) {
      const unsigned index = dirk_dense(l->images, &l->image_count,
                                        DIRK_MAX_IMAGES, var->data.binding, s,
                                        "too many images");
      var->data.descriptor_set = 0;
      var->data.binding = index;
      var->data.driver_location = index;
   }
}

static void
dirk_assign_buffer_locations(nir_shader *nir, struct dirk_lower *s)
{
   struct dirk_shader_layout *l = s->layout;
   nir_foreach_variable_with_modes(var, nir, nir_var_mem_ubo) {
      var->data.driver_location = UINT_MAX;
      for (unsigned i = 0; i < l->ubo_count; i++)
         if (l->ubos[i] == var->data.binding)
            var->data.driver_location = i;

      /* The driver reads a constant buffer as an interface holding one
       * vec4 array: DXVK's are a struct of one, by explicit layout. */
      const struct glsl_type *type = glsl_without_array(var->type);
      unsigned size = glsl_get_explicit_size(type, false);
      if (!size)
         size = 16;
      struct glsl_struct_field field = {0};
      field.type = glsl_array_type(glsl_vec4_type(), DIV_ROUND_UP(size, 16), 16);
      field.name = "values";
      field.location = -1;
      field.offset = 0;
      const struct glsl_type *iface =
         glsl_interface_type(&field, 1, GLSL_INTERFACE_PACKING_STD140, false,
                             glsl_get_type_name(type));
      var->type = iface;
      var->interface_type = iface;
   }
   nir_foreach_variable_with_modes(var, nir, nir_var_mem_ssbo) {
      var->data.driver_location = UINT_MAX;
      for (unsigned i = 0; i < l->ssbo_count; i++)
         if (l->ssbos[i] == var->data.binding)
            var->data.driver_location = i;
   }
}

static void
dirk_add_push_block(nir_shader *nir, struct dirk_lower *s)
{
   struct dirk_shader_layout *l = s->layout;
   if (l->ubo_count >= DIRK_MAX_UBOS) {
      dirk_fail(s, "no constant buffer left for push constants", l->ubo_count);
      return;
   }
   l->push_ubo = l->ubo_count;
   l->ubos[l->ubo_count++] = DIRK_NO_BINDING;

   struct glsl_struct_field field = {0};
   field.type = glsl_array_type(glsl_vec4_type(), 8, 16);
   field.name = "values";
   field.location = -1;
   const struct glsl_type *iface =
      glsl_interface_type(&field, 1, GLSL_INTERFACE_PACKING_STD140, false,
                          "dirk_push");
   nir_variable *var = nir_variable_create(nir, nir_var_mem_ubo, iface,
                                           "dirk_push");
   var->interface_type = iface;
   var->data.driver_location = l->push_ubo;
   var->data.binding = l->push_ubo;
}

static void
dirk_assign_io(nir_shader *nir, struct dirk_lower *s)
{
   nir_foreach_shader_in_variable(var, nir) {
      if (nir->info.stage == MESA_SHADER_VERTEX) {
         if (var->data.location >= VERT_ATTRIB_GENERIC0) {
            const unsigned loc = var->data.location - VERT_ATTRIB_GENERIC0;
            var->data.driver_location = loc;
            const unsigned slots = glsl_count_attribute_slots(var->type, true);
            for (unsigned i = 0; i < slots && loc + i < 32; i++)
               s->layout->inputs |= 1u << (loc + i);
         }
      } else if (var->data.location >= VARYING_SLOT_VAR0) {
         var->data.driver_location = var->data.location - VARYING_SLOT_VAR0;
      } else {
         var->data.driver_location = var->data.location;
      }
   }
   nir_foreach_shader_out_variable(var, nir) {
      if (nir->info.stage == MESA_SHADER_FRAGMENT) {
         var->data.driver_location = var->data.location >= FRAG_RESULT_DATA0
            ? var->data.location - FRAG_RESULT_DATA0 : var->data.location;
      } else if (var->data.location >= VARYING_SLOT_VAR0) {
         var->data.driver_location = var->data.location - VARYING_SLOT_VAR0;
      } else {
         var->data.driver_location = var->data.location;
      }
   }
}

static void
dirk_optimize(nir_shader *nir)
{
   bool progress;
   do {
      progress = false;
      NIR_PASS(progress, nir, nir_lower_vars_to_ssa);
      NIR_PASS(progress, nir, nir_opt_copy_prop);
      NIR_PASS(progress, nir, nir_opt_dce);
      NIR_PASS(progress, nir, nir_opt_cse);
      NIR_PASS(progress, nir, nir_opt_algebraic);
      NIR_PASS(progress, nir, nir_opt_constant_folding);
      NIR_PASS(progress, nir, nir_opt_dead_cf);
      NIR_PASS(progress, nir, nir_opt_remove_phis);
      NIR_PASS(progress, nir, nir_opt_undef);
   } while (progress);
}

static mesa_shader_stage
dirk_mesa_stage(enum dirk_stage stage)
{
   switch (stage) {
   case DIRK_STAGE_VERTEX:    return MESA_SHADER_VERTEX;
   case DIRK_STAGE_TESS_CTRL: return MESA_SHADER_TESS_CTRL;
   case DIRK_STAGE_TESS_EVAL: return MESA_SHADER_TESS_EVAL;
   case DIRK_STAGE_GEOMETRY:  return MESA_SHADER_GEOMETRY;
   case DIRK_STAGE_FRAGMENT:  return MESA_SHADER_FRAGMENT;
   default:                   return MESA_SHADER_COMPUTE;
   }
}

static nir_shader *
dirk_spirv_to_nir(struct pipe_screen *screen, mesa_shader_stage stage,
                  const uint32_t *words, size_t word_count,
                  const struct dirk_spec_constant *spec, unsigned spec_count)
{
   static struct spirv_capabilities caps;
   static bool caps_ready;
   if (!caps_ready) {
      memset(&caps, 1, sizeof(caps));
      caps.Kernel = false;
      caps.Addresses = false;
      caps.Linkage = false;
      caps_ready = true;
   }

   struct spirv_to_nir_options options;
   memset(&options, 0, sizeof(options));
   options.environment = NIR_SPIRV_VULKAN;
   options.capabilities = &caps;
   options.ubo_addr_format = nir_address_format_32bit_index_offset;
   options.ssbo_addr_format = nir_address_format_32bit_index_offset;
   options.phys_ssbo_addr_format = nir_address_format_64bit_global;
   options.push_const_addr_format = nir_address_format_32bit_offset;
   options.shared_addr_format = nir_address_format_32bit_offset;
   options.global_addr_format = nir_address_format_64bit_global;
   options.temp_addr_format = nir_address_format_32bit_offset;
   options.constant_addr_format = nir_address_format_32bit_offset;
   options.min_ubo_alignment = 16;
   options.min_ssbo_alignment = 4;

   struct nir_spirv_specialization *specialization = NULL;
   uint32_t *values = NULL;
   if (spec_count) {
      specialization = vtn_alloc_specialization(spec_count);
      values = calloc(spec_count, sizeof(uint32_t));
      for (unsigned i = 0; i < spec_count; i++) {
         values[i] = spec[i].value;
         specialization->entries[i].id = spec[i].id;
         specialization->entries[i].size = 4;
         specialization->entries[i].data = (uint8_t *)&values[i];
      }
   }

   nir_shader *nir = spirv_to_nir(words, word_count, specialization, stage,
                                  "main", &options, screen->nir_options[stage]);
   free(values);
   free(specialization ? specialization->entries : NULL);
   free(specialization);
   return nir;
}

void *
dirk_nir_compile(struct pipe_context *pipe, enum dirk_stage dstage,
                 const uint32_t *words, size_t word_count,
                 const struct dirk_spec_constant *spec, unsigned spec_count,
                 struct dirk_shader_layout *layout, char *why, size_t why_len)
{
   const mesa_shader_stage stage = dirk_mesa_stage(dstage);
   memset(layout, 0, sizeof(*layout));
   layout->push_ubo = DIRK_NO_BINDING;

   nir_shader *nir = dirk_spirv_to_nir(pipe->screen, stage, words, word_count,
                                       spec, spec_count);
   if (!nir) {
      snprintf(why, why_len, "spirv_to_nir failed");
      return NULL;
   }

   struct dirk_lower s = { .layout = layout };

   NIR_PASS(_, nir, nir_lower_variable_initializers, nir_var_function_temp);
   NIR_PASS(_, nir, nir_lower_returns);
   NIR_PASS(_, nir, nir_inline_functions);
   NIR_PASS(_, nir, nir_opt_copy_prop);
   NIR_PASS(_, nir, nir_opt_deref);
   nir_remove_non_entrypoints(nir);
   NIR_PASS(_, nir, nir_lower_variable_initializers, ~0);
   NIR_PASS(_, nir, nir_split_var_copies);
   NIR_PASS(_, nir, nir_split_per_member_structs);
   NIR_PASS(_, nir, nir_remove_dead_variables,
            nir_var_shader_in | nir_var_shader_out | nir_var_system_value, NULL);
   NIR_PASS(_, nir, nir_lower_system_values);
   if (stage == MESA_SHADER_COMPUTE)
      NIR_PASS(_, nir, nir_lower_compute_system_values, NULL);

   const bool push = dirk_shader_has_push_constants(nir);

   NIR_PASS(_, nir, nir_shader_intrinsics_pass, dirk_lower_resource_index,
            nir_metadata_control_flow, &s);
   NIR_PASS(_, nir, nir_lower_explicit_io, nir_var_mem_ubo | nir_var_mem_ssbo,
            nir_address_format_32bit_index_offset);
   NIR_PASS(_, nir, nir_lower_explicit_io, nir_var_mem_push_const,
            nir_address_format_32bit_offset);
   NIR_PASS(_, nir, nir_lower_explicit_io, nir_var_mem_global,
            nir_address_format_64bit_global);
   if (stage == MESA_SHADER_COMPUTE) {
      NIR_PASS(_, nir, nir_lower_vars_to_explicit_types, nir_var_mem_shared,
               glsl_get_natural_size_align_bytes);
      NIR_PASS(_, nir, nir_lower_explicit_io, nir_var_mem_shared,
               nir_address_format_32bit_offset);
   }
   dirk_assign_buffer_locations(nir, &s);
   if (push || dirk_shader_has_push_constants(nir)) {
      dirk_add_push_block(nir, &s);
      NIR_PASS(_, nir, nir_shader_intrinsics_pass, dirk_lower_push_constants,
               nir_metadata_control_flow, &s);
   }

   if (stage == MESA_SHADER_FRAGMENT)
      NIR_PASS(_, nir, nir_shader_intrinsics_pass, dirk_lower_demote,
               nir_metadata_control_flow, NULL);

   dirk_lower_units(nir, &s);
   dirk_lower_images(nir, &s);
   dirk_assign_io(nir, &s);

   if (s.failed) {
      snprintf(why, why_len, "%s", s.why);
      ralloc_free(nir);
      return NULL;
   }

   dirk_optimize(nir);
   NIR_PASS(_, nir, nir_remove_dead_variables,
            nir_var_function_temp | nir_var_shader_temp, NULL);
   nir_shader_gather_info(nir, nir_shader_get_entrypoint(nir));

   if (stage == MESA_SHADER_COMPUTE) {
      layout->workgroup[0] = nir->info.workgroup_size[0];
      layout->workgroup[1] = nir->info.workgroup_size[1];
      layout->workgroup[2] = nir->info.workgroup_size[2];

      struct pipe_compute_state state;
      memset(&state, 0, sizeof(state));
      state.ir_type = PIPE_SHADER_IR_NIR;
      state.prog = nir;
      state.static_shared_mem = nir->info.shared_size;
      void *cso = pipe->create_compute_state(pipe, &state);
      if (!cso)
         snprintf(why, why_len, "the driver refused the compute shader");
      return cso;
   }

   /* DIRK_NIR_DUMP=1 prints each shader as the driver receives it. */
   static int dump = -1;
   if (dump < 0) {
      const char *env = getenv("DIRK_NIR_DUMP");
      dump = env && *env == '1';
   }
   if (dump) {
      nir_print_shader(nir, stderr);
      fprintf(stderr, "dirk: layout ubos");
      for (unsigned i = 0; i < layout->ubo_count; i++)
         fprintf(stderr, " %u", layout->ubos[i]);
      fprintf(stderr, " units");
      for (unsigned i = 0; i < layout->unit_count; i++)
         fprintf(stderr, " %u/%u", layout->units[i].texture, layout->units[i].sampler);
      fprintf(stderr, " ssbos %u images %u push %u\n", layout->ssbo_count,
              layout->image_count, layout->push_ubo);
   }

   struct pipe_shader_state state;
   memset(&state, 0, sizeof(state));
   state.type = PIPE_SHADER_IR_NIR;
   state.ir.nir = nir;
   state.report_compile_error = true;

   void *cso = NULL;
   switch (stage) {
   case MESA_SHADER_VERTEX:    cso = pipe->create_vs_state(pipe, &state); break;
   case MESA_SHADER_TESS_CTRL: cso = pipe->create_tcs_state(pipe, &state); break;
   case MESA_SHADER_TESS_EVAL: cso = pipe->create_tes_state(pipe, &state); break;
   case MESA_SHADER_GEOMETRY:  cso = pipe->create_gs_state(pipe, &state); break;
   case MESA_SHADER_FRAGMENT:  cso = pipe->create_fs_state(pipe, &state); break;
   default: break;
   }
   if (!cso) {
      snprintf(why, why_len, "%s",
               state.error_message ? state.error_message
                                   : "the driver refused the shader");
      free(state.error_message);
   }
   return cso;
}

void
dirk_nir_delete(struct pipe_context *pipe, enum dirk_stage stage, void *shader)
{
   if (!shader)
      return;
   switch (stage) {
   case DIRK_STAGE_VERTEX:    pipe->delete_vs_state(pipe, shader); break;
   case DIRK_STAGE_TESS_CTRL: pipe->delete_tcs_state(pipe, shader); break;
   case DIRK_STAGE_TESS_EVAL: pipe->delete_tes_state(pipe, shader); break;
   case DIRK_STAGE_GEOMETRY:  pipe->delete_gs_state(pipe, shader); break;
   case DIRK_STAGE_FRAGMENT:  pipe->delete_fs_state(pipe, shader); break;
   case DIRK_STAGE_COMPUTE:   pipe->delete_compute_state(pipe, shader); break;
   }
}
