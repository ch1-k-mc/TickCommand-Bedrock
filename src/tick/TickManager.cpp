// SPDX-License-Identifier: MIT
#include "tick/TickManager.h"

#include "lang/Lang.h"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <functional>

#include "ll/api/memory/Hook.h"
#include "ll/api/mod/NativeMod.h"
#include "ll/api/service/Bedrock.h"

#include "mc/server/ServerLevel.h"
#include "mc/world/Minecraft.h"
#include "mc/world/actor/Actor.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/BlockTickingQueue.h"
#include "mc/world/level/Level.h"
#include "mc/world/level/Tick.h"
#include "mc/world/level/chunk/LevelChunk.h"
#include "mc/world/level/dimension/Dimension.h"

namespace tick_command {

namespace {

using Clock = std::chrono::steady_clock;

constexpr auto kSprintBudget = std::chrono::milliseconds(45);

TickState state;

thread_local bool inExtraTick = false;
thread_local bool inGatedTick = false;

template <class Fn>
void timedWorldTick(Fn&& origin) {
    auto begin = Clock::now();
    origin();
    double ms                       = std::chrono::duration<double, std::milli>(Clock::now() - begin).count();
    state.samples[state.sampleNext] = ms;
    state.sampleNext                = (state.sampleNext + 1) % state.samples.size();
    state.sampleCount               = std::min(state.sampleCount + 1, state.samples.size());
    if (state.sprintRemaining > 0) {
        state.sprintTotalMs += ms;
        ++state.sprintTicks;
    }
}

template <class Fn>
void gatedTick(ServerLevel& level, Fn&& origin) {
    int const time = level.getTime();
    inGatedTick    = true;
    origin();
    inGatedTick = false;
    level.setTime(time);
    ++state.gatedTicks;
}

}

TickState& tickState() { return state; }

void resetTickState() { state = TickState{}; }

void finishSprint() {
    state.sprintRemaining = 0;
    state.frozen          = state.frozenBeforeSprint;
    if (state.sprintTicks == 0) return;
    double const msPerTick = state.sprintTotalMs / static_cast<double>(state.sprintTicks);
    auto const   tps       = fixed(msPerTick > 0.0 ? 1000.0 / msPerTick : 0.0);
    auto const   ms        = fixed(msPerTick);
    Player*      owner     = nullptr;
    if (state.sprintOwner) {
        if (auto level = ll::service::getLevel()) owner = level->getPlayer(*state.sprintOwner);
    }
    if (owner) owner->sendMessage(translate(owner->getLocaleCode(), "tickcommand.sprint.report", {tps, ms}));
    else ll::mod::NativeMod::current()->getLogger().info(translate("en_us", "tickcommand.sprint.report", {tps, ms}));
}

std::optional<int64_t> parseTime(std::string_view text) {
    if (text.empty()) return std::nullopt;
    double scale = 1.0;
    switch (text.back()) {
    case 't':
        text.remove_suffix(1);
        break;
    case 's':
        scale = 20.0;
        text.remove_suffix(1);
        break;
    case 'd':
        scale = 24000.0;
        text.remove_suffix(1);
        break;
    default:
        break;
    }
    if (text.empty()) return std::nullopt;
    float value = 0.0f;
    auto  res   = std::from_chars(text.data(), text.data() + text.size(), value);
    if (res.ec != std::errc{} || res.ptr != text.data() + text.size()) return std::nullopt;
    auto ticks = static_cast<int64_t>(std::llround(value * scale));
    if (ticks < 1) return std::nullopt;
    return ticks;
}

LL_TYPE_INSTANCE_HOOK(TickGateHook, ll::memory::HookPriority::Lowest, ServerLevel, &ServerLevel::$_subTick, void) {
    if (inExtraTick) {
        origin();
        return;
    }
    if (state.sprintRemaining > 0) {
        timedWorldTick([&] { origin(); });
        if (--state.sprintRemaining == 0) finishSprint();
        return;
    }
    if (state.frozen) {
        if (state.stepRemaining > 0) {
            --state.stepRemaining;
            timedWorldTick([&] { origin(); });
        } else {
            gatedTick(*this, [&] { origin(); });
        }
        return;
    }
    state.credit += state.rate / 20.0;
    if (state.credit < 1.0) {
        gatedTick(*this, [&] { origin(); });
        return;
    }
    state.credit -= 1.0;
    timedWorldTick([&] { origin(); });
}

LL_TYPE_INSTANCE_HOOK(ExtraTickHook, ll::memory::HookPriority::Normal, Minecraft, &Minecraft::update, bool) {
    bool const result = origin();
    Level*     level  = getLevel();
    if (!level || level->isClientSide()) return result;
    if (state.sprintRemaining == 0 && state.credit < 1.0) return result;

    auto const deadline = Clock::now() + kSprintBudget;
    inExtraTick         = true;
    while (Clock::now() < deadline) {
        if (state.sprintRemaining > 0) {
            timedWorldTick([&] { level->tick(); });
            if (--state.sprintRemaining == 0) {
                finishSprint();
                break;
            }
        } else if (!state.frozen && state.credit >= 1.0) {
            state.credit -= 1.0;
            timedWorldTick([&] { level->tick(); });
        } else {
            break;
        }
    }
    inExtraTick = false;
    if (state.sprintRemaining == 0 && state.credit >= 1.0) state.credit = 0.0;
    return result;
}

LL_TYPE_INSTANCE_HOOK(ActorTickHook, ll::memory::HookPriority::Normal, Actor, &Actor::tick, bool, BlockSource& region) {
    bool const playerSide = isPlayer() || hasPlayerPassenger();
    if (inExtraTick && playerSide) return true;
    if (inGatedTick && !playerSide) return true;
    return origin(region);
}

LL_TYPE_INSTANCE_HOOK(
    ChunkTickHook,
    ll::memory::HookPriority::Normal,
    LevelChunk,
    &LevelChunk::tickImpl,
    void,
    BlockSource&          region,
    Tick const&           tick,
    std::function<void()> spawnerCallback
) {
    if (inGatedTick) {
        mLastTick->tickID = tick.tickID;
        return;
    }
    origin(region, tick, std::move(spawnerCallback));
}

LL_TYPE_INSTANCE_HOOK(
    BlockEntityTickHook,
    ll::memory::HookPriority::Normal,
    LevelChunk,
    &LevelChunk::tickBlockEntities,
    void,
    BlockSource& region
) {
    if (inGatedTick) return;
    origin(region);
}

LL_TYPE_INSTANCE_HOOK(
    PendingTickHook,
    ll::memory::HookPriority::Normal,
    BlockTickingQueue,
    &BlockTickingQueue::tickPendingTicks,
    bool,
    BlockSource& region,
    Tick const&  until,
    int          max,
    bool         instaTick
) {
    if (inGatedTick) return false;
    if (state.gatedTicks == 0 || region.getLevel().isClientSide()) return origin(region, until, max, instaTick);
    Tick shifted{};
    shifted.tickID = until.tickID - static_cast<uint64_t>(state.gatedTicks);
    return origin(region, shifted, max, instaTick);
}

LL_TYPE_INSTANCE_HOOK(RedstoneTickHook, ll::memory::HookPriority::Normal, Dimension, &Dimension::$tickRedstone, void) {
    if (inGatedTick) return;
    origin();
}

void installTickHooks() {
    TickGateHook::hook();
    ExtraTickHook::hook();
    ActorTickHook::hook();
    ChunkTickHook::hook();
    BlockEntityTickHook::hook();
    PendingTickHook::hook();
    RedstoneTickHook::hook();
}

}
