import { Suspense, useEffect, useMemo, useRef, useState } from "react";
import { Canvas, useLoader, useThree } from "@react-three/fiber";
import { Stage } from "@react-three/drei";
import { invoke } from "@tauri-apps/api/core";
import { listen } from "@tauri-apps/api/event";
import { getCurrentWindow } from "@tauri-apps/api/window";
import {
  ArrowLeft,
  BatteryMedium,
  ChevronDown,
  Crosshair,
  Gamepad2,
  Info,
  Map,
  Orbit,
  Plus,
  RotateCw,
  Send,
  Settings,
  Shield,
  Swords,
  X,
  type LucideIcon
} from "lucide-react";
import { STLLoader } from "three/examples/jsm/loaders/STLLoader.js";
import { MotionMappingPanel } from "./components/MotionMappingPanel";
import { StickConfigPanel } from "./components/StickConfigPanel";
import type { AiScreenPullEvent, ConnectionState, DeviceConfig, DeviceConnectionEvent, DeviceStatus } from "./types";

type Language = "zh" | "en";
type SidebarSection = "button" | "scroll" | "motion" | "settings";
type BasicMappingControlId = "a" | "b" | "x" | "y" | "knob" | "function" | "stick" | "dpad" | "wheel";
type WheelGameId = "call-of-the-wild-angler";

type MappingOption = {
  name: string;
  icon: LucideIcon;
};

type CopyDeck = {
  addDevice: string;
  settingsTitle: string;
  versionLabel: string;
  languageLabel: string;
  button: string;
  scroll: string;
  motion: string;
  sidebarSettings: string;
  mapping: string;
  reconnect: string;
  firmwareSectionTitle: string;
  receiverFirmwareLabel: string;
  controllerFirmwareLabel: string;
  resetSectionTitle: string;
  resetWarning: string;
  resetButton: string;
  resetConfirmButton: string;
  resetSuccess: string;
  basicMappingLabels: Record<BasicMappingControlId, string>;
};

type SmartWheelCopy = {
  feedbackTitle: string;
  enableResistance: string;
  resistanceStatusOn: string;
  resistanceStatusOff: string;
  baseGain: string;
  baseGainUnit: string;
  aiResistanceGain: string;
  aiSectionTitle: string;
  selectGame: string;
  screenPullValue: string;
  screenSource: string;
  testSendAiPull: string;
  testSendAiPullSuccess: string;
  testSendAiPullFailure: string;
  gameLabels: Record<WheelGameId, string>;
};

const BASIC_MAPPING_CONTROLS: Array<{ id: BasicMappingControlId }> = [
  { id: "a" },
  { id: "b" },
  { id: "x" },
  { id: "y" },
  { id: "knob" },
  { id: "function" },
  { id: "stick" },
  { id: "dpad" },
  { id: "wheel" }
];

const STICK_DEADZONE_MAX = 1024;
const STICK_DEADZONE_DEFAULT = 64;
const MOTION_VIEW_DEADZONE_DEFAULT = 1.5;
const MOTION_VIEW_SENSITIVITY_DEFAULT = 1.0;
const MOTION_CAST_DEADZONE_DEFAULT = 1.5;
const WHEEL_BASE_GAIN_DEFAULT = 42;
const AI_PULL_TEST_VALUE = 63;

const WHEEL_GAME_OPTIONS: Array<{ id: WheelGameId }> = [
  { id: "call-of-the-wild-angler" }
];

const MAPPING_OPTIONS: Record<Language, MappingOption[]> = {
  zh: [
    { name: "游戏 1", icon: Crosshair },
    { name: "游戏 2", icon: Swords },
    { name: "游戏 3", icon: Shield }
  ],
  en: [
    { name: "Game 1", icon: Crosshair },
    { name: "Game 2", icon: Swords },
    { name: "Game 3", icon: Shield }
  ]
};

const COPY: Record<Language, CopyDeck> = {
  zh: {
    addDevice: "添加设备",
    settingsTitle: "设置",
    versionLabel: "软件版本",
    languageLabel: "语言",
    button: "基础映射",
    scroll: "智能摇轮",
    motion: "体感映射",
    sidebarSettings: "设备信息",
    mapping: "选择映射方案",
    reconnect: "重新连接",
    firmwareSectionTitle: "固件版本",
    receiverFirmwareLabel: "接收器固件版本",
    controllerFirmwareLabel: "手柄固件版本",
    resetSectionTitle: "重置映射方案",
    resetWarning: "重置后，自定义按键映射将回到出厂布局。",
    resetButton: "恢复默认映射",
    resetConfirmButton: "确认重置映射",
    resetSuccess: "映射方案已重置",
    basicMappingLabels: {
      a: "A按键",
      b: "B按键",
      x: "X按键",
      y: "Y按键",
      knob: "旋钮",
      function: "功能键",
      stick: "摇杆",
      dpad: "方向键",
      wheel: "摇轮"
    }
  },
  en: {
    addDevice: "Add Device",
    settingsTitle: "Settings",
    versionLabel: "Software Version",
    languageLabel: "Language",
    button: "Basic Mapping",
    scroll: "Smart Wheel",
    motion: "Motion Mapping",
    sidebarSettings: "Device Info",
    mapping: "Select Mapping",
    reconnect: "Reconnect",
    firmwareSectionTitle: "Firmware Versions",
    receiverFirmwareLabel: "Receiver Firmware",
    controllerFirmwareLabel: "Controller Firmware",
    resetSectionTitle: "Reset Mapping Preset",
    resetWarning: "After reset, your custom button assignments will return to the factory layout.",
    resetButton: "Restore Default Mapping",
    resetConfirmButton: "Confirm Reset",
    resetSuccess: "Mapping preset reset",
    basicMappingLabels: {
      a: "A Button",
      b: "B Button",
      x: "X Button",
      y: "Y Button",
      knob: "Knob",
      function: "Function Button",
      stick: "Stick",
      dpad: "D-Pad",
      wheel: "Wheel"
    }
  }
};

