// SPDX-License-Identifier: GPL-3.0-or-later
#include "video_core/macro_image_capture.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <memory>
#include <new>
#ifndef _WIN32
#include <fcntl.h>
#include <unistd.h>
#endif

#include "common/cityhash.h"
#include "common/scope_exit.h"
#include "common/swap.h"

namespace VideoCommon {
namespace {

using Image = std::array<u32_le, MacroImageCapture::WordCount>;
using Header = std::array<u32_le, 16>;
static_assert(sizeof(Image) == 128 * 1024 && sizeof(Header) == 64);

bool Publish(const std::filesystem::path& file, const Header& header, const Image& image) {
    auto temporary = file;
    temporary += ".partial";
#ifdef _WIN32
    // Inherit ACLs of the owner's chosen directory. The x modifier requires a new file.
    FILE* out = _wfopen(temporary.c_str(), L"wbx");
    if (!out)
        return false;
#else
    const int fd = ::open(temporary.c_str(), O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC, 0600);
    if (fd < 0)
        return false;
    FILE* out = ::fdopen(fd, "wb");
    if (!out) {
        ::close(fd);
        ::unlink(temporary.c_str());
        return false;
    }
#endif
    std::error_code ec;
    SCOPE_EXIT {
        std::filesystem::remove(temporary, ec);
    };
    const bool complete = std::fwrite(header.data(), sizeof(header), 1, out) == 1 &&
                          std::fwrite(image.data(), sizeof(image), 1, out) == 1;
    const bool closed = std::fclose(out) == 0;
    if (!complete || !closed)
        return false;
    // Atomically expose only a completely closed file; never replace an existing destination.
    std::filesystem::create_hard_link(temporary, file, ec);
    return !ec;
}

} // namespace

MacroImageCapture::Result MacroImageCapture::Try(const std::filesystem::path& file, Entry e,
                                                 const void* owner, std::span<const u32> code,
                                                 CheckedImage& checked) {
    if (claimed.load(std::memory_order_relaxed) || e.channel != 1 || e.address_space != 1 ||
        e.method != 0xe2c || e.start != 0x142 || !owner || code.size() != WordCount ||
        file.empty() || !file.is_absolute())
        return Result::Ignored;
    if (checked.owner == owner && checked.generation == e.generation)
        return Result::Ignored;
    checked = {owner, e.generation};

    // The caller owns the front's code RAM here. Save and hash the same immutable bytes.
    const std::unique_ptr<Image> snapshot{new (std::nothrow) Image};
    if (!snapshot)
        return claimed.exchange(true, std::memory_order_relaxed) ? Result::Ignored : Result::Failed;
    std::copy(code.begin(), code.end(), snapshot->begin());
    const u64 hash =
        Common::CityHash64(reinterpret_cast<const char*>(snapshot->data()), sizeof(*snapshot));
    if (hash != expected_hash)
        return Result::Mismatch;
    if (claimed.exchange(true, std::memory_order_relaxed))
        return Result::Ignored;
    const Header header{0x454d4d53,
                        0x31474d49,
                        1,
                        sizeof(Header),
                        sizeof(Image),
                        WordCount,
                        static_cast<u32>(e.channel),
                        static_cast<u32>(e.address_space),
                        static_cast<u32>(e.address_space >> 32),
                        e.method,
                        e.start,
                        static_cast<u32>(e.generation),
                        static_cast<u32>(e.generation >> 32),
                        static_cast<u32>(hash),
                        static_cast<u32>(hash >> 32),
                        0};
    return Publish(file, header, *snapshot) ? Result::Saved : Result::Failed;
}

} // namespace VideoCommon
