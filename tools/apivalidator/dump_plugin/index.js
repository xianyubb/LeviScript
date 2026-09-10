// Dev tool plugin: writes the live API surface to a file next to this plugin so
// tools/apivalidator/validate.py can check it against an expected spec.
//
// Install this directory under the server's plugins/ folder, start the server
// once, then copy api_surface.json out and run the validator.
"use strict";

const path = ll.dumpApiSurface("api_surface.json");
logger.info("API surface written to:", path);
