// SPDX-License-Identifier: MIT
#include "mod/TickCommand.h"

#include "ll/api/event/EventBus.h"
#include "ll/api/event/server/ServerStartedEvent.h"
#include "ll/api/event/server/ServerStoppingEvent.h"
#include "ll/api/mod/RegisterHelper.h"
#include "ll/api/service/Bedrock.h"

#include "command/TickCmd.h"
#include "lang/Lang.h"
#include "tick/TickManager.h"

namespace tick_command {

TickCommand& TickCommand::getInstance() {
    static TickCommand instance;
    return instance;
}

void TickCommand::tryRegisterCommand() {
    auto registry = ll::service::getCommandRegistry(false);
    if (!registry || registry.as_ptr() == mRegisteredTo) return;
    mRegisteredTo = registry.as_ptr();
    registerTickCommand();
    getSelf().getLogger().info("/tick registered");
}

bool TickCommand::load() {
    getSelf().getLogger().debug("Loading...");
    loadLanguages(getSelf().getModDir() / "assets" / "lang");
    installTickHooks();
    return true;
}

bool TickCommand::enable() {
    getSelf().getLogger().debug("Enabling...");
    resetTickState();
    mServerStarted = ll::event::EventBus::getInstance().emplaceListener<ll::event::ServerStartedEvent>(
        [this](ll::event::ServerStartedEvent&) {
            resetTickState();
            tryRegisterCommand();
        }
    );
    tryRegisterCommand();
    mServerStopping = ll::event::EventBus::getInstance().emplaceListener<ll::event::ServerStoppingEvent>(
        [this](ll::event::ServerStoppingEvent&) {
            resetTickState();
            mRegisteredTo = nullptr;
        }
    );
    return true;
}

bool TickCommand::disable() {
    getSelf().getLogger().debug("Disabling...");
    ll::event::EventBus::getInstance().removeListener(mServerStarted);
    ll::event::EventBus::getInstance().removeListener(mServerStopping);
    resetTickState();
    return true;
}

}

LL_REGISTER_MOD(tick_command::TickCommand, tick_command::TickCommand::getInstance());
