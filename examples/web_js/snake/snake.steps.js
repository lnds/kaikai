function tick(n) { for (var i = 0; i < n; i++) document.body.dispatchEvent(new Event("tick")); }
tick(3);
//--
function key(k) { document.body.dispatchEvent(new KeyboardEvent("keydown", { key: k, bubbles: true })); }
function tick(n) { for (var i = 0; i < n; i++) document.body.dispatchEvent(new Event("tick")); }
key("ArrowLeft"); tick(1); key("ArrowUp"); tick(2);
//--
function tick(n) { for (var i = 0; i < n; i++) document.body.dispatchEvent(new Event("tick")); }
tick(5);
//--
function key(k) { document.body.dispatchEvent(new KeyboardEvent("keydown", { key: k, bubbles: true })); }
function tick(n) { for (var i = 0; i < n; i++) document.body.dispatchEvent(new Event("tick")); }
tick(1); key("Enter"); tick(1);
