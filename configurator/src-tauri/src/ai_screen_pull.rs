//! Rust 移植自 `new3.py`：红色环检测、圆拟合、径向/角度占比估计。
//! This module keeps the OpenCV-based algorithm from `extention-detect` and exposes
//! one single-shot API for the Tauri background worker.

use std::time::{Duration, Instant};

use opencv::core::{self, Mat, Point, Rect, Scalar, Size, Vec3f, Vec4b, Vector};
use opencv::imgcodecs;
use opencv::imgproc;
use opencv::prelude::*;
#[cfg(target_os = "windows")]
use windows_sys::Win32::Foundation::{HWND, RECT};
#[cfg(target_os = "windows")]
use windows_sys::Win32::Graphics::Gdi::{
    BI_RGB, BITMAPINFO, BITMAPINFOHEADER, CreateCompatibleBitmap, CreateCompatibleDC,
    DIB_RGB_COLORS, DeleteDC, DeleteObject, GetDIBits, GetWindowDC, HBITMAP, HDC, RGBQUAD,
    ReleaseDC, SelectObject,
};
#[cfg(target_os = "windows")]
use windows_sys::Win32::Storage::Xps::PrintWindow;
#[cfg(target_os = "windows")]
use windows_sys::Win32::UI::WindowsAndMessaging::{
    GetForegroundWindow, GetWindowRect, SetProcessDPIAware,
};

type Result<T> = std::result::Result<T, Box<dyn std::error::Error + Send + Sync>>;

const ROI_X_START_RATIO: f64 = 0.8;
const ROI_Y_START_RATIO: f64 = 0.66;
const ANGLE_BINS: i32 = 360;
const CACHE_RESET_AFTER_ZERO: Duration = Duration::from_secs(5);

fn bgra_bytes_to_bgr_mat(bgra: &[u8], width: i32, height: i32) -> Result<Mat> {
    if width <= 0 || height <= 0 {
        return Err("image width or height is invalid".into());
    }

    let expected_len = width as usize * height as usize * 4;
    if bgra.len() != expected_len {
        return Err(format!(
            "BGRA buffer length mismatch: expected {}, got {}",
            expected_len,
            bgra.len()
        )
        .into());
    }

    let bgra_pixels: Vec<Vec4b> = bgra
        .chunks_exact(4)
        .map(|px| Vec4b::from([px[0], px[1], px[2], px[3]]))
        .collect();
    let bgra_mat = Mat::new_rows_cols_with_data(height, width, &bgra_pixels)?;
    let mut bgr = Mat::default();
    imgproc::cvt_color(&bgra_mat, &mut bgr, imgproc::COLOR_BGRA2BGR, 0)?;

    Ok(bgr)
}

fn bgra_buffer_roi_to_bgr_mat(
    bgra: &[u8],
    full_width: i32,
    full_height: i32,
    roi: Rect,
) -> Result<Mat> {
    if full_width <= 0 || full_height <= 0 {
        return Err("full image width or height is invalid".into());
    }
    let expected = full_width as usize * full_height as usize * 4;
    if bgra.len() != expected {
        return Err(format!(
            "BGRA buffer length mismatch: expected {}, got {}",
            expected,
            bgra.len()
        )
        .into());
    }
    if roi.width <= 0 || roi.height <= 0 {
        return Err("ROI width or height is invalid".into());
    }
    let x0 = roi.x as i64;
    let y0 = roi.y as i64;
    let rw = roi.width as i64;
    let rh = roi.height as i64;
    let fw = full_width as i64;
    let fh = full_height as i64;
    if x0 < 0 || y0 < 0 || x0 + rw > fw || y0 + rh > fh {
        return Err("ROI is out of bounds for the captured buffer".into());
    }

    let fw = full_width as usize;
    let rw = roi.width as usize;
    let rh = roi.height as usize;
    let x0 = roi.x as usize;
    let y0 = roi.y as usize;

    let mut roi_bgra = vec![0u8; rw * rh * 4];
    for r in 0..rh {
        let src = ((y0 + r) * fw + x0) * 4;
        let dst = r * rw * 4;
        roi_bgra[dst..dst + rw * 4].copy_from_slice(&bgra[src..src + rw * 4]);
    }

    bgra_bytes_to_bgr_mat(&roi_bgra, roi.width, roi.height)
}

fn capture_roi_rect(
    width: i32,
    height: i32,
    x_start_ratio: f64,
    y_start_ratio: f64,
) -> Result<Rect> {
    if width <= 0 || height <= 0 {
        return Err("window width or height is invalid".into());
    }
    if !x_start_ratio.is_finite() || !y_start_ratio.is_finite() {
        return Err("ROI ratio must be finite".into());
    }
    if !(0.0..1.0).contains(&x_start_ratio) || !(0.0..1.0).contains(&y_start_ratio) {
        return Err("ROI ratio must be in [0.0, 1.0)".into());
    }

    let x = (width as f64 * x_start_ratio) as i32;
    let y = (height as f64 * y_start_ratio) as i32;
    let roi_width = width - x;
    let roi_height = height - y;
    if roi_width <= 0 || roi_height <= 0 {
        return Err("ROI width or height is invalid".into());
    }

    Ok(Rect::new(x, y, roi_width, roi_height))
}

