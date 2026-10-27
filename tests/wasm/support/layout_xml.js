// SPDX-License-Identifier: MPL-2.0
// Regex accessors over a serialized layout (XML bytes or text).
const { expect } = require('@playwright/test');

const asText = layout => (Buffer.isBuffer(layout) ? layout.toString('utf8') : String(layout));

// `attribute` of the first `tag` element; fails the test when there is none.
function elementAttribute(layout, tag, attribute) {
  const match = asText(layout).match(new RegExp(`<${tag}\\b[^>]*\\b${attribute}="([^"]*)"`));
  expect(match, `missing ${tag}.${attribute} in serialized layout`).not.toBeNull();
  return match[1];
}

// `attribute` of every `tag` element that has it, in document order.
function elementAttributes(layout, tag, attribute) {
  return [...asText(layout).matchAll(new RegExp(`<${tag}\\b[^>]*\\b${attribute}="([^"]*)"`, 'g'))]
    .map(match => match[1]);
}

// The start tag with `attribute` set, added when absent.
function withAttribute(element, attribute, value) {
  const existing = new RegExp(`\\b${attribute}="[^"]*"`);
  if (existing.test(element)) {
    return element.replace(existing, `${attribute}="${value}"`);
  }
  return element.replace(/\s*\/?>(?=$)/, ending => ` ${attribute}="${value}"${ending}`);
}

function setElementAttribute(xml, tag, attribute, value) {
  return xml.replace(new RegExp(`<${tag}\\b[^>]*>`), element => withAttribute(element, attribute, value));
}

// Sets `attribute` on the index-th `tag` element.
function setElementAttributeAt(xml, tag, index, attribute, value) {
  let current = 0;
  return xml.replace(new RegExp(`<${tag}\\b[^>]*>`, 'g'),
    element => (current++ === index ? withAttribute(element, attribute, value) : element));
}

module.exports = { elementAttribute, elementAttributes, setElementAttribute, setElementAttributeAt };
