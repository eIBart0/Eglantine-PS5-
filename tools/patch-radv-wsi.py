#!/usr/bin/env python3
"""Apply the pinned Eden display-lifetime adaptation to the Mesa build export."""
from pathlib import Path
import hashlib
import sys

root = Path(__file__).resolve().parents[1]
source = root.parent/'mihawk-mesa-review/src/vulkan/wsi/wsi_common_videoout.c'
target = root.parent/'mihawk-vulkan-review/.deps/work/radv-src/src/vulkan/wsi/wsi_common_videoout.c'
text = source.read_text()

def replace(old, new):
    global text
    assert text.count(old) == 1, f'Pinned WSI anchor changed: {old[:70]}'
    text = text.replace(old, new)

replace('   bool opened;\n', '   unsigned owners;\n   bool closing, stop;\n   bool opened;\n')
# A set of framebuffers per output size: each remembers the memory to give back.
replace('      uint8_t *buffers;\n   } sets[VIDEOOUT_SIZES];',
        '      uint8_t *buffers;\n      int64_t physical;\n      size_t bytes;\n   } sets[VIDEOOUT_SIZES];')
replace('static int\nsceVideoOutSetFlipRate', 'static int\nsceVideoOutClose(int32_t handle)\n{\n   return 0;\n}\n\nstatic int\nsceVideoOutSetFlipRate')
replace('   uint8_t *buffers = NULL;\n#if defined(__PROSPERO__)\n   int64_t physical = -1;',
        '   uint8_t *buffers = NULL;\n   int64_t physical = -1;\n#if defined(__PROSPERO__)')
replace('   out->sets[index] = (struct wsi_videoout_set){.size = size, .buffers = buffers};',
        '   out->sets[index] = (struct wsi_videoout_set){.size = size, .buffers = buffers, .physical = physical,\n'
        '                                                .bytes = bytes};')
replace('      while (out->queue_count == 0)\n         cnd_wait(&out->changed, &out->lock);',
        '      while (out->queue_count == 0 && !out->stop)\n         cnd_wait(&out->changed, &out->lock);\n      if (out->queue_count == 0 && out->stop) {\n         mtx_unlock(&out->lock);\n         return 0;\n      }')
replace('VkResult\nwsi_display_init_wsi',
        (root/'tools/radv-videoout-lifecycle.inc').read_text()+'\nVkResult\nwsi_display_init_wsi')
# 120 Hz: the app asks for it per game session (headless/display_refresh.h). The pinned source
# decides from the package's param.json, which the app cannot read once it has left the sandbox,
# and for the whole run; the package declares the capability (tools/package-headless-native.sh).
declares = 'videoout_title_declares_high_frame_rate(void)\n{\n'
assert text.count(declares) == 1, 'Pinned WSI anchor changed: the 120 Hz declaration'
body_start = text.index(declares) + len(declares)
body_end = text.index('\n}\n', body_start)
assert 'param.json' in text[body_start:body_end], 'Pinned WSI anchor changed: the 120 Hz declaration reads param.json'
text = (text[:body_start] + '   const char *const asked = getenv("EDEN_VIDEOOUT_120HZ");\n'
        '   return asked != NULL && asked[0] == \'1\';' + text[body_end:])
# The pinned source takes the 120 Hz output back unless a vblank then comes about every 8.3 ms.
# On a display with a variable refresh rate that is wrong: before the first frame is shown it
# idles at the bottom of its range (a vblank every 20.87 ms, 48 Hz, in two reports), the output
# went back to 59.94 Hz, and a game could not have 120 Hz at all. Only a display that stayed at
# 59.94 Hz (a vblank about every 16.7 ms) gives the mode back.
replace('         if (period == 0 || period > VIDEOOUT_HIGH_REFRESH_LIMIT_NS) {',
        '         if (period == 0 || (period > UINT64_C(15000000) && period < UINT64_C(18500000))) {')
# Say so when the display follows, as the pinned source does when it does not.
replace('''                       (double)period / 1e6);
         }
      } else if (supported > 0) {''', '''                       (double)period / 1e6);
         } else {
            fprintf(stderr, "wsi/videoout: 119.88 Hz output, a vblank every %.3f ms\\n", (double)period / 1e6);
         }
      } else if (supported > 0) {''')
