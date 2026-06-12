#pragma once

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>
#include <functional>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#endif

namespace cavitation {

#pragma pack(push, 1)
struct AERawHeader {
    char     magic[4];
    uint32_t version;
    uint32_t sample_rate_hz;
    uint32_t channels;
    uint64_t start_epoch_ns;
    uint32_t frame_size;
    uint32_t header_crc32;
    uint32_t reserved[4];
};
#pragma pack(pop)

static_assert(sizeof(AERawHeader) == 48, "AERawHeader must be exactly 48 bytes");

struct AEFrame {
    uint64_t hw_timestamp_ns;
    int16_t  amplitude_mv;
};

struct AEReference {
    const AEFrame* ptr;
    size_t         count;
};

inline uint32_t crc32_table[256];
inline bool crc32_table_init = false;

inline void ensure_crc32_table() {
    if (crc32_table_init) return;
    for (uint32_t i = 0; i < 256; ++i) {
        uint32_t c = i;
        for (int j = 0; j < 8; ++j) {
            if (c & 1) c = 0xEDB88320u ^ (c >> 1);
            else       c >>= 1;
        }
        crc32_table[i] = c;
    }
    crc32_table_init = true;
}

inline uint32_t compute_crc32(const void* data, size_t len) {
    ensure_crc32_table();
    const uint8_t* p = static_cast<const uint8_t*>(data);
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; ++i) {
        crc = crc32_table[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
    }
    return crc ^ 0xFFFFFFFFu;
}

#ifdef _WIN32

struct WinHandleDeleter {
    void operator()(void* h) const {
        if (h && h != INVALID_HANDLE_VALUE) CloseHandle(static_cast<HANDLE>(h));
    }
};

struct WinMappingDeleter {
    size_t file_size;
    void operator()(const void* base) const {
        if (base) UnmapViewOfFile(base);
    }
};

struct WinFileMappingDeleter {
    void operator()(void* m) const {
        if (m) CloseHandle(static_cast<HANDLE>(m));
    }
};

using WinHandleUniquePtr = std::unique_ptr<void, WinHandleDeleter>;
using WinMappingUniquePtr = std::unique_ptr<const void, WinMappingDeleter>;
using WinFileMappingUniquePtr = std::unique_ptr<void, WinFileMappingDeleter>;

#else

struct PosixFdDeleter {
    void operator()(int* fd) const {
        if (fd && *fd >= 0) { ::close(*fd); delete fd; }
    }
};

struct PosixMmapDeleter {
    size_t file_size;
    void operator()(void* base) const {
        if (base && base != MAP_FAILED) munmap(base, file_size);
    }
};

using PosixFdUniquePtr = std::unique_ptr<int, PosixFdDeleter>;
using PosixMmapUniquePtr = std::unique_ptr<void, PosixMmapDeleter>;

#endif

class MappedFile {
public:
    MappedFile() = default;
    ~MappedFile() = default;
    MappedFile(const MappedFile&) = delete;
    MappedFile& operator=(const MappedFile&) = delete;

    MappedFile(MappedFile&& other) noexcept
        : base_(other.base_), file_size_(other.file_size_)
#ifdef _WIN32
        , handle_(std::move(other.handle_))
        , file_mapping_(std::move(other.file_mapping_))
        , view_mapping_(std::move(other.view_mapping_))
#else
        , fd_owner_(std::move(other.fd_owner_))
        , mmap_owner_(std::move(other.mmap_owner_))
#endif
    {
        other.base_ = nullptr;
        other.file_size_ = 0;
    }

    MappedFile& operator=(MappedFile&& other) noexcept {
        if (this != &other) {
            close();
            base_ = other.base_;
            file_size_ = other.file_size_;
#ifdef _WIN32
            handle_ = std::move(other.handle_);
            file_mapping_ = std::move(other.file_mapping_);
            view_mapping_ = std::move(other.view_mapping_);
#else
            fd_owner_ = std::move(other.fd_owner_);
            mmap_owner_ = std::move(other.mmap_owner_);
#endif
            other.base_ = nullptr;
            other.file_size_ = 0;
        }
        return *this;
    }