const SMART_WHEEL_COPY: Record<Language, SmartWheelCopy> = {
  zh: {
    feedbackTitle: "阻力反馈",
    enableResistance: "启用阻力反馈",
    resistanceStatusOn: "已启用",
    resistanceStatusOff: "未启用",
    baseGain: "基础增益值",
    baseGainUnit: "增益",
    aiResistanceGain: "AI 阻力增益",
    aiSectionTitle: "AI 画面识别",
    selectGame: "选择游戏",
    screenPullValue: "画面拉力值",
    screenSource: "来自游戏画面",
    testSendAiPull: "发送测试拉力 63%",
    testSendAiPullSuccess: "已发送测试拉力 63%",
    testSendAiPullFailure: "发送失败，请确认接收器已连接并已烧录新固件。",
    gameLabels: {
      "call-of-the-wild-angler": "荒野的召唤：垂钓者"
    }
  },
  en: {
    feedbackTitle: "Resistance Feedback",
    enableResistance: "Enable Resistance Feedback",
    resistanceStatusOn: "Enabled",
    resistanceStatusOff: "Disabled",
    baseGain: "Base Gain",
    baseGainUnit: "gain",
    aiResistanceGain: "AI Resistance Gain",
    aiSectionTitle: "AI Screen Capture",
    selectGame: "Select Game",
    screenPullValue: "Screen Pull Value",
    screenSource: "From game screen",
    testSendAiPull: "Send Test Pull 63%",
    testSendAiPullSuccess: "Test pull 63% sent",
    testSendAiPullFailure: "Send failed. Confirm the receiver is connected and flashed with the new firmware.",
    gameLabels: {
      "call-of-the-wild-angler": "Call of the Wild: The Angler"
    }
  }
};

function cn(...parts: Array<string | false | null | undefined>) {
  return parts.filter(Boolean).join(" ");
}

function isTauriRuntime() {
  return typeof window !== "undefined" && "__TAURI_INTERNALS__" in window;
}

function GamepadModel({ isDimmed }: { isDimmed: boolean }) {
  const geometry = useLoader(STLLoader, "/gamepad.stl");
  const centeredGeometry = useMemo(() => geometry.clone().center(), [geometry]);

  return (
    <group>
      <mesh geometry={centeredGeometry} rotation={[0, 0, 0]} scale={0.012}>
        <meshPhysicalMaterial
          color="#ffc2ce"
          roughness={0.58}
          metalness={0}
          clearcoat={0.22}
          clearcoatRoughness={0.42}
        />
      </mesh>

      {isDimmed ? (
        <mesh geometry={centeredGeometry} rotation={[0, 0, 0]} scale={0.01208} renderOrder={1}>
          <meshPhysicalMaterial
            color="#a8adb4"
            roughness={0.82}
            metalness={0}
            transparent
            opacity={0.42}
            depthWrite={false}
          />
        </mesh>
      ) : null}
    </group>
  );
}

function GamepadSceneLighting() {
  return (
    <>
      <ambientLight intensity={0.52} />
      <hemisphereLight color="#fff7fa" groundColor="#ffd4df" intensity={0.62} />
      <directionalLight
        position={[-2.8, 4.2, 6.4]}
        intensity={2.35}
      />
      <pointLight position={[3.2, -2.2, 4.5]} intensity={0.38} color="#fff1f5" />
    </>
  );
}

function GamepadPreviewCamera() {
  const { camera } = useThree();

  useEffect(() => {
    camera.position.set(0, 0.85, 8.5);
    camera.lookAt(0, -0.25, 0);
    camera.updateProjectionMatrix();
  }, [camera]);

  return null;
}

function FallbackGamepad() {
  return (
    <mesh rotation={[0, 0, 0]}>
      <boxGeometry args={[1.5, 2, 0.4]} />
      <meshPhysicalMaterial color="#ffc2ce" roughness={0.58} metalness={0} clearcoat={0.22} />
    </mesh>
  );
}

function BatteryBadge({
  isConnected,
  percent,
  reconnectLabel,
  onReconnect,
  className
}: {
  isConnected: boolean;
  percent: number;
  reconnectLabel: string;
  onReconnect: () => void;
  className?: string;
}) {
  if (!isConnected) {
    return (
      <button
        type="button"
        onClick={onReconnect}
        className={cn(
          "rounded-lg border border-slate-100 bg-white px-4 py-2 text-sm font-semibold text-slate-700 shadow-[0_2px_10px_rgba(0,0,0,0.04)] transition hover:border-slate-200 hover:text-slate-900",
          className
        )}
      >
        {reconnectLabel}
      </button>
    );
  }

  return (
    <div
      className={cn(
        "flex items-center gap-3 rounded-lg border border-slate-100 bg-white px-4 py-2 shadow-[0_2px_10px_rgba(0,0,0,0.04)]",
        className
      )}
    >
      <span className="text-sm font-bold tracking-wide text-[#f5a623]">{percent}%</span>
      <BatteryMedium className="h-5 w-5 text-[#f5a623]" strokeWidth={2.5} />
    </div>
  );
}

