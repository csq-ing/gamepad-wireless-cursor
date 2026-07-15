use std::ffi::CString;
use std::sync::mpsc::{self, Receiver, Sender};
use std::sync::Mutex;
use std::thread::{self, JoinHandle};
use std::time::{Duration, Instant};

use hidapi::{HidApi, HidDevice};
use serde::{Deserialize, Serialize};
use tauri::{AppHandle, Emitter, Manager, State};

mod ai_screen_pull;

const DEVICE_VID: u16 = 0xCAFE;
const DEVICE_PID: u16 = 0x4012;
const DEVICE_INTERFACE_NUMBER: i32 = 1;

const REPORT_SIZE: usize = 64;
const REPORT_SIZE_WITH_ID: usize = 65;
const POLL_INTERVAL_MS: u64 = 250;
const AI_SCREEN_PULL_INTERVAL_MS: u64 = 33;
const IO_TIMEOUT_MS: i32 = 1200;

const CMD_GET_STATUS: u8 = 0x01;
const CMD_GET_CONFIG: u8 = 0x02;
const CMD_SET_CONFIG: u8 = 0x03;
const CMD_SAVE_CONFIG: u8 = 0x04;
const CMD_GET_FULL_CONFIG: u8 = 0x05;
const CMD_SET_FULL_CONFIG: u8 = 0x06;
const CMD_SAVE_FULL_CONFIG: u8 = 0x07;
const CMD_SET_AI_PULL: u8 = 0x08;
const DEADZONE_MAX: u16 = 1024;
const LEGACY_DEADZONE_DEFAULT: u16 = 64;
const AI_PULL_ZERO_SUPPRESS_AFTER: Duration = Duration::from_millis(1000);

const STATUS_OK: u8 = 0x00;
const STATUS_INVALID_CMD: u8 = 0x01;
const STATUS_INVALID_ARG: u8 = 0x02;
const STATUS_INTERNAL_ERR: u8 = 0x03;

#[derive(Clone, Copy, Debug, Serialize, Deserialize, PartialEq, Eq)]
#[serde(rename_all = "UPPERCASE")]
enum TriggerTarget {
    LT,
    RT,
}

impl TriggerTarget {
    fn from_u8(value: u8) -> Result<Self, String> {
        match value {
            1 => Ok(Self::LT),
            2 => Ok(Self::RT),
            _ => Err(format!("invalid trigger target value: {value}")),
        }
    }

    fn from_text(value: &str) -> Result<Self, String> {
        match value.trim().to_ascii_uppercase().as_str() {
            "LT" => Ok(Self::LT),
            "RT" => Ok(Self::RT),
            _ => Err("trigger target must be LT or RT".to_string()),
        }
    }

    fn as_u8(self) -> u8 {
        match self {
            Self::LT => 1,
            Self::RT => 2,
        }
    }
}

#[derive(Clone, Debug, Serialize)]
#[serde(rename_all = "camelCase")]
struct DeviceStatus {
    protocol_version: u8,
    receiver_usb_online: bool,
    controller_connected: bool,
    last_input_age_ms: u32,
    downward_accel: u16,
    trigger_active: bool,
    trigger_target: TriggerTarget,
}

fn default_payload_format() -> ConfigPayloadFormat {
    ConfigPayloadFormat::Full
}

