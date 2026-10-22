// Run a built wasm32-browser module while a 5 ms host timer ticks, and print the
// timeline: one line per program line, one "tick" per timer firing in between.
//   node web-ticks.mjs out/app.wasm
import { pathToFileURL } from "node:url";
import { dirname, join, resolve } from "node:path";

const path = resolve(process.argv[2]);
const { run } = await import(pathToFileURL(join(dirname(path), "kai_host.mjs")));
const lines = [];
const log = console.log;
console.log = (l) => lines.push(l);
const timer = setInterval(() => lines.push("tick"), 5);
await run(pathToFileURL(path));
clearInterval(timer);
log(lines.join("\n"));