function SettingsPanel({
  language,
  t,
  isResetConfirming,
  hasResetCompleted,
  onResetClick
}: {
  language: Language;
  t: CopyDeck;
  isResetConfirming: boolean;
  hasResetCompleted: boolean;
  onResetClick: () => void;
}) {
  const firmwareVersions =
    language === "zh"
      ? [
        { label: t.receiverFirmwareLabel, value: "v0.1" },
        { label: t.controllerFirmwareLabel, value: "v0.1" }
      ]
      : [
        { label: t.receiverFirmwareLabel, value: "v0.1" },
        { label: t.controllerFirmwareLabel, value: "v0.1" }
      ];

  return (
    <div className="flex w-full justify-start px-3 pb-16 pt-36 sm:px-4">
      <div className="w-full max-w-[980px]">
        <div className="grid gap-12">
          <section>
            <h2 className="mb-4 text-[18px] font-semibold text-slate-900">{t.firmwareSectionTitle}</h2>

            <div className="overflow-hidden rounded-2xl border border-slate-200 bg-white">
              {firmwareVersions.map((item, index) => (
                <div
                  key={item.label}
                  className={cn(
                    "flex flex-col gap-2 px-5 py-5 sm:flex-row sm:items-center sm:justify-between sm:gap-6",
                    index !== firmwareVersions.length - 1 && "border-b border-slate-200"
                  )}
                >
                  <p className="text-[15px] font-medium text-slate-900">{item.label}</p>

                  <div className="font-mono text-sm font-medium tracking-[0.02em] text-slate-700">{item.value}</div>
                </div>
              ))}
            </div>
          </section>

          <section>
            <h2 className="mb-4 text-[18px] font-semibold text-slate-900">{t.resetSectionTitle}</h2>

            <div className="rounded-2xl border border-slate-200 bg-white px-5 py-5">
              <div className="flex flex-col gap-5 lg:flex-row lg:items-center lg:justify-between">
                <div className="max-w-2xl">
                  <p className="text-[15px] font-medium text-slate-900">{t.resetWarning}</p>
                </div>

                <button
                  type="button"
                  onClick={onResetClick}
                  className={cn(
                    "inline-flex shrink-0 items-center justify-center rounded-xl px-5 py-3 text-sm font-semibold transition",
                    hasResetCompleted
                      ? "cursor-default bg-slate-700 text-white"
                      : isResetConfirming
                        ? "bg-slate-800 text-white hover:bg-black"
                        : "bg-slate-900 text-white hover:bg-slate-800"
                  )}
                  disabled={hasResetCompleted}
                >
                  {hasResetCompleted ? t.resetSuccess : isResetConfirming ? t.resetConfirmButton : t.resetButton}
                </button>
              </div>
            </div>
          </section>
        </div>
      </div>
    </div>
  );
}

function ToggleSwitch({
  checked,
  onChange,
  disabled = false,
  ariaLabel
}: {
  checked: boolean;
  onChange: (checked: boolean) => void;
  disabled?: boolean;
  ariaLabel: string;
}) {
  return (
    <button
      type="button"
      role="switch"
      aria-checked={checked}
      aria-label={ariaLabel}
      disabled={disabled}
      onClick={() => onChange(!checked)}
      className={cn(
        "inline-flex h-7 w-12 shrink-0 items-center rounded-full p-1 transition",
        checked ? "bg-[#7a5bff]" : "bg-slate-200",
        disabled && "cursor-not-allowed opacity-50"
      )}
    >
      <span
        className={cn(
          "h-5 w-5 rounded-full bg-white shadow-sm transition-transform",
          checked ? "translate-x-5" : "translate-x-0"
        )}
      />
    </button>
  );
}