struct CapturedWindowRoi {
    img: Mat,
}

#[cfg(target_os = "windows")]
struct WindowDc {
    hwnd: HWND,
    hdc: HDC,
}

#[cfg(target_os = "windows")]
impl Drop for WindowDc {
    fn drop(&mut self) {
        unsafe {
            ReleaseDC(self.hwnd, self.hdc);
        }
    }
}

#[cfg(target_os = "windows")]
struct CompatibleDc(HDC);

#[cfg(target_os = "windows")]
impl Drop for CompatibleDc {
    fn drop(&mut self) {
        unsafe {
            DeleteDC(self.0);
        }
    }
}

#[cfg(target_os = "windows")]
struct Bitmap(HBITMAP);

#[cfg(target_os = "windows")]
impl Drop for Bitmap {
    fn drop(&mut self) {
        unsafe {
            DeleteObject(self.0);
        }
    }
}

#[cfg(target_os = "windows")]
#[allow(dead_code)]
fn grab_foreground_window_bgr() -> Result<Mat> {
    unsafe {
        SetProcessDPIAware();
        let hwnd = GetForegroundWindow();
        if hwnd.is_null() {
            return Err("no foreground window (GetForegroundWindow returned 0)".into());
        }
        capture_hwnd_bgr(hwnd)
    }
}

#[cfg(not(target_os = "windows"))]
#[allow(dead_code)]
fn grab_foreground_window_bgr() -> Result<Mat> {
    Err("foreground window capture is only supported on Windows".into())
}

#[cfg(target_os = "windows")]
fn grab_foreground_window_roi_bgr(
    x_start_ratio: f64,
    y_start_ratio: f64,
) -> Result<CapturedWindowRoi> {
    unsafe {
        SetProcessDPIAware();
        let hwnd = GetForegroundWindow();
        if hwnd.is_null() {
            return Err("no foreground window (GetForegroundWindow returned 0)".into());
        }
        capture_hwnd_roi_bgr(hwnd, x_start_ratio, y_start_ratio)
    }
}

#[cfg(not(target_os = "windows"))]
fn grab_foreground_window_roi_bgr(
    _x_start_ratio: f64,
    _y_start_ratio: f64,
) -> Result<CapturedWindowRoi> {
    Err("foreground window capture is only supported on Windows".into())
}

#[cfg(target_os = "windows")]
unsafe fn capture_hwnd_roi_bgr(
    hwnd: HWND,
    x_start_ratio: f64,
    y_start_ratio: f64,
) -> Result<CapturedWindowRoi> {
    let (bgra, width, height) = capture_hwnd_bgra(hwnd)?;
    let roi = capture_roi_rect(width, height, x_start_ratio, y_start_ratio)?;
    let img = bgra_buffer_roi_to_bgr_mat(&bgra, width, height, roi)?;

    Ok(CapturedWindowRoi { img })
}

#[cfg(target_os = "windows")]
unsafe fn capture_hwnd_bgra(hwnd: HWND) -> Result<(Vec<u8>, i32, i32)> {
    let mut rect = RECT {
        left: 0,
        top: 0,
        right: 0,
        bottom: 0,
    };
    if GetWindowRect(hwnd, &mut rect) == 0 {
        return Err("GetWindowRect failed".into());
    }

    let width = rect.right - rect.left;
    let height = rect.bottom - rect.top;
    if width <= 0 || height <= 0 {
        return Err("window width or height is invalid".into());
    }

    let hwnd_hdc = GetWindowDC(hwnd);
    if hwnd_hdc.is_null() {
        return Err("GetWindowDC failed".into());
    }
    let window_dc = WindowDc {
        hwnd,
        hdc: hwnd_hdc,
    };

    let save_hdc = CreateCompatibleDC(window_dc.hdc);
    if save_hdc.is_null() {
        return Err("CreateCompatibleDC failed".into());
    }
    let save_dc = CompatibleDc(save_hdc);

    let bitmap_handle = CreateCompatibleBitmap(window_dc.hdc, width, height);
    if bitmap_handle.is_null() {
        return Err("CreateCompatibleBitmap failed".into());
    }
    let bitmap = Bitmap(bitmap_handle);

    let old_object = SelectObject(save_dc.0, bitmap.0);
    if old_object.is_null() {
        return Err("SelectObject failed".into());
    }

    const PW_RENDERFULLCONTENT: u32 = 2;
    if PrintWindow(hwnd, save_dc.0, PW_RENDERFULLCONTENT) == 0 {
        SelectObject(save_dc.0, old_object);
        return Err("PrintWindow failed".into());
    }

    let mut info = BITMAPINFO {
        bmiHeader: BITMAPINFOHEADER {
            biSize: std::mem::size_of::<BITMAPINFOHEADER>() as u32,
            biWidth: width,
            biHeight: -height,
            biPlanes: 1,
            biBitCount: 32,
            biCompression: BI_RGB,
            biSizeImage: 0,
            biXPelsPerMeter: 0,
            biYPelsPerMeter: 0,
            biClrUsed: 0,
            biClrImportant: 0,
        },
        bmiColors: [RGBQUAD {
            rgbBlue: 0,
            rgbGreen: 0,
            rgbRed: 0,
            rgbReserved: 0,
        }],
    };
    let mut bgra = vec![0u8; width as usize * height as usize * 4];
    let lines = GetDIBits(
        save_dc.0,
        bitmap.0,
        0,
        height as u32,
        bgra.as_mut_ptr().cast(),
        &mut info,
        DIB_RGB_COLORS,
    );

    SelectObject(save_dc.0, old_object);

    if lines == 0 {
        return Err("GetDIBits failed".into());
    }

    Ok((bgra, width, height))
}

