// SPDX-License-Identifier: MPL-2.0

// Firefox under Playwright can drop console lines, so the load-outcome markers
// (PJ_FILE_LOAD_OK / _REJECTED / _FAILED) are also pushed, with identical text,
// into the page global `pjWasmLoadOutcomes`. The array is created on the first
// push and is reset whenever the page reloads or navigates.
async function loadOutcomes(page, marker) {
  return page.evaluate(
    m => (globalThis.pjWasmLoadOutcomes || []).filter(line => line.includes(m)), marker);
}

module.exports = { loadOutcomes };
