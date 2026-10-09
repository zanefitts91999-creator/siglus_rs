//! WMV YUV colour conversion helpers.
//!
//! WMV3 Simple/Main profile does not carry an explicit transfer-matrix selector
//! in the bitstream parsed by this crate. The original Siglus desktop path used
//! the Windows media stack, so for unspecified matrix metadata we follow the
//! Windows/DXVA convention: SD (source height <= 576) uses BT.601 and HD
//! (source height > 576) uses BT.709.

use crate::decoder::YuvFrame;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum VideoTransferMatrix {
    Bt601,
    Bt709,
}

impl VideoTransferMatrix {
    /// Windows/DXVA fallback for unspecified transfer-matrix metadata.
    ///
    /// DXVA defines HD for this purpose as a source height greater than 576
    /// lines; unknown SD content is treated as BT.601 and unknown HD content as
    /// BT.709.
    #[inline]
    pub const fn for_unspecified_source(height: u32) -> Self {
        if height > 576 {
            Self::Bt709
        } else {
            Self::Bt601
        }
    }
}

#[derive(Clone, Copy)]
struct LimitedRangeCoefficients {
    rv: i32,
    gu: i32,
    gv: i32,
    bu: i32,
}

impl VideoTransferMatrix {
    #[inline]
    const fn limited_range_coefficients(self) -> LimitedRangeCoefficients {
        match self {
            // 1.164(Y-16) + 1.596(V-128)
            // 1.164(Y-16) - 0.391(U-128) - 0.813(V-128)
            // 1.164(Y-16) + 2.016(U-128)
            Self::Bt601 => LimitedRangeCoefficients {
                rv: 409,
                gu: 100,
                gv: 208,
                bu: 516,
            },
            // 1.164(Y-16) + 1.793(V-128)
            // 1.164(Y-16) - 0.213(U-128) - 0.533(V-128)
            // 1.164(Y-16) + 2.112(U-128)
            Self::Bt709 => LimitedRangeCoefficients {
                rv: 459,
                gu: 55,
                gv: 136,
                bu: 541,
            },
        }
    }
}

/// Convert one studio-range Y'CbCr sample to 8-bit RGB using the selected
/// transfer matrix. The integer form intentionally preserves the decoder's
/// existing BT.601 rounding convention while adding the corresponding BT.709
/// coefficients.
#[inline]
pub fn yuv_limited_to_rgb(y: u8, cb: u8, cr: u8, matrix: VideoTransferMatrix) -> [u8; 3] {
    let coeff = matrix.limited_range_coefficients();
    let c = (y as i32 - 16).max(0);
    let d = cb as i32 - 128;
    let e = cr as i32 - 128;

    let r = ((298 * c + coeff.rv * e + 128) >> 8).clamp(0, 255) as u8;
    let g = ((298 * c - coeff.gu * d - coeff.gv * e + 128) >> 8).clamp(0, 255) as u8;
    let b = ((298 * c + coeff.bu * d + 128) >> 8).clamp(0, 255) as u8;
    [r, g, b]
}

/// Convert a WMV YUV420p frame to packed RGB24, selecting the same default
/// transfer matrix Windows uses when the source does not specify one.
pub fn yuv420p_to_rgb(frame: &YuvFrame) -> Vec<u8> {
    let width = frame.width as usize;
    let height = frame.height as usize;
    let chroma_width = width / 2;
    let matrix = VideoTransferMatrix::for_unspecified_source(frame.height);
    let mut rgb = vec![0u8; width.saturating_mul(height).saturating_mul(3)];

    for y in 0..height {
        for x in 0..width {
            let luma = frame.y.get(y * width + x).copied().unwrap_or(16);
            let chroma_index = (y / 2).saturating_mul(chroma_width).saturating_add(x / 2);
            let cb = frame.cb.get(chroma_index).copied().unwrap_or(128);
            let cr = frame.cr.get(chroma_index).copied().unwrap_or(128);
            let [r, g, b] = yuv_limited_to_rgb(luma, cb, cr, matrix);
            let out = (y * width + x) * 3;
            rgb[out] = r;
            rgb[out + 1] = g;
            rgb[out + 2] = b;
        }
    }

    rgb
}

/// Convert a WMV YUV420p frame to packed RGBA8, selecting the Windows/DXVA
/// default transfer matrix for unspecified metadata.
pub fn yuv420p_to_rgba(frame: &YuvFrame) -> Vec<u8> {
    yuv420p_to_rgba_scaled(frame, frame.width, frame.height)
}

/// Convert to a bounded output size directly from YUV, without a full-size
/// RGBA allocation. Sampling uses the nearest source pixel.
pub fn yuv420p_to_rgba_scaled(frame: &YuvFrame, output_width: u32, output_height: u32) -> Vec<u8> {
    let mut rgba = Vec::new();
    yuv420p_to_rgba_scaled_into(
        frame,
        frame.width,
        frame.height,
        output_width,
        output_height,
        &mut rgba,
    );
    rgba
}

