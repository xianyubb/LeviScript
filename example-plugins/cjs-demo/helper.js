// A CommonJS module loaded through ll.require("./helper.js").
"use strict";

module.exports = {
    VERSION: "1.0.0",
    greet(name) {
        return `Hello, ${name}!`;
    },
    square(x) {
        return x * x;
    },
};
