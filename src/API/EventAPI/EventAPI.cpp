#include "API/EventAPI/EventAPI.h"
#include "API/PlayerAPI/PlayerAPI.h"
#include "engine/EngineOwnData.h"
#include "mod/LeviScript.h"



#include "ll/api/Logger.h"
#include "ll/api/event/EventBus.h"
#include "ll/api/event/ListenerBase.h"
#include "ll/api/event/player/PlayerJoinEvent.h"
#include "ll/api/event/server/ServerStartedEvent.h"
#include "ll/api/utils/HashUtils.h"

#include <vector>


std::unordered_map<std::string, std::vector<McEventData>> eventAfterServerStartedListener;
std::unordered_map<std::string, std::vector<McEventData>> eventBeforeServerStartedListener;


Local<Value> McEventClass::listen(const Arguments& args) {
    try {
        levi_script::LeviScript::getInstance().getSelf().getLogger().info(
            "开始监听事件: {}",
            args[0].asString().toString()
        );
        return Boolean::newBoolean(
            addlistener(args[0].asString().toString(), EngineScope::currentEngine(), args[1].asFunction())
        );
    } catch (...) {
        return Boolean::newBoolean(false);
    }
}

Local<Value> McEventClass::RemoveListener(const Arguments& args) {
    try {
        return Boolean::newBoolean(removeListener(args[0].asString().toString(), EngineScope::currentEngine()));
    } catch (...) {
        return Boolean::newBoolean(false);
    }
};

void EnableBeforeServerStartedListener() {
    auto& eventBus = ll::event::EventBus::getInstance();
    for (auto& pair : eventBeforeServerStartedListener) {
        switch (ll::hash_utils::doHash(pair.first)) {
        case ll::hash_utils::doHash("onServerStarted"): {
             ll::event::ListenerPtr eventPtr =
                eventBus.emplaceListener<ll::event::ServerStartedEvent>([&](ll::event::ServerStartedEvent&) {
                    for (auto& data : pair.second) {
                        try {
                            EngineScope entry(data.engine);
                            data.Callback.get().call({});
                            data.listener = eventPtr;
                        } catch (const Exception& e) {
                            ENGINE_OWN_DATA()->logger.error("监听事件错误: {} \n{}", "onServerStarted", e.what());
                        }
                    }
                });
            break;
        }
        }
    }
}

void EnableAfterServerStartedListener() {
    auto& eventBus = ll::event::EventBus::getInstance();
    for (auto& pair : eventAfterServerStartedListener) {
        switch (ll::hash_utils::doHash(pair.first)) {
        case ll::hash_utils::doHash("onPlayerJoin"): {
             ll::event::ListenerPtr eventPtr =
                eventBus.emplaceListener<ll::event::PlayerJoinEvent>([&](ll::event::PlayerJoinEvent& ev) {
                    for (auto& data : pair.second) {
                        try {
                            EngineScope entry(data.engine);
                            data.Callback.get().call({}, PlayerClass::newPlayer(&ev.self()));
                            data.listener = eventPtr;
                        } catch (const Exception& e) {
                            ENGINE_OWN_DATA()->logger.error("监听事件错误: {} \n{}", "onPlayerJoin", e.what());
                        }
                    }
                });
            break;
        }
        }
    }
}

bool addlistener(const std::string& eventName, ScriptEngine* engine, const Local<Function>& func) {
    switch (ll::hash_utils::doHash(eventName)) {
    case ll::hash_utils::doHash("onServerStarted"): {
        if (eventBeforeServerStartedListener.find(eventName) == eventBeforeServerStartedListener.end()) {
            eventBeforeServerStartedListener[eventName] = std::vector<McEventData>();
        }
        eventBeforeServerStartedListener[eventName].push_back(McEventData(engine, func));
        break;
    }
    default: {
        if (eventAfterServerStartedListener.find(eventName) == eventAfterServerStartedListener.end()) {
            eventAfterServerStartedListener[eventName] = std::vector<McEventData>();
        }
        eventAfterServerStartedListener[eventName].push_back(McEventData(engine, func));
        break;
    }
    }

    // EnableListener();
    return true;
};


bool removeListener(const std::string& eventName, ScriptEngine* engine) {
    auto it = eventBeforeServerStartedListener.find(eventName);
    if (it != eventBeforeServerStartedListener.end()) {
        for (const auto& data : it->second) {
            if (data.engine == engine) {
                ll::event::EventBus::getInstance().removeListener(data.listener);
                eventBeforeServerStartedListener.erase(it);
                return true;
            }
        }
    }
    auto it_ = eventAfterServerStartedListener.find(eventName);
    if (it_ != eventAfterServerStartedListener.end()) {
        for (const auto& data : it_->second) {
            if (data.engine == engine) {
                ll::event::EventBus::getInstance().removeListener(data.listener);
                eventAfterServerStartedListener.erase(it_);
                return true;
            }
        }
    }
    return false;
}

script::ClassDefine<McEventClass> McEventClassBuilder = defineClass<McEventClass>("McEvent")
                                                            .function("listen", &McEventClass::listen)
                                                            .function("removeListener", &McEventClass::RemoveListener)
                                                            .build();