#[derive(Clone, Debug, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
struct DeviceConfig {
    trigger_target: TriggerTarget,
    left_deadzone: u16,
    right_deadzone: u16,
    #[serde(skip, default = "default_payload_format")]
    payload_format: ConfigPayloadFormat,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum ConfigPayloadFormat {
    Legacy,
    Full,
}

#[derive(Clone, Copy, Debug, Serialize)]
#[serde(rename_all = "lowercase")]
enum ConnectionState {
    Disconnected,
    Connecting,
    Connected,
    Error,
}

#[derive(Clone, Debug, Serialize)]
#[serde(rename_all = "camelCase")]
struct DeviceConnectionEvent {
    connection: ConnectionState,
    message: Option<String>,
}

#[derive(Clone, Debug, Serialize)]
#[serde(rename_all = "camelCase")]
struct AiScreenPullEvent {
    pull_percent: u8,
    ratio: f64,
    age_ms: u32,
}

#[derive(Default)]
struct BackendState {
    inner: Mutex<InnerState>,
    io_lock: Mutex<()>,
}

#[derive(Default)]
struct InnerState {
    device_path: Option<Vec<u8>>,
    poller_stop: Option<Sender<()>>,
    poller_handle: Option<JoinHandle<()>>,
    ai_pull_stop: Option<Sender<()>>,
    ai_pull_handle: Option<JoinHandle<()>>,
}

fn emit_connection(app: &AppHandle, connection: ConnectionState, message: Option<String>) {
    let payload = DeviceConnectionEvent {
        connection,
        message,
    };
    let _ = app.emit("device-connection", payload);
}

fn read_u16le(bytes: &[u8]) -> u16 {
    u16::from_le_bytes([bytes[0], bytes[1]])
}

fn read_u32le(bytes: &[u8]) -> u32 {
    u32::from_le_bytes([bytes[0], bytes[1], bytes[2], bytes[3]])
}

fn validate_deadzone(value: u16) -> Result<u16, String> {
    if value <= DEADZONE_MAX {
        Ok(value)
    } else {
        Err(format!(
            "deadzone value must be at most {DEADZONE_MAX}, got {value}"
        ))
    }
}

fn ratio_to_pull_percent(ratio: f64) -> u8 {
    if !ratio.is_finite() {
        return 0;
    }

    (ratio * 100.0).round().clamp(0.0, 100.0) as u8
}

fn should_log_ai_screen_pull_error(error: &str) -> bool {
    !error.contains("No circle found")
}

fn ai_screen_pull_event_from_ratio_result(
    ratio_result: std::result::Result<f64, String>,
    elapsed: Duration,
) -> AiScreenPullEvent {
    let ratio = ratio_result.unwrap_or(0.0);
    AiScreenPullEvent {
        pull_percent: ratio_to_pull_percent(ratio),
        ratio,
        age_ms: elapsed.as_millis().min(u32::MAX as u128) as u32,
    }
}

#[derive(Default)]
struct AiPullForwardGate {
    zero_started_at: Option<Instant>,
}

impl AiPullForwardGate {
    fn should_forward(&mut self, pull_percent: u8, now: Instant) -> bool {
        if pull_percent != 0 {
            self.zero_started_at = None;
            return true;
        }

        let started_at = match self.zero_started_at {
            Some(started_at) => started_at,
            None => {
                self.zero_started_at = Some(now);
                return true;
            }
        };

        now.duration_since(started_at) < AI_PULL_ZERO_SUPPRESS_AFTER
    }
}

impl DeviceConfig {
    fn from_payload(payload: &[u8]) -> Result<Self, String> {
        match payload.len() {
            1 => Ok(Self {
                trigger_target: TriggerTarget::from_u8(payload[0])?,
                left_deadzone: LEGACY_DEADZONE_DEFAULT,
                right_deadzone: LEGACY_DEADZONE_DEFAULT,
                payload_format: ConfigPayloadFormat::Legacy,
            }),
            5 => Ok(Self {
                trigger_target: TriggerTarget::from_u8(payload[0])?,
                left_deadzone: validate_deadzone(read_u16le(&payload[1..3]))?,
                right_deadzone: validate_deadzone(read_u16le(&payload[3..5]))?,
                payload_format: ConfigPayloadFormat::Full,
            }),
            _ => Err("config payload must be 1 or 5 bytes".to_string()),
        }
    }

    fn to_payload(&self) -> Result<Vec<u8>, String> {
        let left_deadzone = validate_deadzone(self.left_deadzone)?;
        let right_deadzone = validate_deadzone(self.right_deadzone)?;

        let mut payload = Vec::with_capacity(5);
        payload.push(self.trigger_target.as_u8());
        payload.extend_from_slice(&left_deadzone.to_le_bytes());
        payload.extend_from_slice(&right_deadzone.to_le_bytes());
        Ok(payload)
    }

    fn to_set_payload(&self, target: TriggerTarget) -> Result<Vec<u8>, String> {
        match self.payload_format {
            ConfigPayloadFormat::Legacy => Ok(vec![target.as_u8()]),
            ConfigPayloadFormat::Full => {
                let updated = Self {
                    trigger_target: target,
                    left_deadzone: self.left_deadzone,
                    right_deadzone: self.right_deadzone,
                    payload_format: ConfigPayloadFormat::Full,
                };
                updated.to_payload()
            }
        }
    }

