const BASE_NUMBER_OF_PROCESSES = 3;
function checkBaseProcessCount(description) {
  const {childCount} = Services.ppmm;
  // With preloaded activity-stream, process count is a bit undeterministic, so
  // allow for some variation
  const extraCount = BASE_NUMBER_OF_PROCESSES + 1;
  ok(childCount === BASE_NUMBER_OF_PROCESSES || childCount === extraCount, `${description} (${BASE_NUMBER_OF_PROCESSES} or ${extraCount})`);
}

function processScript() {
  if (Services.cpmm !== this) {
    dump("Test failed: wrong global object\n");
    return;
  }

  this.cpmm = Services.cpmm;

  addMessageListener("ProcessTest:Reply", function listener(msg) {
    removeMessageListener("ProcessTest:Reply", listener);
    sendAsyncMessage("ProcessTest:Finished");
  });
  sendSyncMessage("ProcessTest:Loaded");
}
var processScriptURL = "data:,(" + processScript.toString() + ")()";

function initTestScript() {
  let init = initialProcessData;
  if (init.test123 != "hello") {
    dump("Initial data incorrect\n");
    return;
  }

  sendAsyncMessage("ProcessTest:InitGood", init.test456.get("hi"));
}
var initTestScriptURL = "data:,(" + initTestScript.toString() + ")()";

var checkProcess = async function(mm) {
  let { target } = await promiseMessage(mm, "ProcessTest:Loaded");
  target.sendAsyncMessage("ProcessTest:Reply");
  await promiseMessage(target, "ProcessTest:Finished");
  ok(true, "Saw process finished");
};

function promiseMessage(messageManager, message) {
  return new Promise(resolve => {
    let listener = (msg) => {
      messageManager.removeMessageListener(message, listener);
      resolve(msg);
    };

    messageManager.addMessageListener(message, listener);
  })
}

add_task(async function(){
  // We want to count processes in this test, so let's disable the pre-allocated process manager.
  await SpecialPowers.pushPrefEnv({"set": [
    ["dom.ipc.processPrelaunch.enabled", false],
  ]});
})

add_task(async function(){
  // This test is only relevant in e10s.
  if (!gMultiProcessBrowser)
    return;

  Services.ppmm.releaseCachedProcesses();

  await SpecialPowers.pushPrefEnv({"set": [["dom.ipc.processCount", 5]]})
  await SpecialPowers.pushPrefEnv({"set": [["dom.ipc.keepProcessesAlive.web", 5]]})

  let tabs = [];
  for (let i = 0; i < 3; i++) {
    tabs[i] = await BrowserTestUtils.openNewForegroundTab(gBrowser, "about:blank");
  }

  for (let i = 0; i < 3; i++) {
    // FIXME: This should wait for the tab removal gets reflected to the
    //        process count (bug 1446726).
    let sessionStorePromise = BrowserTestUtils.waitForSessionStoreUpdate(tabs[i]);
    BrowserTestUtils.removeTab(tabs[i]);
    await sessionStorePromise;
  }

  Services.ppmm.releaseCachedProcesses();
  checkBaseProcessCount("Should get back to the base number of processes at this point");
})

// Test that loading a process script loads in all existing processes
add_task(async function() {
  let checks = [];
  for (let i = 0; i < Services.ppmm.childCount; i++)
    checks.push(checkProcess(Services.ppmm.getChildAt(i)));

  Services.ppmm.loadProcessScript(processScriptURL, false);
  await Promise.all(checks);
});

// Test that loading a process script loads in new processes
add_task(async function() {
  // This test is only relevant in e10s
  if (!gMultiProcessBrowser)
    return;

  checkBaseProcessCount("Should still be at the base number of processes at this point");

  // Load something in the main process
  gBrowser.selectedBrowser.loadURI("about:robots");
  await BrowserTestUtils.browserLoaded(gBrowser.selectedBrowser);

  let init = Services.ppmm.initialProcessData;
  init.test123 = "hello";
  init.test456 = new Map();
  init.test456.set("hi", "bye");

  // With no remote frames left we should be down to one process.
  // However, stuff like remote thumbnails can cause a content
  // process to exist nonetheless. This should be rare, though,
  // so the test is useful most of the time.
  if (Services.ppmm.childCount == 2) {
    let mainMM = Services.ppmm.getChildAt(0);

    let check = checkProcess(Services.ppmm);
    Services.ppmm.loadProcessScript(processScriptURL, true);

    // The main process should respond
    await check;

    check = checkProcess(Services.ppmm);
    // Reset the default browser to start a new child process
    gBrowser.updateBrowserRemoteness(gBrowser.selectedBrowser, true);
    gBrowser.selectedBrowser.loadURI("about:blank");
    await BrowserTestUtils.browserLoaded(gBrowser.selectedBrowser);

    checkBaseProcessCount("Should be back to the base number of processes at this point");

    // The new process should have responded
    await check;

    Services.ppmm.removeDelayedProcessScript(processScriptURL);

    let childMM;
    childMM = Services.ppmm.getChildAt(2);

    childMM.loadProcessScript(initTestScriptURL, false);
    let msg = await promiseMessage(childMM, "ProcessTest:InitGood");
    is(msg.data, "bye", "initial process data was correct");
  } else {
    info("Unable to finish test entirely");
  }
});
