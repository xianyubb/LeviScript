#include "engine/GlobalShareData.h"
#include "engine/LocalShareData.h"
#include "mod/LeviScript.h"
// #include "main/Configs.h"

#include <Windows.h>


// 全局共享数据
GlobalDataType* globalShareData;

void InitGlobalShareData() {
    HANDLE hGlobalData = CreateFileMapping(
        INVALID_HANDLE_VALUE,
        nullptr,
        PAGE_READWRITE,
        0,
        sizeof(GlobalDataType),
        (L"LS_GLOBAL_DATA_SECTION" + std::to_wstring(GetCurrentProcessId())).c_str()
    );
    if (hGlobalData == nullptr) {
        levi_script::LeviScript::getInstance().getSelf().getLogger().error("初始化文件映射失败");
        localShareData->isFirstInstance = true;
        return;
    }

    LPVOID address = MapViewOfFile(hGlobalData, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, 0);
    if (address == nullptr) {
        levi_script::LeviScript::getInstance().getSelf().getLogger().error("初始化文件映射失败");
        localShareData->isFirstInstance = true;
        return;
    }

    if (GetLastError() != ERROR_ALREADY_EXISTS) {
        // First Time
        localShareData->isFirstInstance = true;
        globalShareData                 = new (address) GlobalDataType;
    } else {
        // Existing
        localShareData->isFirstInstance = false;
        globalShareData                 = (GlobalDataType*)address;
    }
}