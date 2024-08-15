#include "PlayerAPI.h"
#include "utils/UsingScriptX.h"


PlayerClass::PlayerClass(Player* player) : ScriptClass(ScriptClass::ConstructFromCpp<PlayerClass>{}) { set(player); }

Local<Object> PlayerClass::newPlayer(Player* player) {
    auto newp = new PlayerClass(player);
    return newp->getScriptObject();
}

Player* PlayerClass::extract(Local<Value> v) {
    if (EngineScope::currentEngine()->isInstanceOf<PlayerClass>(v))
        return EngineScope::currentEngine()->getNativeInstance<PlayerClass>(v)->get();
    else return nullptr;
}

void PlayerClass::set(Player* player) {
    try {
        if (player) {
            mWeakEntity = player->getWeakEntity();
            mValid      = true;
        }
    } catch (...) {
        mValid = false;
    }
}

Player* PlayerClass::get() {
    if (mValid) {
        return mWeakEntity.tryUnwrap<Player>().as_ptr();
    } else {
        return nullptr;
    }
}

Local<Value> PlayerClass::getRealName(const Arguments&) {
    auto player = get();
    if (player) return String::newString(player->getRealName());
    else return String::newString("");
}

ClassDefine<PlayerClass> PlayerClassBuilder = defineClass<PlayerClass>("Player")
    .constructor(nullptr)
    .instanceFunction("getRealName", &PlayerClass::getRealName).build();
