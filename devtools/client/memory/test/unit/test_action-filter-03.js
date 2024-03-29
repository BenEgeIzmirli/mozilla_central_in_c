/* Any copyright is dedicated to the Public Domain.
   http://creativecommons.org/publicdomain/zero/1.0/ */

"use strict";

// Test that changing filter state in the middle of taking a snapshot results in
// the properly fitered census.

let { snapshotState: states, censusState, viewState } = require("devtools/client/memory/constants");
let { setFilterString, setFilterStringAndRefresh } = require("devtools/client/memory/actions/filter");
let { takeSnapshotAndCensus } = require("devtools/client/memory/actions/snapshot");
let { changeView } = require("devtools/client/memory/actions/view");

add_task(async function() {
  let front = new StubbedMemoryFront();
  let heapWorker = new HeapAnalysesClient();
  await front.attach();
  let store = Store();
  let { getState, dispatch } = store;

  dispatch(changeView(viewState.CENSUS));

  dispatch(takeSnapshotAndCensus(front, heapWorker));
  await waitUntilSnapshotState(store, [states.SAVING]);

  dispatch(setFilterString("str"));

  await waitUntilCensusState(store, snapshot => snapshot.census,
                             [censusState.SAVED]);
  equal(getState().filter, "str",
        "should want filtered trees");
  equal(getState().snapshots[0].census.filter, "str",
        "snapshot-we-were-in-the-middle-of-saving's census should be filtered");

  dispatch(setFilterStringAndRefresh("", heapWorker));
  await waitUntilCensusState(store, snapshot => snapshot.census,
                             [censusState.SAVING]);
  ok(true, "changing filter string retriggers census");
  ok(!getState().filter, "no longer filtering");

  dispatch(setFilterString("obj"));
  await waitUntilCensusState(store, snapshot => snapshot.census,
                             [censusState.SAVED]);
  equal(getState().filter, "obj", "filtering for obj now");
  equal(getState().snapshots[0].census.filter, "obj",
        "census-we-were-in-the-middle-of-recomputing should be filtered again");

  heapWorker.destroy();
  await front.detach();
});