/// Convert a visible region (`visible_width` x `visible_height`) of a WMV YUV420p
/// frame (whose stride is `frame.width`) into a reusable RGBA8 buffer of size
/// `output_width` x `output_height`.
pub fn yuv420p_to_rgba_scaled_into(
    frame: &YuvFrame,
    visible_width: u32,
    visible_height: u32,
    output_width: u32,
    output_height: u32,
    rgba: &mut Vec<u8>,
) {
    let stride = frame.width as usize;
    let width = (visible_width as usize).min(stride);
    let height = (visible_height as usize).min(frame.height as usize);
    let output_width = output_width as usize;
    let output_height = output_height as usize;
    let chroma_stride = stride / 2;
    let matrix = VideoTransferMatrix::for_unspecified_source(visible_height.max(frame.height));
    let total_bytes = output_width.saturating_mul(output_height).saturating_mul(4);
    rgba.resize(total_bytes, 0);
    if output_width == 0 || output_height == 0 || width == 0 || height == 0 {
        return;
    }

    let chroma_vis_width = (width / 2).max(1);
    let mut x_map = Vec::with_capacity(output_width);
    for out_x in 0..output_width {
        let x = (out_x * width / output_width).min(width.saturating_sub(1));
        let cx = (x / 2).min(chroma_vis_width.saturating_sub(1));
        x_map.push((x, cx));
    }

    let y_plane = &frame.y;
    let cb_plane = &frame.cb;
    let cr_plane = &frame.cr;

    for out_y in 0..output_height {
        let y = (out_y * height / output_height).min(height.saturating_sub(1));
        let y_row_start = y.saturating_mul(stride);
        let c_row_start = (y / 2).saturating_mul(chroma_stride);
        let out_row_start = out_y.saturating_mul(output_width).saturating_mul(4);
        let out_row = &mut rgba[out_row_start..out_row_start + output_width * 4];

        if y_row_start + width <= y_plane.len()
            && c_row_start + chroma_vis_width <= cb_plane.len()
            && c_row_start + chroma_vis_width <= cr_plane.len()
        {
            let y_row = &y_plane[y_row_start..y_row_start + width];
            let cb_row = &cb_plane[c_row_start..c_row_start + chroma_vis_width];
            let cr_row = &cr_plane[c_row_start..c_row_start + chroma_vis_width];
            for (dst_px, &(x, cx)) in out_row.chunks_exact_mut(4).zip(x_map.iter()) {
                let luma = y_row[x];
                let cb = cb_row[cx];
                let cr = cr_row[cx];
                let [r, g, b] = yuv_limited_to_rgb(luma, cb, cr, matrix);
                dst_px[0] = r;
                dst_px[1] = g;
                dst_px[2] = b;
                dst_px[3] = 255;
            }
        } else {
            for (dst_px, &(x, cx)) in out_row.chunks_exact_mut(4).zip(x_map.iter()) {
                let luma = y_plane.get(y_row_start + x).copied().unwrap_or(16);
                let cb = cb_plane.get(c_row_start + cx).copied().unwrap_or(128);
                let cr = cr_plane.get(c_row_start + cx).copied().unwrap_or(128);
                let [r, g, b] = yuv_limited_to_rgb(luma, cb, cr, matrix);
                dst_px[0] = r;
                dst_px[1] = g;
                dst_px[2] = b;
                dst_px[3] = 255;
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn windows_unspecified_matrix_switches_above_576_lines() {
        assert_eq!(
            VideoTransferMatrix::for_unspecified_source(480),
            VideoTransferMatrix::Bt601
        );
        assert_eq!(
            VideoTransferMatrix::for_unspecified_source(576),
            VideoTransferMatrix::Bt601
        );
        assert_eq!(
            VideoTransferMatrix::for_unspecified_source(577),
            VideoTransferMatrix::Bt709
        );
        assert_eq!(
            VideoTransferMatrix::for_unspecified_source(1080),
            VideoTransferMatrix::Bt709
        );
    }

    #[test]
    fn studio_black_and_white_are_matrix_independent() {
        for matrix in [VideoTransferMatrix::Bt601, VideoTransferMatrix::Bt709] {
            assert_eq!(yuv_limited_to_rgb(16, 128, 128, matrix), [0, 0, 0]);
            assert_eq!(yuv_limited_to_rgb(235, 128, 128, matrix), [255, 255, 255]);
        }
    }

    #[test]
    fn bt601_and_bt709_use_different_chroma_matrices() {
        let sample = (100, 90, 200);
        assert_ne!(
            yuv_limited_to_rgb(sample.0, sample.1, sample.2, VideoTransferMatrix::Bt601,),
            yuv_limited_to_rgb(sample.0, sample.1, sample.2, VideoTransferMatrix::Bt709,),
        );
    }
}
