const { Services } = ChromeUtils.import("resource://gre/modules/Services.jsm", {});
const { XPCOMUtils } = ChromeUtils.import("resource://gre/modules/XPCOMUtils.jsm", {});
const env = Cc["@mozilla.org/process/environment;1"].getService(Ci.nsIEnvironment);

XPCOMUtils.defineLazyGetter(this, "require", function() {
  let { require } =
    ChromeUtils.import("resource://devtools/shared/Loader.jsm", {});
  return require;
});
XPCOMUtils.defineLazyGetter(this, "gDevTools", function() {
  let { gDevTools } = require("devtools/client/framework/devtools");
  return gDevTools;
});
XPCOMUtils.defineLazyGetter(this, "TargetFactory", function() {
  let { TargetFactory } = require("devtools/client/framework/target");
  return TargetFactory;
});

// Record allocation count in new subtests if DEBUG_DEVTOOLS_ALLOCATIONS is set to
// "normal". Print allocation sites to stdout if DEBUG_DEVTOOLS_ALLOCATIONS is set to
// "verbose".
const DEBUG_ALLOCATIONS = env.get("DEBUG_DEVTOOLS_ALLOCATIONS");

// Maximum time spent in one test, in milliseconds
const TEST_TIMEOUT = 5 * 60000;

function getMostRecentBrowserWindow() {
  return Services.wm.getMostRecentWindow("navigator:browser");
}

function getActiveTab(window) {
  return window.gBrowser.selectedTab;
}

let gmm = window.getGroupMessageManager("browsers");

const frameScript = "data:," + encodeURIComponent(`(${
  function() {
    addEventListener("load", function(event) {
      let subframe = event.target != content.document;
      sendAsyncMessage("browser-test-utils:loadEvent",
        {subframe, url: event.target.documentURI});
    }, true);
  }
})()`);

gmm.loadFrameScript(frameScript, true);

// This is duplicated from BrowserTestUtils.jsm
function awaitBrowserLoaded(browser, includeSubFrames = false, wantLoad = null) {
  // If browser belongs to tabbrowser-tab, ensure it has been
  // inserted into the document.
  let tabbrowser = browser.ownerGlobal.gBrowser;
  if (tabbrowser && tabbrowser.getTabForBrowser) {
    tabbrowser._insertBrowser(tabbrowser.getTabForBrowser(browser));
  }

  function isWanted(url) {
    if (!wantLoad) {
      return true;
    } else if (typeof(wantLoad) == "function") {
      return wantLoad(url);
    }
    // It's a string.
    return wantLoad == url;
  }

  return new Promise(resolve => {
    let mm = browser.ownerGlobal.messageManager;
    mm.addMessageListener("browser-test-utils:loadEvent", function onLoad(msg) {
      if (msg.target == browser && (!msg.data.subframe || includeSubFrames) &&
          isWanted(msg.data.url)) {
        mm.removeMessageListener("browser-test-utils:loadEvent", onLoad);
        resolve(msg.data.url);
      }
    });
  });
}

/* globals res:true */

function Damp() {
  Services.prefs.setBoolPref("devtools.webconsole.new-frontend-enabled", true);
}

