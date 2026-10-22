// Stands in for the page's document when a web fixture runs under node.
if (typeof globalThis.document === "undefined") {
  const element = (tag) => ({ tag, textContent: "", children: [], appendChild(c) { this.children.push(c); } });
  globalThis.document = { title: "", createElement: element, getElementById: () => null };
}
