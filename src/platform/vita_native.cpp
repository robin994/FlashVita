#include "vita_native.h"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <malloc.h>
#include <new>
#include <vitaGL.h>

#include <psp2/audioout.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/libssl.h>
#include <psp2/kernel/clib.h>
#include <psp2/kernel/cpu.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/threadmgr/semaphore.h>
#include <psp2/kernel/threadmgr/thread.h>
#include <psp2/net/http.h>
#include <psp2/net/net.h>
#include <psp2/net/netctl.h>
#include <psp2/sysmodule.h>

extern "C" unsigned int _get_vita_heap_size(void);
namespace flashvita::vita {

bool httpFetchToFile(const char* url,
                     int32_t method,
                     const uint8_t* body,
                     size_t body_len,
                     const char* content_type,
                     const char* destination,
                     int32_t* http_status);

namespace {

constexpr int kWorkerCount = 2;
constexpr SceSize kWorkerStackSize = 64 * 1024;
constexpr uint32_t kAudioFramesPerBuffer = 512;
constexpr uint32_t kAudioChannels = 2;
constexpr uint32_t kAudioSampleRate = 48000;
constexpr SceSize kAudioThreadStackSize = 256 * 1024;
constexpr size_t kRustNewlibEmergencyReserve = 8 * 1024 * 1024;
constexpr size_t kRustNewlibSpillHeadroom = 32 * 1024 * 1024;
constexpr size_t kVramEmergencyReserve = 32 * 1024 * 1024;
constexpr char kRuntimeLogPath[] = "ux0:data/FlashVita/runtime.log";
constexpr int kNetPoolSize = 2 * 1024 * 1024;
constexpr unsigned kHttpPoolSize = 2 * 1024 * 1024;
constexpr unsigned kSslPoolSize = 2 * 1024 * 1024;
constexpr uint64_t kMaxHttpDownloadBytes = 128ULL * 1024ULL * 1024ULL;
constexpr size_t kLogSlotCount = 128;
constexpr size_t kLogSlotBytes = 512;
constexpr SceSize kLogThreadStackSize = 64 * 1024;
constexpr SceSize kHttpThreadStackSize = 256 * 1024;

struct NativeWorkerPool;

struct WorkerContext {
    NativeWorkerPool* pool = nullptr;
    uint32_t index = 0;
};

struct NativeWorkerPool {
    SceUID threads[kWorkerCount] = {-1, -1};
    SceUID work_semas[kWorkerCount] = {-1, -1};
    SceUID done_sema = -1;
    WorkerContext contexts[kWorkerCount]{};
    bool started[kWorkerCount]{};

