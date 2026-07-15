export type ConnectionState = "disconnected" | "connecting" | "connected" | "error";
export type TriggerTarget = "LT" | "RT";
export type ActiveSection = "overview" | "mapping" | "joystick" | "other";

export interface DeviceStatus {
  protocolVersion: number;
  receiverUsbOnline: boolean;
  controllerConnected: boolean;
  lastInputAgeMs: number;
  downwardAccel: number;
  triggerActive: boolean;
  triggerTarget: TriggerTarget;
}

export interface DeviceConfig {
  triggerTarget: TriggerTarget;
  leftDeadzone: number;
  rightDeadzone: number;
}

export interface DeviceConnectionEvent {
  connection: ConnectionState;
  message?: string;
}

export interface AiScreenPullEvent {
  pullPercent: number;
  ratio: number;
  ageMs: number;
}

