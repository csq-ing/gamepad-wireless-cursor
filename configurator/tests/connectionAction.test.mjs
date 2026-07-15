import assert from "node:assert/strict";
import test from "node:test";
import { getConnectionAction } from "../.tmp-tests/connectionAction.js";

test("shows only disconnect while connected", () => {
  assert.deepEqual(getConnectionAction("connected"), {
    action: "disconnect",
    disabled: false,
    label: "disconnect"
  });
});

test("shows reconnect while disconnected", () => {
  assert.deepEqual(getConnectionAction("disconnected"), {
    action: "connect",
    disabled: false,
    label: "reconnect"
  });
});

test("keeps reconnect visible but disabled while connecting", () => {
  assert.deepEqual(getConnectionAction("connecting"), {
    action: "connect",
    disabled: true,
    label: "reconnect"
  });
});

test("allows reconnect after a connection error", () => {
  assert.deepEqual(getConnectionAction("error"), {
    action: "connect",
    disabled: false,
    label: "reconnect"
  });
});
