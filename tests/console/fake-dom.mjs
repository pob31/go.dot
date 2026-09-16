/*
    This file is part of Go.dot — https://github.com/pob31/go.dot

    Copyright (C) 2026 Pierre-Olivier Boulant

    Go.dot is free software: you can redistribute it and/or modify it under the
    terms of the GNU General Public License as published by the Free Software
    Foundation, either version 3 of the License, or (at your option) any later
    version. Go.dot is distributed in the hope that it will be useful, but
    WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
    or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License
    (LICENSE, at the repository root) for more details.

    SPDX-License-Identifier: GPL-3.0-or-later
*/

/*  THE LEAST DOM THE RECONCILER TOUCHES, and a record of every change made to it.

    views/reconcile.js is the one piece of the page's DOM worth a test (§14.15),
    and node has no DOM. So this is the DOM as reconcile and morph use it and no
    more: elements with attributes, a `dataset` over the `data-*` ones, child
    lists, text nodes, and the five ways a tree changes. Not a browser, and not
    trying to be one: what it is for is the COUNT, because what the reconciler
    promises is about how little it changes - a row moved is one move, a row
    unchanged is nothing at all - and a browser would not say.

    Every change lands in `log` as [what, node], and a move is told from an
    insertion by whether the node already stood in the same parent: that is
    the removal a real browser makes first, and the one that loses a press. */

const log = [];

function kebab(key) {
  return key.replace(/[A-Z]/g, (c) => "-" + c.toLowerCase());
}

class FakeNode {
  constructor(nodeType, nodeName) {
    this.nodeType = nodeType;
    this.nodeName = nodeName;
    this.parentNode = null;
    this.childNodes = [];
  }
}

class FakeText extends FakeNode {
  constructor(text) {
    super(3, "#text");
    this.value = String(text);
  }

  get nodeValue() { return this.value; }
  set nodeValue(text) { this.value = String(text); log.push(["text", this]); }
}

class FakeElement extends FakeNode {
  constructor(name) {
    super(1, String(name).toUpperCase());
    this.attributeMap = new Map();

    const element = this;

    this.dataset = new Proxy({}, {
      get(_, key) {
        return typeof key === "string" ? element.attributeMap.get("data-" + kebab(key)) : undefined;
      },
      set(_, key, value) {
        element.setAttribute("data-" + kebab(String(key)), value);
        return true;
      },

      /*  `delete element.dataset.x` takes the attribute away in a browser, and
          the page relies on that: views/values.js drops its in-flight mark
          that way. Without this trap the proxy deleted a property of an empty
          object and the attribute stayed, which is a fake that lies. */
      deleteProperty(_, key) {
        if (typeof key === "string") element.attributeMap.delete("data-" + kebab(key));
        return true;
      },
    });
  }

  get attributes() {
    return Array.from(this.attributeMap, ([name, value]) => ({ name, value }));
  }

  hasAttribute(name) { return this.attributeMap.has(name); }
  getAttribute(name) { return this.attributeMap.has(name) ? this.attributeMap.get(name) : null; }

  setAttribute(name, value) {
    this.attributeMap.set(name, String(value));
    log.push(["attribute", this]);
  }

  removeAttribute(name) {
    this.attributeMap.delete(name);
    log.push(["attribute", this]);
  }

  get children() { return this.childNodes.filter((node) => node.nodeType === 1); }
  get firstElementChild() { return this.children[0] || null; }

  get nextElementSibling() {
    if (!this.parentNode) return null;

    const siblings = this.parentNode.childNodes;

    for (let n = siblings.indexOf(this) + 1; n < siblings.length; n += 1) {
      if (siblings[n].nodeType === 1) return siblings[n];
    }

    return null;
  }

  /*  Where `child` goes, taking it out of wherever it stood first - which is
      what a browser does, and why putting a node that is already here
      somewhere else is a MOVE. */
  place(child, reference) {
    const moved = child.parentNode === this;

    if (child.parentNode) {
      const siblings = child.parentNode.childNodes;
      siblings.splice(siblings.indexOf(child), 1);
    }

    const at = reference === null ? this.childNodes.length : this.childNodes.indexOf(reference);

    if (at < 0) throw new Error("the reference node is not a child of this one");

    this.childNodes.splice(at, 0, child);
    child.parentNode = this;
    log.push([moved ? "move" : "insert", child]);
    return child;
  }

  appendChild(child) { return this.place(child, null); }
  insertBefore(child, reference) { return this.place(child, reference); }

  removeChild(child) {
    const at = this.childNodes.indexOf(child);

    if (at < 0) throw new Error("not a child of this node");

    this.childNodes.splice(at, 1);
    child.parentNode = null;
    log.push(["remove", child]);
    return child;
  }

  replaceChild(fresh, old) {
    const at = this.childNodes.indexOf(old);

    if (at < 0) throw new Error("not a child of this node");

    if (fresh.parentNode) {
      const siblings = fresh.parentNode.childNodes;
      siblings.splice(siblings.indexOf(fresh), 1);
    }

    this.childNodes.splice(this.childNodes.indexOf(old), 1, fresh);
    fresh.parentNode = this;
    old.parentNode = null;
    log.push(["replace", fresh]);
    return old;
  }

  /*  The text of the subtree, as a test would like to read a row. */
  get textContent() {
    return this.childNodes.map((node) => node.nodeType === 3 ? node.nodeValue : node.textContent)
                          .join("");
  }
}

/*  Markup for the reconciler's `make`: a row is written as JSON rather than
    HTML - ["div", {"class": "row"}, "text", ["button", {}, "kill"]] - so the
    string is still the row's whole state, which is what reconcile compares,
    and this file needs no parser. */
function build(description) {
  if (typeof description === "string") return new FakeText(description);

  const [name, attributes, ...children] = description;
  const element = new FakeElement(name);

  for (const [key, value] of Object.entries(attributes || {})) element.attributeMap.set(key, String(value));
  for (const child of children) {
    const node = build(child);
    node.parentNode = element;
    element.childNodes.push(node);
  }

  return element;
}

const make = (markup) => build(JSON.parse(markup));

/*  The global the page's modules expect at import time, installed before
    views/reconcile.js is imported: it makes a <template> once, at load. */
function installDocument() {
  globalThis.document = {
    activeElement: null,
    createElement: (name) => new FakeElement(name),
  };

  return globalThis.document;
}

export { log, FakeElement, FakeText, build, make, installDocument };