#[cfg(target_os = "windows")]
#[allow(dead_code)]
unsafe fn capture_hwnd_bgr(hwnd: HWND) -> Result<Mat> {
    let (bgra, width, height) = capture_hwnd_bgra(hwnd)?;
    bgra_bytes_to_bgr_mat(&bgra, width, height)
}

fn mask_bgr_around_cube(
    bgr: &Mat,
    center_b: u8,
    center_g: u8,
    center_r: u8,
    delta: u8,
) -> Result<Mat> {
    let b_lo = center_b.saturating_sub(delta) as f64;
    let g_lo = center_g.saturating_sub(delta) as f64;
    let r_lo = center_r.saturating_sub(delta) as f64;
    let b_hi = (center_b as u16 + delta as u16).min(255) as f64;
    let g_hi = (center_g as u16 + delta as u16).min(255) as f64;
    let r_hi = (center_r as u16 + delta as u16).min(255) as f64;
    let lo = Scalar::new(b_lo, g_lo, r_lo, 0.0);
    let hi = Scalar::new(b_hi, g_hi, r_hi, 0.0);
    let mut mask = Mat::default();
    core::in_range(bgr, &lo, &hi, &mut mask)?;
    Ok(mask)
}

fn bitwise_or_masks(a: &Mat, b: &Mat) -> Result<Mat> {
    let mut out = Mat::default();
    core::bitwise_or(a, b, &mut out, &core::no_array())?;
    Ok(out)
}

fn extract_red_mask_bgr(img_bgr: &Mat) -> Result<Mat> {
    const RED_DELTA: u8 = 15;
    let m1 = mask_bgr_around_cube(img_bgr, 20, 18, 179, RED_DELTA)?;
    let m2 = mask_bgr_around_cube(img_bgr, 21, 21, 221, RED_DELTA)?;
    let m3 = mask_bgr_around_cube(img_bgr, 120, 120, 255, RED_DELTA)?;
    let red = bitwise_or_masks(&m1, &m2)?;
    let red = bitwise_or_masks(&red, &m3)?;

    let kernel = imgproc::get_structuring_element(
        imgproc::MORPH_ELLIPSE,
        Size::new(3, 3),
        Point::new(-1, -1),
    )?;
    let border_value = imgproc::morphology_default_border_value()?;
    let mut opened = Mat::default();
    imgproc::morphology_ex(
        &red,
        &mut opened,
        imgproc::MORPH_OPEN,
        &kernel,
        Point::new(-1, -1),
        1,
        core::BORDER_CONSTANT,
        border_value,
    )?;
    let mut closed = Mat::default();
    imgproc::morphology_ex(
        &opened,
        &mut closed,
        imgproc::MORPH_CLOSE,
        &kernel,
        Point::new(-1, -1),
        1,
        core::BORDER_CONSTANT,
        border_value,
    )?;

    Ok(closed)
}

#[derive(Clone, Copy, Debug)]
struct CircleCandidate {
    cx: f64,
    cy: f64,
    r: f64,
}

fn red_mask_points(red_mask: &Mat) -> Result<Vec<(i32, i32)>> {
    let (w, h) = (red_mask.cols(), red_mask.rows());
    let data = red_mask.data_typed::<u8>()?;
    let mut points = Vec::with_capacity(data.iter().filter(|&&v| v > 0).count());

    for y in 0..h {
        let row_start = (y * w) as usize;
        for x in 0..w {
            if data[row_start + x as usize] > 0 {
                points.push((x, y));
            }
        }
    }

    Ok(points)
}

fn red_band_score_from_points(
    red_points: &[(i32, i32)],
    circle: CircleCandidate,
    half_width: f64,
) -> u32 {
    let min_r = (circle.r - half_width).max(0.0);
    let max_r = circle.r + half_width;
    let min_dist_sq = min_r * min_r;
    let max_dist_sq = max_r * max_r;

    red_points
        .iter()
        .filter(|&&(x, y)| {
            let dx = x as f64 - circle.cx;
            let dy = y as f64 - circle.cy;
            let dist_sq = dx * dx + dy * dy;
            dist_sq >= min_dist_sq && dist_sq <= max_dist_sq
        })
        .count() as u32
}

fn min_red_ring_score(circle: CircleCandidate) -> u32 {
    ((circle.r * 0.2).round() as u32).max(8)
}

fn select_best_circle_by_red_band(
    candidates: &[CircleCandidate],
    red_mask: &Mat,
) -> Result<CircleCandidate> {
    let red_points = red_mask_points(red_mask)?;
    let mut best: Option<(CircleCandidate, u32)> = None;
    for &candidate in candidates {
        let score = red_band_score_from_points(&red_points, candidate, 10.0);
        if best.map_or(true, |(_, best_score)| score > best_score) {
            best = Some((candidate, score));
        }
    }

    let (circle, score) = best.ok_or("No circle found")?;
    if score < min_red_ring_score(circle) {
        return Err("No red ring found around circle candidates".into());
    }
    Ok(circle)
}

