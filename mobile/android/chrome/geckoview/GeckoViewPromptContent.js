/* -*- indent-tabs-mode: nil; js-indent-level: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

ChromeUtils.import("resource://gre/modules/GeckoViewUtils.jsm");

GeckoViewUtils.addLazyEventListener(this, ["click", "contextmenu"], {
  handler: _ =>
    Cc["@mozilla.org/prompter;1"].getService(Ci.nsIDOMEventListener),
  options: {
    capture: false,
    mozSystemGroup: true,
  },
});