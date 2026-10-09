/* deko3d layer of the Switch renderer; see gpu.h. */
#include "gpu.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <switch.h>
#include <deko3d.h>

void siglus_switch_log_message(const char* message);

enum {
    DisplayWidth = 1280,
    DisplayHeight = 720,
    FramebufferCount = 3,
    FrameSlots = 3,
    MaxTextures = 4096,
    CodeMemorySize = 2 * 1024 * 1024,
    CommandChunkSize = 1024 * 1024,
    RingSize = 64 * 1024 * 1024,
    MaxPrograms = 32,
    MaxDeferred = 1024,
};

typedef struct Texture {
    bool used;
    bool render_target;
    uint32_t width, height, mip_levels;
    uint32_t memory_size, memory_flags;
    DkMemBlock memory;
    DkImage image;
    uint32_t depth_memory_size, depth_memory_flags;
    DkMemBlock depth_memory;
    DkImage depth;
} Texture;

typedef struct Deferred {
    DkMemBlock memory;
    uint32_t memory_size;
    uint32_t memory_flags;
    int32_t texture; /* descriptor slot to release, or -1 */
} Deferred;

#define MAX_MEMBLOCK_POOL 32

typedef struct {
    DkMemBlock block;
    uint32_t size;
    uint32_t flags;
} MemBlockPoolEntry;

static MemBlockPoolEntry memblock_pool[MAX_MEMBLOCK_POOL];
static uint32_t memblock_pool_count = 0;

typedef struct {
    uint32_t vertex_program;
    uint32_t fragment_program;
    uint8_t cull_mode;
    uint8_t front_face;
    bool blend_enable;
    uint8_t color_write_mask;
    uint8_t color_op, color_src, color_dst;
    uint8_t alpha_op, alpha_src, alpha_dst;
    bool depth_test, depth_write;
    uint8_t depth_compare;
    bool stencil_enable;
    uint8_t stencil_compare, stencil_fail, stencil_depth_fail, stencil_pass, stencil_ref;
    uint32_t stride;
    uint32_t attrib_count;
    DkVtxAttribState attribs[SiglusGpuMaxVertexAttribs];
    bool valid;
} GpuDrawState;

static GpuDrawState current_draw_state;

typedef struct FrameSlot {
    DkCmdBuf cmdbuf;
    DkMemBlock chunks[64];
    uint32_t chunk_count;
    DkMemBlock ring;
    uint32_t ring_used;
    DkFence fence;
    bool fence_pending;
    Deferred deferred[MaxDeferred];
    uint32_t deferred_count;
} FrameSlot;

static DkDevice device;
static DkQueue queue;
static NWindow* window;
static DkMemBlock framebuffer_memory;
static DkImage framebuffers[FramebufferCount];
static DkMemBlock display_depth_memory;
static DkImage display_depth;
static DkSwapchain swapchain;
static DkMemBlock code_memory;
static uint32_t code_used;
static DkShader programs[MaxPrograms];
static char program_names[MaxPrograms][32];
static uint32_t program_count;
static DkMemBlock descriptor_memory;
static DkImageDescriptor* image_descriptors;
static DkSamplerDescriptor* sampler_descriptors;
static Texture textures[MaxTextures];
static FrameSlot slots[FrameSlots];
static uint32_t frame_number;
static FrameSlot* current;
static bool recording;
static int display_slot = -1; /* acquired swapchain image this frame */
static bool textures_dirty;     /* uploads or new descriptors since the last barrier */
static bool pass_skipped;      /* the display could not be acquired */
static unsigned acquire_failures;
static uint64_t total_fence_wait_ticks;
static uint64_t total_acquire_ticks;
static uint64_t total_draw_ticks;
static uint64_t total_upload_ticks;
static uint64_t total_draws;
static uint64_t total_uploads;
static uint64_t total_upload_bytes;

void siglus_gpu_get_bench_stats(uint64_t* fence_ticks, uint64_t* acq_ticks,
                                uint64_t* draw_ticks, uint64_t* upload_ticks,
                                uint64_t* draws, uint64_t* uploads, uint64_t* upload_bytes) {
    if (fence_ticks) *fence_ticks = total_fence_wait_ticks;
    if (acq_ticks) *acq_ticks = total_acquire_ticks;
    if (draw_ticks) *draw_ticks = total_draw_ticks;
    if (upload_ticks) *upload_ticks = total_upload_ticks;
    if (draws) *draws = total_draws;
    if (uploads) *uploads = total_uploads;
    if (upload_bytes) *upload_bytes = total_upload_bytes;
    total_fence_wait_ticks = 0;
    total_acquire_ticks = 0;
    total_draw_ticks = 0;
    total_upload_ticks = 0;
    total_draws = 0;
    total_uploads = 0;
    total_upload_bytes = 0;
}

static void log_result(const char* what, int value) {
    char message[160];
    snprintf(message, sizeof(message), "siglus_switch: gpu %s %d\n", what, value);
    siglus_switch_log_message(message);
}

