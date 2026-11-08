// SPDX-License-Identifier: GPL-3.0-or-later
#include <array>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <thread>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include "common/cityhash.h"
#include "common/swap.h"
#include "video_core/macro_image_capture.h"

namespace {
using Capture = VideoCommon::MacroImageCapture;
using Result = Capture::Result;
using namespace std::filesystem;

struct CaptureFixture {
    CaptureFixture() {
        REQUIRE(create_directory(directory));
        for (u32 i = 0; i < image.size(); ++i)
            image[i] = i * 0x3817u + 91;
    }
    ~CaptureFixture() {
        std::error_code error;
        remove_all(directory, error);
    }
    u64 Hash() const {
        std::vector<u32_le> little(image.begin(), image.end());
        return Common::CityHash64(reinterpret_cast<const char*>(little.data()), little.size() * 4);
    }
    path directory = temp_directory_path() /
                     ("stitch-mme-capture-" +
                      std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    path file = directory / "private-code";
    std::vector<u32> image = std::vector<u32>(Capture::WordCount);
    Capture::Entry entry{1, 1, 0xe2c, 0x142, 0x10000000dULL};
    Capture::CheckedImage checked{};
};
} // namespace

TEST_CASE("Private MME image capture requires the exact identified executable and complete RAM",
          "[mme_image_capture]") {
    CaptureFixture f;
    Capture capture{f.Hash()};
    const auto try_entry = [&](Capture::Entry e) {
        return capture.Try(f.file, e, &f, f.image, f.checked);
    };
    for (u32 field = 0; field < 4; ++field) {
        auto e = f.entry;
        if (field == 0)
            ++e.channel;
        if (field == 1)
            ++e.address_space;
        if (field == 2)
            ++e.method;
        if (field == 3)
            ++e.start;
        REQUIRE(try_entry(e) == Result::Ignored);
    }
    REQUIRE(capture.Try("relative", f.entry, &f, f.image, f.checked) == Result::Ignored);
    REQUIRE(capture.Try(f.file, f.entry, nullptr, f.image, f.checked) == Result::Ignored);
    const auto short_image = std::span{f.image}.first(f.image.size() - 1);
    REQUIRE(capture.Try(f.file, f.entry, &f, short_image, f.checked) == Result::Ignored);
    f.image.push_back(0);
    REQUIRE(try_entry(f.entry) == Result::Ignored);
    f.image.pop_back();

    ++f.image.back(); // Unrelated tail mutation invalidates even the same prefix and entry.
    REQUIRE(try_entry(f.entry) == Result::Mismatch);
    REQUIRE(try_entry(f.entry) == Result::Ignored);
    REQUIRE_FALSE(exists(f.file));
    --f.image.back();
    ++f.entry.generation;
    REQUIRE(try_entry(f.entry) == Result::Saved);
    REQUIRE(file_size(f.file) == 64 + 128 * 1024);
    REQUIRE_FALSE(exists(f.file.string() + ".partial"));
    REQUIRE(try_entry(f.entry) == Result::Ignored);
    std::ifstream in{f.file, std::ios::binary};
    std::array<u32_le, 16> header{};
    in.read(reinterpret_cast<char*>(header.data()), sizeof(header));
    const u64 hash = f.Hash();
    const std::array<u32_le, 16> expected{0x454d4d53,
                                          0x31474d49,
                                          1,
                                          64,
                                          131072,
                                          32768,
                                          1,
                                          1,
                                          0,
                                          0xe2c,
                                          0x142,
                                          static_cast<u32>(f.entry.generation),
                                          1,
                                          static_cast<u32>(hash),
                                          static_cast<u32>(hash >> 32),
                                          0};
    REQUIRE(header == expected);
    std::vector<u32_le> words(Capture::WordCount);
    in.read(reinterpret_cast<char*>(words.data()), words.size() * sizeof(words[0]));
    REQUIRE(in.good());
    REQUIRE(std::equal(words.begin(), words.end(), f.image.begin()));
}

TEST_CASE("Private MME image publication preserves existing files and leaves no partial result",
          "[mme_image_capture]") {
    CaptureFixture f;
    Capture c{f.Hash()};
    SECTION("Existing destination") {
        std::ofstream{f.file} << "keep";
        REQUIRE(c.Try(f.file, f.entry, &f, f.image, f.checked) == Result::Failed);
        REQUIRE(file_size(f.file) == 4);
        REQUIRE_FALSE(exists(f.file.string() + ".partial"));
    }
    SECTION("Existing temporary path") {
        std::ofstream{f.file.string() + ".partial"} << "keep";
        REQUIRE(c.Try(f.file, f.entry, &f, f.image, f.checked) == Result::Failed);
        REQUIRE_FALSE(exists(f.file));
        REQUIRE(file_size(f.file.string() + ".partial") == 4);
    }
    SECTION("Absent output directory") {
        auto absent = f.directory / "absent" / "code";
        REQUIRE(c.Try(absent, f.entry, &f, f.image, f.checked) == Result::Failed);
        REQUIRE_FALSE(exists(absent));
        REQUIRE_FALSE(exists(absent.string() + ".partial"));
    }
    Capture::CheckedImage another{};
    REQUIRE(c.Try(f.file, f.entry, &f, f.image, another) == Result::Ignored);
}

TEST_CASE("Concurrent matching MME captures publish exactly one completed image",
          "[mme_image_capture]") {
    CaptureFixture f;
    Capture c{f.Hash()};
    std::atomic<unsigned> count{};
    std::vector<std::jthread> workers;
    for (unsigned i = 0; i < 8; ++i) {
        workers.emplace_back([&] {
            Capture::CheckedImage local{};
            if (c.Try(f.file, f.entry, &f, f.image, local) == Result::Saved)
                ++count;
        });
    }
    workers.clear();
    REQUIRE(count == 1);
    REQUIRE(file_size(f.file) == 64 + 128 * 1024);
    REQUIRE_FALSE(exists(f.file.string() + ".partial"));
}
