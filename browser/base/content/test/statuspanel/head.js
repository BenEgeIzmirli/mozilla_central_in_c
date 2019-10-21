/* Any copyright is dedicated to the Public Domain.
 * http://creativecommons.org/publicdomain/zero/1.0/ */

/**
 * Returns a Promise that resolves when a StatusPanel for a
 * window has finished being shown. Also asserts that the
 * text content of the StatusPanel matches a value.
 *
 * @param win (browser window)
 *        The window that the StatusPanel belongs to.
 * @param value (string)
 *        The value that the StatusPanel should show.
 * @returns Promise
 */
async function promiseStatusPanelShown(win, value) {
  let panel = win.StatusPanel.panel;
  await BrowserTestUtils.waitForEvent(panel, "transitionend", (e) => {
    return e.propertyName === "opacity" &&
           win.getComputedStyle(e.target).opacity == "1";
  });

  Assert.equal(win.StatusPanel._labelElement.value, value);
}

/**
 * Returns a Promise that resolves when a StatusPanel for a
 * window has finished being hidden.
 *
 * @param win (browser window)
 *        The window that the StatusPanel belongs to.
 */
async function promiseStatusPanelHidden(win) {
  let panel = win.StatusPanel.panel;
  await BrowserTestUtils.waitForEvent(panel, "transitionend", (e) => {
    return e.propertyName === "opacity" &&
           win.getComputedStyle(e.target).opacity == "0";
  });
}