fn detect_circle_candidates_with_opencv(img_bgr: &Mat) -> Result<Vec<CircleCandidate>> {
    let (w, h) = (img_bgr.cols(), img_bgr.rows());
    let min_dim = w.min(h);
    if min_dim < 16 {
        return Ok(Vec::new());
    }

    let mut gray_mat = Mat::default();
    imgproc::cvt_color(img_bgr, &mut gray_mat, imgproc::COLOR_BGR2GRAY, 0)?;
    let mut blurred = Mat::default();
    imgproc::gaussian_blur(
        &gray_mat,
        &mut blurred,
        Size::new(9, 9),
        1.5,
        1.5,
        core::BORDER_DEFAULT,
    )?;

    let mut circles = Mat::default();
    imgproc::hough_circles(
        &blurred,
        &mut circles,
        imgproc::HOUGH_GRADIENT,
        1.2,
        80.0,
        100.0,
        30.0,
        (min_dim / 8).max(4),
        min_dim / 2,
    )?;

    if circles.empty() {
        return Ok(Vec::new());
    }

    Ok(circles
        .data_typed::<Vec3f>()?
        .iter()
        .map(|c| CircleCandidate {
            cx: c[0] as f64,
            cy: c[1] as f64,
            r: c[2] as f64,
        })
        .collect())
}

fn find_best_circle_center(img_bgr: &Mat, red_mask: &Mat) -> Result<CircleCandidate> {
    let candidates = detect_circle_candidates_with_opencv(img_bgr)?;
    select_best_circle_by_red_band(&candidates, red_mask)
}

fn refine_ring_radii_from_red(cx: f64, cy: f64, red_mask: &Mat) -> Result<(i32, i32, Vec<f64>)> {
    let (w, h) = (red_mask.cols(), red_mask.rows());
    let red_data = red_mask.data_typed::<u8>()?;
    let max_possible_r = (f64::from(w).hypot(f64::from(h)).ceil() as usize).max(1);
    let mut total_count = vec![0i32; max_possible_r + 1];
    let mut red_count = vec![0i32; max_possible_r + 1];

    for y in 0..h {
        let row_start = (y * w) as usize;
        for x in 0..w {
            let dx = x as f64 - cx;
            let dy = y as f64 - cy;
            let dist = (dx * dx + dy * dy).sqrt();
            let ri = dist.round() as i32;
            let idx = ri as usize;
            total_count[idx] += 1;
            if red_data[row_start + x as usize] > 0 {
                red_count[idx] += 1;
            }
        }
    }

    let max_r = total_count.len().saturating_sub(1);
    let mut radial_ratio = vec![0f64; max_r + 1];
    for i in 0..=max_r {
        let t = total_count.get(i).copied().unwrap_or(0).max(1);
        let r = red_count.get(i).copied().unwrap_or(0);
        radial_ratio[i] = r as f64 / t as f64;
    }

    let k = 9usize;
    let half = k / 2;
    let mut radial_ratio_smooth = vec![0f64; radial_ratio.len()];
    for i in 0..radial_ratio.len() {
        let mut s = 0.0;
        for t in 0..k {
            let j = i as isize + t as isize - half as isize;
            if j >= 0 && j < radial_ratio.len() as isize {
                s += radial_ratio[j as usize];
            }
        }
        radial_ratio_smooth[i] = s / k as f64;
    }

    let mask: Vec<bool> = radial_ratio_smooth.iter().map(|&v| v > 0.12).collect();
    let mut best_len = 0usize;
    let mut best_seg: Option<(usize, usize)> = None;
    let mut start: Option<usize> = None;

    for (i, &v) in mask.iter().enumerate() {
        if v && start.is_none() {
            start = Some(i);
        } else if !v {
            if let Some(st) = start {
                let seg_len = i - st;
                if seg_len > best_len {
                    best_len = seg_len;
                    best_seg = Some((st, i - 1));
                }
                start = None;
            }
        }
    }
    if let Some(st) = start {
        let seg_len = mask.len() - st;
        if seg_len > best_len {
            best_seg = Some((st, mask.len() - 1));
        }
    }

    let (r_in, r_out) = best_seg.ok_or("Cannot refine ring radii from red mask")?;
    Ok((r_in as i32, r_out as i32, radial_ratio_smooth))
}

#[allow(dead_code)]
pub struct RingEstimate {
    pub ratio: f64,
    pub cx: f64,
    pub cy: f64,
    pub r_mid: f64,
    pub r_in: i32,
    pub r_out: i32,
    pub red_main: Mat,
    pub radial_ratio_smooth: Vec<f64>,
}

#[derive(Clone, Copy, Debug, PartialEq)]
struct RingGeometry {
    cx: f64,
    cy: f64,
    r_mid: f64,
    r_in: i32,
    r_out: i32,
}

impl From<&RingEstimate> for RingGeometry {
    fn from(est: &RingEstimate) -> Self {
        Self {
            cx: est.cx,
            cy: est.cy,
            r_mid: est.r_mid,
            r_in: est.r_in,
            r_out: est.r_out,
        }
    }
}