static void debug_callback(void* user, const char* context, DkResult result, const char* message) {
    (void) user;
    char text[256];
    snprintf(text, sizeof(text), "siglus_switch: deko3d context=%s result=%u message=%s\n",
             context ? context : "(none)", (unsigned) result, message ? message : "(none)");
    siglus_switch_log_message(text);
}

static uint32_t align_up(uint32_t value, uint32_t alignment) {
    return (value + alignment - 1) & ~(alignment - 1);
}

static DkMemBlock make_memory(uint32_t size, uint32_t flags) {
    const uint32_t aligned = align_up(size, DK_MEMBLOCK_ALIGNMENT);
    for (uint32_t i = 0; i < memblock_pool_count; ++i) {
        if (memblock_pool[i].flags == flags && memblock_pool[i].size == aligned) {
            DkMemBlock found = memblock_pool[i].block;
            memblock_pool[i] = memblock_pool[--memblock_pool_count];
            return found;
        }
    }
    DkMemBlockMaker maker;
    dkMemBlockMakerDefaults(&maker, device, aligned);
    maker.flags = flags;
    return dkMemBlockCreate(&maker);
}

/* A command buffer that ran out of memory gets another chunk (freed when
 * the frame's slot is reused). */
static void add_command_memory(void* user, DkCmdBuf cmdbuf, size_t min_size) {
    FrameSlot* slot = (FrameSlot*) user;
    const uint32_t size = align_up((uint32_t) (min_size > CommandChunkSize ? min_size : CommandChunkSize),
                                   DK_MEMBLOCK_ALIGNMENT);
    if (slot->chunk_count >= 64) {
        siglus_switch_log_message("siglus_switch: gpu command memory exhausted\n");
        return;
    }
    DkMemBlock memory = make_memory(size, DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached);
    slot->chunks[slot->chunk_count++] = memory;
    dkCmdBufAddMemory(cmdbuf, memory, 0, size);
}

/* Room in this frame's ring (CPU-written, GPU-read), or 0 when full. */
static DkGpuAddr ring_alloc(uint32_t size, uint32_t alignment, void** cpu) {
    FrameSlot* slot = current;
    const uint32_t start = align_up(slot->ring_used, alignment);
    if (start + size > RingSize) return 0;
    slot->ring_used = start + size;
    *cpu = (uint8_t*) dkMemBlockGetCpuAddr(slot->ring) + start;
    return dkMemBlockGetGpuAddr(slot->ring) + start;
}

/* Frees after the GPU is done with every frame that could use it: during
 * a frame, with that frame's slot; between frames, with the slot of the
 * frame just finished (the other one). */
static void defer(DkMemBlock memory, uint32_t size, uint32_t flags, int32_t texture) {
    FrameSlot* slot = recording ? current : &slots[(frame_number + FrameSlots - 1) % FrameSlots];
    if (slot->deferred_count >= MaxDeferred) {
        /* Rare: wait for the GPU and free right away. */
        dkQueueWaitIdle(queue);
        if (memory) {
            if (memblock_pool_count < MAX_MEMBLOCK_POOL && size > 0) {
                memblock_pool[memblock_pool_count++] = (MemBlockPoolEntry) { memory, size, flags };
            } else {
                dkMemBlockDestroy(memory);
            }
        }
        if (texture >= 0) textures[texture].used = false;
        return;
    }
    slot->deferred[slot->deferred_count++] = (Deferred) { memory, size, flags, texture };
}

static void release_deferred(FrameSlot* slot) {
    for (uint32_t i = 0; i < slot->deferred_count; ++i) {
        if (slot->deferred[i].memory) {
            DkMemBlock mem = slot->deferred[i].memory;
            uint32_t sz = slot->deferred[i].memory_size;
            uint32_t fl = slot->deferred[i].memory_flags;
            if (memblock_pool_count < MAX_MEMBLOCK_POOL && sz > 0) {
                memblock_pool[memblock_pool_count++] = (MemBlockPoolEntry) { mem, sz, fl };
            } else {
                dkMemBlockDestroy(mem);
            }
        }
        if (slot->deferred[i].texture >= 0) textures[slot->deferred[i].texture].used = false;
    }
    slot->deferred_count = 0;
}

static void init_image(DkImage* image, DkMemBlock* memory, uint32_t* out_size, uint32_t* out_flags,
                       uint32_t width, uint32_t height, uint32_t mip_levels, DkImageFormat format, uint32_t flags) {
    DkImageLayoutMaker maker;
    dkImageLayoutMakerDefaults(&maker, device);
    maker.flags = flags;
    maker.format = format;
    maker.dimensions[0] = width;
    maker.dimensions[1] = height;
    maker.mipLevels = mip_levels;
    DkImageLayout layout;
    dkImageLayoutInitialize(&layout, &maker);
    const uint32_t alignment = dkImageLayoutGetAlignment(&layout);
    const uint32_t size = align_up((uint32_t) dkImageLayoutGetSize(&layout),
                                   alignment > DK_MEMBLOCK_ALIGNMENT ? alignment : DK_MEMBLOCK_ALIGNMENT);
    const uint32_t mem_flags = DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image;
    *memory = make_memory(size, mem_flags);
    if (out_size) *out_size = size;
    if (out_flags) *out_flags = mem_flags;
    dkImageInitialize(image, &layout, *memory, 0);
}

