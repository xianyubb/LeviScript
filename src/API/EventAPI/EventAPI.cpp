#include "EventAPI.h"

#include "ll/api/Logger.h"
#include "ll/api/event/EventBus.h"
#include "ll/api/event/ListenerBase.h"
#include "ll/api/event/server/ServerStartedEvent.h"
#include "ll/api/utils/HashUtils.h"
#include <vector>


std::map<std::string, std::vector<McEventData>> eventListener;


Local<Value> McEventClass::listen(const Arguments& args) {
    try {
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

void EnableListener() {
    auto& eventBus = ll::event::EventBus::getInstance();
    for (auto& pair : eventListener) {
        switch (ll::hash_utils::doHash(pair.first)) {
        case ll::hash_utils::doHash("onServerStarted"): {
            const ll::event::ListenerPtr eventPtr =
                eventBus.emplaceListener<ll::event::ServerStartedEvent>([&](ll::event::ServerStartedEvent&) {
                    for (auto& data : pair.second) {
                        try {
                            EngineScope entry(data.engine);
                            data.Callback.get().call({});
                            data.listener = eventPtr;
                        } catch (const Exception& e) {}
                    }
                });
        }
        }
    }
}

bool addlistener(const std::string& eventName, ScriptEngine* engine, const Local<Function>& func) {
    auto& data = eventListener[eventName];
    data.emplace_back(McEventData{engine, func});
    EnableListener();
    return true;
};

bool removeListener(const std::string& eventName, ScriptEngine* engine) {
    const auto it = eventListener.find(eventName);
    if (it != eventListener.end()) {
        for (const auto& data : it->second) {
            if (data.engine == engine) {
                ll::event::EventBus::getInstance().removeListener(data.listener);
                eventListener.erase(it);
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