fn pull_percent_from_ratio(ratio: f64) -> u8 {
    if !ratio.is_finite() {
        return 0;
    }

    (ratio * 100.0).round().clamp(0.0, 100.0) as u8
}

fn estimate_red_ring_ratio_with_geometry(
    red_main: Mat,
    geometry: RingGeometry,
    angle_bins: i32,
) -> Result<RingEstimate> {
    if angle_bins <= 0 {
        return Err("angle_bins must be positive".into());
    }
    if geometry.r_in < 0 || geometry.r_out <= geometry.r_in {
        return Err("cached ring radii are invalid".into());
    }

    let (cx, cy) = (geometry.cx, geometry.cy);
    let (w, h) = (red_main.cols(), red_main.rows());
    let red_data = red_main.data_typed::<u8>()?;
    let bins = angle_bins as usize;
    let mut total_cnt = vec![0i32; bins];
    let mut red_cnt = vec![0i32; bins];
    let r_in_sq = (geometry.r_in as f64) * (geometry.r_in as f64);
    let r_out_sq = (geometry.r_out as f64) * (geometry.r_out as f64);

    for y in 0..h {
        let row_start = (y * w) as usize;
        for x in 0..w {
            let dx = x as f64 - cx;
            let dy = y as f64 - cy;
            let dist_sq = dx * dx + dy * dy;
            if dist_sq < r_in_sq || dist_sq > r_out_sq {
                continue;
            }
            let mut ang = dy.atan2(dx);
            if ang < 0.0 {
                ang += std::f64::consts::TAU;
            }
            let mut idx = ((ang / std::f64::consts::TAU) * angle_bins as f64).floor() as i32;
            idx = idx.clamp(0, angle_bins - 1);
            let ui = idx as usize;
            total_cnt[ui] += 1;
            if red_data[row_start + x as usize] > 0 {
                red_cnt[ui] += 1;
            }
        }
    }

    let mut present_count = 0usize;
    for i in 0..bins {
        let thr = 3.max((total_cnt[i] as f64 * 0.2).floor() as i32);
        if red_cnt[i] > thr {
            present_count += 1;
        }
    }
    let ratio = present_count as f64 / bins as f64;

    Ok(RingEstimate {
        ratio,
        cx: geometry.cx,
        cy: geometry.cy,
        r_mid: geometry.r_mid,
        r_in: geometry.r_in,
        r_out: geometry.r_out,
        red_main,
        radial_ratio_smooth: Vec::new(),
    })
}

fn estimate_red_ring_ratio_from_red_mask(
    img_bgr: &Mat,
    red_main: Mat,
    angle_bins: i32,
) -> Result<RingEstimate> {
    let circle = find_best_circle_center(img_bgr, &red_main)?;
    let (cx, cy) = (circle.cx, circle.cy);
    let (r_in, r_out, radial_ratio_smooth) = refine_ring_radii_from_red(cx, cy, &red_main)?;
    let geometry = RingGeometry {
        cx,
        cy,
        r_mid: 0.5 * (r_in as f64 + r_out as f64),
        r_in,
        r_out,
    };
    let mut est = estimate_red_ring_ratio_with_geometry(red_main, geometry, angle_bins)?;
    est.radial_ratio_smooth = radial_ratio_smooth;
    Ok(est)
}

#[allow(dead_code)]
pub fn estimate_red_ring_ratio(img_bgr: &Mat, angle_bins: i32) -> Result<RingEstimate> {
    let red_main = extract_red_mask_bgr(img_bgr)?;
    estimate_red_ring_ratio_from_red_mask(img_bgr, red_main, angle_bins)
}

pub struct ForegroundPullEstimator {
    cached_geometry: Option<RingGeometry>,
    zero_started_at: Option<Instant>,
}

impl ForegroundPullEstimator {
    pub fn new() -> Self {
        Self {
            cached_geometry: None,
            zero_started_at: None,
        }
    }

    pub fn estimate_foreground_pull_ratio(&mut self, now: Instant) -> Result<f64> {
        let estimate_result = self.estimate_foreground_ring();
        self.update_cache_after_result(estimate_result.as_ref().ok(), now);
        estimate_result.map(|est| est.ratio)
    }

    fn estimate_foreground_ring(&self) -> Result<RingEstimate> {
        let captured = grab_foreground_window_roi_bgr(ROI_X_START_RATIO, ROI_Y_START_RATIO)?;
        let red_main = extract_red_mask_bgr(&captured.img)?;
        match self.cached_geometry {
            Some(geometry) => estimate_red_ring_ratio_with_geometry(red_main, geometry, ANGLE_BINS),
            None => estimate_red_ring_ratio_from_red_mask(&captured.img, red_main, ANGLE_BINS),
        }
    }

    fn update_cache_after_result(&mut self, estimate: Option<&RingEstimate>, now: Instant) {
        if let Some(estimate) = estimate {
            if pull_percent_from_ratio(estimate.ratio) > 0 {
                self.cached_geometry = Some(RingGeometry::from(estimate));
                self.zero_started_at = None;
                return;
            }
        }

        if self.cached_geometry.is_none() {
            self.zero_started_at = None;
            return;
        }

        match self.zero_started_at {
            Some(started_at) if now.duration_since(started_at) >= CACHE_RESET_AFTER_ZERO => {
                self.cached_geometry = None;
                self.zero_started_at = None;
            }
            Some(_) => {}
            None => {
                self.zero_started_at = Some(now);
            }
        }
    }
}