static void write_descriptor(int32_t id) {
    DkImageView view;
    dkImageViewDefaults(&view, &textures[id].image);
    dkImageDescriptorInitialize(&image_descriptors[id], &view, false, false);
}

static void start_recording(void) {
    FrameSlot* slot = current;
    dkCmdBufClear(slot->cmdbuf);
    memset(&current_draw_state, 0, sizeof(current_draw_state));
    /* The first chunk is kept; chunks added when a frame needed more go. */
    for (uint32_t i = 1; i < slot->chunk_count; ++i) dkMemBlockDestroy(slot->chunks[i]);
    if (slot->chunk_count == 0) {
        add_command_memory(slot, slot->cmdbuf, CommandChunkSize);
    } else {
        slot->chunk_count = 1;
        dkCmdBufAddMemory(slot->cmdbuf, slot->chunks[0], 0, CommandChunkSize);
    }
    dkCmdBufBindImageDescriptorSet(slot->cmdbuf, dkMemBlockGetGpuAddr(descriptor_memory), MaxTextures);
    dkCmdBufBindSamplerDescriptorSet(slot->cmdbuf,
        dkMemBlockGetGpuAddr(descriptor_memory) + MaxTextures * sizeof(DkImageDescriptor),
        SiglusGpuSamplerCount);
    /* The ring is rewritten by the CPU every other frame: nothing the GPU
     * cached from its last use (vertices, uniforms, upload sources) or
     * from reused descriptor slots may survive. */
    dkCmdBufBarrier(slot->cmdbuf, DkBarrier_None,
                    DkInvalidateFlags_Image | DkInvalidateFlags_Shader | DkInvalidateFlags_Descriptors |
                        DkInvalidateFlags_L2Cache);
    recording = true;
}

/* Submits what has been recorded so far and keeps recording. */
static void submit_recorded(void) {
    DkCmdList list = dkCmdBufFinishList(current->cmdbuf);
    dkQueueSubmitCommands(queue, list);
}

void siglus_gpu_init(void) {
    DkDeviceMaker device_maker;
    dkDeviceMakerDefaults(&device_maker);
    device_maker.cbDebug = debug_callback;
    /* Default clip space: depth 0..1, y up, as in WGSL (the shaders are the
     * desktop's, translated by platform/switch/shaderc). */
    device = dkDeviceCreate(&device_maker);

    DkQueueMaker queue_maker;
    dkQueueMakerDefaults(&queue_maker, device);
    queue_maker.flags = DkQueueFlags_Graphics;
    queue = dkQueueCreate(&queue_maker);

    DkImageLayoutMaker fb_maker;
    dkImageLayoutMakerDefaults(&fb_maker, device);
    fb_maker.flags = DkImageFlags_UsageRender | DkImageFlags_UsagePresent | DkImageFlags_HwCompression;
    fb_maker.format = DkImageFormat_RGBA8_Unorm;
    fb_maker.dimensions[0] = DisplayWidth;
    fb_maker.dimensions[1] = DisplayHeight;
    DkImageLayout fb_layout;
    dkImageLayoutInitialize(&fb_layout, &fb_maker);
    const uint32_t fb_alignment = dkImageLayoutGetAlignment(&fb_layout);
    const uint32_t fb_size = align_up((uint32_t) dkImageLayoutGetSize(&fb_layout), fb_alignment);
    framebuffer_memory = make_memory(FramebufferCount * fb_size, DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image);
    const DkImage* images[FramebufferCount];
    for (unsigned i = 0; i < FramebufferCount; ++i) {
        dkImageInitialize(&framebuffers[i], &fb_layout, framebuffer_memory, i * fb_size);
        images[i] = &framebuffers[i];
    }
    init_image(&display_depth, &display_depth_memory, NULL, NULL, DisplayWidth, DisplayHeight, 1,
               DkImageFormat_Z24S8, DkImageFlags_UsageRender | DkImageFlags_HwCompression);
    DkSwapchainMaker swapchain_maker;
    window = nwindowGetDefault();
    dkSwapchainMakerDefaults(&swapchain_maker, device, window, images, FramebufferCount);
    swapchain = dkSwapchainCreate(&swapchain_maker);
    dkSwapchainSetSwapInterval(swapchain, 1);

    code_memory = make_memory(CodeMemorySize, DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached | DkMemBlockFlags_Code);
    code_used = 0;

    const uint32_t descriptor_size = MaxTextures * sizeof(DkImageDescriptor) +
                                     SiglusGpuSamplerCount * sizeof(DkSamplerDescriptor);
    descriptor_memory = make_memory(descriptor_size, DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached);
    image_descriptors = (DkImageDescriptor*) dkMemBlockGetCpuAddr(descriptor_memory);
    sampler_descriptors = (DkSamplerDescriptor*) (image_descriptors + MaxTextures);
    for (int i = 0; i < SiglusGpuSamplerCount; ++i) {
        DkSampler sampler;
        dkSamplerDefaults(&sampler);
        sampler.wrapMode[0] = DkWrapMode_ClampToEdge;
        sampler.wrapMode[1] = DkWrapMode_ClampToEdge;
        sampler.wrapMode[2] = DkWrapMode_ClampToEdge;
        if (i == SiglusGpuSampler_Linear) {
            sampler.minFilter = DkFilter_Linear;
            sampler.magFilter = DkFilter_Linear;
            sampler.mipFilter = DkMipFilter_None;
        }
        dkSamplerDescriptorInitialize(&sampler_descriptors[i], &sampler);
    }

    for (unsigned i = 0; i < FrameSlots; ++i) {
        DkCmdBufMaker maker;
        dkCmdBufMakerDefaults(&maker, device);
        maker.userData = &slots[i];
        maker.cbAddMem = add_command_memory;
        slots[i].cmdbuf = dkCmdBufCreate(&maker);
        slots[i].ring = make_memory(RingSize, DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached);
    }
    siglus_switch_log_message("siglus_switch: gpu ready\n");
}

