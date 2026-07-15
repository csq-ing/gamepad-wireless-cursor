import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import test from "node:test";

const appSource = readFileSync(new URL("../src/App.tsx", import.meta.url), "utf8");

test("smart wheel panel exposes a manual AI pull test send action", () => {
  assert.match(appSource, /send_ai_pull_test/);
  assert.match(appSource, /AI_PULL_TEST_VALUE\s*=\s*63/);
  assert.match(appSource, /onAiPullTestSend/);
  assert.match(appSource, /testSendAiPull/);
});

test("manual AI pull send refreshes device connection state after success", () => {
  assert.match(appSource, /await\s+invoke\("send_ai_pull_test",\s*\{\s*value:\s*AI_PULL_TEST_VALUE\s*\}\)/);
  assert.match(appSource, /if\s*\(\s*connectionState\s*!==\s*"connected"\s*\)/);
  assert.match(appSource, /await\s+invoke\("connect_device"\)/);
});

test("home reconnect badge follows receiver connection instead of recent controller input", () => {
  assert.match(appSource, /<BatteryBadge\s+isConnected=\{isReceiverConnected\}/);
  assert.doesNotMatch(appSource, /<BatteryBadge\s+isConnected=\{isControllerConnected\}/);
});

test("app probes receiver connection after device event listeners are attached", () => {
  assert.match(
    appSource,
    /const unlistenStatus = await listen<DeviceStatus>[\s\S]*void invoke\("connect_device"\)[\s\S]*return \(\) => \{[\s\S]*unlistenConnection\(\);/
  );
});
