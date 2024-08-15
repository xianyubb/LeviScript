#pragma once

#include "ll/api/event/ListenerBase.h"
#include "utils/UsingScriptX.h"

#include <unordered_map>
#include <vector>


bool addlistener(const std::string& eventName, ScriptEngine* engine, const Local<Function>& func);

bool removeListener(const std::string& eventName,ScriptEngine* engine);

struct McEventData {
    McEventData(ScriptEngine* engine, const Local<Function>& callback) : engine(engine), Callback(callback){};
    ScriptEngine*    engine;
    Global<Function> Callback;
    ll::event::ListenerPtr listener;
};

extern std::unordered_map<std::string, std::vector<McEventData>> eventAfterServerStartedListener;
extern std::unordered_map<std::string, std::vector<McEventData>> eventBeforeServerStartedListener;


class McEventClass : public script::ScriptClass {
public:
    static Local<Value> listen(const Arguments& args);

    static Local<Value> RemoveListener(const Arguments& args);
};


void EnableBeforeServerStartedListener();

void EnableAfterServerStartedListener();

    extern script::ClassDefine<McEventClass>
        McEventClassBuilder;
