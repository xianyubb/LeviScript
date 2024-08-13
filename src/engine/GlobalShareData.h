#pragma once

#include "utils/UsingScriptX.h"

// #include <memory>
#include <shared_mutex>
#include <vector>
#include <unordered_map>


// 全局共享数据
struct GlobalDataType {
    // 引擎管理器表
    std::shared_mutex        engineVectorLock;
    std::vector<ScriptEngine*> globalEngineVector;

    // 注册过的命令
    std::unordered_map<std::string, std::string> playerRegisteredCmd;
    std::unordered_map<std::string, std::string> consoleRegisteredCmd;

    // 导出函数表 (暂时无需)
    // std::unordered_map<std::string, ExportedFuncData> exportedFuncs;


    // 模块消息系统 (暂时无需)
    // int                                    messageSystemNextId = 0;
    // std::map<std::string, MessageHandlers> messageSystemHandlers;
    // std::map<std::string, HANDLE>          messageThreads;

    // OperationCount (暂时无需)
    // std::map<std::string, int> operationCountData;
};

//////////////////// Externs ////////////////////

// 全局共享数据
extern GlobalDataType* globalShareData;

//////////////////// APIs ////////////////////

void InitGlobalShareData();