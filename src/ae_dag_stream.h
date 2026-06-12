#pragma once

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>
#include <functional>

#ifdef _WIN32
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
    uint32_t reserved[5];
};

struct AEFrame {
    uint64_t hw_timestamp_ns;
    int16_t  amplitude_mv;
};
#pragma pack(pop)

struct AEReference {
    const AEFrame* ptr;
    size_t         count;
};

class MappedFile {
public:
    MappedFile() = default;
    ~MappedFile() { unmap(); }
    MappedFile(const MappedFile&) = delete;
    MappedFile& operator=(const MappedFile&) = delete;

    bool open(const std::string& path) {
#ifdef _WIN32
        handle_ = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_READONLY | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
        if (handle_ == INVALID_HANDLE_VALUE) return false;

        LARGE_INTEGER li;
        if (!GetFileSizeEx(handle_, &li)) { CloseHandle(handle_); handle_ = INVALID_HANDLE_VALUE; return false; }
        file_size_ = static_cast<size_t>(li.QuadPart);

        mapping_ = CreateFileMappingA(handle_, nullptr, PAGE_READONLY, 0, 0, nullptr);
        if (!mapping_) { CloseHandle(handle_); handle_ = INVALID_HANDLE_VALUE; return false; }

        base_ = static_cast<const uint8_t*>(MapViewOfFile(mapping_, FILE_MAP_READ, 0, 0, 0));
        if (!base_) { CloseHandle(mapping_); CloseHandle(handle_); mapping_ = nullptr; handle_ = INVALID_HANDLE_VALUE; return false; }
#else
        fd_ = ::open(path.c_str(), O_RDONLY);
        if (fd_ < 0) return false;

        struct stat st;
        if (fstat(fd_, &st) < 0) { ::close(fd_); fd_ = -1; return false; }
        file_size_ = static_cast<size_t>(st.st_size);

        base_ = static_cast<const uint8_t*>(mmap(nullptr, file_size_, PROT_READ, MAP_PRIVATE, fd_, 0));
        if (base_ == MAP_FAILED) { base_ = nullptr; ::close(fd_); fd_ = -1; return false; }
#endif
        return true;
    }

    void unmap() {
        if (base_) {
#ifdef _WIN32
            UnmapViewOfFile(base_);
            CloseHandle(mapping_);
            CloseHandle(handle_);
            mapping_ = nullptr;
            handle_ = INVALID_HANDLE_VALUE;
#else
            munmap(const_cast<uint8_t*>(base_), file_size_);
            ::close(fd_);
            fd_ = -1;
#endif
            base_ = nullptr;
            file_size_ = 0;
        }
    }

    const uint8_t* data() const { return base_; }
    size_t         size() const { return file_size_; }

private:
    const uint8_t* base_ = nullptr;
    size_t         file_size_ = 0;

#ifdef _WIN32
    HANDLE handle_  = INVALID_HANDLE_VALUE;
    HANDLE mapping_ = nullptr;
#else
    int fd_ = -1;
#endif
};

class AEDAGStream {
public:
    AEDAGStream() = default;

    bool load(const std::string& path) {
        if (!mmap_.open(path)) return false;

        const uint8_t* base = mmap_.data();
        size_t sz = mmap_.size();
        if (sz < sizeof(AERawHeader)) return false;

        const AERawHeader* hdr = reinterpret_cast<const AERawHeader*>(base);
        if (hdr->magic[0] != 'A' || hdr->magic[1] != 'E' ||
            hdr->magic[2] != 'R' || hdr->magic[3] != 'W')
            return false;

        header_ = *hdr;

        const uint8_t* payload = base + sizeof(AERawHeader);
        size_t payload_sz = sz - sizeof(AERawHeader);
        size_t frame_sz = sizeof(AEFrame);
        if (hdr->frame_size != frame_sz) return false;

        total_frames_ = payload_sz / frame_sz;
        frames_ = reinterpret_cast<const AEFrame*>(payload);
        return true;
    }

    const AERawHeader& header() const { return header_; }
    size_t total_frames() const { return total_frames_; }

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
    MappedFile      mmap_;
    AERawHeader     header_{};
    const AEFrame*  frames_ = nullptr;
    size_t          total_frames_ = 0;
};

}