void siglus_gpu_exit(void) {
    /* Horizon reclaims the device and its memory when the process ends;
     * waiting on a swapchain the compositor holds can block forever (see
     * main.c). */
    for (unsigned i = 0; i < FrameSlots; ++i) {
        if (slots[i].fence_pending) dkFenceWait(&slots[i].fence, 250 * 1000 * 1000LL);
    }
    for (uint32_t i = 0; i < memblock_pool_count; ++i) {
        if (memblock_pool[i].block) dkMemBlockDestroy(memblock_pool[i].block);
    }
    memblock_pool_count = 0;
}

uint32_t siglus_gpu_display_width(void) { return DisplayWidth; }
uint32_t siglus_gpu_display_height(void) { return DisplayHeight; }

uint32_t siglus_gpu_program(const char* name) {
    for (uint32_t i = 0; i < program_count; ++i) {
        if (strcmp(program_names[i], name) == 0) return i + 1;
    }
    if (program_count >= MaxPrograms) return 0;
    char path[96];
    snprintf(path, sizeof(path), "romfs:/shaders/%s.dksh", name);
    FILE* file = fopen(path, "rb");
    if (!file) {
        log_result("missing shader (see log above)", 0);
        siglus_switch_log_message(path);
        return 0;
    }
    fseek(file, 0, SEEK_END);
    const long length = ftell(file);
    rewind(file);
    if (length <= 0 || (uint32_t) length > CodeMemorySize - code_used) {
        fclose(file);
        log_result("shader too large", (int) length);
        return 0;
    }
    const uint32_t offset = code_used;
    code_used += align_up((uint32_t) length, DK_SHADER_CODE_ALIGNMENT);
    fread((uint8_t*) dkMemBlockGetCpuAddr(code_memory) + offset, (size_t) length, 1, file);
    fclose(file);
    DkShaderMaker maker;
    dkShaderMakerDefaults(&maker, code_memory, offset);
    dkShaderInitialize(&programs[program_count], &maker);
    snprintf(program_names[program_count], sizeof(program_names[0]), "%s", name);
    return ++program_count;
}

int32_t siglus_gpu_texture_create(uint32_t width, uint32_t height, uint32_t mip_levels, uint32_t flags) {
    if (width == 0 || height == 0 || mip_levels == 0) return -1;
    static int32_t next_texture_slot = 0;
    int32_t id = -1;
    for (int32_t step = 0; step < MaxTextures; ++step) {
        const int32_t candidate = (next_texture_slot + step) % MaxTextures;
        if (!textures[candidate].used) {
            id = candidate;
            next_texture_slot = (candidate + 1) % MaxTextures;
            break;
        }
    }
    if (id < 0) {
        siglus_switch_log_message("siglus_switch: gpu texture slots exhausted\n");
        return -1;
    }
    Texture* texture = &textures[id];
    memset(texture, 0, sizeof(*texture));
    texture->used = true;
    texture->width = width;
    texture->height = height;
    texture->mip_levels = mip_levels;
    texture->render_target = (flags & SiglusGpuTexture_RenderTarget) != 0;
    init_image(&texture->image, &texture->memory, &texture->memory_size, &texture->memory_flags,
               width, height, mip_levels, DkImageFormat_RGBA8_Unorm,
               texture->render_target ? DkImageFlags_UsageRender | DkImageFlags_HwCompression : 0);
    if (texture->render_target) {
        init_image(&texture->depth, &texture->depth_memory, &texture->depth_memory_size, &texture->depth_memory_flags,
                   width, height, 1, DkImageFormat_Z24S8,
                   DkImageFlags_UsageRender | DkImageFlags_HwCompression);
    }
    write_descriptor(id);
    textures_dirty = true;
    return id;
}

