/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

const nsIBLS = Ci.nsIBlocklistService;

var gNotifier = null;
var gNextTest = null;
var gPluginHost = null;

var gTestserver = AddonTestUtils.createHttpServer({hosts: ["example.com"]});
gTestserver.registerDirectory("/data/", do_get_file("data"));

var PLUGINS = [{
  // severity=0, vulnerabilitystatus=0 -> outdated
  name: "test_plugin_0",
  version: "5",
  disabled: false,
  blocklisted: false
},
{
  // severity=0, vulnerabilitystatus=1 -> update available
  name: "test_plugin_1",
  version: "5",
  disabled: false,
  blocklisted: false
},
{
  // severity=0, vulnerabilitystatus=2 -> no update
  name: "test_plugin_2",
  version: "5",
  disabled: false,
  blocklisted: false
},
{
  // no severity field -> severity=3 by default -> hardblock
  name: "test_plugin_3",
  version: "5",
  disabled: false,
  blocklisted: false
},
{
  // severity=1, vulnerabilitystatus=2 -> softblock
  name: "test_plugin_4",
  version: "5",
  disabled: false,
  blocklisted: false
},
{
  // not in the blocklist -> not blocked
  name: "test_plugin_5",
  version: "5",
  disabled: false,
  blocklisted: false
}];

function test_basic() {
  var blocklist = Cc["@mozilla.org/extensions/blocklist;1"].getService(nsIBLS);

  Assert.ok(blocklist.getPluginBlocklistState(PLUGINS[0], "1", "1.9") == nsIBLS.STATE_OUTDATED);

  Assert.ok(blocklist.getPluginBlocklistState(PLUGINS[1], "1", "1.9") == nsIBLS.STATE_VULNERABLE_UPDATE_AVAILABLE);

  Assert.ok(blocklist.getPluginBlocklistState(PLUGINS[2], "1", "1.9") == nsIBLS.STATE_VULNERABLE_NO_UPDATE);

  Assert.ok(blocklist.getPluginBlocklistState(PLUGINS[3], "1", "1.9") == nsIBLS.STATE_BLOCKED);

  Assert.ok(blocklist.getPluginBlocklistState(PLUGINS[4], "1", "1.9") == nsIBLS.STATE_SOFTBLOCKED);

  Assert.ok(blocklist.getPluginBlocklistState(PLUGINS[5], "1", "1.9") == nsIBLS.STATE_NOT_BLOCKED);

  gNextTest = test_is_not_clicktoplay;
  executeSoon(gNextTest);
}

function get_test_plugin() {
  var pluginHost = Cc["@mozilla.org/plugin/host;1"].getService(Ci.nsIPluginHost);
  for (var plugin of pluginHost.getPluginTags()) {
    if (plugin.name == "Test Plug-in")
      return plugin;
  }
  Assert.ok(false);
  return null;
}

// At this time, the blocklist does not have an entry for the test plugin,
// so it shouldn't be click-to-play.
function test_is_not_clicktoplay() {
  var plugin = get_test_plugin();
  var blocklistState = Services.blocklist.getPluginBlocklistState(plugin, "1", "1.9");
  Assert.notEqual(blocklistState, Ci.nsIBlocklistService.STATE_VULNERABLE_UPDATE_AVAILABLE);
  Assert.notEqual(blocklistState, Ci.nsIBlocklistService.STATE_VULNERABLE_NO_UPDATE);

  Services.prefs.setCharPref("extensions.blocklist.url", "http://example.com/data/test_pluginBlocklistCtpUndo.xml");
  gNextTest = test_is_clicktoplay;
  gNotifier.notify(null);
}

// Here, we've updated the blocklist to have a block for the test plugin,
// so it should be click-to-play.
function test_is_clicktoplay() {
  var plugin = get_test_plugin();
  var blocklistState = Services.blocklist.getPluginBlocklistState(plugin, "1", "1.9");
  Assert.equal(blocklistState, Ci.nsIBlocklistService.STATE_VULNERABLE_NO_UPDATE);

  Services.prefs.setCharPref("extensions.blocklist.url", "http://example.com/data/test_pluginBlocklistCtp.xml");
  gNextTest = test_is_not_clicktoplay2;
  gNotifier.notify(null);
}

// But now we've removed that entry from the blocklist (really we've gone back
// to the old one), so the plugin shouldn't be click-to-play any more.
function test_is_not_clicktoplay2() {
  var plugin = get_test_plugin();
  var blocklistState = Services.blocklist.getPluginBlocklistState(plugin, "1", "1.9");
  Assert.notEqual(blocklistState, Ci.nsIBlocklistService.STATE_VULNERABLE_UPDATE_AVAILABLE);
  Assert.notEqual(blocklistState, Ci.nsIBlocklistService.STATE_VULNERABLE_NO_UPDATE);

  Services.prefs.setCharPref("extensions.blocklist.url", "http://example.com/data/test_pluginBlocklistCtpUndo.xml");
  gNextTest = test_disable_blocklist;
  gNotifier.notify(null);
}

// Test that disabling the blocklist when a plugin is ctp-blocklisted will
// result in the plugin not being click-to-play.
function test_disable_blocklist() {
  var plugin = get_test_plugin();
  var blocklistState = Services.blocklist.getPluginBlocklistState(plugin, "1", "1.9");
  Assert.equal(blocklistState, Ci.nsIBlocklistService.STATE_VULNERABLE_NO_UPDATE);

  gNextTest = null;
  Services.prefs.setBoolPref("extensions.blocklist.enabled", false);
  blocklistState = Services.blocklist.getPluginBlocklistState(plugin, "1", "1.9");
  Assert.notEqual(blocklistState, Ci.nsIBlocklistService.STATE_VULNERABLE_NO_UPDATE);
  Assert.notEqual(blocklistState, Ci.nsIBlocklistService.STATE_VULNERABLE_UPDATE_AVAILABLE);

  // it should still be possible to make a plugin click-to-play via the pref
  // and setting that plugin's enabled state to click-to-play
  Services.prefs.setBoolPref("plugins.click_to_play", true);
  let previousEnabledState = plugin.enabledState;
  plugin.enabledState = Ci.nsIPluginTag.STATE_CLICKTOPLAY;
  Assert.equal(gPluginHost.getStateForType("application/x-test"), Ci.nsIPluginTag.STATE_CLICKTOPLAY);
  // clean up plugin state
  plugin.enabledState = previousEnabledState;

  do_test_finished();
}

// Observe "blocklist-updated" so we know when to advance to the next test
function observer() {
  if (gNextTest)
    executeSoon(gNextTest);
}

function run_test() {
  createAppInfo("xpcshell@tests.mozilla.org", "XPCShell", "1", "1.9");

  Services.prefs.setCharPref("extensions.blocklist.url", "http://example.com/data/test_pluginBlocklistCtp.xml");
  Services.prefs.setBoolPref("plugin.load_flash_only", false);
  startupManager();

  gPluginHost = Cc["@mozilla.org/plugin/host;1"].getService(Ci.nsIPluginHost);
  gNotifier = Cc["@mozilla.org/extensions/blocklist;1"].getService(Ci.nsITimerCallback);
  Services.obs.addObserver(observer, "blocklist-updated");

  registerCleanupFunction(function() {
    Services.prefs.clearUserPref("extensions.blocklist.url");
    Services.prefs.clearUserPref("extensions.blocklist.enabled");
    Services.prefs.clearUserPref("plugins.click_to_play");
    Services.obs.removeObserver(observer, "blocklist-updated");
  });

  gNextTest = test_basic;
  do_test_pending();
  gNotifier.notify(null);
}
