// SPDX-License-Identifier: MPL-2.0

// Firefox under Playwright can drop console lines, so the load-outcome markers
// (PJ_FILE_LOAD_OK / _REJECTED / _FAILED) are also pushed, with identical text,
// into the page global `pjWasmLoadOutcomes`. The array is created on the first
// push and is reset whenever the page reloads or navigates.

// Every outcome line containing `marker`, in arrival order.
async function loadOutcomes(page, marker) {
  return page.evaluate(
    m => (globalThis.pjWasmLoadOutcomes || []).filter(line => line.includes(m)), marker);
}

// The first outcome line containing `marker`, or ''.
async function firstLoadOutcome(page, marker = 'PJ_FILE_LOAD_OK') {
  return page.evaluate(
    m => (globalThis.pjWasmLoadOutcomes || []).find(line => line.includes(m)) || '', marker);
}

// How many outcome lines contain `marker`.
async function loadOutcomeCount(page, marker = 'PJ_FILE_LOAD_OK') {
  return page.evaluate(
    m => (globalThis.pjWasmLoadOutcomes || []).filter(line => line.includes(m)).length, marker);
}

module.exports = { loadOutcomes, firstLoadOutcome, loadOutcomeCount };