void siglus_gpu_texture_upload(int32_t id, uint32_t level, const uint8_t* rgba, uint32_t width, uint32_t height) {
    if (!recording || id < 0 || id >= MaxTextures || !textures[id].used) return;
    const uint64_t t0 = armGetSystemTick();
    const uint32_t size = width * height * 4;
    void* cpu = NULL;
    DkGpuAddr addr = 0;
    /* Reserve 4 MiB of the per-frame ring buffer for vertices and uniforms so
     * heavy scene-transition texture uploads never exhaust the ring and cause
     * siglus_gpu_draw to skip draws (which produces a 1-frame black flash). */
    const uint32_t DrawRingReserve = 4 * 1024 * 1024;
    if (align_up(current->ring_used, 256) + size <= RingSize - DrawRingReserve) {
        addr = ring_alloc(size, 256, &cpu);
    }
    DkMemBlock temporary = NULL;
    uint32_t temp_size = 0;
    uint32_t temp_flags = 0;
    if (addr == 0) {
        temp_flags = DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached;
        temp_size = align_up(size, DK_MEMBLOCK_ALIGNMENT);
        temporary = make_memory(temp_size, temp_flags);
        cpu = dkMemBlockGetCpuAddr(temporary);
        addr = dkMemBlockGetGpuAddr(temporary);
    }
    memcpy(cpu, rgba, size);
    DkImageView view;
    dkImageViewDefaults(&view, &textures[id].image);
    view.mipLevelOffset = (uint8_t) level;
    view.mipLevelCount = 1;
    const DkCopyBuf src = { addr, 0, 0 }; /* tightly packed */
    const DkImageRect rect = { 0, 0, 0, width, height, 1 };
    /* Wait for any earlier fragment reads of this texture before overwriting it in-place. */
    dkCmdBufBarrier(current->cmdbuf, DkBarrier_Fragments, 0);
    dkCmdBufCopyBufferToImage(current->cmdbuf, &src, &view, &rect, 0);
    if (temporary) defer(temporary, temp_size, temp_flags, -1);
    textures_dirty = true;
    total_uploads++;
    total_upload_bytes += size;
    total_upload_ticks += (armGetSystemTick() - t0);
}

void siglus_gpu_texture_destroy(int32_t id) {
    if (id < 0 || id >= MaxTextures || !textures[id].used) return;
    Texture* texture = &textures[id];
    defer(texture->memory, texture->memory_size, texture->memory_flags, -1);
    if (texture->render_target) {
        defer(texture->depth_memory, texture->depth_memory_size, texture->depth_memory_flags, -1);
    }
    /* The slot is reused only after the frames that may read it are done. */
    defer(NULL, 0, 0, id);
    texture->memory = NULL;
    texture->depth_memory = NULL;
}

bool siglus_gpu_texture_read(int32_t id, uint8_t* rgba) {
    if (!recording || id < 0 || id >= MaxTextures || !textures[id].used) return false;
    Texture* texture = &textures[id];
    const uint32_t size = texture->width * texture->height * 4;
    const uint32_t staging_flags = DkMemBlockFlags_CpuCached | DkMemBlockFlags_GpuCached;
    const uint32_t staging_size = align_up(size, DK_MEMBLOCK_ALIGNMENT);
    DkMemBlock staging = make_memory(staging_size, staging_flags);
    dkCmdBufBarrier(current->cmdbuf, DkBarrier_Fragments, DkInvalidateFlags_Image | DkInvalidateFlags_L2Cache);
    DkImageView view;
    dkImageViewDefaults(&view, &texture->image);
    const DkImageRect rect = { 0, 0, 0, texture->width, texture->height, 1 };
    const DkCopyBuf dst = { dkMemBlockGetGpuAddr(staging), 0, 0 };
    dkCmdBufCopyImageToBuffer(current->cmdbuf, &view, &rect, &dst, 0);
    submit_recorded();
    dkQueueWaitIdle(queue);
    dkMemBlockFlushCpuCache(staging, 0, size);
    memcpy(rgba, dkMemBlockGetCpuAddr(staging), size);
    if (memblock_pool_count < MAX_MEMBLOCK_POOL) {
        memblock_pool[memblock_pool_count++] = (MemBlockPoolEntry) { staging, staging_size, staging_flags };
    } else {
        dkMemBlockDestroy(staging);
    }
    return true;
}

bool siglus_gpu_begin_frame(void) {
    FrameSlot* slot = &slots[frame_number % FrameSlots];
    if (slot->fence_pending) {
        /* Ryujinx can spend hundreds of milliseconds compiling a pipeline;
         * one second avoids treating that as a wedged queue. */
        const uint64_t t0 = armGetSystemTick();
        const DkResult result = dkFenceWait(&slot->fence, 1000 * 1000 * 1000LL);
        total_fence_wait_ticks += (armGetSystemTick() - t0);
        if (result != DkResult_Success) {
            log_result("frame fence wait", (int) result);
            return false;
        }
        slot->fence_pending = false;
    }
    release_deferred(slot);
    slot->ring_used = 0;
    current = slot;
    display_slot = -1;
    start_recording();
    return true;
}