    fn validate(&self) -> Result<(), String> {
        let _ = validate_deadzone(self.left_deadzone)?;
        let _ = validate_deadzone(self.right_deadzone)?;
        Ok(())
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn decode_config_payload_reads_trigger_and_deadzones() {
        let config = DeviceConfig::from_payload(&[1, 0x01, 0x02, 0x04, 0x03]).unwrap();

        assert_eq!(config.trigger_target, TriggerTarget::LT);
        assert_eq!(config.left_deadzone, 0x0201);
        assert_eq!(config.right_deadzone, 0x0304);
    }

    #[test]
    fn encode_config_payload_writes_little_endian_deadzones() {
        let config = DeviceConfig {
            trigger_target: TriggerTarget::RT,
            left_deadzone: 0x0201,
            right_deadzone: 0x0304,
            payload_format: ConfigPayloadFormat::Full,
        };

        assert_eq!(
            config.to_payload().unwrap(),
            vec![2, 0x01, 0x02, 0x04, 0x03]
        );
    }

    #[test]
    fn encode_config_payload_rejects_invalid_deadzones() {
        let config = DeviceConfig {
            trigger_target: TriggerTarget::RT,
            left_deadzone: 1025,
            right_deadzone: 0,
            payload_format: ConfigPayloadFormat::Full,
        };

        assert!(config.to_payload().is_err());
    }

    #[test]
    fn set_config_rejects_deadzone_above_maximum_before_hid_write() {
        let config = DeviceConfig {
            trigger_target: TriggerTarget::LT,
            left_deadzone: 1200,
            right_deadzone: 64,
            payload_format: ConfigPayloadFormat::Full,
        };

        assert!(config.validate().is_err());
    }

    #[test]
    fn legacy_set_config_write_stays_one_byte() {
        let legacy_config = DeviceConfig {
            trigger_target: TriggerTarget::LT,
            left_deadzone: LEGACY_DEADZONE_DEFAULT,
            right_deadzone: LEGACY_DEADZONE_DEFAULT,
            payload_format: ConfigPayloadFormat::Legacy,
        };

        assert_eq!(
            legacy_config.to_set_payload(TriggerTarget::RT).unwrap(),
            vec![TriggerTarget::RT.as_u8()]
        );
    }

    #[test]
    fn validate_deadzone_rejects_values_above_maximum() {
        assert_eq!(validate_deadzone(1024).unwrap(), 1024);
        assert!(validate_deadzone(1025).is_err());
    }

    #[test]
    fn decode_legacy_get_config_response_accepts_single_byte_payload() {
        let raw = [0, CMD_GET_CONFIG, STATUS_OK, 2];
        let payload = extract_response(&raw, CMD_GET_CONFIG).unwrap();
        let config = parse_config_payload(payload, CMD_GET_CONFIG).unwrap();

        assert_eq!(config.trigger_target, TriggerTarget::RT);
        assert_eq!(config.left_deadzone, LEGACY_DEADZONE_DEFAULT);
        assert_eq!(config.right_deadzone, LEGACY_DEADZONE_DEFAULT);
    }

    #[test]
    fn decode_future_save_config_response_accepts_full_payload() {
        let raw = [CMD_SAVE_FULL_CONFIG, STATUS_OK, 1, 0x40, 0x00, 0x40, 0x00];
        let payload = extract_response(&raw, CMD_SAVE_FULL_CONFIG).unwrap();
        let config = parse_config_payload(payload, CMD_SAVE_FULL_CONFIG).unwrap();

        assert_eq!(config.trigger_target, TriggerTarget::LT);
        assert_eq!(config.left_deadzone, 64);
        assert_eq!(config.right_deadzone, 64);
    }

    #[test]
    fn decode_padded_legacy_response_ignores_trailing_zero_fill() {
        let payload = [2, 0, 0, 0];
        let config = parse_config_payload(&payload, CMD_GET_CONFIG).unwrap();

        assert_eq!(config.trigger_target, TriggerTarget::RT);
        assert_eq!(config.left_deadzone, LEGACY_DEADZONE_DEFAULT);
        assert_eq!(config.right_deadzone, LEGACY_DEADZONE_DEFAULT);
    }

    #[test]
    fn decode_padded_full_response_ignores_trailing_zero_fill() {
        let payload = [1, 0x40, 0x00, 0x40, 0x00, 0, 0, 0];
        let config = parse_config_payload(&payload, CMD_GET_FULL_CONFIG).unwrap();

        assert_eq!(config.trigger_target, TriggerTarget::LT);
        assert_eq!(config.left_deadzone, 64);
        assert_eq!(config.right_deadzone, 64);
    }

    #[test]
    fn invalid_command_error_matches_unknown_command_response() {
        assert!(is_invalid_cmd_error(&translate_status_error(
            STATUS_INVALID_CMD
        )));
    }

    #[test]
    fn ratio_to_pull_percent_rounds_and_clamps() {
        assert_eq!(ratio_to_pull_percent(0.0), 0);
        assert_eq!(ratio_to_pull_percent(0.634), 63);
        assert_eq!(ratio_to_pull_percent(0.635), 64);
        assert_eq!(ratio_to_pull_percent(1.0), 100);
        assert_eq!(ratio_to_pull_percent(1.2), 100);
        assert_eq!(ratio_to_pull_percent(-0.5), 0);
        assert_eq!(ratio_to_pull_percent(f64::NAN), 0);
    }

    #[test]
    fn ai_screen_pull_no_circle_error_is_not_logged() {
        assert!(!should_log_ai_screen_pull_error("No circle found"));
        assert!(!should_log_ai_screen_pull_error(
            "AI screen pull update failed: No circle found"
        ));
        assert!(should_log_ai_screen_pull_error("PrintWindow failed"));
    }

    #[test]
    fn ai_screen_pull_event_resets_to_zero_when_ratio_capture_fails() {
        let payload =
            ai_screen_pull_event_from_ratio_result(Err("No circle found".into()), Duration::ZERO);

        assert_eq!(payload.pull_percent, 0);
        assert_eq!(payload.ratio, 0.0);
        assert_eq!(payload.age_ms, 0);
    }

    #[test]
    fn encode_ai_pull_command_report_writes_command_and_percent_payload() {
        let report = encode_command_report(CMD_SET_AI_PULL, &[63]);

        assert_eq!(report[0], 0);
        assert_eq!(report[1], CMD_SET_AI_PULL);
        assert_eq!(report[2], 63);
        assert!(report[3..].iter().all(|&byte| byte == 0));
    }

    #[test]
    fn ai_pull_payload_clamps_manual_test_value_to_protocol_range() {
        assert_eq!(ai_pull_payload(63), [63]);
        assert_eq!(ai_pull_payload(101), [100]);
    }

    #[test]
    fn ai_pull_path_selection_uses_cached_path_before_discovery() {
        let cached = Some(vec![1, 2, 3]);
        let discovered = Err("receiver HID interface not found".to_string());

        assert_eq!(
            select_ai_pull_path(cached, discovered).unwrap(),
            vec![1, 2, 3]
        );
    }

    #[test]
    fn ai_pull_path_selection_discovers_path_when_cache_is_empty() {
        let discovered = Ok(vec![4, 5, 6]);

        assert_eq!(
            select_ai_pull_path(None, discovered).unwrap(),
            vec![4, 5, 6]
        );
    }

    #[test]
    fn ai_pull_forward_gate_suppresses_zero_after_one_second_until_nonzero_returns() {
        let start = Instant::now();
        let mut gate = AiPullForwardGate::default();

        assert!(gate.should_forward(0, start));
        assert!(gate.should_forward(0, start + Duration::from_millis(999)));
        assert!(!gate.should_forward(0, start + Duration::from_millis(1000)));
        assert!(!gate.should_forward(0, start + Duration::from_millis(1500)));
        assert!(gate.should_forward(24, start + Duration::from_millis(1600)));
        assert!(gate.should_forward(0, start + Duration::from_millis(1700)));
    }

    #[test]
    fn install_ai_screen_pull_worker_rejects_duplicate_start() {
        let (first_stop_tx, first_stop_rx) = mpsc::channel();
        let first_handle = thread::spawn(move || {
            let _ = first_stop_rx.recv();
        });
        let mut inner = InnerState::default();
        install_ai_screen_pull_worker(&mut inner, first_stop_tx, first_handle).unwrap();

        let (second_stop_tx, second_stop_rx) = mpsc::channel();
        let second_handle = thread::spawn(move || {
            let _ = second_stop_rx.recv();
        });

        let result = install_ai_screen_pull_worker(&mut inner, second_stop_tx, second_handle);

        assert!(result.is_err());
        assert_eq!(
            result.unwrap_err(),
            "AI screen pull worker is already running"
        );
        stop_ai_screen_pull_worker(&mut inner);
    }

    #[test]
    fn stop_ai_screen_pull_worker_clears_worker_state() {
        let (stop_tx, stop_rx) = mpsc::channel();
        let handle = thread::spawn(move || {
            let _ = stop_rx.recv();
        });
        let mut inner = InnerState::default();
        install_ai_screen_pull_worker(&mut inner, stop_tx, handle).unwrap();

        stop_ai_screen_pull_worker(&mut inner);

        assert!(inner.ai_pull_stop.is_none());
        assert!(inner.ai_pull_handle.is_none());
    }
}

fn translate_status_error(code: u8) -> String {
    match code {
        STATUS_INVALID_CMD => "device rejected unknown command".to_string(),
        STATUS_INVALID_ARG => "device rejected invalid argument".to_string(),
        STATUS_INTERNAL_ERR => "device reported internal error".to_string(),
        _ => format!("device returned unknown status code {code}"),
    }
}

fn is_invalid_cmd_error(error: &str) -> bool {
    error == translate_status_error(STATUS_INVALID_CMD)
}

fn config_payload_len_for_cmd(cmd_id: u8) -> Result<usize, String> {
    match cmd_id {
        CMD_GET_CONFIG | CMD_SET_CONFIG | CMD_SAVE_CONFIG => Ok(1),
        CMD_GET_FULL_CONFIG | CMD_SET_FULL_CONFIG | CMD_SAVE_FULL_CONFIG => Ok(5),
        _ => Err(format!(
            "command 0x{cmd_id:02X} does not carry config payload"
        )),
    }
}

fn parse_config_payload(payload: &[u8], cmd_id: u8) -> Result<DeviceConfig, String> {
    let expected_len = config_payload_len_for_cmd(cmd_id)?;
    if payload.len() < expected_len {
        return Err(format!(
            "config payload too short: expected {expected_len} bytes, got {}",
            payload.len()
        ));
    }

    DeviceConfig::from_payload(&payload[..expected_len])
}

fn extract_response(raw: &[u8], expected_cmd: u8) -> Result<&[u8], String> {
    if raw.len() < 2 {
        return Err("response too short".to_string());
    }

    let (cmd, status, payload) = if raw[0] == 0 && raw.len() >= 3 {
        (raw[1], raw[2], &raw[3..])
    } else {
        (raw[0], raw[1], &raw[2..])
    };

    if cmd != expected_cmd {
        return Err(format!(
            "mismatched response command, expected 0x{expected_cmd:02X}, got 0x{cmd:02X}"
        ));
    }

    if status != STATUS_OK {
        return Err(translate_status_error(status));
    }

    Ok(payload)
}

fn find_device_path() -> Result<Vec<u8>, String> {
    let api = HidApi::new().map_err(|e| format!("hid init failed: {e}"))?;
    for info in api.device_list() {
        if info.vendor_id() == DEVICE_VID
            && info.product_id() == DEVICE_PID
            && info.interface_number() == DEVICE_INTERFACE_NUMBER
        {
            return Ok(info.path().to_bytes().to_vec());
        }
    }

    Err(format!(
        "receiver HID interface not found (VID=0x{DEVICE_VID:04X}, PID=0x{DEVICE_PID:04X}, interface={DEVICE_INTERFACE_NUMBER})"
    ))
}

fn open_device(path: &[u8]) -> Result<HidDevice, String> {
    let api = HidApi::new().map_err(|e| format!("hid init failed: {e}"))?;
    let c_path = CString::new(path).map_err(|_| "invalid HID path bytes".to_string())?;
    api.open_path(c_path.as_c_str())
        .map_err(|e| format!("failed to open HID device: {e}"))
}

fn encode_command_report(cmd_id: u8, payload: &[u8]) -> [u8; REPORT_SIZE_WITH_ID] {
    let mut out_report = [0u8; REPORT_SIZE_WITH_ID];
    out_report[1] = cmd_id;
    let copy_len = payload.len().min(REPORT_SIZE - 1);
    out_report[2..2 + copy_len].copy_from_slice(&payload[..copy_len]);
    out_report
}

fn send_command(path: &[u8], cmd_id: u8, payload: &[u8]) -> Result<Vec<u8>, String> {
    let device = open_device(path)?;
    let out_report = encode_command_report(cmd_id, payload);

    device
        .write(&out_report)
        .map_err(|e| format!("write failed: {e}"))?;

    let mut in_report = [0u8; REPORT_SIZE_WITH_ID];
    let read_len = device
        .read_timeout(&mut in_report, IO_TIMEOUT_MS)
        .map_err(|e| format!("read failed: {e}"))?;

    if read_len == 0 {
        return Err("read timeout".to_string());
    }

    Ok(in_report[..read_len].to_vec())
}

fn read_status(path: &[u8]) -> Result<DeviceStatus, String> {
    let raw = send_command(path, CMD_GET_STATUS, &[])?;
    let payload = extract_response(&raw, CMD_GET_STATUS)?;
    if payload.len() < 11 {
        return Err("status payload too short".to_string());
    }

    let trigger_target = TriggerTarget::from_u8(payload[10])?;
    Ok(DeviceStatus {
        protocol_version: payload[0],
        receiver_usb_online: payload[1] != 0,
        controller_connected: payload[2] != 0,
        last_input_age_ms: read_u32le(&payload[3..7]),
        downward_accel: read_u16le(&payload[7..9]),
        trigger_active: payload[9] != 0,
        trigger_target,
    })
}

fn read_device_config(path: &[u8]) -> Result<DeviceConfig, String> {
    let raw = send_command(path, CMD_GET_FULL_CONFIG, &[])?;
    match parse_device_config_response(&raw, CMD_GET_FULL_CONFIG) {
        Ok(config) => Ok(config),
        Err(err) if is_invalid_cmd_error(&err) => {
            let legacy_raw = send_command(path, CMD_GET_CONFIG, &[])?;
            parse_device_config_response(&legacy_raw, CMD_GET_CONFIG)
        }
        Err(err) => Err(err),
    }
}

fn send_set_config(path: &[u8], config: &DeviceConfig) -> Result<DeviceConfig, String> {
    config.validate()?;
    let raw = send_command(path, CMD_SET_FULL_CONFIG, &config.to_payload()?)?;
    match parse_device_config_response(&raw, CMD_SET_FULL_CONFIG) {
        Ok(updated) => Ok(updated),
        Err(err) if is_invalid_cmd_error(&err) => {
            let current = read_device_config(path)?;
            if current.left_deadzone != config.left_deadzone
                || current.right_deadzone != config.right_deadzone
            {
                return Err(
                    "connected receiver firmware does not support joystick deadzone configuration"
                        .to_string(),
                );
            }

            let payload = current.to_set_payload(config.trigger_target)?;
            let legacy_raw = send_command(path, CMD_SET_CONFIG, &payload)?;
            parse_device_config_response(&legacy_raw, CMD_SET_CONFIG)
        }
        Err(err) => Err(err),
    }
}

fn set_target(path: &[u8], target: TriggerTarget) -> Result<DeviceConfig, String> {
    let current = read_device_config(path)?;
    match current.payload_format {
        ConfigPayloadFormat::Legacy => {
            let payload = current.to_set_payload(target)?;
            let raw = send_command(path, CMD_SET_CONFIG, &payload)?;
            parse_device_config_response(&raw, CMD_SET_CONFIG)
        }
        ConfigPayloadFormat::Full => {
            let updated = DeviceConfig {
                trigger_target: target,
                left_deadzone: current.left_deadzone,
                right_deadzone: current.right_deadzone,
                payload_format: ConfigPayloadFormat::Full,
            };
            let raw = send_command(path, CMD_SET_FULL_CONFIG, &updated.to_payload()?)?;
            parse_device_config_response(&raw, CMD_SET_FULL_CONFIG)
        }
    }
}

fn save_device_config(path: &[u8]) -> Result<DeviceConfig, String> {
    let raw = send_command(path, CMD_SAVE_FULL_CONFIG, &[])?;
    match parse_device_config_response(&raw, CMD_SAVE_FULL_CONFIG) {
        Ok(saved) => Ok(saved),
        Err(err) if is_invalid_cmd_error(&err) => {
            let legacy_raw = send_command(path, CMD_SAVE_CONFIG, &[])?;
            parse_device_config_response(&legacy_raw, CMD_SAVE_CONFIG)
        }
        Err(err) => Err(err),
    }
}

fn ai_pull_payload(pull_percent: u8) -> [u8; 1] {
    [pull_percent.min(100)]
}

fn send_ai_pull(path: &[u8], pull_percent: u8) -> Result<(), String> {
    let payload = ai_pull_payload(pull_percent);
    let raw = send_command(path, CMD_SET_AI_PULL, &payload)?;
    let _ = extract_response(&raw, CMD_SET_AI_PULL)?;
    Ok(())
}

fn select_ai_pull_path(
    cached_path: Option<Vec<u8>>,
    discovered_path: Result<Vec<u8>, String>,
) -> Result<Vec<u8>, String> {
    match cached_path {
        Some(path) => Ok(path),
        None => discovered_path,
    }
}

fn get_ai_pull_path(state: &State<BackendState>) -> Result<Vec<u8>, String> {
    let cached_path = {
        let guard = state
            .inner
            .lock()
            .map_err(|_| "state lock poisoned".to_string())?;
        guard.device_path.clone()
    };

    let path = if cached_path.is_some() {
        select_ai_pull_path(cached_path, Err(String::new()))?
    } else {
        select_ai_pull_path(None, find_device_path())?
    };

    if let Ok(mut guard) = state.inner.lock() {
        if guard.device_path.is_none() {
            guard.device_path = Some(path.clone());
        }
    }

    Ok(path)
}

fn forward_ai_pull_to_connected_device(app: &AppHandle, pull_percent: u8) -> Result<(), String> {
    let state = app.state::<BackendState>();
    let path = get_ai_pull_path(&state)?;

    let _io_guard = state
        .io_lock
        .lock()
        .map_err(|_| "io lock poisoned".to_string())?;
    send_ai_pull(&path, pull_percent)
}

fn parse_device_config_response(raw: &[u8], expected_cmd: u8) -> Result<DeviceConfig, String> {
    let payload = extract_response(raw, expected_cmd)?;
    parse_config_payload(payload, expected_cmd)
}

fn get_current_path(state: &State<BackendState>) -> Result<Vec<u8>, String> {
    let guard = state
        .inner
        .lock()
        .map_err(|_| "state lock poisoned".to_string())?;
    guard
        .device_path
        .clone()
        .ok_or_else(|| "device not connected".to_string())
}

fn stop_existing_poller(state: &State<BackendState>) {
    let (stop_sender, handle) = {
        let mut guard = match state.inner.lock() {
            Ok(guard) => guard,
            Err(_) => return,
        };
        (guard.poller_stop.take(), guard.poller_handle.take())
    };

    if let Some(tx) = stop_sender {
        let _ = tx.send(());
    }
    if let Some(handle) = handle {
        let _ = handle.join();
    }
}

fn install_ai_screen_pull_worker(
    inner: &mut InnerState,
    stop_sender: Sender<()>,
    handle: JoinHandle<()>,
) -> Result<(), String> {
    if inner.ai_pull_stop.is_some() || inner.ai_pull_handle.is_some() {
        let _ = stop_sender.send(());
        let _ = handle.join();
        return Err("AI screen pull worker is already running".to_string());
    }

    inner.ai_pull_stop = Some(stop_sender);
    inner.ai_pull_handle = Some(handle);
    Ok(())
}

fn stop_ai_screen_pull_worker(inner: &mut InnerState) {
    let stop_sender = inner.ai_pull_stop.take();
    let handle = inner.ai_pull_handle.take();

    if let Some(tx) = stop_sender {
        let _ = tx.send(());
    }
    if let Some(handle) = handle {
        let _ = handle.join();
    }
}

fn spawn_ai_screen_pull_worker(app: AppHandle, stop_rx: Receiver<()>) -> JoinHandle<()> {
    thread::spawn(move || {
        let mut forward_gate = AiPullForwardGate::default();
        let mut forwarding_supported = true;
        let mut pull_estimator = ai_screen_pull::ForegroundPullEstimator::new();

        loop {
            let iter_start = Instant::now();

            let ratio_result = match pull_estimator.estimate_foreground_pull_ratio(iter_start) {
                Ok(ratio) => Ok(ratio),
                Err(err) => {
                    let error = err.to_string();
                    if should_log_ai_screen_pull_error(&error) {
                        eprintln!("AI screen pull update failed: {error}");
                    }
                    Err(error)
                }
            };
            let payload =
                ai_screen_pull_event_from_ratio_result(ratio_result, iter_start.elapsed());
            let pull_percent = payload.pull_percent;
            let _ = app.emit("ai-screen-pull", payload);

            if forwarding_supported && forward_gate.should_forward(pull_percent, Instant::now()) {
                match forward_ai_pull_to_connected_device(&app, pull_percent) {
                    Ok(()) => {}
                    Err(err) if is_invalid_cmd_error(&err) => {
                        forwarding_supported = false;
                        eprintln!(
                            "AI pull forwarding disabled: receiver firmware does not support it"
                        );
                    }
                    Err(err) => {
                        eprintln!("AI pull forwarding failed: {err}");
                    }
                }
            }

            let elapsed = iter_start.elapsed();
            let interval = Duration::from_millis(AI_SCREEN_PULL_INTERVAL_MS);
            let wait = interval.saturating_sub(elapsed);
            if stop_rx.recv_timeout(wait).is_ok() {
                break;
            }
        }
    })
}

fn spawn_poller(app: AppHandle, path: Vec<u8>, stop_rx: Receiver<()>) -> JoinHandle<()> {
    thread::spawn(move || loop {
        let status_result = {
            let state = app.state::<BackendState>();
            let _io_guard = match state.io_lock.lock() {
                Ok(guard) => guard,
                Err(_) => {
                    emit_connection(
                        &app,
                        ConnectionState::Error,
                        Some("io lock poisoned".to_string()),
                    );
                    break;
                }
            };
            read_status(&path)
        };

        match status_result {
            Ok(status) => {
                let _ = app.emit("device-status", status);
            }
            Err(err) => {
                emit_connection(&app, ConnectionState::Disconnected, Some(err));
                if let Ok(mut guard) = app.state::<BackendState>().inner.lock() {
                    guard.device_path = None;
                    guard.poller_stop = None;
                }
                break;
            }
        }

        if stop_rx
            .recv_timeout(Duration::from_millis(POLL_INTERVAL_MS))
            .is_ok()
        {
            break;
        }
    })
}

#[tauri::command]
fn connect_device(app: AppHandle, state: State<BackendState>) -> Result<(), String> {
    #[cfg(not(target_os = "windows"))]
    {
        let _ = app;
        let _ = state;
        return Err("this configurator only supports Windows".to_string());
    }

    #[cfg(target_os = "windows")]
    {
        emit_connection(&app, ConnectionState::Connecting, None);
        stop_existing_poller(&state);

        let result = (|| -> Result<(), String> {
            let path = find_device_path()?;
            {
                let _io_guard = state
                    .io_lock
                    .lock()
                    .map_err(|_| "io lock poisoned".to_string())?;
                let _ = read_device_config(&path)?;
            }

            let (stop_tx, stop_rx) = mpsc::channel();
            let poller = spawn_poller(app.clone(), path.clone(), stop_rx);

            {
                let mut guard = state
                    .inner
                    .lock()
                    .map_err(|_| "state lock poisoned".to_string())?;
                guard.device_path = Some(path);
                guard.poller_stop = Some(stop_tx);
                guard.poller_handle = Some(poller);
            }

            Ok(())
        })();

        match result {
            Ok(()) => {
                emit_connection(&app, ConnectionState::Connected, None);
                Ok(())
            }
            Err(err) => {
                emit_connection(&app, ConnectionState::Error, Some(err.clone()));
                Err(err)
            }
        }
    }
}

#[tauri::command]
fn disconnect_device(app: AppHandle, state: State<BackendState>) -> Result<(), String> {
    stop_existing_poller(&state);
    {
        let mut guard = state
            .inner
            .lock()
            .map_err(|_| "state lock poisoned".to_string())?;
        guard.device_path = None;
        stop_ai_screen_pull_worker(&mut guard);
    }
    emit_connection(&app, ConnectionState::Disconnected, None);
    Ok(())
}

#[tauri::command]
fn read_config(state: State<BackendState>) -> Result<DeviceConfig, String> {
    let path = get_current_path(&state)?;
    let _io_guard = state
        .io_lock
        .lock()
        .map_err(|_| "io lock poisoned".to_string())?;
    read_device_config(&path)
}

#[tauri::command]
fn set_trigger_target(state: State<BackendState>, target: String) -> Result<DeviceConfig, String> {
    let path = get_current_path(&state)?;
    let target = TriggerTarget::from_text(&target)?;
    let _io_guard = state
        .io_lock
        .lock()
        .map_err(|_| "io lock poisoned".to_string())?;
    set_target(&path, target)
}

#[tauri::command]
fn set_config_command(
    state: State<BackendState>,
    config: DeviceConfig,
) -> Result<DeviceConfig, String> {
    let path = get_current_path(&state)?;
    let _io_guard = state
        .io_lock
        .lock()
        .map_err(|_| "io lock poisoned".to_string())?;
    send_set_config(&path, &config)
}

#[tauri::command]
fn save_config(state: State<BackendState>) -> Result<DeviceConfig, String> {
    let path = get_current_path(&state)?;
    let _io_guard = state
        .io_lock
        .lock()
        .map_err(|_| "io lock poisoned".to_string())?;
    save_device_config(&path)
}

#[tauri::command]
fn send_ai_pull_test(state: State<BackendState>, value: u8) -> Result<(), String> {
    let path = get_ai_pull_path(&state)?;
    let _io_guard = state
        .io_lock
        .lock()
        .map_err(|_| "io lock poisoned".to_string())?;
    send_ai_pull(&path, value)
}

#[tauri::command]
fn start_ai_screen_pull(app: AppHandle, state: State<BackendState>) -> Result<(), String> {
    #[cfg(not(target_os = "windows"))]
    {
        let _ = app;
        let _ = state;
        return Err("AI screen pull capture only supports Windows".to_string());
    }

    #[cfg(target_os = "windows")]
    {
        let (stop_tx, stop_rx) = mpsc::channel();
        let handle = spawn_ai_screen_pull_worker(app, stop_rx);
        let mut guard = state
            .inner
            .lock()
            .map_err(|_| "state lock poisoned".to_string())?;
        install_ai_screen_pull_worker(&mut guard, stop_tx, handle)
    }
}

#[tauri::command]
fn stop_ai_screen_pull(state: State<BackendState>) -> Result<(), String> {
    let mut guard = state
        .inner
        .lock()
        .map_err(|_| "state lock poisoned".to_string())?;
    stop_ai_screen_pull_worker(&mut guard);
    Ok(())
}

pub fn run() {
    tauri::Builder::default()
        .manage(BackendState::default())
        .invoke_handler(tauri::generate_handler![
            connect_device,
            disconnect_device,
            read_config,
            set_trigger_target,
            set_config_command,
            save_config,
            send_ai_pull_test,
            start_ai_screen_pull,
            stop_ai_screen_pull
        ])
        .run(tauri::generate_context!())
        .expect("error while running receiver configurator");
}
