/* Prints the inputs of Mesa's compute_version_es2() (src/mesa/main/version.c)
 * for the ps5 screen, i.e. why the driver tops out at a given ES version. */
#include <stdio.h>
#include <stdlib.h>
#include "main/mtypes.h"
#include "main/extensions.h"
#include "main/consts_exts.h"
#include "main/version.h"
#include "main/context.h"
#include "pipe/p_screen.h"
#include "frontend/api.h"
#include "state_tracker/st_extensions.h"
#include "ps5_screen.h"

void hostcheck_es_diag(int compressed_fallback);

void
hostcheck_es_diag(int compressed_fallback)
{
   struct pipe_screen *screen = ps5_screen_create();
   struct gl_constants c = {0};
   struct gl_extensions e = {0};
   struct st_config_options o = {0};
   o.allow_compressed_fallback = compressed_fallback;
   o.allow_higher_compat_version = true;
   _mesa_init_constants(&c, API_OPENGLES2);
   _mesa_init_extensions(&e);
   st_init_limits(screen, &c, &e, API_OPENGLES2);
   st_init_extensions(screen, &c, &e, &o, API_OPENGLES2);
#define F(x) printf("   %-44s %s\n", #x, (e.x) ? "yes" : "NO")
#define K(x, v) printf("   %-44s %s (%d)\n", #x, (c.x) >= (v) ? "yes" : "NO", (int)(c.x))
   printf("es-diag: compute_version_es2 inputs (API_OPENGLES2, compressed_fallback=%d)\n",
          compressed_fallback);
   printf("  ES 3.0:\n");
   F(ARB_half_float_vertex); F(ARB_internalformat_query); F(ARB_map_buffer_range);
   F(ARB_shader_texture_lod); F(OES_texture_float); F(OES_texture_half_float);
   F(OES_texture_half_float_linear); F(ARB_texture_rg); F(ARB_depth_buffer_float);
   F(ARB_framebuffer_object); F(EXT_sRGB); F(EXT_packed_float); F(EXT_texture_array);
   F(EXT_texture_shared_exponent); F(EXT_texture_sRGB); F(EXT_transform_feedback);
   F(ARB_draw_instanced); F(ARB_instanced_arrays); F(ARB_uniform_buffer_object);
   F(EXT_texture_snorm); F(NV_primitive_restart); K(PrimitiveRestartFixedIndex, 1);
   F(OES_depth_texture_cube_map); F(EXT_texture_type_2_10_10_10_REV);
   K(MaxColorAttachments, 4);
   printf("  ES 3.1:\n");
   K(MaxVertexAttribStride, 2048); F(ARB_arrays_of_arrays);
   K(MaxComputeWorkGroupInvocations, 128);
   K(Program[MESA_SHADER_COMPUTE].MaxShaderStorageBlocks, 1);
   K(Program[MESA_SHADER_COMPUTE].MaxAtomicBuffers, 1);
   K(Program[MESA_SHADER_COMPUTE].MaxImageUniforms, 1);
   F(ARB_draw_indirect); F(ARB_explicit_uniform_location); F(ARB_framebuffer_no_attachments);
   F(ARB_shading_language_packing); F(ARB_stencil_texturing); F(ARB_texture_multisample);
   F(ARB_texture_gather); F(MESA_shader_integer_functions); F(EXT_shader_integer_mix);
   printf("  ES 3.2:\n");
   F(ARB_shader_atomic_counters); F(ARB_shader_image_load_store); F(ARB_shader_image_size);
   F(ARB_shader_storage_buffer_object); F(EXT_color_buffer_float); F(EXT_draw_buffers2);
   F(KHR_blend_equation_advanced); F(KHR_robustness); F(KHR_texture_compression_astc_ldr);
   F(OES_copy_image); F(ARB_draw_buffers_blend); F(ARB_draw_elements_base_vertex);
   F(OES_geometry_shader); F(OES_primitive_bounding_box); F(OES_sample_variables);
   F(ARB_tessellation_shader); F(OES_texture_buffer); F(OES_texture_cube_map_array);
   F(ARB_texture_stencil8);
   printf("  related: ARB_ES3_compatibility=%d ARB_ES3_1_compatibility=%d ARB_ES3_2_compatibility=%d"
          " GLSLVersion=%u\n", e.ARB_ES3_compatibility, e.ARB_ES3_1_compatibility,
          e.ARB_ES3_2_compatibility, c.GLSLVersion);
   printf("  => _mesa_get_version(API_OPENGLES2) = %u\n",
          _mesa_get_version(&e, &c, API_OPENGLES2));
   free(c.SpirVExtensions);
   screen->destroy(screen);
}
