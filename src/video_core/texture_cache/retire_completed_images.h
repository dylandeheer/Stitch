// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

namespace VideoCommon {

template <class Queue, class Runtime, class Bytes>
void RetireCompletedImages(Queue& queue, Runtime& runtime, Bytes& sentenced_bytes,
                          Bytes view_rings_clear_by) {
    while (!queue.empty() && runtime.IsFree(queue.front().tick)) {
        auto& entry = queue.front();
        if constexpr (requires { entry.image.RecycleRetiredTarget(view_rings_clear_by); }) {
            entry.image.RecycleRetiredTarget(view_rings_clear_by);
        }
        sentenced_bytes -= entry.bytes;
        queue.pop_front();
    }
}

} // namespace VideoCommon