    bool open(const std::string& path) {
        close();

#ifdef _WIN32
        HANDLE hFile = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ,
                                   nullptr, OPEN_EXISTING,
                                   FILE_ATTRIBUTE_READONLY | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
        if (hFile == INVALID_HANDLE_VALUE) return false;
        handle_.reset(hFile);

        LARGE_INTEGER li;
        if (!GetFileSizeEx(hFile, &li)) { handle_.reset(); return false; }
        file_size_ = static_cast<size_t>(li.QuadPart);

        HANDLE hMap = CreateFileMappingA(hFile, nullptr, PAGE_READONLY, 0, 0, nullptr);
        if (!hMap) { handle_.reset(); return false; }
        file_mapping_.reset(hMap);

        const void* pView = MapViewOfFile(hMap, FILE_MAP_READ, 0, 0, 0);
        if (!pView) { file_mapping_.reset(); handle_.reset(); return false; }
        view_mapping_ = WinMappingUniquePtr(pView, WinMappingDeleter{file_size_});
        base_ = static_cast<const uint8_t*>(pView);
#else
        int fd = ::open(path.c_str(), O_RDONLY);
        if (fd < 0) return false;
        fd_owner_ = PosixFdUniquePtr(new int(fd));

        struct stat st;
        if (fstat(fd, &st) < 0) { fd_owner_.reset(); return false; }
        file_size_ = static_cast<size_t>(st.st_size);

        void* pMap = mmap(nullptr, file_size_, PROT_READ, MAP_PRIVATE, fd, 0);
        if (pMap == MAP_FAILED) { fd_owner_.reset(); return false; }
        mmap_owner_ = PosixMmapUniquePtr(pMap, PosixMmapDeleter{file_size_});
        base_ = static_cast<const uint8_t*>(pMap);
#endif
        return true;
    }

    void close() {
#ifdef _WIN32
        view_mapping_.reset();
        file_mapping_.reset();
        handle_.reset();
#else
        mmap_owner_.reset();
        fd_owner_.reset();
#endif
        base_ = nullptr;
        file_size_ = 0;
    }

    const uint8_t* data() const { return base_; }
    size_t         size() const { return file_size_; }
    bool           is_open() const { return base_ != nullptr; }

private:
    const uint8_t* base_ = nullptr;
    size_t         file_size_ = 0;

#ifdef _WIN32
    WinHandleUniquePtr       handle_{nullptr, WinHandleDeleter{}};
    WinFileMappingUniquePtr  file_mapping_{nullptr, WinFileMappingDeleter{}};
    WinMappingUniquePtr      view_mapping_{nullptr, WinMappingDeleter{0}};
#else
    PosixFdUniquePtr    fd_owner_{nullptr, PosixFdDeleter{}};
    PosixMmapUniquePtr  mmap_owner_{nullptr, PosixMmapDeleter{0}};
#endif
};

enum class AELoadError {
    None,
    FileOpenFailed,
    FileTooSmall,
    MagicMismatch,
    VersionUnsupported,
    ChecksumError,
    FrameSizeMismatch,
    ZeroFrames,
};

class AEDAGStream {
public:
    AEDAGStream() = default;