impl Default for ForegroundPullEstimator {
    fn default() -> Self {
        Self::new()
    }
}

#[allow(dead_code)]
fn debug_circle_flag_enabled(value: Option<&str>) -> bool {
    value
        .map(|v| {
            let normalized = v.trim();
            !normalized.is_empty() && normalized != "0" && !normalized.eq_ignore_ascii_case("false")
        })
        .unwrap_or(false)
}

#[allow(dead_code)]
fn draw_debug_circle_overlay(img_bgr: &Mat, est: &RingEstimate) -> Result<Mat> {
    let mut out = img_bgr.try_clone()?;
    let center = Point::new(est.cx.round() as i32, est.cy.round() as i32);
    imgproc::circle(
        &mut out,
        center,
        est.r_mid.round() as i32,
        Scalar::new(255.0, 0.0, 255.0, 0.0),
        1,
        imgproc::LINE_8,
        0,
    )?;
    imgproc::circle(
        &mut out,
        center,
        est.r_in,
        Scalar::new(255.0, 255.0, 0.0, 0.0),
        1,
        imgproc::LINE_8,
        0,
    )?;
    imgproc::circle(
        &mut out,
        center,
        est.r_out,
        Scalar::new(0.0, 255.0, 0.0, 0.0),
        1,
        imgproc::LINE_8,
        0,
    )?;
    imgproc::draw_marker(
        &mut out,
        center,
        Scalar::new(255.0, 0.0, 0.0, 0.0),
        imgproc::MARKER_CROSS,
        7,
        1,
        imgproc::LINE_8,
    )?;
    Ok(out)
}

#[allow(dead_code)]
fn save_debug_circle_overlay(img_bgr: &Mat, est: &RingEstimate) -> Result<std::path::PathBuf> {
    let path = std::env::current_dir()?.join("debug_circle.png");
    let overlay = draw_debug_circle_overlay(img_bgr, est)?;
    let params = Vector::<i32>::new();
    imgcodecs::imwrite(path.to_string_lossy().as_ref(), &overlay, &params)?;
    Ok(path)
}

#[allow(dead_code)]
pub fn estimate_foreground_pull_ratio() -> Result<f64> {
    let captured = grab_foreground_window_roi_bgr(ROI_X_START_RATIO, ROI_Y_START_RATIO)?;
    let est = estimate_red_ring_ratio(&captured.img, ANGLE_BINS)?;
    Ok(est.ratio)
}

#[cfg(test)]
mod tests {
    use super::*;

    use opencv::core::Vec3b;

    fn synthetic_red_ring_bgr_and_mask(
        width: i32,
        height: i32,
        cx: f64,
        cy: f64,
        r: f64,
    ) -> (Mat, Mat) {
        let mut img = vec![Vec3b::from([0, 0, 0]); (width * height) as usize];
        let mut red = vec![0u8; (width * height) as usize];
        for y in 0..height {
            for x in 0..width {
                let dx = x as f64 - cx;
                let dy = y as f64 - cy;
                let dist = (dx * dx + dy * dy).sqrt();
                if (dist - r).abs() <= 5.0 {
                    let idx = (y * width + x) as usize;
                    img[idx] = Vec3b::from([21, 21, 221]);
                    red[idx] = 255;
                }
            }
        }

        (
            Mat::new_rows_cols_with_data(height, width, &img)
                .unwrap()
                .try_clone()
                .unwrap(),
            Mat::new_rows_cols_with_data(height, width, &red)
                .unwrap()
                .try_clone()
                .unwrap(),
        )
    }

    #[test]
    fn bgra_bytes_to_bgr_mat_converts_capture_buffer_to_opencv_order() {
        let bgra = [10, 20, 30, 255, 40, 50, 60, 0];

        let mat = bgra_bytes_to_bgr_mat(&bgra, 2, 1).expect("valid BGRA buffer");

        assert_eq!(mat.rows(), 1);
        assert_eq!(mat.cols(), 2);
        assert_eq!(mat.channels(), 3);
        assert_eq!(
            *mat.at_2d::<opencv::core::Vec3b>(0, 0).unwrap(),
            [10, 20, 30].into()
        );
        assert_eq!(
            *mat.at_2d::<opencv::core::Vec3b>(0, 1).unwrap(),
            [40, 50, 60].into()
        );
    }

    #[test]
    fn bgra_buffer_roi_to_bgr_mat_crops_before_color_convert() {
        let bgra: Vec<u8> = vec![1, 2, 3, 255, 4, 5, 6, 255, 7, 8, 9, 255, 10, 11, 12, 255];
        let roi = Rect::new(1, 1, 1, 1);
        let mat = bgra_buffer_roi_to_bgr_mat(&bgra, 2, 2, roi).expect("valid ROI BGRA");

        assert_eq!(mat.rows(), 1);
        assert_eq!(mat.cols(), 1);
        assert_eq!(
            *mat.at_2d::<opencv::core::Vec3b>(0, 0).unwrap(),
            [10, 11, 12].into()
        );
    }