function SmartWheelPanel({
  language,
  isResistanceEnabled,
  baseGain,
  isAiGainEnabled,
  selectedGameId,
  screenPullPercent,
  aiPullTestMessage,
  onResistanceEnabledChange,
  onBaseGainChange,
  onAiGainEnabledChange,
  onSelectedGameChange,
  onAiPullTestSend
}: {
  language: Language;
  isResistanceEnabled: boolean;
  baseGain: number;
  isAiGainEnabled: boolean;
  selectedGameId: WheelGameId;
  screenPullPercent: number;
  aiPullTestMessage: string | null;
  onResistanceEnabledChange: (enabled: boolean) => void;
  onBaseGainChange: (value: number) => void;
  onAiGainEnabledChange: (enabled: boolean) => void;
  onSelectedGameChange: (gameId: WheelGameId) => void;
  onAiPullTestSend: () => void;
}) {
  const copy = SMART_WHEEL_COPY[language];
  const normalizedBaseGain = Math.min(100, Math.max(0, Math.round(baseGain)));
  const normalizedPull = Math.min(100, Math.max(0, Math.round(screenPullPercent)));
  const [isGameMenuOpen, setIsGameMenuOpen] = useState(false);
  const gameMenuRef = useRef<HTMLDivElement>(null);
  const selectedGameLabel = copy.gameLabels[selectedGameId];

  useEffect(() => {
    if (!isGameMenuOpen) {
      return undefined;
    }

    const handlePointerDown = (event: PointerEvent) => {
      if (gameMenuRef.current?.contains(event.target as Node)) {
        return;
      }

      setIsGameMenuOpen(false);
    };

    const handleKeyDown = (event: KeyboardEvent) => {
      if (event.key === "Escape") {
        setIsGameMenuOpen(false);
      }
    };

    document.addEventListener("pointerdown", handlePointerDown);
    document.addEventListener("keydown", handleKeyDown);

    return () => {
      document.removeEventListener("pointerdown", handlePointerDown);
      document.removeEventListener("keydown", handleKeyDown);
    };
  }, [isGameMenuOpen]);

  return (
    <div className="flex w-full justify-start px-3 pb-16 pt-36 sm:px-4">
      <div className="w-full max-w-[980px]">
        <div className="grid gap-12">
          <section>
            <h2 className="mb-4 text-[18px] font-semibold text-slate-900">{copy.feedbackTitle}</h2>

            <div className="overflow-hidden rounded-2xl border border-slate-200 bg-white">
              <div className="flex flex-col gap-4 px-5 py-5 sm:flex-row sm:items-center sm:justify-between">
                <div className="min-w-0">
                  <p className="text-[15px] font-medium text-slate-900">{copy.enableResistance}</p>
                  <p className="mt-1 text-sm font-medium text-slate-500">
                    {isResistanceEnabled ? copy.resistanceStatusOn : copy.resistanceStatusOff}
                  </p>
                </div>

                <ToggleSwitch
                  checked={isResistanceEnabled}
                  onChange={onResistanceEnabledChange}
                  ariaLabel={copy.enableResistance}
                />
              </div>

              {isResistanceEnabled ? (
                <div className="grid border-t border-slate-200 md:grid-cols-2">
                  <div className="px-5 py-5 md:border-r md:border-slate-200">
                    <div className="mb-4 flex items-center justify-between gap-4">
                      <p className="text-[15px] font-medium text-slate-900">{copy.baseGain}</p>
                      <div className="font-mono text-sm font-medium tracking-[0.02em] text-slate-700">
                        {normalizedBaseGain}% {copy.baseGainUnit}
                      </div>
                    </div>

                    <div className="flex items-center gap-4">
                      <span className="w-10 text-right text-[11px] font-semibold uppercase text-slate-400">0</span>
                      <input
                        type="range"
                        min={0}
                        max={100}
                        step={1}
                        value={normalizedBaseGain}
                        onChange={(event) => onBaseGainChange(Number(event.target.value))}
                        className="h-2 w-full cursor-pointer accent-[#7a5bff]"
                      />
                      <span className="w-10 text-[11px] font-semibold uppercase text-slate-400">100</span>
                    </div>
                  </div>

                  <div className="flex items-center justify-between gap-5 border-t border-slate-200 px-5 py-5 md:border-t-0">
                    <p className="text-[15px] font-medium text-slate-900">{copy.aiResistanceGain}</p>
                    <ToggleSwitch
                      checked={isAiGainEnabled}
                      onChange={onAiGainEnabledChange}
                      ariaLabel={copy.aiResistanceGain}
                    />
                  </div>
                </div>
              ) : null}
            </div>
          </section>

          {isResistanceEnabled && isAiGainEnabled ? (
            <section>
              <h2 className="mb-4 text-[18px] font-semibold text-slate-900">{copy.aiSectionTitle}</h2>

              <div className="overflow-hidden rounded-2xl border border-slate-200 bg-white">
                <div className="border-b border-slate-200 px-5 py-5">
                  <div className="flex flex-col gap-3 sm:flex-row sm:items-center sm:justify-between">
                    <label className="text-[15px] font-medium text-slate-900" htmlFor="smart-wheel-game">
                      {copy.selectGame}
                    </label>

                    <div ref={gameMenuRef} className="relative w-full sm:w-auto">
                      <button
                        type="button"
                        id="smart-wheel-game"
                        aria-haspopup="listbox"
                        aria-expanded={isGameMenuOpen}
                        onClick={() => setIsGameMenuOpen((isOpen) => !isOpen)}
                        className="flex h-10 w-full min-w-[280px] items-center justify-between gap-3 rounded-lg border border-[#dcdcdc] bg-[#f9f9f9] pl-3.5 pr-3 text-left text-sm font-medium text-slate-800 outline-none transition hover:border-slate-300 hover:bg-white focus:border-slate-400 focus:bg-white focus:ring-3 focus:ring-slate-200/70 sm:min-w-[300px]"
                      >
                        <span className="truncate">{selectedGameLabel}</span>
                        <ChevronDown
                          className={cn(
                            "h-4 w-4 shrink-0 text-slate-500 transition-transform",
                            isGameMenuOpen && "rotate-180"
                          )}
                        />
                      </button>

                      {isGameMenuOpen ? (
                        <div
                          role="listbox"
                          aria-labelledby="smart-wheel-game"
                          className="absolute right-0 z-20 mt-2 w-full overflow-hidden rounded-lg border border-[#dcdcdc] bg-white p-1 shadow-[0_12px_28px_rgba(15,23,42,0.12)]"
                        >
                          {WHEEL_GAME_OPTIONS.map((game) => {
                            const isSelected = game.id === selectedGameId;

                            return (
                              <button
                                key={game.id}
                                type="button"
                                role="option"
                                aria-selected={isSelected}
                                onClick={() => {
                                  onSelectedGameChange(game.id);
                                  setIsGameMenuOpen(false);
                                }}
                                className={cn(
                                  "flex w-full items-center rounded-md px-3 py-2.5 text-left text-sm font-medium transition",
                                  isSelected
                                    ? "bg-slate-100 text-slate-900"
                                    : "text-slate-700 hover:bg-slate-50 hover:text-slate-900"
                                )}
                              >
                                {copy.gameLabels[game.id]}
                              </button>
                            );
                          })}
                        </div>
                      ) : null}
                    </div>
                  </div>
                </div>

                <div className="px-5 py-5">
                  <div className="flex flex-col gap-4 sm:flex-row sm:items-center sm:justify-between">
                    <div>
                      <p className="text-[15px] font-medium text-slate-900">{copy.screenPullValue}</p>
                      <p className="mt-1 text-sm font-medium text-slate-500">{copy.screenSource}</p>
                    </div>

                    <div className="font-mono text-3xl font-semibold tracking-[0.02em] text-slate-900">
                      {normalizedPull}%
                    </div>
                  </div>

                  <div className="mt-5 flex flex-col gap-2 sm:flex-row sm:items-center sm:justify-between">
                    <button
                      type="button"
                      onClick={onAiPullTestSend}
                      className="inline-flex w-fit items-center gap-2 rounded-lg border border-slate-200 bg-white px-4 py-2.5 text-sm font-semibold text-slate-800 transition hover:border-slate-300 hover:bg-slate-50"
                    >
                      <Send className="h-4 w-4 text-[#7a5bff]" />
                      {copy.testSendAiPull}
                    </button>

                    {aiPullTestMessage ? (
                      <p className="text-sm font-medium text-slate-500">{aiPullTestMessage}</p>
                    ) : null}
                  </div>
                </div>
              </div>
            </section>
          ) : null}
        </div>
      </div>
    </div>
  );
}

