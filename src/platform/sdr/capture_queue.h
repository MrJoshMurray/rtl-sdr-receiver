/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <utility>
#include <vector>

namespace platform::sdr {

// The caller synchronizes access between the reader and receiver threads.
class CaptureQueue {
public:
    struct Chunk {
        std::vector<uint8_t> bytes;
        uint64_t generation;
        uint64_t target_hz;
        int offset_hz;
        bool direct;
    };

    uint64_t generation() const { return generation_; }
    bool empty() const { return chunks_.empty(); }

    void retune(bool retain_audio) {
        ++generation_;
        if (!retain_audio) minimum_generation_ = generation_;
        const size_t keep = retain_audio ? 4 : 0;
        while (chunks_.size() > keep) chunks_.pop_front();
    }

    void push(const uint8_t* data, size_t size, uint64_t generation,
              uint64_t target_hz, int offset_hz, bool direct) {
        if (generation >= minimum_generation_ && chunks_.size() < 64)
            chunks_.push_back({std::vector<uint8_t>(data, data + size), generation,
                               target_hz, offset_hz, direct});
    }

    Chunk pop() {
        Chunk chunk = std::move(chunks_.front());
        chunks_.pop_front();
        return chunk;
    }

private:
    uint64_t generation_{0};
    uint64_t minimum_generation_{0};
    std::deque<Chunk> chunks_;
};

} // namespace platform::sdr