    #[test]
    fn capture_roi_rect_uses_lower_right_window_region() {
        let rect = capture_roi_rect(1497, 1251, 0.8, 0.66).expect("valid ROI");

        assert_eq!(rect.x, 1197);
        assert_eq!(rect.y, 825);
        assert_eq!(rect.width, 300);
        assert_eq!(rect.height, 426);
    }

    #[test]
    fn detect_circle_candidates_returns_empty_for_blank_image() {
        let pixels = vec![Vec3b::from([0, 0, 0]); 80 * 80];
        let img = Mat::new_rows_cols_with_data(80, 80, &pixels)
            .unwrap()
            .try_clone()
            .unwrap();

        let candidates = detect_circle_candidates_with_opencv(&img).unwrap();

        assert!(candidates.is_empty());
    }

    #[test]
    fn red_band_score_from_points_counts_only_points_near_radius() {
        let points = [(15, 10), (16, 10), (17, 10), (10, 10)];
        let circle = CircleCandidate {
            cx: 10.0,
            cy: 10.0,
            r: 5.0,
        };

        let score = red_band_score_from_points(&points, circle, 1.0);

        assert_eq!(score, 2);
    }

    #[test]
    fn select_best_circle_by_red_band_prefers_candidate_with_red_ring() {
        let (_, red) = synthetic_red_ring_bgr_and_mask(120, 120, 80.0, 70.0, 26.0);

        let candidates = [
            CircleCandidate {
                cx: 35.0,
                cy: 35.0,
                r: 24.0,
            },
            CircleCandidate {
                cx: 80.0,
                cy: 70.0,
                r: 26.0,
            },
        ];

        let best =
            select_best_circle_by_red_band(&candidates, &red).expect("red ring candidate exists");

        assert!((best.cx - 80.0).abs() < 0.001);
        assert!((best.cy - 70.0).abs() < 0.001);
        assert!((best.r - 26.0).abs() < 0.001);
    }

    #[test]
    fn select_best_circle_by_red_band_rejects_candidates_without_red_ring() {
        let mut red_pixels = vec![0u8; 120 * 120];
        for y in 95..110 {
            for x in 5..20 {
                red_pixels[y * 120 + x] = 255;
            }
        }
        let red = Mat::new_rows_cols_with_data(120, 120, &red_pixels)
            .unwrap()
            .try_clone()
            .unwrap();
        let candidates = [CircleCandidate {
            cx: 60.0,
            cy: 60.0,
            r: 28.0,
        }];

        let err = select_best_circle_by_red_band(&candidates, &red)
            .expect_err("unrelated red pixels must not count as a ring");

        assert!(err.to_string().contains("No red ring found"));
    }

    #[test]
    fn find_best_circle_center_detects_synthetic_red_ring() {
        let (img, red) = synthetic_red_ring_bgr_and_mask(160, 160, 78.0, 82.0, 42.0);

        let circle = find_best_circle_center(&img, &red).expect("synthetic circle should be found");

        assert!((circle.cx - 78.0).abs() <= 3.0, "cx={}", circle.cx);
        assert!((circle.cy - 82.0).abs() <= 3.0, "cy={}", circle.cy);
    }

    #[test]
    fn opencv_hough_candidates_detect_synthetic_red_ring() {
        let (img, red) = synthetic_red_ring_bgr_and_mask(160, 160, 78.0, 82.0, 42.0);

        let candidates =
            detect_circle_candidates_with_opencv(&img).expect("opencv hough should run");
        let circle =
            select_best_circle_by_red_band(&candidates, &red).expect("red ring candidate exists");

        assert!((circle.cx - 78.0).abs() <= 3.0, "cx={}", circle.cx);
        assert!((circle.cy - 82.0).abs() <= 3.0, "cy={}", circle.cy);
    }

    #[test]
    fn estimate_red_ring_ratio_accepts_bgr_mat() {
        let (img, _) = synthetic_red_ring_bgr_and_mask(160, 160, 78.0, 82.0, 42.0);

        let est = estimate_red_ring_ratio(&img, 360).expect("synthetic circle should be estimated");

        assert!((est.cx - 78.0).abs() <= 3.0, "cx={}", est.cx);
        assert!((est.cy - 82.0).abs() <= 3.0, "cy={}", est.cy);
        assert!(est.ratio > 0.95, "ratio={}", est.ratio);
    }

    #[test]
    fn cached_geometry_ratio_does_not_need_hough_candidates() {
        let (_, red) = synthetic_red_ring_bgr_and_mask(160, 160, 78.0, 82.0, 42.0);
        let geometry = RingGeometry {
            cx: 78.0,
            cy: 82.0,
            r_mid: 42.0,
            r_in: 37,
            r_out: 47,
        };

        let est = estimate_red_ring_ratio_with_geometry(red, geometry, 360)
            .expect("cached geometry should estimate ratio directly");

        assert!(est.ratio > 0.95, "ratio={}", est.ratio);
        assert_eq!(est.cx, 78.0);
        assert_eq!(est.cy, 82.0);
        assert_eq!(est.r_in, 37);
        assert_eq!(est.r_out, 47);
    }

    fn ring_estimate_for_cache_test(ratio: f64) -> RingEstimate {
        RingEstimate {
            ratio,
            cx: 78.0,
            cy: 82.0,
            r_mid: 42.0,
            r_in: 37,
            r_out: 47,
            red_main: Mat::default(),
            radial_ratio_smooth: Vec::new(),
        }
    }