    AELoadError load(const std::string& path) {
        mmap_.close();
        header_ = AERawHeader{};
        frames_ = nullptr;
        total_frames_ = 0;
        last_error_ = AELoadError::None;
        last_error_file_ = path;

        if (!mmap_.open(path)) {
            return fail(AELoadError::FileOpenFailed);
        }

        auto guard = make_guard();

        const uint8_t* base = mmap_.data();
        size_t sz = mmap_.size();
        if (sz < sizeof(AERawHeader)) {
            return fail(AELoadError::FileTooSmall);
        }

        const AERawHeader* hdr = reinterpret_cast<const AERawHeader*>(base);

        if (hdr->magic[0] != 'A' || hdr->magic[1] != 'E' ||
            hdr->magic[2] != 'R' || hdr->magic[3] != 'W') {
            return fail(AELoadError::MagicMismatch);
        }

        if (hdr->version != 1) {
            return fail(AELoadError::VersionUnsupported);
        }

        size_t crc_len = offsetof(AERawHeader, header_crc32);
        uint32_t computed = compute_crc32(base, crc_len);
        if (computed != hdr->header_crc32 && hdr->header_crc32 != 0) {
            return fail(AELoadError::ChecksumError);
        }

        size_t frame_sz = sizeof(AEFrame);
        if (hdr->frame_size != frame_sz) {
            return fail(AELoadError::FrameSizeMismatch);
        }

        const uint8_t* payload = base + sizeof(AERawHeader);
        size_t payload_sz = sz - sizeof(AERawHeader);
        size_t nframes = payload_sz / frame_sz;
        if (nframes == 0) {
            return fail(AELoadError::ZeroFrames);
        }

        header_ = *hdr;
        frames_ = reinterpret_cast<const AEFrame*>(payload);
        total_frames_ = nframes;

        guard.dismiss();
        last_error_ = AELoadError::None;
        return AELoadError::None;
    }

    void close() {
        mmap_.close();
        frames_ = nullptr;
        total_frames_ = 0;
    }

    const AERawHeader& header() const { return header_; }
    size_t total_frames() const { return total_frames_; }
    AELoadError last_error() const { return last_error_; }
    const std::string& last_error_file() const { return last_error_file_; }

    static const char* error_string(AELoadError e) {
        switch (e) {
            case AELoadError::None:              return "No error";
            case AELoadError::FileOpenFailed:    return "Cannot open file (missing or locked)";
            case AELoadError::FileTooSmall:      return "File too small for header";
            case AELoadError::MagicMismatch:     return "Magic signature mismatch (not AERW)";
            case AELoadError::VersionUnsupported:return "Unsupported header version";
            case AELoadError::ChecksumError:     return "Header CRC32 checksum error (EMI corruption?)";
            case AELoadError::FrameSizeMismatch: return "Frame size mismatch";
            case AELoadError::ZeroFrames:        return "File contains zero frames";
            default:                             return "Unknown error";
        }
    }

    AEReference reference(size_t offset, size_t count) const {
        if (offset + count > total_frames_) count = total_frames_ - offset;
        return { frames_ + offset, count };
    }

    void stream_chunks(size_t chunk_frames,
                       std::function<void(const AEReference&, size_t chunk_idx)> fn) const {
        size_t idx = 0;
        size_t chunk_idx = 0;
        while (idx < total_frames_) {
            size_t n = (std::min)(chunk_frames, total_frames_ - idx);
            AEReference ref{ frames_ + idx, n };
            fn(ref, chunk_idx);
            idx += n;
            chunk_idx++;
        }
    }

    void extract_voltages(const AEReference& ref, double* out) const {
        for (size_t i = 0; i < ref.count; ++i) {
            out[i] = static_cast<double>(ref.ptr[i].amplitude_mv) * 0.001;
        }
    }

    void extract_timestamps(const AEReference& ref, uint64_t* out) const {
        for (size_t i = 0; i < ref.count; ++i) {
            out[i] = ref.ptr[i].hw_timestamp_ns;
        }
    }

private:
    struct ScopeGuard {
        std::function<void()> fn;
        bool active = true;
        ~ScopeGuard() { if (active) fn(); }
        void dismiss() { active = false; }
    };

    ScopeGuard make_guard() {
        return ScopeGuard{[this]() {
            this->mmap_.close();
            this->frames_ = nullptr;
            this->total_frames_ = 0;
        }};
    }

    AELoadError fail(AELoadError e) {
        mmap_.close();
        frames_ = nullptr;
        total_frames_ = 0;
        last_error_ = e;
        return e;
    }

    MappedFile      mmap_;
    AERawHeader     header_{};
    const AEFrame*  frames_ = nullptr;
    size_t          total_frames_ = 0;
    AELoadError     last_error_ = AELoadError::None;
    std::string     last_error_file_;
};

}
