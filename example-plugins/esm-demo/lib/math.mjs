// A plain ES module imported by index.mjs to demonstrate the module loader
// (relative specifier resolution + named exports).

export const VERSION = "1.0.0";

export function add(a, b) {
    return a + b;
}

export function mul(a, b) {
    return a * b;
}

export default { add, mul, VERSION };