export default function App() {
  const [showDetails, setShowDetails] = useState(false);
  const [isDropdownOpen, setIsDropdownOpen] = useState(false);
  const [isSettingsOpen, setIsSettingsOpen] = useState(false);
  const [language, setLanguage] = useState<Language>("zh");
  const [selectedMapping, setSelectedMapping] = useState<MappingOption | null>(null);
  const [connectionState, setConnectionState] = useState<ConnectionState>("disconnected");
  const [deviceStatus, setDeviceStatus] = useState<DeviceStatus | null>(null);
  const [activeSection, setActiveSection] = useState<SidebarSection>("button");
  const [activeBasicConfig, setActiveBasicConfig] = useState<BasicMappingControlId | null>(null);
  const [stickDeadzone, setStickDeadzone] = useState(STICK_DEADZONE_DEFAULT);
  const [motionViewDeadzone, setMotionViewDeadzone] = useState(MOTION_VIEW_DEADZONE_DEFAULT);
  const [motionViewSensitivity, setMotionViewSensitivity] = useState(MOTION_VIEW_SENSITIVITY_DEFAULT);
  const [motionCastDeadzone, setMotionCastDeadzone] = useState(MOTION_CAST_DEADZONE_DEFAULT);
  const [isSavingStickConfig, setIsSavingStickConfig] = useState(false);
  const [stickConfigMessage, setStickConfigMessage] = useState<string | null>(null);
  const [isResetConfirming, setIsResetConfirming] = useState(false);
  const [hasResetCompleted, setHasResetCompleted] = useState(false);
  const [isWheelResistanceEnabled, setIsWheelResistanceEnabled] = useState(false);
  const [wheelBaseGain, setWheelBaseGain] = useState(WHEEL_BASE_GAIN_DEFAULT);
  const [isWheelAiGainEnabled, setIsWheelAiGainEnabled] = useState(false);
  const [wheelAiScreenPullPercent, setWheelAiScreenPullPercent] = useState(0);
  const [aiPullTestMessage, setAiPullTestMessage] = useState<string | null>(null);
  const [selectedWheelGameId, setSelectedWheelGameId] = useState<WheelGameId>("call-of-the-wild-angler");
  const [mockBatteryPercent] = useState(30);

  const t = COPY[language];
  const mappingOptions = MAPPING_OPTIONS[language];
  const isSettingsView = showDetails && activeSection === "settings";
  const isSmartWheelView = showDetails && activeSection === "scroll";
  const isMotionMappingView = showDetails && activeSection === "motion";
  const isStandaloneContentView = isSettingsView || isSmartWheelView || isMotionMappingView;
  const showModelLabels = showDetails && activeSection === "button";
  const isReceiverConnected = connectionState === "connected";
  const isControllerConnected = isReceiverConnected && (deviceStatus?.controllerConnected ?? false);
  const wheelScreenPullPercent = isWheelAiGainEnabled ? wheelAiScreenPullPercent : 0;
  const sidebarItems: Array<{ id: SidebarSection; label: string; icon: LucideIcon }> = [
    { id: "button", label: t.button, icon: Gamepad2 },
    { id: "motion", label: t.motion, icon: Orbit },
    { id: "scroll", label: t.scroll, icon: RotateCw },
    { id: "settings", label: t.sidebarSettings, icon: Info }
  ];

  const resetSettingsViewState = () => {
    setIsResetConfirming(false);
    setHasResetCompleted(false);
  };

  const resetBasicConfigState = () => {
    setActiveBasicConfig(null);
    setStickConfigMessage(null);
  };

  const handleWheelResistanceEnabledChange = (enabled: boolean) => {
    setIsWheelResistanceEnabled(enabled);

    if (!enabled) {
      setIsWheelAiGainEnabled(false);
      setWheelAiScreenPullPercent(0);
      setAiPullTestMessage(null);
    }
  };

  const handleWheelAiGainEnabledChange = (enabled: boolean) => {
    if (!isWheelResistanceEnabled) {
      return;
    }

    setIsWheelAiGainEnabled(enabled);
    setAiPullTestMessage(null);
  };

  useEffect(() => {
    if (!isTauriRuntime()) {
      return undefined;
    }

    const setupListeners = async () => {
      const unlistenConnection = await listen<DeviceConnectionEvent>("device-connection", (event) => {
        setConnectionState(event.payload.connection);

        if (event.payload.connection !== "connected") {
          setDeviceStatus(null);
        }
      });

      const unlistenStatus = await listen<DeviceStatus>("device-status", (event) => {
        setDeviceStatus(event.payload);
      });

      const unlistenAiScreenPull = await listen<AiScreenPullEvent>("ai-screen-pull", (event) => {
        setWheelAiScreenPullPercent(Math.min(100, Math.max(0, Math.round(event.payload.pullPercent))));
      });

      void invoke("connect_device").catch((error) => {
        console.error("Initial device connection failed", error);
      });

      return () => {
        unlistenConnection();
        unlistenStatus();
        unlistenAiScreenPull();
      };
    };

    let cleanup: (() => void) | undefined;
    let isCancelled = false;
    void setupListeners()
      .then((unlisten) => {
        if (isCancelled) {
          unlisten();
          return;
        }

        cleanup = unlisten;
      })
      .catch((error) => {
        console.error("Device event listener setup failed", error);
      });

    return () => {
      isCancelled = true;
      cleanup?.();
    };
  }, []);

  useEffect(() => {
    if (!isTauriRuntime()) {
      return undefined;
    }

    if (!isWheelAiGainEnabled) {
      setWheelAiScreenPullPercent(0);
      void invoke("stop_ai_screen_pull").catch((error) => {
        console.error("AI screen pull stop failed", error);
      });
      return undefined;
    }

    void invoke("start_ai_screen_pull").catch((error) => {
      setIsWheelAiGainEnabled(false);
      setWheelAiScreenPullPercent(0);
      console.error("AI screen pull start failed", error);
    });

    return () => {
      void invoke("stop_ai_screen_pull").catch((error) => {
        console.error("AI screen pull stop failed", error);
      });
    };
  }, [isWheelAiGainEnabled]);

  const handleConnect = async () => {
    if (connectionState === "connecting") {
      return;
    }

    if (!isTauriRuntime()) {
      return;
    }

    setConnectionState("connecting");
    try {
      await invoke("connect_device");
    } catch (error) {
      setConnectionState("error");
      setDeviceStatus(null);
      console.error("Device connection failed", error);
    }
  };

  const handleSendAiPullTest = async () => {
    if (!isTauriRuntime()) {
      return;
    }

    try {
      await invoke("send_ai_pull_test", { value: AI_PULL_TEST_VALUE });
      if (connectionState !== "connected") {
        await invoke("connect_device");
      }
      setAiPullTestMessage(SMART_WHEEL_COPY[language].testSendAiPullSuccess);
    } catch (error) {
      const detail = String(error);
      setAiPullTestMessage(`${SMART_WHEEL_COPY[language].testSendAiPullFailure} ${detail}`);
      console.error("AI pull test send failed", error);
    }
  };

  const handleMinimize = async () => {
    try {
      await getCurrentWindow().minimize();
    } catch (error) {
      console.error("Window minimize failed", error);
    }
  };

  const handleClose = async () => {
    try {
      await getCurrentWindow().close();
    } catch (error) {
      console.error("Window close failed", error);
    }
  };

  const handleLanguageChange = (nextLanguage: Language) => {
    const previousOptions = MAPPING_OPTIONS[language];
    const nextOptions = MAPPING_OPTIONS[nextLanguage];
    const selectedIndex = selectedMapping
      ? previousOptions.findIndex((option) => option.name === selectedMapping.name)
      : -1;

    setLanguage(nextLanguage);
    setSelectedMapping(selectedIndex >= 0 ? nextOptions[selectedIndex] ?? null : null);
  };

  const handleBackToOverview = () => {
    setShowDetails(false);
    setActiveSection("button");
    setIsDropdownOpen(false);
    resetBasicConfigState();
    resetSettingsViewState();
  };

  const handleOpenDetails = () => {
    setShowDetails(true);
    setActiveSection("button");
    resetBasicConfigState();
  };

  const handleSelectSection = (section: SidebarSection) => {
    setShowDetails(true);
    setActiveSection(section);
    setIsDropdownOpen(false);
    resetBasicConfigState();

    if (section !== "settings") {
      resetSettingsViewState();
    }
  };

  const handleBasicControlClick = (controlId: BasicMappingControlId) => {
    if (controlId !== "stick") {
      return;
    }

    setActiveBasicConfig("stick");
    setStickConfigMessage(null);
  };

  const handleResetStickDeadzone = () => {
    setStickDeadzone(STICK_DEADZONE_DEFAULT);
    setStickConfigMessage(language === "zh" ? "已恢复默认死区值。" : "Default deadzone restored.");
  };

  const handleApplyStickConfig = async () => {
    const config: DeviceConfig = {
      triggerTarget: deviceStatus?.triggerTarget ?? "RT",
      leftDeadzone: stickDeadzone,
      rightDeadzone: stickDeadzone
    };

    if (!isTauriRuntime()) {
      setStickConfigMessage(language === "zh" ? "已更新界面预览值。" : "Preview values updated.");
      return;
    }

    setIsSavingStickConfig(true);
    setStickConfigMessage(null);

    try {
      const savedConfig = await invoke<DeviceConfig>("set_config_command", { config });
      setStickDeadzone(Math.round((savedConfig.leftDeadzone + savedConfig.rightDeadzone) / 2));
      setStickConfigMessage(language === "zh" ? "摇杆死区已应用到接收器。" : "Stick deadzone applied to receiver.");
    } catch (error) {
      console.error("Stick deadzone update failed", error);
      setStickConfigMessage(language === "zh" ? "应用失败，请检查设备连接。" : "Apply failed. Check the device connection.");
    } finally {
      setIsSavingStickConfig(false);
    }
  };

  const handleResetMappings = () => {
    if (hasResetCompleted) {
      return;
    }

    if (!isResetConfirming) {
      setIsResetConfirming(true);
      return;
    }

    setHasResetCompleted(true);
    setIsResetConfirming(false);
  };

  return (
    <div className="flex h-screen flex-col overflow-hidden bg-[#fafafa] font-sans text-slate-800">
      <div className="flex h-8 shrink-0 items-center border-b border-[#e8e8e8] bg-[#f5f5f5] select-none">
        <div className="h-full flex-1" data-tauri-drag-region />
        <div className="flex h-full [-webkit-app-region:no-drag]">
          <button
            type="button"
            onClick={() => void handleMinimize()}
            className="flex h-full w-12 items-center justify-center border-0 bg-transparent text-[#1c1c1e] outline-none transition hover:bg-black/5"
          >
            <svg width="12" height="12" viewBox="0 0 12 12" fill="none" stroke="currentColor" strokeWidth="1.5">
              <line x1="1" y1="6" x2="11" y2="6" />
            </svg>
          </button>
          <button
            type="button"
            onClick={() => void handleClose()}
            className="flex h-full w-12 items-center justify-center border-0 bg-transparent text-[#1c1c1e] outline-none transition hover:bg-[#e81123] hover:text-white"
          >
            <svg width="12" height="12" viewBox="0 0 12 12" fill="none" stroke="currentColor" strokeWidth="1.5">
              <line x1="2" y1="2" x2="10" y2="10" />
              <line x1="2" y1="10" x2="10" y2="2" />
            </svg>
          </button>
        </div>
      </div>

      <div className="relative flex min-h-0 flex-1 overflow-hidden bg-[#fafafa] font-sans text-slate-800">
        {isSettingsOpen ? (
          <div className="fixed inset-0 z-50 flex items-center justify-center bg-slate-900/20 p-4 backdrop-blur-sm">
            <div className="w-full max-w-sm overflow-hidden rounded-2xl bg-white shadow-xl">
              <div className="flex items-center justify-between border-b border-slate-100 px-6 py-4">
                <h2 className="text-lg font-semibold text-slate-800">{t.settingsTitle}</h2>
                <button
                  type="button"
                  onClick={() => setIsSettingsOpen(false)}
                  className="text-slate-400 transition hover:text-slate-600"
                >
                  <X className="h-5 w-5" />
                </button>
              </div>

              <div className="flex flex-col gap-6 p-6">
                <div className="flex items-center justify-between">
                  <span className="text-sm font-medium text-slate-600">{t.versionLabel}</span>
                  <span className="text-sm font-semibold tracking-wide text-slate-800">v0.1</span>
                </div>

                <div className="flex items-center justify-between">
                  <span className="text-sm font-medium text-slate-600">{t.languageLabel}</span>
                  <div className="flex rounded-lg bg-slate-100 p-1">
                    <button
                      type="button"
                      onClick={() => handleLanguageChange("zh")}
                      className={cn(
                        "rounded-md px-3 py-1 text-xs font-semibold transition-all",
                        language === "zh" ? "bg-white text-[#7a5bff] shadow-sm" : "text-slate-500 hover:text-slate-700"
                      )}
                    >
                      中文
                    </button>
                    <button
                      type="button"
                      onClick={() => handleLanguageChange("en")}
                      className={cn(
                        "rounded-md px-3 py-1 text-xs font-semibold transition-all",
                        language === "en" ? "bg-white text-[#7a5bff] shadow-sm" : "text-slate-500 hover:text-slate-700"
                      )}
                    >
                      English
                    </button>
                  </div>
                </div>
              </div>
            </div>
          </div>
        ) : null}

        <header className="pointer-events-none absolute left-0 right-0 top-0 z-20 flex items-start justify-between px-10 py-8">
          <div className="pointer-events-auto flex items-start gap-4">
            <div
              className={cn(
                "mt-1.5 flex items-center overflow-hidden transition-all duration-500 ease-out",
                showDetails ? "w-8 opacity-100" : "w-0 opacity-0"
              )}
            >
              <button
                type="button"
                onClick={handleBackToOverview}
                className="text-slate-800 transition-all hover:-translate-x-1 hover:text-black"
              >
                <ArrowLeft className="h-7 w-7" strokeWidth={2.5} />
              </button>
            </div>

            <div className="relative flex flex-col">
              <h1
                className={cn(
                  "w-max text-3xl font-bold tracking-tight text-slate-900 transition-all duration-500",
                  showDetails ? "absolute top-0 translate-y-4 opacity-0" : "relative translate-y-0 opacity-100"
                )}
              >
                Go Fishing
              </h1>
              <h1
                className={cn(
                  "w-max text-3xl font-bold tracking-tight text-slate-900 transition-all duration-500",
                  showDetails ? "relative translate-y-0 opacity-100" : "absolute top-0 -translate-y-4 opacity-0"
                )}
              >
                Controller
              </h1>
              <h1
                className={cn(
                  "text-3xl font-bold tracking-tight text-slate-900 transition-all duration-500 delay-75",
                  showDetails ? "translate-y-0 opacity-100" : "h-0 -translate-y-4 overflow-hidden opacity-0"
                )}
              >
                MVP
              </h1>
            </div>
          </div>

          <div className="pointer-events-auto relative flex h-10 w-80 items-center justify-end">
            <div
              className={cn(
                "absolute right-0 flex items-center gap-6 whitespace-nowrap transition-all duration-500",
                showDetails ? "pointer-events-none translate-x-8 opacity-0" : "translate-x-0 opacity-100"
              )}
            >
              <button
                type="button"
                onClick={() => void handleConnect()}
                className="group flex items-center gap-2 text-sm font-semibold text-slate-800 transition-colors hover:text-black"
              >
                <Plus
                  className="h-5 w-5 text-slate-600 transition-all duration-300 group-hover:scale-110 group-hover:text-black"
                  strokeWidth={2.5}
                />
                <span>{t.addDevice}</span>
              </button>
              <div className="h-4 w-px bg-slate-300" />
              <button
                type="button"
                onClick={() => setIsSettingsOpen(true)}
                className="text-slate-600 transition-all duration-500 hover:rotate-90 hover:text-black"
              >
                <Settings className="h-6 w-6" strokeWidth={1.5} />
              </button>
            </div>

            <div
              className={cn(
                "absolute right-0 flex items-center whitespace-nowrap transition-all duration-500",
                showDetails ? "translate-x-0 opacity-100 delay-200" : "pointer-events-none translate-x-8 opacity-0"
              )}
            >
              {isStandaloneContentView ? null : (
                <div className="relative z-50">
                  <button
                    type="button"
                    onClick={() => setIsDropdownOpen((current) => !current)}
                    className="group flex items-center gap-2 rounded-xl border border-slate-200 bg-white px-4 py-2 shadow-sm transition-all duration-300 hover:border-slate-300 hover:shadow-md"
                  >
                    {selectedMapping ? (
                      <>
                        <selectedMapping.icon className="h-4 w-4 text-slate-500" />
                        <span className="text-sm font-semibold text-slate-800">{selectedMapping.name}</span>
                      </>
                    ) : (
                      <>
                        <Map className="h-4 w-4 text-slate-500 transition-colors duration-300 group-hover:text-[#7a5bff]" />
                        <span className="pl-1 text-sm font-semibold text-slate-800 transition-colors duration-300 group-hover:text-[#7a5bff]">
                          {t.mapping}
                        </span>
                      </>
                    )}
                    <ChevronDown
                      className={cn("h-4 w-4 text-slate-400 transition-transform duration-300", isDropdownOpen && "rotate-180")}
                    />
                  </button>

                  <div
                    className={cn(
                      "absolute right-0 mt-2 w-40 origin-top-right overflow-hidden rounded-xl border border-slate-100 bg-white shadow-[0_4px_20px_rgba(0,0,0,0.08)] transition-all duration-200",
                      isDropdownOpen ? "pointer-events-auto scale-100 opacity-100" : "pointer-events-none scale-95 opacity-0"
                    )}
                  >
                    <div className="py-1">
                      {mappingOptions.map((option) => (
                        <button
                          key={option.name}
                          type="button"
                          onClick={() => {
                            setSelectedMapping(option);
                            setIsDropdownOpen(false);
                          }}
                          className="group flex w-full items-center gap-3 px-4 py-3 text-left transition-colors hover:bg-slate-50"
                        >
                          <option.icon className="h-4 w-4 text-slate-400 transition-colors group-hover:text-[#7a5bff]" />
                          <span className="text-sm font-medium text-slate-700 transition-colors group-hover:text-slate-900">
                            {option.name}
                          </span>
                        </button>
                      ))}
                    </div>
                  </div>
                </div>
              )}
            </div>
          </div>
        </header>

        <div className="absolute inset-0">
          <div
            className={cn(
              "absolute inset-y-0 left-0 z-10 w-80 overflow-hidden transition-[opacity,transform] duration-500 ease-in-out",
              showDetails ? "pointer-events-auto translate-x-0 opacity-100" : "pointer-events-none -translate-x-8 opacity-0"
            )}
          >
            <aside
              className="flex h-full w-80 shrink-0 flex-col gap-4 px-8 pt-36"
            >
              <div className="mt-2 flex flex-col gap-3">
                {sidebarItems.map((item) => {
                  const ItemIcon = item.icon;
                  const isActive = activeSection === item.id;

                  return (
                    <button
                      key={item.id}
                      type="button"
                      onClick={() => handleSelectSection(item.id)}
                      className={cn(
                        "flex items-center gap-4 rounded-xl px-5 py-3.5 font-medium transition-all",
                        isActive
                          ? "bg-[#7a5bff] text-white shadow-lg shadow-[#7a5bff]/25"
                          : "text-slate-700 hover:bg-slate-100"
                      )}
                    >
                      <ItemIcon className="h-5 w-5" strokeWidth={2} />
                      <span>{item.label}</span>
                    </button>
                  );
                })}
              </div>

              <BatteryBadge
                isConnected={isReceiverConnected}
                percent={mockBatteryPercent}
                reconnectLabel={t.reconnect}
                onReconnect={() => void handleConnect()}
                className="mb-10 mt-auto w-max"
              />
            </aside>
          </div>

          <main
            className={cn(
              "absolute inset-y-0 right-0 flex min-w-0 flex-col overflow-hidden transition-[left] duration-500 ease-in-out",
              showDetails ? "left-80" : "left-0",
              isStandaloneContentView ? "items-stretch justify-start" : "items-center justify-center"
            )}
          >
            {isSettingsView ? (
              <div className="h-full w-full overflow-y-auto overscroll-contain">
                <SettingsPanel
                  language={language}
                  t={t}
                  isResetConfirming={isResetConfirming}
                  hasResetCompleted={hasResetCompleted}
                  onResetClick={handleResetMappings}
                />
              </div>
            ) : isSmartWheelView ? (
              <div className="h-full w-full overflow-y-auto overscroll-contain">
                <SmartWheelPanel
                  language={language}
                  isResistanceEnabled={isWheelResistanceEnabled}
                  baseGain={wheelBaseGain}
                  isAiGainEnabled={isWheelAiGainEnabled}
                  selectedGameId={selectedWheelGameId}
                  screenPullPercent={wheelScreenPullPercent}
                  aiPullTestMessage={aiPullTestMessage}
                  onResistanceEnabledChange={handleWheelResistanceEnabledChange}
                  onBaseGainChange={setWheelBaseGain}
                  onAiGainEnabledChange={handleWheelAiGainEnabledChange}
                  onSelectedGameChange={setSelectedWheelGameId}
                  onAiPullTestSend={() => void handleSendAiPullTest()}
                />
              </div>
            ) : isMotionMappingView ? (
              <div className="h-full w-full overflow-y-auto overscroll-contain">
                <MotionMappingPanel
                  language={language}
                  viewDeadzone={motionViewDeadzone}
                  viewSensitivity={motionViewSensitivity}
                  castDeadzone={motionCastDeadzone}
                  onViewDeadzoneChange={setMotionViewDeadzone}
                  onViewSensitivityChange={setMotionViewSensitivity}
                  onCastDeadzoneChange={setMotionCastDeadzone}
                />
              </div>
            ) : (
              <div
                role={!showDetails ? "button" : undefined}
                tabIndex={!showDetails ? 0 : -1}
                onClick={() => {
                  if (!showDetails) {
                    handleOpenDetails();
                  }
                }}
                onKeyDown={(event) => {
                  if (!showDetails && (event.key === "Enter" || event.key === " ")) {
                    event.preventDefault();
                    handleOpenDetails();
                  }
                }}
                className={cn(
                  "relative flex h-[min(640px,calc(100vh_-_160px))] items-center justify-center outline-none",
                  showDetails ? "w-[min(880px,calc(100vw_-_384px))]" : "w-[min(880px,calc(100vw_-_64px))]",
                  !showDetails && "cursor-pointer"
                )}
              >
                <div className="pointer-events-none absolute left-1/2 top-0 h-full w-[min(880px,calc(100vw_-_64px))] -translate-x-1/2">
                  <Canvas camera={{ position: [0, 0.85, 8.5], fov: 45 }}>
                    <GamepadPreviewCamera />
                    <GamepadSceneLighting />
                    <Suspense fallback={<FallbackGamepad />}>
                      <Stage
                        intensity={0}
                        environment={null}
                        shadows={false}
                        adjustCamera={false}
                      >
                        <GamepadModel isDimmed={!showDetails && !isControllerConnected} />
                      </Stage>
                    </Suspense>
                  </Canvas>
                </div>

                <div
                  className={cn(
                    "pointer-events-none absolute inset-0 z-10 transition-opacity delay-300 duration-500",
                    showModelLabels ? "opacity-100" : "opacity-0"
                  )}
                >
                  <div className="absolute left-6 top-[50%] z-20 flex -translate-y-1/2 flex-col items-start gap-2.5">
                    {BASIC_MAPPING_CONTROLS.map((control) => (
                      <button
                        key={control.id}
                        type="button"
                        onClick={() => handleBasicControlClick(control.id)}
                        className={cn(
                          "pointer-events-auto flex items-center text-left transition",
                          control.id === "stick" ? "cursor-pointer" : "cursor-default"
                        )}
                      >
                        <span
                          className={cn(
                            "whitespace-nowrap rounded-md border px-3 py-1.5 text-xs font-semibold shadow-xl transition",
                            activeBasicConfig === control.id
                              ? "border-[#7a5bff] bg-[#7a5bff] text-white"
                              : control.id === "stick"
                                ? "border-slate-100 bg-white text-slate-800 hover:border-slate-200 hover:text-[#7a5bff]"
                                : "border-slate-100 bg-white text-slate-700"
                          )}
                        >
                          {t.basicMappingLabels[control.id]}
                        </span>
                        <div className="ml-2 h-px w-10 bg-slate-300" />
                      </button>
                    ))}
                  </div>

                  {activeBasicConfig === "stick" ? (
                    <StickConfigPanel
                      language={language}
                      deadzone={stickDeadzone}
                      maxDeadzone={STICK_DEADZONE_MAX}
                      isSaving={isSavingStickConfig}
                      statusMessage={stickConfigMessage}
                      onDeadzoneChange={setStickDeadzone}
                      onReset={handleResetStickDeadzone}
                      onApply={() => void handleApplyStickConfig()}
                      onClose={() => setActiveBasicConfig(null)}
                    />
                  ) : null}
                </div>
              </div>
            )}

            <BatteryBadge
              isConnected={isReceiverConnected}
              percent={mockBatteryPercent}
              reconnectLabel={t.reconnect}
              onReconnect={() => void handleConnect()}
              className={cn(
                "absolute bottom-24 transition-all duration-500",
                showDetails ? "pointer-events-none translate-y-8 opacity-0" : "translate-y-0 opacity-100"
              )}
            />
          </main>
        </div>
      </div>
    </div>
  );
}
