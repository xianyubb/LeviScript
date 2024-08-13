#pragma once

#include "ll/api/event/ListenerBase.h"
#include "utils/UsingScriptX.h"

#include <map>
#include <vector>


bool addlistener(const std::string& eventName, ScriptEngine* engine, const Local<Function>& func);

bool removeListener(const std::string& eventName,ScriptEngine* engine);

struct McEventData {
    McEventData(ScriptEngine* engine, const Local<Function>& callback) : engine(engine), Callback(callback){};
    ScriptEngine*    engine;
    Global<Function> Callback;
    ll::event::ListenerPtr listener;
};

extern std::map<std::string, std::vector<McEventData>> eventListener;

class McEventClass : public script::ScriptClass {
public:
    static Local<Value> listen(const Arguments& args);

    static Local<Value> RemoveListener(const Arguments& args);
};



void EnableListener();

extern script::ClassDefine<McEventClass> McEventClassBuilder;

