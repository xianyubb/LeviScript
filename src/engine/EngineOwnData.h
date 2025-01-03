#pragma once
#include "utils/UsingScriptX.h"

#include "dyncall/dyncall.h"

#include <string>
#include <unordered_map>

#include "ll/api/Logger.h"


struct FormCallbackData {
    script::ScriptEngine*            engine;
    script::Global<script::Function> func;
};

struct RemoteCallData {
    std::string              nameSpace;
    std::string              funcName;
    script::Global<Function> callback;
};

/*
struct SimpleCallbackData
{
    ScriptEngine* engine;
    script::Global<script::Function> func;
    std::vector<script::Global<Value>> values;
};
*/

class Player;

struct EngineOwnData {
    // 基础信息
    std::string modName;
    std::string modFileOrDirPath;
    std::string engineType = "JS";

    // 表单回调
    // std::map<unsigned, FormCallbackData> formCallbacks;

    // RemoteCall Exported Functions: unordered_map<nameSpace, funcName>
    // std::unordered_map<std::string, RemoteCallData> exportFuncs;

    /*
    uint64_t simpleCallbackIndex = 0;
    std::unordered_map<uint64_t, SimpleCallbackData> simpleCallbacks;

    inline uint64_t addSimpleCallback(script::Local<Function> func,
    std::vector<script::Local<Value>> values)
    {
        auto index = ++simpleCallbackIndex;
        std::vector<script::Global<Value>> globalValues;
        for (auto& value : values)
            globalValues.emplace_back(value);
        SimpleCallbackData data{EngineScope::currentEngine(),
    script::Global<Function>(func), std::move(globalValues)};
        simpleCallbacks.emplace(index, std::move(data));
        return index;
    }
    inline bool removeSimpleCallback(uint64_t index)
    {
        return simpleCallbacks.erase(index);
    }
    */

    // I18nAPI
    // ll::i18n::I18N* i18n = nullptr;

    // LoggerAPI
    ll::Logger logger      = ll::Logger("");
    int        maxLogLevel = 4;

    // 玩家绑定数据
    // std::unordered_map<std::string, script::Global<Value>> playerDataDB;

    // Dynamic Call vm for NativeFFI
    DCCallVM* dynamicCallVM;

    // Unload Callbacks, use for close database...
    int                                                           index = 0;
    std::unordered_map<int, std::function<void(UniqueEnginePtr)>> unloadCallbacks;
    inline int addUnloadCallback(std::function<void(UniqueEnginePtr)>&& cb) {
        unloadCallbacks[++index] = cb;
        return index;
    }
    inline bool removeUnloadCallback(int _index) { return unloadCallbacks.erase(_index); }

    // Init
    EngineOwnData() {
        dynamicCallVM = dcNewCallVM(4096);
        dcMode(dynamicCallVM, DC_CALL_C_DEFAULT);
    }
};

// 引擎附加数据
#define ENGINE_GET_DATA(e) (std::static_pointer_cast<EngineOwnData>((e)->getData()))
#define ENGINE_OWN_DATA()  (std::static_pointer_cast<EngineOwnData>(EngineScope::currentEngine()->getData()))
