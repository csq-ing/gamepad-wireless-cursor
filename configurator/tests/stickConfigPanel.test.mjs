import assert from "node:assert/strict";
import { existsSync, readFileSync } from "node:fs";
import test from "node:test";

const appSource = readFileSync(new URL("../src/App.tsx", import.meta.url), "utf8");
const stylesSource = readFileSync(new URL("../src/styles.css", import.meta.url), "utf8");
const panelUrl = new URL("../src/components/StickConfigPanel.tsx", import.meta.url);

test("stick deadzone configuration lives in a separate panel module", () => {
  assert.ok(existsSync(panelUrl), "expected StickConfigPanel.tsx to be a separate component module");

  const panelSource = readFileSync(panelUrl, "utf8");
  assert.match(panelSource, /export\s+function\s+StickConfigPanel\b/);
  assert.match(panelSource, /deadzone:\s*number/);
  assert.match(panelSource, /onDeadzoneChange/);
  assert.match(panelSource, /type="range"/);
});

test("stick panel presents one physical stick deadzone control without an internal scrollbar", () => {
  const panelSource = readFileSync(panelUrl, "utf8");
  const deadzoneControlUsages = panelSource.match(/<DeadzoneControl\b/g) ?? [];

  assert.equal(deadzoneControlUsages.length, 1);
  assert.doesNotMatch(panelSource, /leftStick|rightStick|Left Stick|Right Stick|左摇杆|右摇杆/);
  assert.doesNotMatch(panelSource, /overflow-y-auto/);
  assert.match(panelSource, /stick-deadzone-range/);
});

test("basic mapping stick label opens the stick configuration panel", () => {
  assert.match(appSource, /import\s+\{\s*StickConfigPanel\s*\}\s+from\s+"\.\/components\/StickConfigPanel"/);
  assert.match(appSource, /activeBasicConfig,\s*setActiveBasicConfig/);
  assert.match(appSource, /id:\s*"stick"/);
  assert.match(appSource, /onClick=\{\(\)\s*=>\s*handleBasicControlClick\(control\.id\)\}/);
  assert.match(appSource, /activeBasicConfig\s*===\s*"stick"\s*\?\s*\(/);
});

test("single visible deadzone is written to both protocol deadzone fields", () => {
  assert.match(appSource, /stickDeadzone,\s*setStickDeadzone/);
  assert.match(appSource, /leftDeadzone:\s*stickDeadzone/);
  assert.match(appSource, /rightDeadzone:\s*stickDeadzone/);
  assert.match(appSource, /deadzone=\{stickDeadzone\}/);
  assert.match(appSource, /onDeadzoneChange=\{setStickDeadzone\}/);
  assert.doesNotMatch(appSource, /onLeftDeadzoneChange|onRightDeadzoneChange/);
});

test("deadzone range is fully custom styled instead of leaking native scrollbar styling", () => {
  const panelSource = readFileSync(panelUrl, "utf8");

  assert.match(panelSource, /--stick-progress/);
  assert.doesNotMatch(panelSource, /absolute left-0 right-0 top-1\/2 h-2/);
  assert.match(stylesSource, /\.stick-deadzone-range\s*\{[^}]*-webkit-appearance:\s*none/s);
  assert.match(stylesSource, /::-webkit-slider-runnable-track\s*\{[^}]*linear-gradient/s);
  assert.match(stylesSource, /::-webkit-slider-thumb\s*\{[^}]*background:\s*#7a5bff/s);
  assert.doesNotMatch(stylesSource, /accent-color:\s*#7a5bff/);
});