Damp.prototype = {
  async garbageCollect() {
    dump("Garbage collect\n");

    // Minimize memory usage
    // mimic miminizeMemoryUsage, by only flushing JS objects via GC.
    // We don't want to flush all the cache like minimizeMemoryUsage,
    // as it slow down next executions almost like a cold start.

    // See minimizeMemoryUsage code to justify the 3 iterations and the setTimeout:
    // https://searchfox.org/mozilla-central/source/xpcom/base/nsMemoryReporterManager.cpp#2574-2585
    for (let i = 0; i < 3; i++) {
      // See minimizeMemoryUsage code here to justify the GC+CC+GC:
      // https://searchfox.org/mozilla-central/rev/be78e6ea9b10b1f5b2b3b013f01d86e1062abb2b/dom/base/nsJSEnvironment.cpp#341-349
      Cu.forceGC();
      Cu.forceCC();
      Cu.forceGC();
      await new Promise(done => setTimeout(done, 0));
    }
  },

  /**
   * Helper to tell when a test start and when it is finished.
   * It helps recording its duration, but also put markers for perf-html when profiling
   * DAMP.
   *
   * When this method is called, the test is considered to be starting immediately
   * When the test is over, the returned object's `done` method should be called.
   *
   * @param label String
   *        Test title, displayed everywhere in PerfHerder, DevTools Perf Dashboard, ...
   *
   * @return object
   *         With a `done` method, to be called whenever the test is finished running
   *         and we should record its duration.
   */
  runTest(label) {
    if (DEBUG_ALLOCATIONS) {
      if (!this.allocationTracker) {
        this.allocationTracker = this.startAllocationTracker();
      }
      // Flush the current allocations before running the test
      this.allocationTracker.flushAllocations();
    }

    let startLabel = label + ".start";
    performance.mark(startLabel);
    let start = performance.now();

    return {
      done: () => {
        let end = performance.now();
        let duration = end - start;
        performance.measure(label, startLabel);
        this._results.push({
          name: label,
          value: duration
        });

        if (DEBUG_ALLOCATIONS == "normal") {
          this._results.push({
            name: label + ".allocations",
            value: this.allocationTracker.countAllocations()
          });
        } else if (DEBUG_ALLOCATIONS == "verbose") {
          this.allocationTracker.logAllocationSites();
        }
      }
    };
  },

  async addTab(url) {
    let tab = this._win.gBrowser.selectedTab = this._win.gBrowser.addTab(url);
    let browser = tab.linkedBrowser;
    await awaitBrowserLoaded(browser);
    return tab;
  },

  closeCurrentTab() {
    this._win.BrowserCloseTabOrWindow();
    return this._win.gBrowser.selectedTab;
  },

  reloadPage(onReload) {
    return new Promise(resolve => {
      let browser = gBrowser.selectedBrowser;
      if (typeof(onReload) == "function") {
        onReload().then(resolve);
      } else {
        resolve(awaitBrowserLoaded(browser));
      }
      browser.reload();
    });
  },

  async testSetup(url) {
    let tab = await this.addTab(url);
    await new Promise(resolve => {
      setTimeout(resolve, this._config.rest);
    });
    return tab;
  },

  async testTeardown(url) {
    this.closeCurrentTab();

    // Force freeing memory now so that it doesn't happen during the next test
    await this.garbageCollect();

    let duration = Math.round(performance.now() - this._startTime);
    dump(`${this._currentTest} took ${duration}ms.\n`);

    this._runNextTest();
  },

  // Everything below here are common pieces needed for the test runner to function,
  // just copy and pasted from Tart with /s/TART/DAMP

  _win: undefined,
  _dampTab: undefined,
  _results: [],
  _config: {subtests: [], repeat: 1, rest: 100},
  _nextTestIndex: 0,
  _tests: [],
  _onSequenceComplete: 0,

  // Timeout ID to guard against current test never finishing
  _timeout: null,

  // The unix time at which the current test started (ms)
  _startTime: null,

  // Name of the test currently executed (i.e. path from /tests folder)
  _currentTest: null,

  // Is DAMP finished executing? Help preventing async execution when DAMP had an error
  _done: false,

  _runNextTest() {
    window.clearTimeout(this._timeout);

    if (this._nextTestIndex >= this._tests.length) {
      this._onSequenceComplete();
      return;
    }

    let test = this._tests[this._nextTestIndex++];
    this._startTime = performance.now();
    this._currentTest = test;

    dump(`Loading test '${test}'\n`);
    let testMethod = require("chrome://damp/content/tests/" + test);

    this._timeout = window.setTimeout(() => {
      this.error("Test timed out");
    }, TEST_TIMEOUT);

    dump(`Executing test '${test}'\n`);
    let promise = testMethod();

    // If test method is an async function, ensure catching its exceptions
    if (promise && typeof(promise.catch) == "function") {
      promise.catch(e => {
        this.exception(e);
      });
    }
  },
  // Each command at the array a function which must call nextCommand once it's done
  _doSequence(tests, onComplete) {
    this._tests = tests;
    this._onSequenceComplete = onComplete;
    this._results = [];
    this._nextTestIndex = 0;

    this._runNextTest();
  },

  _log(str) {
    if (window.MozillaFileLogger && window.MozillaFileLogger.log)
      window.MozillaFileLogger.log(str);

    window.dump(str);
  },

  _logLine(str) {
    return this._log(str + "\n");
  },

  _reportAllResults() {
    var testNames = [];
    var testResults = [];

    var out = "";
    for (var i in this._results) {
      res = this._results[i];
      var disp = [].concat(res.value).map(function(a) { return (isNaN(a) ? -1 : a.toFixed(1)); }).join(" ");
      out += res.name + ": " + disp + "\n";

      if (!Array.isArray(res.value)) { // Waw intervals array is not reported to talos
        testNames.push(res.name);
        testResults.push(res.value);
      }
    }
    this._log("\n" + out);

    if (content && content.tpRecordTime) {
      content.tpRecordTime(testResults.join(","), 0, testNames.join(","));
    } else {
      // alert(out);
    }
  },

  _onTestComplete: null,

  _doneInternal() {
    // Ignore any duplicated call to this method
    if (this._done) {
      return;
    }
    this._done = true;

    if (this.allocationTracker) {
      this.allocationTracker.stop();
      this.allocationTracker = null;
    }
    this._win.gBrowser.selectedTab = this._dampTab;

    if (this._results) {
      this._logLine("DAMP_RESULTS_JSON=" + JSON.stringify(this._results));
      this._reportAllResults();
    }

    if (this._onTestComplete) {
      this._onTestComplete(JSON.parse(JSON.stringify(this._results))); // Clone results
    }
  },

  startAllocationTracker() {
    const { allocationTracker } = require("devtools/shared/test-helpers/allocation-tracker");
    return allocationTracker();
  },

  error(message) {
    // Log a unique prefix in order to be interpreted as an error and stop DAMP from
    // testing/talos/talos/talos_process.py
    dump("TEST-UNEXPECTED-FAIL | damp | ");

    // Print the currently executed test, if we already started executing one
    if (this._currentTest) {
      dump(this._currentTest + ": ");
    }

    dump(message + "\n");

    // Stop further test execution and immediatly close DAMP
    this._tests = [];
    this._results = null;
    this._doneInternal();
  },

  exception(e) {
    this.error(e);
    dump(e.stack + "\n");
  },

  startTest(doneCallback, config) {
    try {
      dump("Initialize the head file with a reference to this DAMP instance\n");
      let head = require("chrome://damp/content/tests/head.js");
      head.initialize(this);

      this._onTestComplete = function(results) {
        TalosParentProfiler.pause("DAMP - end");
        doneCallback(results);
      };
      this._config = config;

      this._win = Services.wm.getMostRecentWindow("navigator:browser");
      this._dampTab = this._win.gBrowser.selectedTab;
      this._win.gBrowser.selectedBrowser.focus(); // Unfocus the URL bar to avoid caret blink

      TalosParentProfiler.resume("DAMP - start");

      // Filter tests via `./mach --subtests filter` command line argument
      let filter = Services.prefs.getCharPref("talos.subtests", "");

      let tests = config.subtests.filter(test => !test.disabled)
                                 .filter(test => test.name.includes(filter));

      if (tests.length === 0) {
        this.error(`Unable to find any test matching '${filter}'`);
      }

      // Run cold test only once
      let topWindow = getMostRecentBrowserWindow();
      if (topWindow.coldRunDAMPDone) {
        tests = tests.filter(test => !test.cold);
      } else {
        topWindow.coldRunDAMPDone = true;
      }

      // Construct the sequence array while filtering tests
      let sequenceArray = [];
      for (let test of tests) {
        for (let r = 0; r < config.repeat; r++) {
          sequenceArray.push(test.path);
        }
      }

      // Free memory before running the first test, otherwise we may have a GC
      // related to Firefox startup or DAMP setup during the first test.
      this.garbageCollect().then(() => {
        this._doSequence(sequenceArray, this._doneInternal);
      }).catch(e => {
        this.exception(e);
      });
    } catch (e) {
      this.exception(e);
    }
  }
};