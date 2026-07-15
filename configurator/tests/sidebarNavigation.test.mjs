import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import test from "node:test";

const source = readFileSync(new URL("../src/App.tsx", import.meta.url), "utf8");

test("sidebar navigation uses product-specific Chinese labels", () => {
  for (const label of ["基础映射", "体感映射", "智能摇轮", "设备信息"]) {
    assert.match(source, new RegExp(`["']${label}["']`), `expected sidebar label ${label}`);
  }

  for (const legacyLabel of ["按键", "滚动", "体感"]) {
    assert.doesNotMatch(source, new RegExp(`\\b(?:button|scroll|motion):\\s*["']${legacyLabel}["']`));
  }
});

test("sidebar navigation uses semantic lucide icons", () => {
  assert.match(source, /\bGamepad2\b/, "basic mapping should use a gamepad icon");
  assert.match(source, /\bOrbit\b/, "motion mapping should use a motion-oriented icon");
  assert.match(source, /\bRotateCw\b/, "smart wheel should use a rotation icon");
  assert.match(source, /\bInfo\b/, "device information should use an information icon");
  assert.match(source, /\{\s*id:\s*"button",\s*label:\s*t\.button,\s*icon:\s*Gamepad2\s*\}/);
  assert.match(source, /\{\s*id:\s*"motion",\s*label:\s*t\.motion,\s*icon:\s*Orbit\s*\}/);
  assert.match(source, /\{\s*id:\s*"scroll",\s*label:\s*t\.scroll,\s*icon:\s*RotateCw\s*\}/);
  assert.match(source, /\{\s*id:\s*"settings",\s*label:\s*t\.sidebarSettings,\s*icon:\s*Info\s*\}/);
});
