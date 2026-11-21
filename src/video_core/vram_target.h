// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "common/common_types.h"
#include "common/settings_enums.h"

namespace VideoCommon {

/// Device memory the emulator aims to stay under for `mode`, given the total VRAM of the GPU.
/// 0 means no target: the caches follow the driver's budget alone (Aggressive mode).
/// Conservative dynamically targets 85% of total GPU VRAM (~6.95 GB on an 8 GB card,
/// ~10.4 GB on a 12 GB card, ~13.9 GB on a 16 GB card).
[[nodiscard]] constexpr u64 VramTarget(Settings::VramUsageMode mode, u64 total_gpu_vram) {
    switch (mode) {
    case Settings::VramUsageMode::Aggressive:
        return 0;
    case Settings::VramUsageMode::Conservative:
    default:
        return total_gpu_vram * 85 / 100;
    }
}

} // namespace VideoCommon
