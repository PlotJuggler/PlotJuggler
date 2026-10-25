// SPDX-License-Identifier: MPL-2.0
// Matchers over a page's console log (`messages`, in arrival order).

// How many messages contain `marker`.
const count = (messages, marker) => messages.filter(message => message.includes(marker)).length;

// The last message containing `marker`, or ''.
const latest = (messages, marker) => messages.filter(message => message.includes(marker)).at(-1) || '';

// The PJ_WASM_LAYOUT_LOAD_OK / _FAILED message of the layout named `name`, logged at or after
// index `since`; '' until there is one.
const layoutOutcome = (messages, name, since = 0) => messages.slice(since).find(
  message => /PJ_WASM_LAYOUT_LOAD_(OK|FAILED) name=/.test(message) && message.includes(`name=${name}`),
) || '';

module.exports = { count, latest, layoutOutcome };