/* dkQueueAcquireImage treats nwindowDequeueBuffer failure as fatal, and
 * Ryujinx fails it transiently while its window is resized or focused:
 * acquire as deko3d does, keeping that failure recoverable. */
static bool acquire_display(void) {
    int slot = -1;
    DkFence acquire_fence = {0};
    *(uint32_t*) acquire_fence._storage = 2; /* deko3d Fence::Status_Waiting */
    NvMultiFence* const nv_fence = (NvMultiFence*) (void*) (acquire_fence._storage + sizeof(uint32_t));
    const uint64_t t0 = armGetSystemTick();
    const Result result = nwindowDequeueBuffer(window, &slot, nv_fence);
    total_acquire_ticks += (armGetSystemTick() - t0);
    if (R_FAILED(result) || slot < 0 || slot >= FramebufferCount) {
        if (acquire_failures++ == 0) log_result("display acquire failed", (int) result);
        return false;
    }
    acquire_failures = 0;
    /* Work recorded so far (offscreen passes) goes first; the display
     * pass waits for the compositor to release the image. */
    submit_recorded();
    dkQueueWaitFence(queue, &acquire_fence);
    display_slot = slot;
    return true;
}

void siglus_gpu_begin_pass(int32_t target, const float* clear_color, bool clear_depth, int32_t clear_stencil) {
    if (!recording) return;
    DkCmdBuf cmdbuf = current->cmdbuf;
    DkImageView color_view;
    DkImageView depth_view;
    uint32_t width, height;
    pass_skipped = false;
    if (target < 0) {
        if (display_slot < 0 && !acquire_display()) {
            /* The compositor keeps its images; this frame is not shown. */
            pass_skipped = true;
            return;
        }
        dkImageViewDefaults(&color_view, &framebuffers[display_slot]);
        dkImageViewDefaults(&depth_view, &display_depth);
        width = DisplayWidth;
        height = DisplayHeight;
    } else {
        if (target >= MaxTextures || !textures[target].used || !textures[target].render_target) {
            pass_skipped = true;
            return;
        }
        dkImageViewDefaults(&color_view, &textures[target].image);
        dkImageViewDefaults(&depth_view, &textures[target].depth);
        width = textures[target].width;
        height = textures[target].height;
    }
    /* Earlier passes' targets and uploads are read from here on; new
     * texture descriptors (written by CPU to CpuUncached|GpuCached memory)
     * require L2 invalidation when textures_dirty is set. */
    dkCmdBufBarrier(cmdbuf, DkBarrier_Full,
                    DkInvalidateFlags_Image | DkInvalidateFlags_Descriptors |
                        (textures_dirty ? DkInvalidateFlags_L2Cache : 0));
    textures_dirty = false;
    dkCmdBufBindRenderTarget(cmdbuf, &color_view, &depth_view);
    const DkScissor scissor = { 0, 0, width, height };
    dkCmdBufSetScissors(cmdbuf, 0, &scissor, 1);
    if (clear_color) {
        dkCmdBufClearColorFloat(cmdbuf, 0, DkColorMask_RGBA, clear_color[0], clear_color[1],
                                clear_color[2], clear_color[3]);
    }
    if (clear_depth || clear_stencil >= 0) {
        dkCmdBufClearDepthStencil(cmdbuf, clear_depth, 1.0f, clear_stencil >= 0 ? 0xff : 0,
                                  clear_stencil >= 0 ? (uint8_t) clear_stencil : 0);
    }
}

void siglus_gpu_clear_stencil(uint8_t value) {
    if (!recording || pass_skipped) return;
    dkCmdBufClearDepthStencil(current->cmdbuf, false, 1.0f, 0xff, value);
}

