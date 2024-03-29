"use strict";

add_task(async function test_summary() {
  await BrowserTestUtils.withNewTab({
    gBrowser,
    url: "resource://payments/paymentRequest.xhtml",
  }, async browser => {
    ok(browser.contentDocumentAsCPOW.getElementById("cancel"), "Cancel button exists");
  });
});
