// SPDX-License-Identifier: MPL-2.0
const { test, expect } = require('@playwright/test');
const { bootApp } = require('./support/app');

for (const language of ['zh_CN', 'zh_TW']) {
  test(`bundled fonts render Chinese UI and menu separator: ${language}`, async ({ page }, testInfo) => {
    test.setTimeout(120000);
    // Select a real app locale for this session; do not modify persisted settings.
    await page.route('**/*', async route => {
      if (route.request().resourceType() !== 'document') {
        await route.continue();
        return;
      }
      const response = await route.fetch();
      const html = await response.text();
      expect(html).toContain('await qtLoad({');
      await route.fulfill({ response, body: html.replace('await qtLoad({',
        `await qtLoad({ arguments: ['--language', '${language}'],`) });
    });
    const { messages, errors } = await bootApp(page);
    const result = messages.find(message => message.includes('PJ_WASM_CJK_RENDER'));
    expect(result).toBeDefined();
    const values = result.match(/glyphs=(\d+) missing=(\d+) ink=(\d+) families=(.*)/);
    expect(values).not.toBeNull();
    expect(Number(values[1])).toBeGreaterThan(20);
    expect(Number(values[2])).toBe(0);
    expect(Number(values[3])).toBeGreaterThan(100);
    expect(values[4]).toContain('Noto Sans SC');
    expect(values[4]).toContain('Noto Sans Symbols 2');
    expect(messages.some(message => message.includes('Chinese translation catalog is missing'))).toBe(false);
    const localeText = language === 'zh_CN'
      ? '本次会话的界面语言（zh_CN、zh_TW 或 en_US）。'
      : '此工作階段的介面語言（zh_CN、zh_TW 或 en_US）。';
    const localeProbe = messages.filter(message => message.startsWith('PJ_WASM_CJK_LOCALE text='));
    expect(localeProbe.map(message => message.trimEnd())).toEqual([`PJ_WASM_CJK_LOCALE text=${localeText}`]);
    expect(errors).toEqual([]);
    await testInfo.attach(`${language}-ui`, { body: await page.screenshot(), contentType: 'image/png' });
  });
}
