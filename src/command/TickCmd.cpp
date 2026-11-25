// SPDX-License-Identifier: MIT
#include "command/TickCmd.h"

#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

#include "ll/api/command/Command.h"
#include "ll/api/command/CommandHandle.h"
#include "ll/api/command/CommandRegistrar.h"

#include "mc/server/commands/CommandOrigin.h"
#include "mc/server/commands/CommandOriginType.h"
#include "mc/server/commands/CommandOutput.h"
#include "mc/server/commands/CommandPermissionLevel.h"
#include "mc/server/commands/CommandRawText.h"
#include "mc/world/actor/Actor.h"
#include "mc/world/actor/player/Player.h"

#include "lang/Lang.h"
#include "tick/TickManager.h"

namespace tick_command {

struct RateParam {
    float rate;
};
struct TimeParam {
    CommandRawText time;
};

namespace {

class Reply {
public:
    Reply(CommandOrigin const& origin, CommandOutput& output) : mOutput(output), mPlayer(nullptr) {
        Actor* entity = origin.getEntity();
        if (entity && entity->isPlayer()) {
            mPlayer = static_cast<Player*>(entity);
            mLocale = mPlayer->getLocaleCode();
        } else {
            mLocale = "en_us";
        }
    }

    void ok(std::string_view key, std::initializer_list<std::string> args = {}) {
        mOutput.success(translate(mLocale, key, args));
    }
    void fail(std::string_view key, std::initializer_list<std::string> args = {}) {
        mOutput.error(translate(mLocale, key, args));
    }
    [[nodiscard]] Player* player() const { return mPlayer; }

private:
    CommandOutput& mOutput;
    Player*        mPlayer;
    std::string    mLocale;
};

std::string_view trimmed(std::string_view text) {
    while (!text.empty() && text.front() == ' ') text.remove_prefix(1);
    while (!text.empty() && text.back() == ' ') text.remove_suffix(1);
    return text;
}

bool rejectCommandBlock(CommandOrigin const& origin, Reply& reply) {
    auto type = origin.getOriginType();
    if (type != CommandOriginType::CommandBlock && type != CommandOriginType::MinecartCommandBlock) return false;
    reply.fail("tickcommand.error.command_block");
    return true;
}

char const* statusKey() {
    auto const& s = tickState();
    if (s.sprintRemaining > 0) return "tickcommand.status.sprinting";
    if (s.frozen) return "tickcommand.status.frozen";
    if (s.sampleCount > 0) {
        double total = 0.0;
        for (size_t i = 0; i < s.sampleCount; ++i) total += s.samples[i];
        if (total / static_cast<double>(s.sampleCount) > 1000.0 / s.rate) return "tickcommand.status.lagging";
    }
    return "tickcommand.status.running";
}

void startStep(Reply& reply, int64_t ticks) {
    auto& s = tickState();
    if (!s.frozen || s.sprintRemaining > 0) {
        reply.fail("tickcommand.step.fail");
        return;
    }
    s.stepRemaining = ticks;
    reply.ok("tickcommand.step.success", {std::to_string(ticks)});
}

void startSprint(Reply& reply, int64_t ticks) {
    auto& s = tickState();
    if (s.sprintRemaining > 0) {
        finishSprint();
        reply.ok("tickcommand.sprint.stop.success");
    }
    s.frozenBeforeSprint = s.frozen;
    s.stepRemaining      = 0;
    s.sprintTicks        = 0;
    s.sprintTotalMs      = 0.0;
    s.sprintRemaining    = ticks + 1;
    s.sprintOwner.reset();
    if (Player* player = reply.player()) s.sprintOwner = player->getUuid();
    reply.ok("tickcommand.status.sprinting");
}

void stopStep(Reply& reply) {
    auto& s = tickState();
    if (s.stepRemaining == 0) {
        reply.fail("tickcommand.step.stop.fail");
        return;
    }
    s.stepRemaining = 0;
    reply.ok("tickcommand.step.stop.success");
}

void stopSprint(Reply& reply) {
    if (tickState().sprintRemaining == 0) {
        reply.fail("tickcommand.sprint.stop.fail");
        return;
    }
    finishSprint();
    reply.ok("tickcommand.sprint.stop.success");
}

void query(Reply& reply) {
    auto const& s = tickState();
    reply.ok(statusKey());

    std::vector<double> sorted(s.samples.begin(), s.samples.begin() + static_cast<std::ptrdiff_t>(s.sampleCount));
    std::sort(sorted.begin(), sorted.end());
    double average = 0.0;
    for (double ms : sorted) average += ms;
    if (!sorted.empty()) average /= static_cast<double>(sorted.size());

    if (s.sprintRemaining > 0) {
        reply.ok("tickcommand.query.rate.sprinting", {fixed(s.rate), fixed(average)});
    } else {
        reply.ok("tickcommand.query.rate.running", {fixed(s.rate), fixed(average), fixed(1000.0 / s.rate)});
    }
    if (sorted.empty()) return;
    auto percentile = [&](double p) {
        return fixed(sorted[static_cast<size_t>(p * static_cast<double>(sorted.size() - 1))]);
    };
    reply.ok(
        "tickcommand.query.percentiles",
        {percentile(0.50), percentile(0.95), percentile(0.99), std::to_string(sorted.size())}
    );
}

void freeze(Reply& reply) {
    auto& s = tickState();
    if (s.sprintRemaining > 0) finishSprint();
    s.stepRemaining = 0;
    s.frozen        = true;
    reply.ok("tickcommand.status.frozen");
}

void unfreeze(Reply& reply) {
    auto& s              = tickState();
    s.stepRemaining      = 0;
    s.frozen             = false;
    s.frozenBeforeSprint = false;
    reply.ok(s.sprintRemaining > 0 ? "tickcommand.status.sprinting" : "tickcommand.status.running");
}

template <class Fn>
auto guarded(Fn fn) {
    return [fn](CommandOrigin const& origin, CommandOutput& output) {
        Reply reply(origin, output);
        if (!rejectCommandBlock(origin, reply)) fn(reply);
    };
}

template <class Start, class Stop>
auto timed(Start start, Stop stop) {
    return [start, stop](CommandOrigin const& origin, CommandOutput& output, TimeParam const& param) {
        Reply reply(origin, output);
        if (rejectCommandBlock(origin, reply)) return;
        auto const text = trimmed(param.time.mText);
        if (text == "stop") stop(reply);
        else if (auto ticks = parseTime(text)) start(reply, *ticks);
        else reply.fail("tickcommand.error.time", {std::string(text)});
    };
}

}

void registerTickCommand() {
    auto& cmd = ll::command::CommandRegistrar::getInstance(false).getOrCreateCommand(
        "tick",
        "Controls the game tick rate (Java Edition syntax)",
        CommandPermissionLevel::GameDirectors
    );

    cmd.overload().text("query").execute(guarded([](Reply& r) { query(r); }));
    cmd.overload<RateParam>().text("rate").required("rate").execute(
        [](CommandOrigin const& origin, CommandOutput& output, RateParam const& p) {
            Reply reply(origin, output);
            if (rejectCommandBlock(origin, reply)) return;
            if (!(p.rate >= 1.0f && p.rate <= 10000.0f)) {
                reply.fail("tickcommand.error.rate");
                return;
            }
            tickState().rate = p.rate;
            reply.ok("tickcommand.rate.success", {fixed(p.rate)});
        }
    );
    cmd.overload().text("freeze").execute(guarded([](Reply& r) { freeze(r); }));
    cmd.overload().text("unfreeze").execute(guarded([](Reply& r) { unfreeze(r); }));
    cmd.overload().text("step").execute(guarded([](Reply& r) { startStep(r, 1); }));
    cmd.overload().text("step").text("stop").execute(guarded([](Reply& r) { stopStep(r); }));
    cmd.overload<TimeParam>().text("step").required("time").execute(timed(startStep, stopStep));
    cmd.overload().text("sprint").execute(guarded([](Reply& r) { startSprint(r, 1); }));
    cmd.overload().text("sprint").text("stop").execute(guarded([](Reply& r) { stopSprint(r); }));
    cmd.overload<TimeParam>().text("sprint").required("time").execute(timed(startSprint, stopSprint));
}

}
