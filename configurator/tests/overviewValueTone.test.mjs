import assert from "node:assert/strict";
import test from "node:test";
import { getOverviewConnectivityTone } from "../.tmp-tests/overviewValueTone.js";

test("uses positive tone for connected-like values", () => {
  assert.equal(getOverviewConnectivityTone(true), "positive");
});

test("uses muted tone for disconnected-like values", () => {
  assert.equal(getOverviewConnectivityTone(false), "muted");
});