    #[test]
    fn foreground_pull_estimator_caches_geometry_after_nonzero_pull_percent() {
        let start = Instant::now();
        let mut estimator = ForegroundPullEstimator::new();
        let estimate = ring_estimate_for_cache_test(0.01);

        estimator.update_cache_after_result(Some(&estimate), start);

        assert_eq!(
            estimator.cached_geometry,
            Some(RingGeometry::from(&estimate))
        );
    }

    #[test]
    fn foreground_pull_estimator_does_not_cache_sub_percent_pull() {
        let start = Instant::now();
        let mut estimator = ForegroundPullEstimator::new();
        let estimate = ring_estimate_for_cache_test(0.004);

        estimator.update_cache_after_result(Some(&estimate), start);

        assert!(estimator.cached_geometry.is_none());
    }

    #[test]
    fn foreground_pull_estimator_keeps_cache_until_zero_reaches_five_seconds() {
        let start = Instant::now();
        let mut estimator = ForegroundPullEstimator::new();
        let nonzero = ring_estimate_for_cache_test(0.25);
        let zero = ring_estimate_for_cache_test(0.0);
        estimator.update_cache_after_result(Some(&nonzero), start);

        estimator.update_cache_after_result(Some(&zero), start + Duration::from_secs(1));
        estimator.update_cache_after_result(Some(&zero), start + Duration::from_secs(5));

        assert!(estimator.cached_geometry.is_some());
        assert!(estimator.zero_started_at.is_some());
    }

    #[test]
    fn foreground_pull_estimator_clears_cache_after_five_seconds_of_zero() {
        let start = Instant::now();
        let mut estimator = ForegroundPullEstimator::new();
        let nonzero = ring_estimate_for_cache_test(0.25);
        let zero = ring_estimate_for_cache_test(0.0);
        estimator.update_cache_after_result(Some(&nonzero), start);

        estimator.update_cache_after_result(Some(&zero), start + Duration::from_secs(1));
        estimator.update_cache_after_result(Some(&zero), start + Duration::from_secs(6));

        assert!(estimator.cached_geometry.is_none());
        assert!(estimator.zero_started_at.is_none());
    }

    #[test]
    fn foreground_pull_estimator_clears_cache_after_five_seconds_of_errors() {
        let start = Instant::now();
        let mut estimator = ForegroundPullEstimator::new();
        let nonzero = ring_estimate_for_cache_test(0.25);
        estimator.update_cache_after_result(Some(&nonzero), start);

        estimator.update_cache_after_result(None, start + Duration::from_secs(1));
        estimator.update_cache_after_result(None, start + Duration::from_secs(6));

        assert!(estimator.cached_geometry.is_none());
        assert!(estimator.zero_started_at.is_none());
    }

    #[test]
    fn foreground_pull_estimator_resets_zero_timer_when_nonzero_returns() {
        let start = Instant::now();
        let mut estimator = ForegroundPullEstimator::new();
        let nonzero = ring_estimate_for_cache_test(0.25);
        let zero = ring_estimate_for_cache_test(0.0);
        estimator.update_cache_after_result(Some(&nonzero), start);

        estimator.update_cache_after_result(Some(&zero), start + Duration::from_secs(1));
        estimator.update_cache_after_result(Some(&nonzero), start + Duration::from_secs(3));

        assert!(estimator.cached_geometry.is_some());
        assert!(estimator.zero_started_at.is_none());
    }

    #[test]
    fn debug_circle_flag_treats_nonzero_nonfalse_values_as_enabled() {
        assert!(!debug_circle_flag_enabled(None));
        assert!(!debug_circle_flag_enabled(Some("")));
        assert!(!debug_circle_flag_enabled(Some("0")));
        assert!(!debug_circle_flag_enabled(Some("false")));
        assert!(debug_circle_flag_enabled(Some("1")));
        assert!(debug_circle_flag_enabled(Some("true")));
    }

    #[test]
    fn draw_debug_circle_overlay_marks_detected_circle_radii_and_center() {
        let pixels = vec![Vec3b::from([0, 0, 0]); 50 * 50];
        let img = Mat::new_rows_cols_with_data(50, 50, &pixels)
            .unwrap()
            .try_clone()
            .unwrap();
        let est = RingEstimate {
            ratio: 0.0,
            cx: 25.0,
            cy: 25.0,
            r_mid: 10.0,
            r_in: 8,
            r_out: 12,
            red_main: Mat::new_rows_cols_with_data(50, 50, &vec![0u8; 50 * 50])
                .unwrap()
                .try_clone()
                .unwrap(),
            radial_ratio_smooth: Vec::new(),
        };

        let overlay = draw_debug_circle_overlay(&img, &est).unwrap();

        assert_eq!(
            *overlay.at_2d::<Vec3b>(25, 35).unwrap(),
            [255, 0, 255].into()
        );
        assert_eq!(
            *overlay.at_2d::<Vec3b>(25, 33).unwrap(),
            [255, 255, 0].into()
        );
        assert_eq!(*overlay.at_2d::<Vec3b>(25, 37).unwrap(), [0, 255, 0].into());
        assert_eq!(*overlay.at_2d::<Vec3b>(25, 25).unwrap(), [255, 0, 0].into());
    }
}
