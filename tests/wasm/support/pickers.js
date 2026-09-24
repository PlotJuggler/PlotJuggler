// SPDX-License-Identifier: MPL-2.0
const { expect } = require('@playwright/test');

const PICKER_ATTEMPTS = 3;
const PICKER_DELIVERY_TIMEOUT_MS = 8000;
const PICKER_RETRY_DELAY_MS = 500;

const interceptedPages = new WeakSet();

// Playwright intercepts native choosers only while a 'filechooser' listener
// exists: each waitForEvent re-sends Page.setInterceptFileChooserDialog without
// awaiting it, and removing the waiter disables it again. A trigger that reaches
// input.click() before that enable lands opens the real picker instead, which
// headless Chromium cancels at once, so the waiter times out. A permanent no-op
// listener keeps interception on for the page's lifetime: only the page's very
// first request can race the enable, and the retry below covers that one.
function keepChooserInterception(page) {
  if (!interceptedPages.has(page)) {
    interceptedPages.add(page);
    page.on('filechooser', () => {});
  }
}

async function retryChooser(page, trigger) {
  keepChooserInterception(page);
  let lastError;
  for (let attempt = 0; attempt < PICKER_ATTEMPTS; ++attempt) {
    const chooserPromise = page.waitForEvent('filechooser', { timeout: PICKER_DELIVERY_TIMEOUT_MS });
    try {
      await trigger(attempt);
      return await chooserPromise;
    } catch (error) {
      lastError = error;
      // Drain a still-pending waiter before retrying so an earlier attempt
      // cannot steal the next chooser event.
      await chooserPromise.catch(() => undefined);
      if (attempt + 1 < PICKER_ATTEMPTS) {
        await page.waitForTimeout(PICKER_RETRY_DELAY_MS);
      }
    }
  }
  throw lastError;
}

async function openFileChooser(page, screen, useProbe = false) {
  return retryChooser(page, async (attempt) => {
    if (attempt === 0 && !useProbe) {
      // Exercise the real user-facing canvas button first.
      await page.mouse.click(screen.x + 15, screen.y + 80);
    } else {
      // Probe builds expose the same MainWindow slot. This avoids turning
      // a dropped synthetic canvas gesture into a parser/ingest false failure.
      await page.evaluate(() => window.pjWasmOpenLoadProbe());
    }
  });
}

async function openProbedChooser(page, probeName) {
  const hasProbe = await page.evaluate(name => typeof window[name] === 'function', probeName);
  if (!hasProbe) {
    throw new Error(`${probeName} not installed — check EM_JS probe installers`);
  }
  return retryChooser(page, () => page.evaluate(name => window[name](), probeName));
}

async function openLayoutChooser(page) {
  return openProbedChooser(page, 'pjWasmOpenLayoutProbe');
}

async function openRobotChooser(page) {
  return openProbedChooser(page, 'pjWasmOpenRobotPickerProbe');
}

async function openSourceReplayChooser(page, screen, geometry, consoleMessages) {
  return retryChooser(page, async () => {
    const previousRequests = consoleMessages.filter(
      message => message.includes('Opening browser file picker with filter:'),
    ).length;
    // Every attempt remains a physical click on the production Reselect
    // button. A missing browser event is terminal for that request, so an
    // eight-second delivery window is more useful than waiting 30 seconds five
    // times for callbacks that can no longer arrive.
    await page.mouse.click(screen.x + geometry.reselect.x, screen.y + geometry.reselect.y);
    await expect.poll(
      () => consoleMessages.filter(
        message => message.includes('Opening browser file picker with filter:'),
      ).length,
      { timeout: 3000 },
    ).toBeGreaterThan(previousRequests);
  });
}


module.exports = { openFileChooser, openLayoutChooser, openRobotChooser, openSourceReplayChooser };