    ParallelCallback callback = nullptr;
    void* user = nullptr;
    uint32_t begin[kWorkerCount]{};
    uint32_t end[kWorkerCount]{};
    bool stopping = false;
    bool initialized = false;
};

NativeWorkerPool g_pool;
SceUID g_log_thread = -1;
SceUID g_log_sema = -1;
std::atomic<bool> g_log_thread_started{false};
std::atomic<bool> g_log_stopping{false};
std::atomic_flag g_log_lock = ATOMIC_FLAG_INIT;
char g_log_slots[kLogSlotCount][kLogSlotBytes]{};
uint16_t g_log_lengths[kLogSlotCount]{};
size_t g_log_head = 0;
size_t g_log_tail = 0;
size_t g_log_count = 0;
std::atomic<uint64_t> g_log_dropped{0};
alignas(64) uint8_t g_net_memory[kNetPoolSize];
bool g_network_initialized = false;
bool g_network_attempted = false;
std::atomic<bool> g_rust_allocator_vgl_ready{false};
std::atomic<bool> g_rust_allocator_spill{false};
std::atomic<void*> g_rust_newlib_reserve{nullptr};
std::atomic<uint64_t> g_rust_spill_allocations{0};
std::atomic<uint64_t> g_rust_spill_bytes{0};
std::atomic<uint32_t> g_rust_allocator_probe_counter{0};
std::atomic<uintptr_t> g_vgl_ram_base{0};
std::atomic<uintptr_t> g_vgl_ram_end{0};
std::atomic<uintptr_t> g_vgl_phycont_base{0};
std::atomic<uintptr_t> g_vgl_phycont_end{0};
std::atomic<uintptr_t> g_newlib_memblock_base{0};
std::atomic<uintptr_t> g_newlib_memblock_end{0};
std::atomic<bool> g_phycont_spill_logged{false};
std::atomic<bool> g_vram_spill_logged{false};
std::atomic<bool> g_logging_enabled{true};
std::atomic<bool> g_perf_logging_enabled{false};
SceUID g_rust_heap_uid = -1;
SceClibMspace g_rust_heap = nullptr;
std::atomic<uintptr_t> g_rust_heap_base{0};
std::atomic<uintptr_t> g_rust_heap_end{0};
std::atomic_flag g_rust_heap_lock = ATOMIC_FLAG_INIT;

void rustHeapLock() {
    while (g_rust_heap_lock.test_and_set(std::memory_order_acquire)) {
        sceKernelDelayThread(0);
    }
}

void rustHeapUnlock() {
    g_rust_heap_lock.clear(std::memory_order_release);
}

bool isRustHeapPointer(const void* ptr) {
    if (!ptr) return false;
    const uintptr_t address = reinterpret_cast<uintptr_t>(ptr);
    const uintptr_t base = g_rust_heap_base.load(std::memory_order_acquire);
    const uintptr_t end = g_rust_heap_end.load(std::memory_order_acquire);
    return base != 0 && address >= base && address < end;
}

void logLock() {
    while (g_log_lock.test_and_set(std::memory_order_acquire)) {
        sceKernelDelayThread(0);
    }
}

void logUnlock() {
    g_log_lock.clear(std::memory_order_release);
}

bool writeAllRaw(SceUID fd, const uint8_t* data, size_t bytes) {
    size_t written = 0;
    while (written < bytes) {
        const int result = sceIoWrite(fd, data + written, bytes - written);
        if (result <= 0) return false;
        written += static_cast<size_t>(result);
    }
    return true;
}

int logThreadEntry(SceSize, void*) {
    const SceUID fd = sceIoOpen(
        kRuntimeLogPath,
        SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND,
        0666);
    if (fd < 0) return fd;

    char local[kLogSlotBytes];
    for (;;) {
        sceKernelWaitSema(g_log_sema, 1, nullptr);
        for (;;) {
            size_t length = 0;
            logLock();
            if (g_log_count != 0) {
                length = g_log_lengths[g_log_head];
                std::memcpy(local, g_log_slots[g_log_head], length);
                g_log_head = (g_log_head + 1) % kLogSlotCount;
                --g_log_count;
            }
            const bool stopping = g_log_stopping.load(std::memory_order_acquire);
            logUnlock();

            if (length != 0) {
                writeAllRaw(fd, reinterpret_cast<const uint8_t*>(local), length);
                continue;
            }
            if (stopping) {
                sceIoClose(fd);
                return 0;
            }
            break;
        }
    }
}

void startLogThread() {
    if (!g_logging_enabled.load(std::memory_order_acquire) ||
        g_log_thread_started.load(std::memory_order_acquire)) {
        return;
    }

    g_log_sema = sceKernelCreateSema("FlashVitaLog", 0, 0, kLogSlotCount, nullptr);
    if (g_log_sema < 0) return;

    const int base_priority = sceKernelGetThreadCurrentPriority();
    const int log_priority = base_priority >= 0 ? base_priority + 16 : 0x10000110;
    g_log_thread = sceKernelCreateThread(
        "FlashVitaLog",
        logThreadEntry,
        log_priority,
        kLogThreadStackSize,
        0,
        SCE_KERNEL_CPU_MASK_USER_2,
        nullptr);
    if (g_log_thread < 0) {
        sceKernelDeleteSema(g_log_sema);
        g_log_sema = -1;
        return;
    }

    g_log_stopping.store(false, std::memory_order_release);
    if (sceKernelStartThread(g_log_thread, 0, nullptr) < 0) {
        sceKernelDeleteThread(g_log_thread);
        sceKernelDeleteSema(g_log_sema);
        g_log_thread = -1;
        g_log_sema = -1;
        return;
    }
    g_log_thread_started.store(true, std::memory_order_release);
}

void stopLogThread() {
    if (!g_log_thread_started.exchange(false, std::memory_order_acq_rel)) return;
    g_log_stopping.store(true, std::memory_order_release);
    sceKernelSignalSema(g_log_sema, 1);
    sceKernelWaitThreadEnd(g_log_thread, nullptr, nullptr);
    sceKernelDeleteThread(g_log_thread);
    sceKernelDeleteSema(g_log_sema);
    g_log_thread = -1;
    g_log_sema = -1;
}

bool enqueueRuntimeLog(const void* data, size_t bytes) {
    if (!data || bytes == 0) return true;
    if (!g_logging_enabled.load(std::memory_order_relaxed)) return true;

    if (!g_log_thread_started.load(std::memory_order_acquire)) {
        const SceUID fd = sceIoOpen(
            kRuntimeLogPath,
            SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND,
            0666);
        if (fd < 0) return false;
        const bool ok = writeAllRaw(fd, static_cast<const uint8_t*>(data), bytes);
        sceIoClose(fd);
        return ok;
    }

    const uint8_t* input = static_cast<const uint8_t*>(data);
    while (bytes != 0) {
        const size_t chunk = std::min(bytes, kLogSlotBytes - 1);
        bool queued = false;
        logLock();
        if (g_log_count < kLogSlotCount) {
            std::memcpy(g_log_slots[g_log_tail], input, chunk);
            g_log_lengths[g_log_tail] = static_cast<uint16_t>(chunk);
            g_log_tail = (g_log_tail + 1) % kLogSlotCount;
            ++g_log_count;
            queued = true;
        } else {
            g_log_dropped.fetch_add(1, std::memory_order_relaxed);
        }
        logUnlock();
        if (queued) sceKernelSignalSema(g_log_sema, 1);
        input += chunk;
        bytes -= chunk;
    }
    return true;
}

void rustAllocatorLog(const char* line) {
    if (!line || !g_logging_enabled.load(std::memory_order_relaxed)) return;
    enqueueRuntimeLog(line, std::strlen(line));
}

bool rustAllocatorShouldSpill(size_t bytes) {
    if (g_rust_allocator_spill.load(std::memory_order_acquire)) return true;
    if (!g_rust_allocator_vgl_ready.load(std::memory_order_acquire)) return false;

    const uint32_t probe = g_rust_allocator_probe_counter.fetch_add(1, std::memory_order_relaxed);
    if (bytes < 64 * 1024 && (probe & 63u) != 0) return false;

    const struct mallinfo info = mallinfo();
    const size_t total = static_cast<size_t>(_get_vita_heap_size());
    const size_t arena = info.arena > 0 ? static_cast<size_t>(info.arena) : 0;
    const size_t uncommitted = total > arena ? total - arena : 0;
    return uncommitted <= kRustNewlibSpillHeadroom;
}

bool isVglManagedPointer(const void* ptr) {
    if (!ptr) return false;
    const uintptr_t address = reinterpret_cast<uintptr_t>(ptr);

    const uintptr_t newlib_base = g_newlib_memblock_base.load(std::memory_order_acquire);
    const uintptr_t newlib_end = g_newlib_memblock_end.load(std::memory_order_acquire);
    if (newlib_base != 0 && address >= newlib_base && address < newlib_end) return false;

    const uintptr_t ram_base = g_vgl_ram_base.load(std::memory_order_acquire);
    const uintptr_t ram_end = g_vgl_ram_end.load(std::memory_order_acquire);
    if (ram_base != 0 && address >= ram_base && address < ram_end) return true;

    const uintptr_t phy_base = g_vgl_phycont_base.load(std::memory_order_acquire);
    const uintptr_t phy_end = g_vgl_phycont_end.load(std::memory_order_acquire);
    if (phy_base != 0 && address >= phy_base && address < phy_end) return true;

    if (!g_rust_allocator_vgl_ready.load(std::memory_order_acquire)) return false;

    // With PHYCONT_ON_DEMAND, every PHYCONT allocation can live in a separate
    // kernel memblock. Detect those dynamically instead of relying on the probe range.
    SceKernelMemBlockInfo info{};
    info.size = sizeof(info);
    if (sceKernelGetMemBlockInfoByAddr(const_cast<void*>(ptr), &info) < 0 ||
        !info.mappedBase || info.mappedSize == 0) {
        return false;
    }

    const uintptr_t block_base = reinterpret_cast<uintptr_t>(info.mappedBase);
    const uintptr_t block_end = block_base + info.mappedSize;
    if (newlib_base != 0 && block_base == newlib_base) return false;
    return address >= block_base && address < block_end;
}

void* allocateVglManaged(size_t bytes) {
    if (bytes == 0) bytes = 1;
    if (bytes > static_cast<size_t>(UINT32_MAX)) return nullptr;
    void* ptr = vglAlloc(static_cast<uint32_t>(bytes), VGL_MEM_RAM);
    if (!ptr) {
        ptr = vglAlloc(static_cast<uint32_t>(bytes), VGL_MEM_PHYCONT);
        if (ptr && !g_phycont_spill_logged.exchange(true, std::memory_order_acq_rel)) {
            char line[192];
            sceClibSnprintf(
                line,
                sizeof(line),
                "rust_allocator spill=phycont bytes=%u phycont_free=%llu\n",
                static_cast<unsigned>(bytes),
                static_cast<unsigned long long>(vglMemFree(VGL_MEM_PHYCONT)));
            rustAllocatorLog(line);
        }
    }
    if (!ptr) {
        const size_t vram_free = vglMemFree(VGL_MEM_VRAM);
        if (vram_free > kVramEmergencyReserve + bytes) {
            ptr = vglAlloc(static_cast<uint32_t>(bytes), VGL_MEM_VRAM);
            if (ptr && !g_vram_spill_logged.exchange(true, std::memory_order_acq_rel)) {
                char line[192];
                sceClibSnprintf(
                    line,
                    sizeof(line),
                    "rust_allocator spill=vram bytes=%u vram_free=%llu reserve=%u\n",
                    static_cast<unsigned>(bytes),
                    static_cast<unsigned long long>(vglMemFree(VGL_MEM_VRAM)),
                    static_cast<unsigned>(kVramEmergencyReserve));
                rustAllocatorLog(line);
            }
        }
    }
    if (ptr) {
        g_rust_spill_allocations.fetch_add(1, std::memory_order_relaxed);
        g_rust_spill_bytes.fetch_add(bytes, std::memory_order_relaxed);
    }
    return ptr;
}

void enterRustAllocatorSpillMode() {
    bool expected = false;
    if (!g_rust_allocator_spill.compare_exchange_strong(
            expected, true, std::memory_order_acq_rel)) {
        return;
    }

    char line[192];
    sceClibSnprintf(
        line,
        sizeof(line),
        "rust_allocator spill=vgl_pools reserve_held=%u vgl_ram_free=%llu phycont_free=%llu\n",
        static_cast<unsigned>(g_rust_newlib_reserve.load(std::memory_order_acquire)
                                  ? kRustNewlibEmergencyReserve
                                  : 0),
        static_cast<unsigned long long>(vglMemFree(VGL_MEM_RAM)),
        static_cast<unsigned long long>(vglMemFree(VGL_MEM_PHYCONT)));
    rustAllocatorLog(line);
}

void releaseRustNewlibReserve() {
    void* reserve = g_rust_newlib_reserve.exchange(nullptr, std::memory_order_acq_rel);
    if (!reserve) return;
    std::free(reserve);

    char line[160];
    sceClibSnprintf(
        line,
        sizeof(line),
        "rust_allocator newlib_reserve_released=%u\n",
        static_cast<unsigned>(kRustNewlibEmergencyReserve));
    rustAllocatorLog(line);
}

struct NativeAudioThread {
    SceUID thread = -1;
    SceUID ready_sema = -1;
    FlashVitaAudioFillCallback callback = nullptr;
    void* user = nullptr;
    std::atomic<bool> stopping{false};
    std::atomic<bool> paused{false};
    std::atomic<int32_t> open_result{-1};
    std::atomic<int32_t> last_error{0};
    std::atomic<uint32_t> buffers{0};
    alignas(64) int16_t samples[kAudioFramesPerBuffer * kAudioChannels]{};
};

struct NativeHttpJob {
    SceUID thread = -1;
    std::string url;
    std::vector<uint8_t> body;
    std::string content_type;
    std::string destination;
    int32_t method = 0;
    std::atomic<int32_t> result{0};
    std::atomic<int32_t> status{0};
};

int workerEntry(SceSize args, void* argp) {
    (void)args;
    auto* context = static_cast<WorkerContext*>(argp);
    if (!context || !context->pool) return -1;

    NativeWorkerPool& pool = *context->pool;
    const uint32_t index = context->index;

    for (;;) {
        if (sceKernelWaitSema(pool.work_semas[index], 1, nullptr) < 0) return -2;
        if (pool.stopping) break;

        ParallelCallback callback = pool.callback;
        if (callback && pool.begin[index] < pool.end[index]) {
            callback(pool.user, pool.begin[index], pool.end[index]);
        }

        if (sceKernelSignalSema(pool.done_sema, 1) < 0) return -3;
    }

    return 0;
}

int audioThreadEntry(SceSize args, void* argp) {
    (void)args;
    if (!argp) return -1;

    auto* state = *static_cast<NativeAudioThread**>(argp);
    if (!state || !state->callback) return -2;

    const int port = sceAudioOutOpenPort(
        SCE_AUDIO_OUT_PORT_TYPE_MAIN,
        static_cast<int>(kAudioFramesPerBuffer),
        static_cast<int>(kAudioSampleRate),
        SCE_AUDIO_OUT_MODE_STEREO);
    state->open_result.store(port, std::memory_order_release);
    sceKernelSignalSema(state->ready_sema, 1);
    if (port < 0) return port;

    while (!state->stopping.load(std::memory_order_acquire)) {
        if (state->paused.load(std::memory_order_relaxed)) {
            std::memset(state->samples, 0, sizeof(state->samples));
        } else {
            state->callback(state->user, state->samples, kAudioFramesPerBuffer);
        }

        const int output_result = sceAudioOutOutput(port, state->samples);
        if (output_result < 0) {
            state->last_error.store(output_result, std::memory_order_relaxed);
            break;
        }
        state->buffers.fetch_add(1, std::memory_order_relaxed);
    }

    sceAudioOutOutput(port, nullptr);
    sceAudioOutReleasePort(port);
    return 0;
}

int httpThreadEntry(SceSize args, void* argp) {
    (void)args;
    if (!argp) return -1;
    auto* job = *static_cast<NativeHttpJob**>(argp);
    if (!job) return -2;

    int32_t status = 0;
    const bool ok = httpFetchToFile(
        job->url.c_str(),
        job->method,
        job->body.empty() ? nullptr : job->body.data(),
        job->body.size(),
        job->content_type.empty() ? nullptr : job->content_type.c_str(),
        job->destination.c_str(),
        &status);
    job->status.store(status, std::memory_order_release);
    job->result.store(ok ? 1 : -1, std::memory_order_release);
    return ok ? 0 : -1;
}

void destroyPool() {
    g_pool.stopping = true;
    for (int i = 0; i < kWorkerCount; ++i) {
        if (g_pool.started[i] && g_pool.work_semas[i] >= 0) {
            sceKernelSignalSema(g_pool.work_semas[i], 1);
        }
    }

    for (int i = 0; i < kWorkerCount; ++i) {
        if (g_pool.threads[i] >= 0) {
            if (g_pool.started[i]) {
                sceKernelWaitThreadEnd(g_pool.threads[i], nullptr, nullptr);
            }
            sceKernelDeleteThread(g_pool.threads[i]);
            g_pool.threads[i] = -1;
        }
        if (g_pool.work_semas[i] >= 0) {
            sceKernelDeleteSema(g_pool.work_semas[i]);
            g_pool.work_semas[i] = -1;
        }
    }

    if (g_pool.done_sema >= 0) {
        sceKernelDeleteSema(g_pool.done_sema);
        g_pool.done_sema = -1;
    }

    g_pool = NativeWorkerPool{};
}

bool writeAll(SceUID fd, const uint8_t* data, size_t bytes) {
    size_t written = 0;
    while (written < bytes) {
        const int result = sceIoWrite(fd, data + written, bytes - written);
        if (result <= 0) return false;
        written += static_cast<size_t>(result);
    }
    return true;
}

bool ensureNetworkInitialized() {
    if (g_network_initialized) return true;
    if (g_network_attempted) return false;
    g_network_attempted = true;

    sceSysmoduleLoadModule(SCE_SYSMODULE_NET);
    sceSysmoduleLoadModule(SCE_SYSMODULE_HTTP);
    sceSysmoduleLoadModule(SCE_SYSMODULE_SSL);
    sceSysmoduleLoadModule(SCE_SYSMODULE_HTTPS);

    SceNetInitParam net{};
    net.memory = g_net_memory;
    net.size = sizeof(g_net_memory);
    net.flags = 0;
    if (sceNetInit(&net) < 0) return false;
    if (sceNetCtlInit() < 0) {
        sceNetTerm();
        return false;
    }
    if (sceHttpInit(kHttpPoolSize) < 0) {
        sceNetCtlTerm();
        sceNetTerm();
        return false;
    }
    if (sceSslInit(kSslPoolSize) < 0) {
        sceHttpTerm();
        sceNetCtlTerm();
        sceNetTerm();
        return false;
    }

    g_network_initialized = true;
    return true;
}

void shutdownNetwork() {
    if (!g_network_initialized) return;
    sceSslTerm();
    sceHttpTerm();
    sceNetCtlTerm();
    sceNetTerm();
    g_network_initialized = false;
}

bool makeParentDirectories(const std::string& file_path) {
    const size_t slash = file_path.find_last_of('/');
    if (slash == std::string::npos) return true;
    return makeDirectories(file_path.substr(0, slash));
}

} // namespace

bool initialize() {
    if (g_pool.initialized) return true;

    startLogThread();
    appendTextFile(kRuntimeLogPath, "native_init begin\n");

    g_pool.done_sema = sceKernelCreateSema("FlashVitaDone", 0, 0, kWorkerCount, nullptr);
    if (g_pool.done_sema < 0) {
        destroyPool();
        return false;
    }

    const int base_priority = sceKernelGetThreadCurrentPriority();
    const int worker_priority = base_priority >= 0 ? base_priority + 1 : 0x10000101;
    const int affinities[kWorkerCount] = {
        SCE_KERNEL_CPU_MASK_USER_1,
        SCE_KERNEL_CPU_MASK_USER_2,
    };

    for (int i = 0; i < kWorkerCount; ++i) {
        char sema_name[32];
        char thread_name[32];
        sceClibSnprintf(sema_name, sizeof(sema_name), "FlashVitaWork%d", i);
        sceClibSnprintf(thread_name, sizeof(thread_name), "FlashVitaCpu%d", i + 1);

        g_pool.work_semas[i] = sceKernelCreateSema(sema_name, 0, 0, 1, nullptr);
        if (g_pool.work_semas[i] < 0) {
            destroyPool();
            return false;
        }

        g_pool.contexts[i].pool = &g_pool;
        g_pool.contexts[i].index = static_cast<uint32_t>(i);
        g_pool.threads[i] = sceKernelCreateThread(
            thread_name,
            workerEntry,
            worker_priority,
            kWorkerStackSize,
            0,
            affinities[i],
            nullptr);
        if (g_pool.threads[i] < 0) {
            destroyPool();
            return false;
        }

        if (sceKernelStartThread(
                g_pool.threads[i],
                sizeof(g_pool.contexts[i]),
                &g_pool.contexts[i]) < 0) {
            destroyPool();
            return false;
        }
        g_pool.started[i] = true;
        char marker[64];
        const int marker_len = sceClibSnprintf(
            marker,
            sizeof(marker),
            "native_init worker%d_started\n",
            i + 1);
        if (marker_len > 0) appendFile(kRuntimeLogPath, marker, static_cast<size_t>(marker_len));
    }

    appendTextFile(kRuntimeLogPath, "native_init affinity_begin\n");
    sceKernelChangeThreadCpuAffinityMask(sceKernelGetThreadId(), SCE_KERNEL_CPU_MASK_USER_0);
    appendTextFile(kRuntimeLogPath, "native_init affinity_done\n");
    g_pool.initialized = true;
    return true;
}

void shutdown() {
    shutdownNetwork();
    destroyPool();
    stopLogThread();
}

bool parallelFor(uint32_t count, uint32_t min_grain, ParallelCallback callback, void* user) {
    if (!callback || count == 0) return false;
    if (!g_pool.initialized || count < min_grain || count < 3) return false;

    const uint32_t chunk = (count + 2) / 3;
    g_pool.callback = callback;
    g_pool.user = user;
    g_pool.begin[0] = 0;
    g_pool.end[0] = std::min(count, chunk);
    g_pool.begin[1] = g_pool.end[0];
    g_pool.end[1] = std::min(count, chunk * 2);

    int dispatched_workers = 0;
    for (int i = 0; i < kWorkerCount; ++i) {
        if (sceKernelSignalSema(g_pool.work_semas[i], 1) >= 0) {
            ++dispatched_workers;
        } else if (g_pool.begin[i] < g_pool.end[i]) {
            callback(user, g_pool.begin[i], g_pool.end[i]);
        }
    }

    const uint32_t main_begin = g_pool.end[1];
    if (main_begin < count) callback(user, main_begin, count);

    if (dispatched_workers > 0) {
        // Once work is dispatched we never fall back to rewriting the full
        // destination buffer concurrently. A kernel wait failure is reported
        // as completed to keep the caller from racing an in-flight worker.
        sceKernelWaitSema(g_pool.done_sema, dispatched_workers, nullptr);
    }
    return true;
}

bool getWorkerRuntimeStats(WorkerRuntimeStats& out) {
    bool any = false;
    for (int i = 0; i < kWorkerCount; ++i) {
        out.run_clocks[i] = 0;
        out.last_cpu[i] = -1;
        if (g_pool.threads[i] < 0) {
            continue;
        }

        SceKernelThreadInfo info{};
        info.size = sizeof(info);
        if (sceKernelGetThreadInfo(g_pool.threads[i], &info) < 0) {
            continue;
        }
        out.run_clocks[i] = info.runClocks;
        out.last_cpu[i] = info.lastExecutedCpuId;
        any = true;
    }
    return any;
}

bool readFile(const std::string& path, std::vector<uint8_t>& out, size_t max_bytes) {
    out.clear();
    const SceUID fd = sceIoOpen(path.c_str(), SCE_O_RDONLY, 0);
    if (fd < 0) return false;

    const SceOff size = sceIoLseek(fd, 0, SCE_SEEK_END);
    if (size < 0 || static_cast<uint64_t>(size) > static_cast<uint64_t>(max_bytes) ||
        sceIoLseek(fd, 0, SCE_SEEK_SET) < 0) {
        sceIoClose(fd);
        return false;
    }

    try {
        out.resize(static_cast<size_t>(size));
    } catch (...) {
        sceIoClose(fd);
        out.clear();
        return false;
    }

    size_t read_total = 0;
    while (read_total < out.size()) {
        const int result = sceIoRead(fd, out.data() + read_total, out.size() - read_total);
        if (result <= 0) {
            sceIoClose(fd);
            out.clear();
            return false;
        }
        read_total += static_cast<size_t>(result);
    }

    sceIoClose(fd);
    return true;
}

bool readFilePrefix(const std::string& path, void* dst, size_t bytes, size_t& bytes_read) {
    bytes_read = 0;
    if (!dst || bytes == 0) return false;

    const SceUID fd = sceIoOpen(path.c_str(), SCE_O_RDONLY, 0);
    if (fd < 0) return false;

    const int result = sceIoRead(fd, dst, bytes);
    sceIoClose(fd);
    if (result < 0) return false;

    bytes_read = static_cast<size_t>(result);
    return true;
}

bool readTextFile(const std::string& path, std::string& out) {
    std::vector<uint8_t> bytes;
    if (!readFile(path, bytes)) {
        out.clear();
        return false;
    }
    out.assign(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    return true;
}

bool writeFile(const std::string& path, const void* data, size_t bytes) {
    const SceUID fd = sceIoOpen(path.c_str(), SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
    if (fd < 0) return false;
    const bool ok = bytes == 0 || writeAll(fd, static_cast<const uint8_t*>(data), bytes);
    sceIoClose(fd);
    return ok;
}

bool writeTextFile(const std::string& path, const std::string& text) {
    return writeFile(path, text.data(), text.size());
}

bool appendFile(const std::string& path, const void* data, size_t bytes) {
    if (path == kRuntimeLogPath && !g_logging_enabled.load(std::memory_order_relaxed)) {
        return true;
    }
    if (path == kRuntimeLogPath) {
        return enqueueRuntimeLog(data, bytes);
    }
    const SceUID fd = sceIoOpen(path.c_str(), SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND, 0666);
    if (fd < 0) return false;
    const bool ok = bytes == 0 || writeAll(fd, static_cast<const uint8_t*>(data), bytes);
    sceIoClose(fd);
    return ok;
}

bool appendTextFile(const std::string& path, const std::string& text) {
    return appendFile(path, text.data(), text.size());
}

void setLoggingEnabled(bool enabled) {
    g_logging_enabled.store(enabled, std::memory_order_release);
}

bool loggingEnabled() {
    return g_logging_enabled.load(std::memory_order_acquire);
}

void setPerfLoggingEnabled(bool enabled) {
    g_perf_logging_enabled.store(enabled, std::memory_order_release);
}

bool perfLoggingEnabled() {
    return loggingEnabled() && g_perf_logging_enabled.load(std::memory_order_acquire);
}

bool makeDirectories(const std::string& path) {
    if (path.empty()) return false;
    std::string current;
    current.reserve(path.size());

    for (size_t i = 0; i < path.size(); ++i) {
        current.push_back(path[i]);
        if (path[i] != '/' || i == 0) continue;
        if (current.size() <= 1) continue;
        current.pop_back();
        if (!current.empty() && current.back() != ':') sceIoMkdir(current.c_str(), 0777);
        current.push_back('/');
    }
    if (!current.empty() && current.back() != ':') sceIoMkdir(current.c_str(), 0777);
    return true;
}

bool fileExists(const std::string& path) {
    SceIoStat st{};
    return sceIoGetstat(path.c_str(), &st) == 0;
}

bool httpFetchToFile(const char* url,
                     int32_t method,
                     const uint8_t* body,
                     size_t body_len,
                     const char* content_type,
                     const char* destination,
                     int32_t* http_status) {
    if (http_status) *http_status = 0;
    if (!url || !destination || !ensureNetworkInitialized()) return false;
    if (!makeParentDirectories(destination)) return false;

    const int tpl = sceHttpCreateTemplate("FlashVita/0.11", SCE_HTTP_VERSION_1_1, SCE_HTTP_PROXY_AUTO);
    if (tpl < 0) return false;
    sceHttpSetAutoRedirect(tpl, SCE_HTTP_ENABLE);
    sceHttpSetConnectTimeOut(tpl, 15 * 1000 * 1000U);
    sceHttpSetRecvTimeOut(tpl, 60 * 1000 * 1000U);
    sceHttpSetSendTimeOut(tpl, 30 * 1000 * 1000U);

    const int conn = sceHttpCreateConnectionWithURL(tpl, url, 1);
    if (conn < 0) {
        sceHttpDeleteTemplate(tpl);
        return false;
    }

    const int sce_method = method == 1 ? SCE_HTTP_METHOD_POST : SCE_HTTP_METHOD_GET;
    const int req = sceHttpCreateRequestWithURL(
        conn,
        sce_method,
        url,
        static_cast<unsigned long long>(method == 1 ? body_len : 0));
    if (req < 0) {
        sceHttpDeleteConnection(conn);
        sceHttpDeleteTemplate(tpl);
        return false;
    }

    if (content_type && content_type[0]) {
        sceHttpAddRequestHeader(req, "Content-Type", content_type, SCE_HTTP_HEADER_OVERWRITE);
    }

    const int send_result = sceHttpSendRequest(
        req,
        method == 1 ? body : nullptr,
        method == 1 ? static_cast<unsigned int>(body_len) : 0);
    if (send_result < 0) {
        sceHttpDeleteRequest(req);
        sceHttpDeleteConnection(conn);
        sceHttpDeleteTemplate(tpl);
        return false;
    }

    int status = 0;
    sceHttpGetStatusCode(req, &status);
    if (http_status) *http_status = status;
    if (status < 200 || status >= 300) {
        sceHttpAbortRequest(req);
        sceHttpDeleteRequest(req);
        sceHttpDeleteConnection(conn);
        sceHttpDeleteTemplate(tpl);
        return false;
    }

    unsigned long long content_length = 0;
    if (sceHttpGetResponseContentLength(req, &content_length) >= 0 &&
        content_length > kMaxHttpDownloadBytes) {
        sceHttpAbortRequest(req);
        sceHttpDeleteRequest(req);
        sceHttpDeleteConnection(conn);
        sceHttpDeleteTemplate(tpl);
        return false;
    }

    std::string part_path = std::string(destination) + ".part";
    const SceUID fd = sceIoOpen(
        part_path.c_str(),
        SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC,
        0666);
    if (fd < 0) {
        sceHttpAbortRequest(req);
        sceHttpDeleteRequest(req);
        sceHttpDeleteConnection(conn);
        sceHttpDeleteTemplate(tpl);
        return false;
    }

    bool ok = true;
    uint64_t total_bytes = 0;
    uint8_t buffer[16 * 1024];
    for (;;) {
        const int read = sceHttpReadData(req, buffer, sizeof(buffer));
        if (read == 0) break;
        if (read < 0 ||
            total_bytes + static_cast<uint64_t>(read) > kMaxHttpDownloadBytes ||
            !writeAll(fd, buffer, static_cast<size_t>(read))) {
            ok = false;
            break;
        }
        total_bytes += static_cast<uint64_t>(read);
    }
    sceIoClose(fd);

    if (ok) {
        sceIoRemove(destination);
        ok = sceIoRename(part_path.c_str(), destination) >= 0;
    }
    if (!ok) sceIoRemove(part_path.c_str());

    sceHttpAbortRequest(req);
    sceHttpDeleteRequest(req);
    sceHttpDeleteConnection(conn);
    sceHttpDeleteTemplate(tpl);
    return ok;
}

} // namespace flashvita::vita

extern "C" void* __wrap_malloc(size_t bytes) {
    if (bytes == 0) bytes = 1;

    const bool vgl_ready = flashvita::vita::g_rust_allocator_vgl_ready.load(std::memory_order_acquire);
    if (vgl_ready && flashvita::vita::rustAllocatorShouldSpill(bytes)) {
        flashvita::vita::enterRustAllocatorSpillMode();
    }

    if (!flashvita::vita::g_rust_allocator_spill.load(std::memory_order_acquire)) {
        if (void* ptr = std::malloc(bytes)) return ptr;
        if (!vgl_ready) return nullptr;
        flashvita::vita::enterRustAllocatorSpillMode();
    }

    if (void* ptr = flashvita::vita::allocateVglManaged(bytes)) return ptr;
    flashvita::vita::releaseRustNewlibReserve();
    return std::malloc(bytes);
}

extern "C" void* __wrap_calloc(size_t count, size_t size) {
    if (count != 0 && size > SIZE_MAX / count) return nullptr;
    const size_t bytes = count * size;

    void* ptr = __wrap_malloc(bytes == 0 ? 1 : bytes);
    if (ptr) std::memset(ptr, 0, bytes);
    return ptr;
}

extern "C" void __wrap_free(void* ptr) {
    if (!ptr) return;
    if (flashvita::vita::isVglManagedPointer(ptr)) {
        vglFree(ptr);
    } else {
        std::free(ptr);
    }
}

extern "C" void* __wrap_realloc(void* ptr, size_t new_size) {
    if (!ptr) return __wrap_malloc(new_size);
    if (new_size == 0) {
        __wrap_free(ptr);
        return nullptr;
    }

    if (!flashvita::vita::isVglManagedPointer(ptr) &&
        !flashvita::vita::g_rust_allocator_spill.load(std::memory_order_acquire)) {
        if (void* resized = std::realloc(ptr, new_size)) return resized;
        if (!flashvita::vita::g_rust_allocator_vgl_ready.load(std::memory_order_acquire)) {
            return nullptr;
        }
        flashvita::vita::enterRustAllocatorSpillMode();
    }

    const size_t old_size = flashvita::vita::isVglManagedPointer(ptr)
        ? vglMallocUsableSize(ptr)
        : malloc_usable_size(ptr);
    void* replacement = __wrap_malloc(new_size);
    if (!replacement) return nullptr;
    std::memcpy(replacement, ptr, std::min(old_size, new_size));
    __wrap_free(ptr);
    return replacement;
}

extern "C" int32_t flashvita_vita_parallel_for(uint32_t count, uint32_t min_grain,
                                                 FlashVitaParallelCallback callback, void* user) {
    return flashvita::vita::parallelFor(count, min_grain, callback, user) ? 1 : 0;
}

extern "C" void* flashvita_vita_audio_create(FlashVitaAudioFillCallback callback, void* user) {
    if (!callback) return nullptr;

    auto* state = new (std::nothrow) flashvita::vita::NativeAudioThread();
    if (!state) return nullptr;
    state->callback = callback;
    state->user = user;

    state->ready_sema = sceKernelCreateSema("FlashVitaAudioReady", 0, 0, 1, nullptr);
    if (state->ready_sema < 0) {
        delete state;
        return nullptr;
    }

    const int base_priority = sceKernelGetThreadCurrentPriority();
    // Audio must preempt transform/predecode workers to avoid underruns while
    // CPU1/CPU2 are saturated by a parallel frame job. Lower value = higher priority.
    const int audio_priority = base_priority >= 0 ? base_priority - 8 : 0x100000F8;
    const int audio_affinity = SCE_KERNEL_CPU_MASK_USER_1 | SCE_KERNEL_CPU_MASK_USER_2;

    state->thread = sceKernelCreateThread(
        "FlashVitaAudio",
        flashvita::vita::audioThreadEntry,
        audio_priority,
        flashvita::vita::kAudioThreadStackSize,
        0,
        audio_affinity,
        nullptr);
    if (state->thread < 0) {
        sceKernelDeleteSema(state->ready_sema);
        delete state;
        return nullptr;
    }

    flashvita::vita::NativeAudioThread* thread_arg = state;
    const int start_result = sceKernelStartThread(
        state->thread,
        sizeof(thread_arg),
        &thread_arg);
    if (start_result < 0) {
        sceKernelDeleteThread(state->thread);
        sceKernelDeleteSema(state->ready_sema);
        delete state;
        return nullptr;
    }

    const int wait_result = sceKernelWaitSema(state->ready_sema, 1, nullptr);
    const int open_result = state->open_result.load(std::memory_order_acquire);
    if (wait_result < 0 || open_result < 0) {
        state->stopping.store(true, std::memory_order_release);
        sceKernelWaitThreadEnd(state->thread, nullptr, nullptr);
        sceKernelDeleteThread(state->thread);
        sceKernelDeleteSema(state->ready_sema);

        char line[160];
        sceClibSnprintf(
            line,
            sizeof(line),
            "audio_native init_failed wait=0x%08X open=0x%08X\n",
            static_cast<unsigned>(wait_result),
            static_cast<unsigned>(open_result));
        flashvita::vita::appendTextFile("ux0:data/FlashVita/runtime.log", line);
        delete state;
        return nullptr;
    }

    char line[192];
    sceClibSnprintf(
        line,
        sizeof(line),
        "audio_native started port=%d hz=%u frames=%u priority=0x%08X affinity=cpu1|cpu2\n",
        open_result,
        static_cast<unsigned>(flashvita::vita::kAudioSampleRate),
        static_cast<unsigned>(flashvita::vita::kAudioFramesPerBuffer),
        static_cast<unsigned>(audio_priority));
    flashvita::vita::appendTextFile("ux0:data/FlashVita/runtime.log", line);
    return state;
}

extern "C" int32_t flashvita_vita_audio_set_paused(void* handle, int32_t paused) {
    auto* state = static_cast<flashvita::vita::NativeAudioThread*>(handle);
    if (!state) return -1;
    state->paused.store(paused != 0, std::memory_order_release);
    return 0;
}

extern "C" int32_t flashvita_vita_audio_get_stats(void* handle, uint64_t* buffers,
                                                    int32_t* last_error, uint64_t* run_clocks,
                                                    int32_t* last_cpu) {
    auto* state = static_cast<flashvita::vita::NativeAudioThread*>(handle);
    if (!state) return -1;

    if (buffers) {
        *buffers = static_cast<uint64_t>(state->buffers.load(std::memory_order_relaxed));
    }
    if (last_error) {
        *last_error = state->last_error.load(std::memory_order_relaxed);
    }

    SceKernelThreadInfo info{};
    info.size = sizeof(info);
    const int info_result = state->thread >= 0 ? sceKernelGetThreadInfo(state->thread, &info) : -1;
    if (run_clocks) *run_clocks = info_result >= 0 ? info.runClocks : 0;
    if (last_cpu) *last_cpu = info_result >= 0 ? info.lastExecutedCpuId : -1;
    return 0;
}

extern "C" void flashvita_vita_audio_destroy(void* handle) {
    auto* state = static_cast<flashvita::vita::NativeAudioThread*>(handle);
    if (!state) return;

    state->stopping.store(true, std::memory_order_release);
    if (state->thread >= 0) {
        sceKernelWaitThreadEnd(state->thread, nullptr, nullptr);
        sceKernelDeleteThread(state->thread);
        state->thread = -1;
    }
    if (state->ready_sema >= 0) {
        sceKernelDeleteSema(state->ready_sema);
        state->ready_sema = -1;
    }
    delete state;
}

extern "C" int64_t flashvita_vita_file_size(const char* path) {
    if (!path) return -1;
    SceIoStat st{};
    if (sceIoGetstat(path, &st) < 0) return -1;
    return static_cast<int64_t>(st.st_size);
}

extern "C" int32_t flashvita_vita_file_read(const char* path, uint8_t* dst, size_t capacity) {
    if (!path || (!dst && capacity != 0)) return -1;
    const SceUID fd = sceIoOpen(path, SCE_O_RDONLY, 0);
    if (fd < 0) return fd;
    size_t total = 0;
    while (total < capacity) {
        const int read = sceIoRead(fd, dst + total, capacity - total);
        if (read < 0) {
            sceIoClose(fd);
            return read;
        }
        if (read == 0) break;
        total += static_cast<size_t>(read);
    }
    sceIoClose(fd);
    return static_cast<int32_t>(total);
}

extern "C" int32_t flashvita_vita_file_open(const char* path) {
    if (!path) return -1;
    return sceIoOpen(path, SCE_O_RDONLY, 0);
}

extern "C" int32_t flashvita_vita_file_read_fd(int32_t fd, uint8_t* dst, size_t capacity) {
    if (fd < 0 || (!dst && capacity != 0)) return -1;
    const size_t chunk = std::min(capacity, static_cast<size_t>(0x7fffffff));
    return sceIoRead(fd, dst, static_cast<SceSize>(chunk));
}

extern "C" int32_t flashvita_vita_file_close(int32_t fd) {
    return fd >= 0 ? sceIoClose(fd) : -1;
}

extern "C" void* flashvita_vita_memblock_alloc(size_t bytes, int32_t* uid_out) {
    if (uid_out) *uid_out = -1;
    if (bytes == 0) return nullptr;

    constexpr size_t kAlignment = 256 * 1024;
    const size_t rounded = (bytes + kAlignment - 1) & ~(kAlignment - 1);
    if (rounded > static_cast<size_t>(UINT32_MAX)) return nullptr;

    struct Candidate {
        SceKernelMemBlockType type;
        const char* label;
    };
    static constexpr Candidate kCandidates[] = {
        {SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_GAME_RW, "main_game"},
        {SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_L1WBWA_RW, "cdram_cached"},
        {SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW, "cdram"},
        {SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_RW, "main"},
    };

    int32_t last_error = -1;
    for (const Candidate& candidate : kCandidates) {
        const SceUID uid = sceKernelAllocMemBlock(
            "FlashVitaSwf",
            candidate.type,
            static_cast<SceSize>(rounded),
            nullptr);
        if (uid < 0) {
            last_error = uid;
            char line[192];
            sceClibSnprintf(
                line,
                sizeof(line),
                "swf_memblock candidate=%s bytes=%u result=0x%08X\n",
                candidate.label,
                static_cast<unsigned>(rounded),
                static_cast<unsigned>(uid));
            flashvita::vita::appendTextFile("ux0:data/FlashVita/runtime.log", line);
            continue;
        }

        void* base = nullptr;
        const int base_result = sceKernelGetMemBlockBase(uid, &base);
        if (base_result < 0 || !base) {
            last_error = base_result < 0 ? base_result : -1;
            sceKernelFreeMemBlock(uid);
            continue;
        }

        char line[192];
        sceClibSnprintf(
            line,
            sizeof(line),
            "swf_memblock candidate=%s bytes=%u uid=%d base=%p success\n",
            candidate.label,
            static_cast<unsigned>(rounded),
            uid,
            base);
        flashvita::vita::appendTextFile("ux0:data/FlashVita/runtime.log", line);
        if (uid_out) *uid_out = uid;
        return base;
    }

    constexpr int32_t kVglRamHandle = -2;
    void* vgl_base = vglAlloc(static_cast<uint32_t>(rounded), VGL_MEM_RAM);
    if (vgl_base) {
        char line[192];
        sceClibSnprintf(
            line,
            sizeof(line),
            "swf_memblock candidate=vgl_ram bytes=%u base=%p success\n",
            static_cast<unsigned>(rounded),
            vgl_base);
        flashvita::vita::appendTextFile("ux0:data/FlashVita/runtime.log", line);
        if (uid_out) *uid_out = kVglRamHandle;
        return vgl_base;
    }

    {
        char line[160];
        sceClibSnprintf(
            line,
            sizeof(line),
            "swf_memblock candidate=vgl_ram bytes=%u result=alloc_failed\n",
            static_cast<unsigned>(rounded));
        flashvita::vita::appendTextFile("ux0:data/FlashVita/runtime.log", line);
    }

    if (uid_out) *uid_out = last_error;
    return nullptr;
}

extern "C" int32_t flashvita_vita_memblock_free(int32_t uid, void* base) {
    constexpr int32_t kVglRamHandle = -2;
    if (uid == kVglRamHandle) {
        if (!base) return -1;
        vglFree(base);
        return 0;
    }
    return uid >= 0 ? sceKernelFreeMemBlock(uid) : -1;
}

extern "C" void* flashvita_vita_vgl_ram_alloc(size_t bytes) {
    if (bytes == 0 || bytes > static_cast<size_t>(UINT32_MAX)) return nullptr;
    void* ptr = vglAlloc(static_cast<uint32_t>(bytes), VGL_MEM_RAM);
    if (ptr && flashvita::vita::g_vgl_ram_base.load(std::memory_order_acquire) == 0) {
        SceKernelMemBlockInfo info{};
        info.size = sizeof(info);
        if (sceKernelGetMemBlockInfoByAddr(ptr, &info) >= 0 && info.mappedBase && info.mappedSize) {
            const uintptr_t base = reinterpret_cast<uintptr_t>(info.mappedBase);
            flashvita::vita::g_vgl_ram_base.store(base, std::memory_order_release);
            flashvita::vita::g_vgl_ram_end.store(base + info.mappedSize, std::memory_order_release);
        }
    }
    return ptr;
}

extern "C" void flashvita_vita_vgl_ram_free(void* base) {
    if (base) vglFree(base);
}

extern "C" int32_t flashvita_vita_vgl_ram_owns(const void* ptr) {
    if (!ptr) return 0;
    const uintptr_t address = reinterpret_cast<uintptr_t>(ptr);
    const uintptr_t base = flashvita::vita::g_vgl_ram_base.load(std::memory_order_acquire);
    const uintptr_t end = flashvita::vita::g_vgl_ram_end.load(std::memory_order_acquire);
    return base != 0 && address >= base && address < end ? 1 : 0;
}

extern "C" void flashvita_vita_rust_allocator_init_cached(void) {
    if (flashvita::vita::g_rust_heap != nullptr) return;

    static constexpr size_t kCandidateSizes[] = {
        64u * 1024u * 1024u,
        48u * 1024u * 1024u,
        32u * 1024u * 1024u,
    };
    static constexpr SceKernelMemBlockType kTypes[] = {
        SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_GAME_RW,
        SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_RW,
    };

    for (size_t bytes : kCandidateSizes) {
        for (SceKernelMemBlockType type : kTypes) {
            const SceUID uid = sceKernelAllocMemBlock(
                "FlashVitaRustHeap", type, static_cast<SceSize>(bytes), nullptr);
            if (uid < 0) continue;

            void* base = nullptr;
            if (sceKernelGetMemBlockBase(uid, &base) < 0 || !base) {
                sceKernelFreeMemBlock(uid);
                continue;
            }

            SceClibMspace mspace = sceClibMspaceCreate(base, static_cast<SceSize>(bytes));
            if (!mspace) {
                sceKernelFreeMemBlock(uid);
                continue;
            }

            flashvita::vita::g_rust_heap_uid = uid;
            flashvita::vita::g_rust_heap = mspace;
            const uintptr_t begin = reinterpret_cast<uintptr_t>(base);
            flashvita::vita::g_rust_heap_base.store(begin, std::memory_order_release);
            flashvita::vita::g_rust_heap_end.store(begin + bytes, std::memory_order_release);

            char line[192];
            sceClibSnprintf(
                line,
                sizeof(line),
                "rust_allocator dedicated_cached=1 bytes=%u uid=%d type=0x%08X base=%p\n",
                static_cast<unsigned>(bytes),
                uid,
                static_cast<unsigned>(type),
                base);
            flashvita::vita::rustAllocatorLog(line);
            return;
        }
    }

    flashvita::vita::rustAllocatorLog(
        "rust_allocator dedicated_cached=0 fallback=newlib\n");
}

extern "C" void* flashvita_vita_rust_alloc(size_t bytes, size_t alignment) {
    if (bytes == 0) bytes = 1;
    if (alignment < sizeof(void*)) alignment = sizeof(void*);

    if (flashvita::vita::g_rust_heap &&
        bytes <= static_cast<size_t>(UINT32_MAX) &&
        alignment <= static_cast<size_t>(UINT32_MAX)) {
        flashvita::vita::rustHeapLock();
        void* ptr = alignment <= alignof(std::max_align_t)
            ? sceClibMspaceMalloc(flashvita::vita::g_rust_heap, static_cast<SceSize>(bytes))
            : sceClibMspaceMemalign(
                  flashvita::vita::g_rust_heap,
                  static_cast<SceSize>(alignment),
                  static_cast<SceSize>(bytes));
        flashvita::vita::rustHeapUnlock();
        if (ptr) return ptr;
    }

    return alignment <= alignof(std::max_align_t)
        ? std::malloc(bytes)
        : memalign(alignment, bytes);
}

extern "C" void* flashvita_vita_rust_alloc_zeroed(size_t bytes, size_t alignment) {
    void* ptr = flashvita_vita_rust_alloc(bytes, alignment);
    if (ptr) std::memset(ptr, 0, bytes);
    return ptr;
}

extern "C" void* flashvita_vita_rust_realloc(void* ptr, size_t old_size, size_t alignment,
                                               size_t new_size) {
    if (!ptr) return flashvita_vita_rust_alloc(new_size, alignment);
    if (new_size == 0) {
        flashvita_vita_rust_dealloc(ptr);
        return nullptr;
    }

    if (flashvita::vita::isRustHeapPointer(ptr) && flashvita::vita::g_rust_heap &&
        new_size <= static_cast<size_t>(UINT32_MAX) &&
        alignment <= static_cast<size_t>(UINT32_MAX)) {
        flashvita::vita::rustHeapLock();
        void* resized = alignment <= alignof(std::max_align_t)
            ? sceClibMspaceRealloc(
                  flashvita::vita::g_rust_heap, ptr, static_cast<SceSize>(new_size))
            : sceClibMspaceReallocalign(
                  flashvita::vita::g_rust_heap,
                  ptr,
                  static_cast<SceSize>(new_size),
                  static_cast<SceSize>(alignment));
        flashvita::vita::rustHeapUnlock();
        if (resized) return resized;
    } else if (!flashvita::vita::isRustHeapPointer(ptr) &&
               alignment <= alignof(std::max_align_t)) {
        if (void* resized = std::realloc(ptr, new_size)) return resized;
    }

    void* replacement = flashvita_vita_rust_alloc(new_size, alignment);
    if (!replacement) return nullptr;
    std::memcpy(replacement, ptr, std::min(old_size, new_size));
    flashvita_vita_rust_dealloc(ptr);
    return replacement;
}

extern "C" void flashvita_vita_rust_dealloc(void* ptr) {
    if (!ptr) return;
    if (flashvita::vita::isRustHeapPointer(ptr) && flashvita::vita::g_rust_heap) {
        flashvita::vita::rustHeapLock();
        sceClibMspaceFree(flashvita::vita::g_rust_heap, ptr);
        flashvita::vita::rustHeapUnlock();
    } else {
        std::free(ptr);
    }
}

extern "C" int32_t flashvita_vita_remove_file(const char* path) {
    return path ? sceIoRemove(path) : -1;
}

extern "C" int32_t flashvita_vita_mkdirs(const char* path) {
    return path && flashvita::vita::makeDirectories(path) ? 0 : -1;
}

extern "C" int32_t flashvita_vita_http_fetch_to_file(const char* url,
                                                       int32_t method,
                                                       const uint8_t* body,
                                                       size_t body_len,
                                                       const char* content_type,
                                                       const char* destination,
                                                       int32_t* http_status) {
    return flashvita::vita::httpFetchToFile(
               url, method, body, body_len, content_type, destination, http_status)
        ? 0
        : -1;
}

extern "C" void* flashvita_vita_http_fetch_start(const char* url,
                                                   int32_t method,
                                                   const uint8_t* body,
                                                   size_t body_len,
                                                   const char* content_type,
                                                   const char* destination) {
    if (!url || !destination || (body_len != 0 && !body)) return nullptr;

    auto* job = new (std::nothrow) flashvita::vita::NativeHttpJob();
    if (!job) return nullptr;
    try {
        job->url = url;
        job->method = method;
        if (body_len != 0) job->body.assign(body, body + body_len);
        if (content_type) job->content_type = content_type;
        job->destination = destination;
    } catch (...) {
        delete job;
        return nullptr;
    }

    const int base_priority = sceKernelGetThreadCurrentPriority();
    const int io_priority = base_priority >= 0 ? base_priority + 12 : 0x1000010C;
    job->thread = sceKernelCreateThread(
        "FlashVitaHttp",
        flashvita::vita::httpThreadEntry,
        io_priority,
        flashvita::vita::kHttpThreadStackSize,
        0,
        SCE_KERNEL_CPU_MASK_USER_2,
        nullptr);
    if (job->thread < 0) {
        delete job;
        return nullptr;
    }

    auto* arg = job;
    if (sceKernelStartThread(job->thread, sizeof(arg), &arg) < 0) {
        sceKernelDeleteThread(job->thread);
        delete job;
        return nullptr;
    }
    return job;
}

extern "C" int32_t flashvita_vita_http_fetch_poll(void* handle, int32_t* http_status) {
    auto* job = static_cast<flashvita::vita::NativeHttpJob*>(handle);
    if (!job) return -2;
    if (http_status) {
        *http_status = job->status.load(std::memory_order_acquire);
    }
    return job->result.load(std::memory_order_acquire);
}

extern "C" void flashvita_vita_http_fetch_destroy(void* handle) {
    auto* job = static_cast<flashvita::vita::NativeHttpJob*>(handle);
    if (!job) return;
    if (job->thread >= 0) {
        sceKernelWaitThreadEnd(job->thread, nullptr, nullptr);
        sceKernelDeleteThread(job->thread);
        job->thread = -1;
    }
    delete job;
}

extern "C" void flashvita_vita_log_line(const char* line) {
    if (!line) return;
    flashvita::vita::appendTextFile(
        "ux0:data/FlashVita/runtime.log",
        std::string(line) + "\n");
}

extern "C" int32_t flashvita_vita_logging_enabled(void) {
    return flashvita::vita::loggingEnabled() ? 1 : 0;
}

extern "C" int32_t flashvita_vita_perf_logging_enabled(void) {
    return flashvita::vita::perfLoggingEnabled() ? 1 : 0;
}