replace('   wsi->base.get_support =', '   videoout_ref();\n\n   wsi->base.get_support =')
replace('   if (wsi)\n      vk_free(alloc, wsi);',
        '   if (wsi) {\n      videoout_unref();\n      vk_free(alloc, wsi);\n      wsi_device->wsi[VK_ICD_WSI_PLATFORM_DISPLAY] = NULL;\n   }')
# Preserve the reason behind RADV's successful-but-empty device enumeration.
# These startup-only diagnostics avoid repeated full game runs for each stage.
adaptations = {
    # Restore the pinned upload path: explicit CPU flush did not affect HUD corruption.
    # The first task/mesh draws, and how the driver runs them (stderr.log).
    'src/amd/vulkan/radv_cmd_buffer.c': [
        ('#include "radv_cmd_buffer.h"\n', '#include "radv_cmd_buffer.h"\n#include <stdio.h>\n'),
        ('   if (!radv_before_taskmesh_draw(cmd_buffer, &info, 1, false))\n      return;\n\n'
         '   if (radv_cmdbuf_has_stage(cmd_buffer, MESA_SHADER_TASK) && !cmd_buffer->gang.cs) {\n'
         '      radv_task_emu_draw_direct(cmd_buffer, x, y, z);\n',
         '   if (!radv_before_taskmesh_draw(cmd_buffer, &info, 1, false))\n      return;\n\n'
         '   {\n'
         '      static unsigned logged;\n'
         '      const unsigned n = __atomic_add_fetch(&logged, 1, __ATOMIC_RELAXED);\n'
         '      if (n <= 32) {\n'
         '         const struct radv_shader *ms = cmd_buffer->state.shaders[MESA_SHADER_MESH];\n'
         '         fprintf(stderr, "radv/ps5: mesh draw %u: %ux%ux%u workgroups, %s, parts=%u records=%d publish=%d\\n", n, x, y,\n'
         '                 z, !radv_cmdbuf_has_stage(cmd_buffer, MESA_SHADER_TASK) ? "mesh only"\n'
         '                    : cmd_buffer->gang.cs ? "task on the compute ring" : "task emulated",\n'
         '                 ms ? (unsigned)ms->info.ms.prim_parts : 0u, ms ? (int)ms->info.ms.draw_records : 0,\n'
         '                 ms ? (int)radv_ms_publishes(&ms->info) : 0);\n'
         '      }\n'
         '   }\n'
         '   if (radv_cmdbuf_has_stage(cmd_buffer, MESA_SHADER_TASK) && !cmd_buffer->gang.cs) {\n'
         '      radv_task_emu_draw_direct(cmd_buffer, x, y, z);\n'),
    ],
    'src/amd/vulkan/radv_physical_device.c': [
        ('   if (result == VK_ERROR_INCOMPATIBLE_DRIVER || result == VK_ERROR_INITIALIZATION_FAILED)\n      return VK_SUCCESS;',
         '   if (result != VK_SUCCESS) fprintf(stderr, "radv/ps5: physical device creation result=%d\\n", result);\n   if (result == VK_ERROR_INCOMPATIBLE_DRIVER || result == VK_ERROR_INITIALIZATION_FAILED)\n      return VK_SUCCESS;'),
        ('   if (!pdev->addrlib) {', '   if (!pdev->addrlib) {\n      fprintf(stderr, "radv/ps5: addrlib creation failed\\n");'),
        # A mesh shader's parts take the first part's outputs from the publish ring only with
        # RADV_PS5_MS_PUBLISH=1 (radv_shader_info.h below). The setting goes into the pipeline
        # cache's UUID: a cache written with the other setting holds shaders compiled the other way.
        ('static int\nradv_device_get_cache_uuid(struct radv_physical_device *pdev, void *uuid)\n{\n'
         '   blake3_hasher ctx;\n   unsigned char blake3[BLAKE3_KEY_LEN];\n\n'
         '   memset(uuid, 0, VK_UUID_SIZE);\n   _mesa_blake3_init(&ctx);\n',
         'bool radv_ps5_ms_publish;\n\n'
         'static int\nradv_device_get_cache_uuid(struct radv_physical_device *pdev, void *uuid)\n{\n'
         '   blake3_hasher ctx;\n   unsigned char blake3[BLAKE3_KEY_LEN];\n\n'
         '   static bool told;\n'
         '   radv_ps5_ms_publish = debug_get_bool_option("RADV_PS5_MS_PUBLISH", false);\n'
         '   if (!told) {\n'
         '      told = true;\n'
         '      fprintf(stderr, "radv/ps5: mesh shader parts %s (RADV_PS5_MS_PUBLISH)\\n",\n'
         '              radv_ps5_ms_publish ? "share the first part\'s outputs through the publish ring"\n'
         '                                  : "each run the workgroup");\n'
         '   }\n\n'
         '   memset(uuid, 0, VK_UUID_SIZE);\n   _mesa_blake3_init(&ctx);\n'),
        ('   _mesa_blake3_final(&ctx, blake3);\n\n   memcpy(uuid, blake3, VK_UUID_SIZE);\n   return 0;\n}\n',
         '   _mesa_blake3_update(&ctx, &radv_ps5_ms_publish, sizeof(radv_ps5_ms_publish));\n'
         '   _mesa_blake3_final(&ctx, blake3);\n\n   memcpy(uuid, blake3, VK_UUID_SIZE);\n   return 0;\n}\n'),
    ],
    # The publish ring makes a mesh shader's later parts wait in a loop for the first one's slot,
    # GPU-wide; the GPU stopped on the first task/mesh draws of Mario Kart World and The Legend of
    # Zelda: Ocarina of Time. Without it each part runs the API workgroup again.
    'src/amd/vulkan/radv_shader_info.h': [
        ('static inline bool\nradv_ms_publishes(const struct radv_shader_info *info)\n{\n'
         '   return info->ms.prim_parts > 1 && info->ms.draw_records;\n}\n',
         '/* EglantinePS5: RADV_PS5_MS_PUBLISH=1 (radv_physical_device.c); off by default. */\n'
         'extern bool radv_ps5_ms_publish;\n\n'
         'static inline bool\nradv_ms_publishes(const struct radv_shader_info *info)\n{\n'
         '   return radv_ps5_ms_publish && info->ms.prim_parts > 1 && info->ms.draw_records;\n}\n'),
    ],
    # What each mesh shader compiled into (stderr.log, the first 64).
    'src/amd/vulkan/radv_shader.c': [
        ('      options.ms_prim_parts = info->ms.prim_parts;\n      options.ms_publish = radv_ms_publishes(info);\n',
         '      options.ms_prim_parts = info->ms.prim_parts;\n      options.ms_publish = radv_ms_publishes(info);\n'
         '      {\n'
         '         static unsigned logged;\n'
         '         const unsigned n = __atomic_add_fetch(&logged, 1, __ATOMIC_RELAXED);\n'
         '         if (n <= 64)\n'
         '            fprintf(stderr, "radv/ps5: mesh shader %u: parts=%u publish=%d draw_records=%d task=%d workgroup=%u "\n'
         '                    "max_vertices=%u max_primitives=%u primitive=%u per_primitive_outputs=%#llx\\n", n,\n'
         '                    (unsigned)info->ms.prim_parts, (int)options.ms_publish, (int)info->ms.draw_records,\n'
         '                    (int)info->ms.has_task, (unsigned)info->workgroup_size,\n'
         '                    (unsigned)nir->info.mesh.max_vertices_out, (unsigned)nir->info.mesh.max_primitives_out,\n'
         '                    (unsigned)nir->info.mesh.primitive_type,\n'
         '                    (unsigned long long)(nir->info.outputs_written & nir->info.per_primitive_outputs));\n'
         '      }\n'),
    ],
    'src/amd/vulkan/winsys/ps5/radv_ps5_winsys.c': [
        ('   if (!radv_ps5_platform_init())\n      return;', '   if (!radv_ps5_platform_init()) {\n      fprintf(stderr, "radv/ps5: queue platform initialization failed\\n");\n      return;\n   }'),
        ('   if (!radv_ps5_memory_alloc(RADV_PS5_RING_BYTES, RADV_PS5_LARGE_BYTES, true, &queue->ring))\n      return;', '   if (!radv_ps5_memory_alloc(RADV_PS5_RING_BYTES, RADV_PS5_LARGE_BYTES, true, &queue->ring)) {\n      fprintf(stderr, "radv/ps5: queue ring allocation failed\\n");\n      return;\n   }'),
        ('      radv_ps5_memory_free(&queue->ring);', '      fprintf(stderr, "radv/ps5: queue marker allocation failed\\n");\n      radv_ps5_memory_free(&queue->ring);'),
    ],
    'src/amd/vulkan/winsys/ps5/radv_ps5_platform.c': [
        ('''bool
radv_ps5_memory_grant_gpu(void *address, uint64_t bytes)
{
   /* A title's anonymous memory takes GPU access this way, and a shader then
    * writes it through the CPU's address (HARDWARE_FINDINGS.md, 2026-09-27). */
   return sceKernelMprotect(address, bytes,
                            PS5_KERNEL_PROT_CPU_READ | PS5_KERNEL_PROT_CPU_WRITE | PS5_KERNEL_PROT_GPU_READ |
                               PS5_KERNEL_PROT_GPU_WRITE) == 0;
}''', (root/'tools/radv-host-memory.inc').read_text().strip()),
        ('   if (!placed) {', '   if (!placed) {\n      fprintf(stderr, "radv/ps5: mapping rejected result=%d address=%p bytes=%llu window32=%d\\n", result, address, (unsigned long long)bytes, window32);'),
    ],
}
adapted_texts = {}
for relative, replacements in adaptations.items():
    adapted = (root.parent/'mihawk-mesa-review'/relative).read_text()
    for old, new in replacements:
        assert adapted.count(old) == 1, f'Pinned initialization anchor changed: {relative}'
        adapted = adapted.replace(old, new)
    adapted_texts[relative] = adapted

