// SPDX-License-Identifier: MIT
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

#include "mc/platform/UUID.h"

namespace tick_command {

struct TickState {
    float                    rate               = 20.0f;
    bool                     frozen             = false;
    int64_t                  stepRemaining      = 0;
    int64_t                  sprintRemaining    = 0;
    bool                     frozenBeforeSprint = false;
    double                   credit             = 0.0;
    int64_t                  sprintTicks        = 0;
    double                   sprintTotalMs      = 0.0;
    std::array<double, 100>  samples{};
    size_t                   sampleNext  = 0;
    size_t                   sampleCount = 0;
    std::optional<mce::UUID> sprintOwner;
    int64_t                  gatedTicks = 0;
};

TickState& tickState();
void       resetTickState();
void finishSprint();

std::optional<int64_t> parseTime(std::string_view text);

void installTickHooks();

}