void siglus_gpu_draw(const SiglusGpuDraw* d) {
    if (!recording || pass_skipped || d->vertex_count == 0 || d->vertex_program == 0 || d->fragment_program == 0 ||
        d->vertex_program > program_count || d->fragment_program > program_count) {
        return;
    }
    const uint64_t t0 = armGetSystemTick();
    DkCmdBuf cmdbuf = current->cmdbuf;
    if (textures_dirty) {
        /* A texture was created or uploaded inside this pass (copies run
         * on their own engine): finish them and drop stale cache lines. */
        dkCmdBufBarrier(cmdbuf, DkBarrier_Full,
                        DkInvalidateFlags_Image | DkInvalidateFlags_Descriptors | DkInvalidateFlags_L2Cache);
        textures_dirty = false;
    }
    if (!current_draw_state.valid ||
        current_draw_state.vertex_program != d->vertex_program ||
        current_draw_state.fragment_program != d->fragment_program) {
        const DkShader* shaders[2] = { &programs[d->vertex_program - 1], &programs[d->fragment_program - 1] };
        dkCmdBufBindShaders(cmdbuf, DkStageFlag_GraphicsMask, shaders, 2);
        current_draw_state.vertex_program = d->vertex_program;
        current_draw_state.fragment_program = d->fragment_program;
    }

    if (!current_draw_state.valid ||
        current_draw_state.cull_mode != d->cull_mode ||
        current_draw_state.front_face != d->front_face) {
        DkRasterizerState rasterizer;
        dkRasterizerStateDefaults(&rasterizer);
        rasterizer.cullMode = (DkFace) d->cull_mode;
        rasterizer.frontFace = (DkFrontFace) d->front_face;
        dkCmdBufBindRasterizerState(cmdbuf, &rasterizer);
        current_draw_state.cull_mode = d->cull_mode;
        current_draw_state.front_face = d->front_face;
    }

    if (!current_draw_state.valid ||
        current_draw_state.blend_enable != d->blend_enable) {
        DkColorState color_state;
        dkColorStateDefaults(&color_state);
        color_state.blendEnableMask = d->blend_enable ? 1 : 0;
        dkCmdBufBindColorState(cmdbuf, &color_state);
        current_draw_state.blend_enable = d->blend_enable;
    }

    if (!current_draw_state.valid ||
        current_draw_state.color_write_mask != d->color_write_mask) {
        DkColorWriteState color_write;
        dkColorWriteStateDefaults(&color_write);
        dkColorWriteStateSetMask(&color_write, 0, d->color_write_mask);
        dkCmdBufBindColorWriteState(cmdbuf, &color_write);
        current_draw_state.color_write_mask = d->color_write_mask;
    }

    if (d->blend_enable) {
        if (!current_draw_state.valid ||
            current_draw_state.color_op != d->color_op ||
            current_draw_state.color_src != d->color_src ||
            current_draw_state.color_dst != d->color_dst ||
            current_draw_state.alpha_op != d->alpha_op ||
            current_draw_state.alpha_src != d->alpha_src ||
            current_draw_state.alpha_dst != d->alpha_dst) {
            DkBlendState blend;
            dkBlendStateDefaults(&blend);
            blend.colorBlendOp = (DkBlendOp) d->color_op;
            blend.srcColorBlendFactor = (DkBlendFactor) d->color_src;
            blend.dstColorBlendFactor = (DkBlendFactor) d->color_dst;
            blend.alphaBlendOp = (DkBlendOp) d->alpha_op;
            blend.srcAlphaBlendFactor = (DkBlendFactor) d->alpha_src;
            blend.dstAlphaBlendFactor = (DkBlendFactor) d->alpha_dst;
            dkCmdBufBindBlendState(cmdbuf, 0, &blend);
            current_draw_state.color_op = d->color_op;
            current_draw_state.color_src = d->color_src;
            current_draw_state.color_dst = d->color_dst;
            current_draw_state.alpha_op = d->alpha_op;
            current_draw_state.alpha_src = d->alpha_src;
            current_draw_state.alpha_dst = d->alpha_dst;
        }
    }

    if (!current_draw_state.valid ||
        current_draw_state.depth_test != d->depth_test ||
        current_draw_state.depth_write != d->depth_write ||
        current_draw_state.depth_compare != d->depth_compare ||
        current_draw_state.stencil_enable != d->stencil_enable ||
        current_draw_state.stencil_compare != d->stencil_compare ||
        current_draw_state.stencil_fail != d->stencil_fail ||
        current_draw_state.stencil_depth_fail != d->stencil_depth_fail ||
        current_draw_state.stencil_pass != d->stencil_pass) {
        DkDepthStencilState depth;
        dkDepthStencilStateDefaults(&depth);
        depth.depthTestEnable = d->depth_test;
        depth.depthWriteEnable = d->depth_write;
        depth.depthCompareOp = (DkCompareOp) d->depth_compare;
        depth.stencilTestEnable = d->stencil_enable;
        depth.stencilFrontCompareOp = depth.stencilBackCompareOp = (DkCompareOp) d->stencil_compare;
        depth.stencilFrontFailOp = depth.stencilBackFailOp = (DkStencilOp) d->stencil_fail;
        depth.stencilFrontDepthFailOp = depth.stencilBackDepthFailOp = (DkStencilOp) d->stencil_depth_fail;
        depth.stencilFrontPassOp = depth.stencilBackPassOp = (DkStencilOp) d->stencil_pass;
        dkCmdBufBindDepthStencilState(cmdbuf, &depth);
        current_draw_state.depth_test = d->depth_test;
        current_draw_state.depth_write = d->depth_write;
        current_draw_state.depth_compare = d->depth_compare;
        current_draw_state.stencil_enable = d->stencil_enable;
        current_draw_state.stencil_compare = d->stencil_compare;
        current_draw_state.stencil_fail = d->stencil_fail;
        current_draw_state.stencil_depth_fail = d->stencil_depth_fail;
        current_draw_state.stencil_pass = d->stencil_pass;
    }
    if (d->stencil_enable) {
        if (!current_draw_state.valid || current_draw_state.stencil_ref != d->stencil_ref) {
            dkCmdBufSetStencil(cmdbuf, DkFace_FrontAndBack, 0xff, d->stencil_ref, 0xff);
            current_draw_state.stencil_ref = d->stencil_ref;
        }
    }

    /* Attributes by location; unused locations in between read zero. */
    DkVtxAttribState attribs[SiglusGpuMaxVertexAttribs];
    uint32_t attrib_count = 0;
    for (uint32_t i = 0; i < SiglusGpuMaxVertexAttribs; ++i) {
        if (d->attribs[i].components) attrib_count = i + 1;
    }
    static const DkVtxAttribSize sizes[5] = { DkVtxAttribSize_1x32, DkVtxAttribSize_1x32, DkVtxAttribSize_2x32,
                                              DkVtxAttribSize_3x32, DkVtxAttribSize_4x32 };
    for (uint32_t i = 0; i < attrib_count; ++i) {
        memset(&attribs[i], 0, sizeof(attribs[i]));
        const uint32_t components = d->attribs[i].components;
        attribs[i].bufferId = 0;
        attribs[i].isFixed = components == 0;
        attribs[i].offset = components ? d->attribs[i].offset : 0;
        attribs[i].size = sizes[components > 4 ? 4 : components];
        attribs[i].type = DkVtxAttribType_Float;
    }
    if (!current_draw_state.valid ||
        current_draw_state.attrib_count != attrib_count ||
        memcmp(current_draw_state.attribs, attribs, attrib_count * sizeof(DkVtxAttribState)) != 0) {
        dkCmdBufBindVtxAttribState(cmdbuf, attribs, attrib_count);
        current_draw_state.attrib_count = attrib_count;
        memcpy(current_draw_state.attribs, attribs, attrib_count * sizeof(DkVtxAttribState));
    }
    if (!current_draw_state.valid || current_draw_state.stride != d->stride) {
        const DkVtxBufferState buffer_state = { d->stride, 0 };
        dkCmdBufBindVtxBufferState(cmdbuf, &buffer_state, 1);
        current_draw_state.stride = d->stride;
    }
    current_draw_state.valid = true;
    const uint32_t vertex_bytes = d->vertex_count * d->stride;
    void* cpu = NULL;
    const DkGpuAddr vertices = ring_alloc(vertex_bytes, 16, &cpu);
    if (vertices == 0) {
        static bool logged;
        if (!logged) siglus_switch_log_message("siglus_switch: gpu frame ring full; draws skipped\n");
        logged = true;
        return;
    }
    memcpy(cpu, d->vertices, vertex_bytes);
    dkCmdBufBindVtxBuffer(cmdbuf, 0, vertices, vertex_bytes);

    DkResHandle handles[SiglusGpuMaxTextureUnits];
    for (uint32_t i = 0; i < SiglusGpuMaxTextureUnits; ++i) {
        int32_t texture = d->textures[i];
        if (texture < 0 || texture >= MaxTextures || !textures[texture].used) texture = 0;
        handles[i] = dkMakeTextureHandle((uint32_t) texture, d->samplers[i]);
    }
    dkCmdBufBindTextures(cmdbuf, DkStage_Fragment, 0, handles, SiglusGpuMaxTextureUnits);

    for (int stage = 0; stage < 2; ++stage) {
        const SiglusGpuUniform* uniforms = stage == 0 ? d->vertex_uniforms : d->fragment_uniforms;
        for (uint32_t i = 0; i < SiglusGpuMaxUniformBlocks; ++i) {
            if (!uniforms[i].data || !uniforms[i].size) continue;
            const uint32_t size = align_up(uniforms[i].size, DK_UNIFORM_BUF_ALIGNMENT);
            void* dst = NULL;
            const DkGpuAddr addr = ring_alloc(size, DK_UNIFORM_BUF_ALIGNMENT, &dst);
            if (addr == 0) return;
            memcpy(dst, uniforms[i].data, uniforms[i].size);
            dkCmdBufBindUniformBuffer(cmdbuf, stage == 0 ? DkStage_Vertex : DkStage_Fragment, i, addr, size);
        }
    }

    const DkViewport viewport = { d->viewport[0], d->viewport[1], d->viewport[2], d->viewport[3], 0.0f, 1.0f };
    dkCmdBufSetViewports(cmdbuf, 0, &viewport, 1);
    const DkScissor scissor = {
        (uint32_t) (d->scissor[0] < 0 ? 0 : d->scissor[0]), (uint32_t) (d->scissor[1] < 0 ? 0 : d->scissor[1]),
        (uint32_t) (d->scissor[2] < 0 ? 0 : d->scissor[2]), (uint32_t) (d->scissor[3] < 0 ? 0 : d->scissor[3]),
    };
    dkCmdBufSetScissors(cmdbuf, 0, &scissor, 1);
    dkCmdBufDraw(cmdbuf, DkPrimitive_Triangles, d->vertex_count, 1, 0, 0);
    total_draws++;
    total_draw_ticks += (armGetSystemTick() - t0);
}

void siglus_gpu_end_frame(bool present) {
    if (!recording) return;
    submit_recorded();
    dkQueueSignalFence(queue, &current->fence, true);
    current->fence_pending = true;
    if (present && display_slot >= 0) {
        dkQueuePresentImage(queue, swapchain, display_slot);
    } else {
        dkQueueFlush(queue);
    }
    recording = false;
    frame_number++;
}