# What a built driver must have been built from: the display code and every other adapted file
# (EDEN_WSI_SHA256; tools/prepare-build.sh rebuilds the driver when it differs).
whole = hashlib.sha256(text.encode())
for relative in sorted(adapted_texts):
    whole.update(b'\0' + relative.encode() + b'\0' + adapted_texts[relative].encode())
digest = whole.hexdigest()
if sys.argv[1:2] == ['--check']:
    # Is the driver that was built (its recorded source digest, EDEN_WSI_SHA256) this adaptation?
    recorded = Path(sys.argv[2]).read_text().split()[0] if Path(sys.argv[2]).is_file() else ''
    sys.exit(0 if recorded == digest else 1)
if sys.argv[1:2] == ['--digest']:
    print(f'{digest}  RADV adaptation (wsi_common_videoout.c and {len(adapted_texts)} more files)')
    sys.exit(0)

changed = not target.exists() or target.read_text() != text
if changed:
    target.write_text(text)
    # Upstream caches the whole build by pin. Invalidate only that receipt;
    # Ninja retains all unchanged objects and rebuilds the patched WSI.
    provenance = root.parent/'mihawk-vulkan-review/.deps/native/radv-release/PROVENANCE.txt'
    if provenance.exists():
        provenance.replace(provenance.with_suffix('.base.txt'))
print(f'RADV WSI adaptation changed={changed} sha256={hashlib.sha256(text.encode()).hexdigest()}')

for relative, adapted in adapted_texts.items():
    destination = root.parent/'mihawk-vulkan-review/.deps/work/radv-src'/relative
    if destination.read_text() != adapted:
        destination.write_text(adapted)
        provenance = root.parent/'mihawk-vulkan-review/.deps/native/radv-release/PROVENANCE.txt'
        if provenance.exists():
            provenance.replace(provenance.with_suffix('.base.txt'))
    print(f'RADV initialization diagnostic {relative} sha256={hashlib.sha256(adapted.encode()).hexdigest()}')
print(f'RADV adaptation digest {digest}')
