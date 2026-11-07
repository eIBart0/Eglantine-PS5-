namespace {
// GPU execution time (dev_vulkan.h gpu_time; in a release, the testing switch "GPU thread
// profile"): timestamps at the start and end of each upload+graphics submission, summed once the
// GPU has run it and printed every 5 s as EDEN_GPU_TIME. Inside a submission the core marks where
// a kind of work starts (video_core/renderer_vulkan/vk_gpu_time_marks.h: draws, task/mesh draws,
// compute by shader, the emulator's passes, clears, the way to the screen), and EDEN_GPU_KINDS says
// how long each took, the uploads that run before them included; EDEN_GPU_COMPUTE names the
// compute shaders that took longest. Probe/counters are worker-thread-only. Origin is captured on
// the recording thread, then copied into the submission command.
using ::Eden::VulkanProbe::GpuSubmitOrigin;
using ::Eden::VulkanProbe::gpu_submit_origin;
using ::Eden::VulkanProbe::GpuSubmitOriginScope;
struct GpuTimeProbe {
    static constexpr u32 SLOTS = 512;
    // A submission's queries: its start (upload command buffer), the start of its graphics
    // command buffer, its marks, then its end in the last one.
    static constexpr u32 BLOCK = 128;
    static constexpr u32 MARKS = BLOCK - 3;
    // The kinds of GpuTimeMarks, then the uploads.
    static constexpr u32 KINDS = GpuTimeMarks::Count + 1;
    static constexpr u32 UPLOAD = GpuTimeMarks::Count;
    vk::QueryPool pool;
    PFN_vkCmdWriteTimestamp write{};
    double period_ns{};
    u32 valid_bits{};
    u64 next{}, first_pending{};
    u32 current{};
    u64 busy_ns{}, max_ns{}, submissions{}, total_ns{}, total_submissions{};
    std::chrono::steady_clock::time_point report{};
    bool failed{};
    struct OriginCount { GpuSubmitOrigin origin; u64 count{}; };
    std::array<OriginCount, 64> origins{};
    u32 origin_slots{};
    u64 origin_overflow{};
    // The submission being recorded: the kind of work running and its marks so far.
    u32 kind{};
    u64 key{};
    u32 marks{};
    // Per submission in flight: what each part between two of its timestamps was.
    struct Part { u32 kind; u64 key; };
    std::array<std::array<Part, MARKS + 1>, SLOTS> parts{};
    std::array<u32, SLOTS> mark_count{};
    std::array<u64, KINDS> kind_ns{};
    u64 dropped_marks{};
    struct ShaderTime { u64 key; u64 ns; };
    std::array<ShaderTime, 256> shaders{};
    u32 shader_slots{};
    u64 shader_overflow_ns{};
} gpu_time;

void GpuTimeCountOrigin(GpuSubmitOrigin origin) {
    if (!::Eden::DevVulkan::gpu_time || gpu_time.failed) return;
    for (u32 i = 0; i < gpu_time.origin_slots; ++i) {
        auto& row = gpu_time.origins[i];
        if (row.origin.caller == origin.caller && row.origin.finish == origin.finish) {
            ++row.count;
            return;
        }
    }
    if (gpu_time.origin_slots == gpu_time.origins.size()) { ++gpu_time.origin_overflow; return; }
    gpu_time.origins[gpu_time.origin_slots++] = {origin, 1};
}

// A mark from the core (GpuTimeMarks::Mark, run on this worker thread): the work recorded after it
// is of `kind` (for compute, of the shader `key`).
void GpuTimeMarkWrite(VkCommandBuffer cmdbuf, u32 kind, u64 key) {
    if (!::Eden::DevVulkan::gpu_time || gpu_time.failed || !gpu_time.write) return;
    if (kind == gpu_time.kind && key == gpu_time.key) return;
    if (gpu_time.marks >= GpuTimeProbe::MARKS) {
        ++gpu_time.dropped_marks;
        return;
    }
    gpu_time.parts[gpu_time.current][gpu_time.marks] = {gpu_time.kind, gpu_time.key};
    gpu_time.write(cmdbuf, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, *gpu_time.pool,
                   gpu_time.current * GpuTimeProbe::BLOCK + 2 + gpu_time.marks);
    ++gpu_time.marks;
    gpu_time.kind = kind;
    gpu_time.key = key;
}

void GpuTimeBegin(const Device& device, vk::CommandBuffer upload_cmdbuf, vk::CommandBuffer cmdbuf) {
    if (!::Eden::DevVulkan::gpu_time || gpu_time.failed) return;
    if (!gpu_time.write) {
        const auto& dld = device.GetDispatchLoader();
        gpu_time.write = reinterpret_cast<PFN_vkCmdWriteTimestamp>(
            dld.vkGetDeviceProcAddr(*device.GetLogical(), "vkCmdWriteTimestamp"));
        gpu_time.period_ns = device.GetPhysical().GetProperties().limits.timestampPeriod;
        const auto families = device.GetPhysical().GetQueueFamilyProperties();
        if (device.GetGraphicsFamily() < families.size())
            gpu_time.valid_bits = families[device.GetGraphicsFamily()].timestampValidBits;
        if (!gpu_time.write || gpu_time.period_ns <= 0 || !gpu_time.valid_bits || gpu_time.valid_bits > 64) { gpu_time.failed = true; return; }
        gpu_time.pool = device.GetLogical().CreateQueryPool({
            .sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .queryType = VK_QUERY_TYPE_TIMESTAMP,
            .queryCount = GpuTimeProbe::SLOTS * GpuTimeProbe::BLOCK,
            .pipelineStatistics = 0,
        });
        gpu_time.report = std::chrono::steady_clock::now();
        std::printf("EDEN_GPU_TIME_INIT period_ns=%.9f valid_bits=%u slots=%u marks=%u\n",
                    gpu_time.period_ns, gpu_time.valid_bits, GpuTimeProbe::SLOTS, GpuTimeProbe::MARKS);
        std::printf("EDEN_GPU_SUBMIT_ANCHOR address=%p coverage=upload+graphics\n",
                    reinterpret_cast<void*>(&GpuTimeCountOrigin));
        GpuTimeMarks::write = &GpuTimeMarkWrite;
        GpuTimeMarks::active.store(true, std::memory_order_relaxed);
    }
    if (gpu_time.next - gpu_time.first_pending >= GpuTimeProbe::SLOTS) {
        gpu_time.failed = true; // results stopped arriving; never overwrite a pending slot
        GpuTimeMarks::active.store(false, std::memory_order_relaxed);
        std::printf("EDEN_GPU_TIME failed=ring\n");
        return;
    }
    gpu_time.current = static_cast<u32>(gpu_time.next++ % GpuTimeProbe::SLOTS);
    const u32 base = gpu_time.current * GpuTimeProbe::BLOCK;
    upload_cmdbuf.ResetQueryPool(*gpu_time.pool, base, GpuTimeProbe::BLOCK);
    gpu_time.write(*upload_cmdbuf, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, *gpu_time.pool, base);
    // The uploads end where the graphics command buffer starts.
    gpu_time.write(*cmdbuf, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, *gpu_time.pool, base + 1);
    gpu_time.kind = GpuTimeMarks::Other;
    gpu_time.key = 0;
    gpu_time.marks = 0;
}

void GpuTimeEnd(vk::CommandBuffer cmdbuf) {
    if (!::Eden::DevVulkan::gpu_time || gpu_time.failed || !gpu_time.write) return;
    gpu_time.parts[gpu_time.current][gpu_time.marks] = {gpu_time.kind, gpu_time.key};
    gpu_time.mark_count[gpu_time.current] = gpu_time.marks;
    gpu_time.write(*cmdbuf, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, *gpu_time.pool,
                   gpu_time.current * GpuTimeProbe::BLOCK + GpuTimeProbe::BLOCK - 1);
}

void GpuTimeAddShader(u64 key, u64 ns) {
    for (u32 i = 0; i < gpu_time.shader_slots; ++i) {
        if (gpu_time.shaders[i].key == key) {
            gpu_time.shaders[i].ns += ns;
            return;
        }
    }
    if (gpu_time.shader_slots == gpu_time.shaders.size()) {
        gpu_time.shader_overflow_ns += ns;
        return;
    }
    gpu_time.shaders[gpu_time.shader_slots++] = {key, ns};
}

void GpuTimeCollect(const Device& device) {
    if (!::Eden::DevVulkan::gpu_time || gpu_time.failed || !gpu_time.write) return;
    const u64 mask = gpu_time.valid_bits == 64 ? ~u64{0} : (u64{1} << gpu_time.valid_bits) - 1;
    const auto span_ns = [&](u64 from, u64 to) {
        return static_cast<u64>(double((to - from) & mask) * gpu_time.period_ns);
    };
    while (gpu_time.first_pending + 1 < gpu_time.next) {
        const u32 slot = static_cast<u32>(gpu_time.first_pending % GpuTimeProbe::SLOTS);
        const u32 base = slot * GpuTimeProbe::BLOCK;
        const u32 marks = gpu_time.mark_count[slot];
        std::array<u64, GpuTimeProbe::BLOCK> stamps{};
        if (device.GetLogical().GetQueryResults(*gpu_time.pool, base, 2 + marks, (2 + marks) * sizeof(u64),
                                               stamps.data(), sizeof(u64), VK_QUERY_RESULT_64_BIT) != VK_SUCCESS ||
            device.GetLogical().GetQueryResults(*gpu_time.pool, base + GpuTimeProbe::BLOCK - 1, 1, sizeof(u64),
                                               &stamps[2 + marks], sizeof(u64), VK_QUERY_RESULT_64_BIT) != VK_SUCCESS) break;
        const u64 ns = span_ns(stamps[0], stamps[2 + marks]);
        gpu_time.busy_ns += ns;
        gpu_time.total_ns += ns;
        ++gpu_time.total_submissions;
        gpu_time.max_ns = (std::max)(gpu_time.max_ns, ns);
        ++gpu_time.submissions;
        gpu_time.kind_ns[GpuTimeProbe::UPLOAD] += span_ns(stamps[0], stamps[1]);
        for (u32 part = 0; part <= marks; ++part) {
            const auto& what = gpu_time.parts[slot][part];
            const u64 part_ns = span_ns(stamps[1 + part], stamps[2 + part]);
            gpu_time.kind_ns[what.kind < GpuTimeMarks::Count ? what.kind : GpuTimeMarks::Other] += part_ns;
            if (what.kind == GpuTimeMarks::Compute) GpuTimeAddShader(what.key, part_ns);
        }
        ++gpu_time.first_pending;
    }
    const auto now = std::chrono::steady_clock::now();
    const auto wall = std::chrono::duration_cast<std::chrono::nanoseconds>(now - gpu_time.report).count();
    if (wall >= 5'000'000'000) {
        std::printf("EDEN_GPU_TIME wall_ms=%.1f busy_ms=%.1f submissions=%llu max_ms=%.2f cumulative_ns=%llu cumulative_submissions=%llu pending=%llu\n", wall / 1e6,
                    gpu_time.busy_ns / 1e6, static_cast<unsigned long long>(gpu_time.submissions),
                    gpu_time.max_ns / 1e6, static_cast<unsigned long long>(gpu_time.total_ns),
                    static_cast<unsigned long long>(gpu_time.total_submissions),
                    static_cast<unsigned long long>(gpu_time.next - gpu_time.first_pending));
        const auto& k = gpu_time.kind_ns;
        std::printf("EDEN_GPU_KINDS wall_ms=%.1f uploads=%.1f other=%.1f draws=%.1f mesh_draws=%.1f compute=%.1f "
                    "passes=%.1f clears=%.1f present=%.1f dropped_marks=%llu\n",
                    wall / 1e6, k[GpuTimeProbe::UPLOAD] / 1e6, k[GpuTimeMarks::Other] / 1e6,
                    k[GpuTimeMarks::Draw] / 1e6, k[GpuTimeMarks::MeshDraw] / 1e6, k[GpuTimeMarks::Compute] / 1e6,
                    k[GpuTimeMarks::Pass] / 1e6, k[GpuTimeMarks::Clear] / 1e6, k[GpuTimeMarks::Present] / 1e6,
                    static_cast<unsigned long long>(gpu_time.dropped_marks));
        if (gpu_time.shader_slots) {
            std::sort(gpu_time.shaders.begin(), gpu_time.shaders.begin() + gpu_time.shader_slots,
                      [](const auto& a, const auto& b) { return a.ns > b.ns; });
            char line[640];
            int used = std::snprintf(line, sizeof(line), "EDEN_GPU_COMPUTE shaders=%u", gpu_time.shader_slots);
            for (u32 i = 0; i < gpu_time.shader_slots && i < 8 && used > 0 && used < int(sizeof(line)); ++i)
                used += std::snprintf(line + used, sizeof(line) - used, " %016llx=%.1fms",
                                      static_cast<unsigned long long>(gpu_time.shaders[i].key),
                                      gpu_time.shaders[i].ns / 1e6);
            std::printf("%s overflow_ms=%.1f\n", line, gpu_time.shader_overflow_ns / 1e6);
        }
        gpu_time.kind_ns = {};
        gpu_time.dropped_marks = 0;
        gpu_time.shader_slots = 0;
        gpu_time.shader_overflow_ns = 0;
        gpu_time.busy_ns = gpu_time.max_ns = gpu_time.submissions = 0;
        u64 origin_total = gpu_time.origin_overflow;
        for (u32 i = 0; i < gpu_time.origin_slots; ++i) {
            auto& row = gpu_time.origins[i];
            origin_total += row.count;
            if (row.count) std::printf("EDEN_GPU_SUBMIT_ORIGIN caller=%p finish=%d count=%llu\n",
                row.origin.caller, int(row.origin.finish), static_cast<unsigned long long>(row.count));
            row.count = 0;
        }
        std::printf("EDEN_GPU_SUBMIT_TOTAL count=%llu overflow=%llu\n",
                    static_cast<unsigned long long>(origin_total),
                    static_cast<unsigned long long>(gpu_time.origin_overflow));
        gpu_time.origin_overflow = 0;
        gpu_time.report = now;
    }
}
void GpuTimeReset(const Device& device) {
    GpuTimeMarks::active.store(false, std::memory_order_relaxed);
    GpuTimeMarks::write = nullptr;
    if (gpu_time.pool) {
        const auto result = device.GetDispatchLoader().vkDeviceWaitIdle(*device.GetLogical());
        if (result != VK_SUCCESS) std::fprintf(stderr, "EDEN_GPU_TIME reset_idle_result=%d\n", int(result));
    }
    // Called only after the scheduler worker joined, while its device is alive. In place: the
    // probe holds about 1 MiB, too much for a temporary on a thread's stack.
    std::destroy_at(&gpu_time);
    std::construct_at(&gpu_time);
}
} // namespace
