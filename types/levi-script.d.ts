// Type declarations for the LeviScript built-in script API (QuickJS backend).
//
// These are ambient globals: reference this file from your plugin's tsconfig
// (`"types": []` + a /// <reference path=".../levi-script.d.ts" />`, or include the
// folder) to get autocomplete and type checking in both classic-script and ES
// module plugins.
//
// It reflects exactly what src/native binds today: timers, logger/console, the
// `ll` namespace, the universal `NativePointer` system, and the sample `mc`
// native class hierarchy (Entity <- LivingEntity <- Player).

// ---------------------------------------------------------------------------
// Lifecycle hooks (a plugin may define any of these; classic scripts declare
// them as top-level functions, ES modules assign them on globalThis).
// ---------------------------------------------------------------------------
declare var onLoad: (() => void) | undefined;
declare var onEnable: (() => void) | undefined;
declare var onDisable: (() => void) | undefined;
declare var onUnload: (() => void) | undefined;

// ---------------------------------------------------------------------------
// Timers (dispatched on the server thread)
// ---------------------------------------------------------------------------
declare function setTimeout(callback: () => void, delayMs?: number): number;
declare function setInterval(callback: () => void, intervalMs?: number): number;
declare function clearTimeout(id: number): boolean;
declare function clearInterval(id: number): boolean;
declare function setImmediate(callback: (...args: any[]) => void, ...args: any[]): number;
declare function clearImmediate(id: number): boolean;

/** Wall-clock milliseconds since the Unix epoch. */
declare function systemTimeMillis(): number;

// ---------------------------------------------------------------------------
// Logging
// ---------------------------------------------------------------------------
interface Logger {
    trace(...args: any[]): void;
    debug(...args: any[]): void;
    info(...args: any[]): void;
    warn(...args: any[]): void;
    error(...args: any[]): void;
    fatal(...args: any[]): void;
}

/** The plugin's own LeviLamina logger. */
declare var logger: Logger;

/** LSE-compatible logging shortcuts. */
declare function log(...args: any[]): void;
declare function logDebug(...args: any[]): void;
declare function colorLog(...args: any[]): void;

/** console aliased to the plugin logger. */
declare var console: {
    log(...args: any[]): void;
    info(...args: any[]): void;
    warn(...args: any[]): void;
    error(...args: any[]): void;
    debug(...args: any[]): void;
    trace(...args: any[]): void;
};

// ---------------------------------------------------------------------------
// ll namespace
// ---------------------------------------------------------------------------
interface LlVersion {
    major: number;
    minor: number;
    patch: number;
}

/** A function imported from another plugin (arguments are bridged through JSON). */
type ImportedFunc = (...args: any[]) => any;

declare namespace ll {
    /** LeviScript version. */
    function version(): LlVersion;
    /** Names of all currently loaded script plugins. */
    function listPlugins(): string[];
    /** Whether the engine runs in debug mode. */
    function isDebugMode(): boolean;

    /** CommonJS-style require, resolved relative to the plugin directory. */
    function require(spec: string): any;

    /** Export a function so other plugins can import it. */
    function exportFunc(fn: (...args: any[]) => any, name: string, namespace?: string): boolean;
    /** Import a function exported by another plugin; null when unavailable. */
    function importFunc(name: string, namespace?: string): ImportedFunc | null;

    /**
     * Serialize the live API surface as JSON. When `path` is given it is also
     * written to that file (relative to the plugin directory) and the resolved
     * path is returned. Used by tools/apivalidator.
     */
    function dumpApiSurface(path?: string): string;

    // -- internal helpers backing ll.require (not part of the public API) ----
    /** @internal */ function __pluginDir(): string;
    /** @internal */ function __resolvePath(baseDir: string, spec: string): string | null;
    /** @internal */ function __dirname(path: string): string;
    /** @internal */ function __readFile(path: string): string | null;
}

// ---------------------------------------------------------------------------
// Universal pointer system
// ---------------------------------------------------------------------------
/** Any bound native class constructor (used by NativePointer.as). */
interface NativeClass<T> {
    new (...args: any[]): T;
}

/**
 * A universal native pointer. Memory obtained through `alloc` is owned via a
 * smart pointer and freed automatically when the last referring NativePointer is
 * garbage collected, so the pointer system never leaks. Pointers obtained from
 * `fromAddress` / `of` / a bound function returning a raw pointer are *borrowed*
 * and never free foreign memory.
 */
declare class NativePointer {
    /** Allocate `size` zero-initialized, owned bytes. */
    static alloc(size: number): NativePointer;
    /** Wrap an existing address (hex string, e.g. "0x7ff6..."); borrowed. */
    static fromAddress(address: string, size?: number): NativePointer;
    /** Borrowed pointer to the memory of a bound native object. */
    static of(obj: object): NativePointer;

    /** True when the address is null. */
    isNull(): boolean;
    /** True when this pointer owns its memory (freed on GC). */
    isOwned(): boolean;
    /** The address as a hex string (full 64-bit precision). */
    address(): string;
    /** Known byte extent (0 = unknown, e.g. borrowed pointers). */
    readonly size: number;

    /** Pointer arithmetic; the result shares this pointer's owner. */
    add(bytes: number): NativePointer;
    sub(bytes: number): NativePointer;

    getInt8(offset: number): number;
    getInt16(offset: number): number;
    getInt32(offset: number): number;
    getInt64(offset: number): number;
    getUint8(offset: number): number;
    getUint16(offset: number): number;
    getUint32(offset: number): number;
    getFloat(offset: number): number;
    getDouble(offset: number): number;
    /** Read a string; `length` omitted or 0 reads up to the NUL terminator. */
    getString(offset: number, length?: number): string;

    setInt8(offset: number, value: number): void;
    setInt16(offset: number, value: number): void;
    setInt32(offset: number, value: number): void;
    setInt64(offset: number, value: number): void;
    setUint8(offset: number, value: number): void;
    setUint16(offset: number, value: number): void;
    setUint32(offset: number, value: number): void;
    setFloat(offset: number, value: number): void;
    setDouble(offset: number, value: number): void;
    setString(offset: number, value: string): void;

    /** Reinterpret the address as an instance of a bound native class (borrowed). */
    as<T>(clazz: NativeClass<T>): T;
}

// ---------------------------------------------------------------------------
// Sample `mc` native hierarchy (demonstrates inheritance-capable binding).
// Replace/extend with generated bindings for real MC/LL types.
// ---------------------------------------------------------------------------
declare namespace mc {
    class Entity {
        constructor();
        getTypeName(): string;
        getName(): string;
        setName(name: string): void;
        getPosition(): number[];
        setPosition(x: number, y: number, z: number): void;
        id: number;
    }

    class LivingEntity extends Entity {
        constructor();
        hurt(amount: number): void;
        isAlive(): boolean;
        health: number;
    }

    class Player extends LivingEntity {
        constructor();
        static create(name: string): Player;
        sendMessage(message: string): void;
        getLastMessage(): string;
        uuid: string;
        level: number;
    }

    /** Takes the base type: a Player may be passed (inheritance-aware unwrap). */
    function describe(entity: Entity): string;
}
