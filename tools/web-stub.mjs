// Stands in for the page's document when a web fixture runs under node.
if (typeof globalThis.document === "undefined") {
  const element = (tag) => ({
    tag,
    textContent: "",
    children: [],
    attributes: {},
    style: { setProperty() {} },
    appendChild(c) { this.children.push(c); },
    setAttribute(k, v) { this.attributes[k] = v; },
    addEventListener() {},
    remove() {},
  });
  globalThis.document = {
    title: "",
    createElement: element,
    getElementById: () => null,
    querySelector: () => null,
  };
}
if (typeof globalThis.localStorage === "undefined") {
  const items = new Map();
  globalThis.localStorage = {
    getItem: (k) => (items.has(k) ? items.get(k) : null),
    setItem: (k, v) => { items.set(k, String(v)); },
    removeItem: (k) => { items.delete(k); },
  };
}
