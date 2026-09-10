#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "native/NativeApi.h"
#include "script/Local.h"
#include "script/ScriptEngine.h"
#include "script/bind/Bind.h"

// ---------------------------------------------------------------------------
// A small native class hierarchy used to demonstrate - and unit test - the
// inheritance-capable class binder. Real Minecraft/LeviLamina bindings are
// generated the exact same way by tools/autobind (see the generator).
// ---------------------------------------------------------------------------
namespace ls::native::mcsample {

class Entity {
public:
    Entity()          = default;
    virtual ~Entity() = default;

    [[nodiscard]] virtual std::string typeName() const { return "Entity"; }

    [[nodiscard]] std::string const& name() const { return mName; }
    void                             setName(std::string name) { mName = std::move(name); }

    [[nodiscard]] int  id() const { return mId; }
    void               setId(int id) { mId = id; }

    [[nodiscard]] std::vector<float> position() const { return {mX, mY, mZ}; }
    void                             setPosition(double x, double y, double z) {
        mX = static_cast<float>(x);
        mY = static_cast<float>(y);
        mZ = static_cast<float>(z);
    }

private:
    std::string mName = "entity";
    int         mId   = 0;
    float       mX    = 0.0F;
    float       mY    = 0.0F;
    float       mZ    = 0.0F;
};

class LivingEntity : public Entity {
public:
    [[nodiscard]] std::string typeName() const override { return "LivingEntity"; }

    [[nodiscard]] float health() const { return mHealth; }
    void                setHealth(float health) { mHealth = health; }
    void                hurt(float amount) {
        mHealth -= amount;
        if (mHealth < 0.0F) {
            mHealth = 0.0F;
        }
    }
    [[nodiscard]] bool isAlive() const { return mHealth > 0.0F; }

private:
    float mHealth = 20.0F;
};

class Player : public LivingEntity {
public:
    [[nodiscard]] std::string typeName() const override { return "Player"; }

    [[nodiscard]] std::string const& uuid() const { return mUuid; }
    void                             setUuid(std::string uuid) { mUuid = std::move(uuid); }

    void                             sendMessage(std::string const& message) { mLastMessage = message; }
    [[nodiscard]] std::string const& lastMessage() const { return mLastMessage; }

    [[nodiscard]] int  level() const { return mLevel; }
    void               setLevel(int level) { mLevel = level; }

private:
    std::string mUuid = "00000000-0000-0000-0000-000000000000";
    std::string mLastMessage;
    int         mLevel = 0;
};

} // namespace ls::native::mcsample

// Declare the classes to the binding framework (must be at global scope).
LS_NATIVE_CLASS(::ls::native::mcsample::Entity)
LS_NATIVE_CLASS(::ls::native::mcsample::LivingEntity)
LS_NATIVE_CLASS(::ls::native::mcsample::Player)

namespace ls::native {

using ls::native::mcsample::Entity;
using ls::native::mcsample::LivingEntity;
using ls::native::mcsample::Player;
using ls::script::ClassBinder;
using ls::script::getGlobal;
using ls::script::Local;
using ls::script::makeFunction;
using ls::script::makeNativeFunction;
using ls::script::makeObject;
using ls::script::Object;
using ls::script::ScriptEngine;

void bindMcApi(ScriptEngine& engine) {
    Local<Object> global = getGlobal(engine);
    Local<Object> mc     = makeObject(engine);

    // -- Entity (root) ------------------------------------------------------
    ClassBinder::registerClass<Entity>(engine, "Entity");
    ClassBinder::method<Entity>(engine, "getTypeName", &Entity::typeName);
    ClassBinder::method<Entity>(engine, "getName", &Entity::name);
    ClassBinder::method<Entity>(engine, "setName", &Entity::setName);
    ClassBinder::method<Entity>(engine, "getPosition", &Entity::position);
    ClassBinder::method<Entity>(engine, "setPosition", &Entity::setPosition);
    ClassBinder::property<Entity>(engine, "id", &Entity::id, &Entity::setId);
    ClassBinder::constructor<Entity>(engine, +[]() -> Entity* { return new Entity(); });

    // -- LivingEntity : Entity ---------------------------------------------
    ClassBinder::registerClass<LivingEntity, Entity>(engine, "LivingEntity");
    ClassBinder::method<LivingEntity>(engine, "hurt", &LivingEntity::hurt);
    ClassBinder::method<LivingEntity>(engine, "isAlive", &LivingEntity::isAlive);
    ClassBinder::property<LivingEntity>(engine, "health", &LivingEntity::health, &LivingEntity::setHealth);
    ClassBinder::constructor<LivingEntity>(engine, +[]() -> LivingEntity* { return new LivingEntity(); });

    // -- Player : LivingEntity : Entity ------------------------------------
    ClassBinder::registerClass<Player, LivingEntity>(engine, "Player");
    ClassBinder::method<Player>(engine, "sendMessage", &Player::sendMessage);
    ClassBinder::method<Player>(engine, "getLastMessage", &Player::lastMessage);
    ClassBinder::property<Player>(engine, "uuid", &Player::uuid, &Player::setUuid);
    ClassBinder::property<Player>(engine, "level", &Player::level, &Player::setLevel);
    ClassBinder::constructor<Player>(engine, +[]() -> Player* { return new Player(); });
    // Static factory returning an owned instance.
    ClassBinder::staticMethod<Player>(engine, "create", +[](std::string name) -> std::unique_ptr<Player> {
        auto player = std::make_unique<Player>();
        player->setName(std::move(name));
        return player;
    });

    // Expose the constructors on the `mc` namespace.
    ClassBinder::expose<Entity>(engine, mc.handle(), "Entity");
    ClassBinder::expose<LivingEntity>(engine, mc.handle(), "LivingEntity");
    ClassBinder::expose<Player>(engine, mc.handle(), "Player");

    // A free function that accepts the *base* type: passing a Player instance
    // here exercises the inheritance-aware unwrap + pointer adjustment, and the
    // virtual call resolves to the most derived override.
    mc.setProperty(
        "describe",
        makeFunction(engine, makeNativeFunction([](Entity& entity) -> std::string {
                         return entity.typeName() + "(name=" + entity.name() + ", id=" + std::to_string(entity.id()) +
                                ")";
                     }))
    );

    global.setProperty("mc", mc);
}

} // namespace ls::native
