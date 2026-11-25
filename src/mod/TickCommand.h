// SPDX-License-Identifier: MIT
#pragma once

#include "ll/api/event/ListenerBase.h"
#include "ll/api/mod/NativeMod.h"

class CommandRegistry;

namespace tick_command {

class TickCommand {

public:
    static TickCommand& getInstance();

    TickCommand() : mSelf(*ll::mod::NativeMod::current()) {}

    [[nodiscard]] ll::mod::NativeMod& getSelf() const { return mSelf; }

    bool load();

    bool enable();

    bool disable();
private:
    void tryRegisterCommand();

    ll::mod::NativeMod&    mSelf;
    ll::event::ListenerPtr mServerStarted;
    ll::event::ListenerPtr mServerStopping;
    CommandRegistry*       mRegisteredTo = nullptr;
};

}
