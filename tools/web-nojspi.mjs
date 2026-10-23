// Makes the engine look like one without JSPI, whatever the node version ships.
delete WebAssembly.Suspending;
delete WebAssembly.promising;
