// LeviScript ES module demo plugin.
//
// Demonstrates: ES module entry (.mjs) + `import`, native class inheritance
// (Entity <- LivingEntity <- Player), accessor properties, static factories,
// timers, logging and cross-plugin function export.

import { add, VERSION } from "./lib/math.mjs";

logger.info(`esm-demo loading (ES module entry), math lib v${VERSION}`);

// --- native class inheritance -------------------------------------------
const player = new mc.Player();
player.setName("Steve");
player.id = 42;          // accessor property declared on Entity (inherited)
player.health = 12.5;    // accessor property declared on LivingEntity (inherited)
player.sendMessage("hello world");

logger.info("add(2, 3) =", add(2, 3));
// describe() takes the *base* type; passing a Player exercises the
// inheritance-aware unwrap + pointer adjustment, and the virtual call resolves
// to the most derived override.
logger.info("mc.describe(player) =", mc.describe(player));

logger.info("player instanceof mc.Player      =", player instanceof mc.Player);
logger.info("player instanceof mc.LivingEntity=", player instanceof mc.LivingEntity);
logger.info("player instanceof mc.Entity      =", player instanceof mc.Entity);

logger.info("player.getTypeName() =", player.getTypeName()); // overridden virtual
logger.info("player.getName()     =", player.getName());     // inherited from Entity
logger.info("player.isAlive()     =", player.isAlive());     // inherited from LivingEntity

player.hurt(3.0);
logger.info("health after hurt(3) =", player.health);

// static factory returning an owned instance (deleted when GC'd)
const alex = mc.Player.create("Alex");
logger.info("created player:", alex.getName(), "uuid:", alex.uuid, "level:", alex.level);

// --- timers --------------------------------------------------------------
setTimeout(() => logger.info("setTimeout(1000) fired"), 1000);
const interval = setInterval(() => logger.info("interval tick @", systemTimeMillis()), 2000);
setTimeout(() => {
    clearInterval(interval);
    logger.info("interval cleared");
}, 6500);

// --- universal pointer system (smart-pointer backed, leak free) ----------
const buf = NativePointer.alloc(16);   // 16 owned bytes, freed automatically on GC
buf.setInt32(0, 0x12345678);
buf.setFloat(4, 3.5);
buf.setString(8, "hi");
logger.info("buf.getInt32(0) = 0x" + buf.getInt32(0).toString(16));
logger.info("buf.getFloat(4) =", buf.getFloat(4));
logger.info("buf.getString(8) =", buf.getString(8));

const view = buf.add(4);               // derived pointer shares buf's ownership
logger.info("view.getFloat(0) =", view.getFloat(0), "remaining size =", view.size);
logger.info("buf.address() =", buf.address(), "owned =", buf.isOwned());

// Borrow the memory of a native object and reinterpret it back as its class.
const playerPtr = NativePointer.of(player);
const roundTrip = playerPtr.as(mc.Player);
logger.info("pointer round-trip getName() =", roundTrip.getName());

// --- real Minecraft header binding (mc/world/level/BlockPos.h) -------------
// Auto-generated with closure: BlockPos plus Vec3 / Vec2 / intN3<BlockPos> /
// floatN3<Vec3> / floatN2<Vec2> were discovered and bound transitively. The MCAPI
// methods resolve at link/run time through LeviLamina's bedrock symbol provider.
const zero = mc.BlockPos.ZERO();
logger.info("mc.BlockPos.ZERO().toString() =", zero.toString());
logger.info("mc.BlockPos.MAX().toString()  =", mc.BlockPos.MAX().toString());
logger.info("zero.east().toString() (by-value return) =", zero.east().toString());
// inheritance is reflected: BlockPos : intN3<BlockPos> (CRTP base), so the
// prototype chain makes a BlockPos an instance of its bound base too.
logger.info("zero instanceof mc.BlockPos      =", zero instanceof mc.BlockPos);
logger.info("zero instanceof mc.intN3BlockPos =", zero instanceof mc.intN3BlockPos);

// --- lifecycle hooks (modules must attach them to globalThis) ------------
globalThis.onEnable = () => logger.info("esm-demo enabled");
globalThis.onDisable = () => logger.info("esm-demo disabled");
globalThis.onUnload = () => logger.info("esm-demo unloading");

// --- export a function so other plugins can import it --------------------
ll.exportFunc((a, b) => a * b, "multiply", "math");
logger.info("exported math::multiply");
