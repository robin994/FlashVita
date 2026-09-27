#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace flashvita::vita {

using ParallelCallback = void (*)(void* user, uint32_t begin, uint32_t end);

struct WorkerRuntimeStats {
    uint64_t run_clocks[2]{};
    int32_t last_cpu[2]{-1, -1};
};

bool initialize();
void shutdown();
bool parallelFor(uint32_t count, uint32_t min_grain, ParallelCallback callback, void* user);
bool getWorkerRuntimeStats(WorkerRuntimeStats& out);

bool readFile(const std::string& path, std::vector<uint8_t>& out,
              size_t max_bytes = static_cast<size_t>(-1));
bool readFilePrefix(const std::string& path, void* dst, size_t bytes, size_t& bytes_read);
bool readTextFile(const std::string& path, std::string& out);
bool writeFile(const std::string& path, const void* data, size_t bytes);
bool writeTextFile(const std::string& path, const std::string& text);
bool appendFile(const std::string& path, const void* data, size_t bytes);
bool appendTextFile(const std::string& path, const std::string& text);
bool makeDirectories(const std::string& path);
bool fileExists(const std::string& path);
void setLoggingEnabled(bool enabled);
bool loggingEnabled();

} // namespace flashvita::vita

extern "C" {
typedef void (*FlashVitaParallelCallback)(void* user, uint32_t begin, uint32_t end);
typedef void (*FlashVitaAudioFillCallback)(void* user, int16_t* samples, uint32_t frames);
int32_t flashvita_vita_parallel_for(uint32_t count, uint32_t min_grain,
                                    FlashVitaParallelCallback callback, void* user);
void* flashvita_vita_audio_create(FlashVitaAudioFillCallback callback, void* user);
int32_t flashvita_vita_audio_set_paused(void* handle, int32_t paused);
int32_t flashvita_vita_audio_get_stats(void* handle, uint64_t* buffers,
                                       int32_t* last_error, uint64_t* run_clocks,
                                       int32_t* last_cpu);
void flashvita_vita_audio_destroy(void* handle);
int64_t flashvita_vita_file_size(const char* path);
int32_t flashvita_vita_file_read(const char* path, uint8_t* dst, size_t capacity);
int32_t flashvita_vita_file_open(const char* path);
int32_t flashvita_vita_file_read_fd(int32_t fd, uint8_t* dst, size_t capacity);
int32_t flashvita_vita_file_close(int32_t fd);
void* flashvita_vita_memblock_alloc(size_t bytes, int32_t* uid_out);
int32_t flashvita_vita_memblock_free(int32_t uid, void* base);
void* flashvita_vita_vgl_ram_alloc(size_t bytes);
void flashvita_vita_vgl_ram_free(void* base);
void flashvita_vita_rust_allocator_enable_vgl(void);
void* flashvita_vita_rust_alloc(size_t bytes, size_t alignment);
void* flashvita_vita_rust_alloc_zeroed(size_t bytes, size_t alignment);
void* flashvita_vita_rust_realloc(void* ptr, size_t old_size, size_t alignment,
                                  size_t new_size);
void flashvita_vita_rust_dealloc(void* ptr);
int32_t flashvita_vita_remove_file(const char* path);
int32_t flashvita_vita_mkdirs(const char* path);
int32_t flashvita_vita_http_fetch_to_file(const char* url,
                                          int32_t method,
                                          const uint8_t* body,
                                          size_t body_len,
                                          const char* content_type,
                                          const char* destination,
                                          int32_t* http_status);
void flashvita_vita_log_line(const char* line);
int32_t flashvita_vita_logging_enabled(void);
}
