//! 24-bit uncompressed bottom-up BMP encoder for `GET /api/preview`.

pub const PREVIEW_WIDTH: u32 = 240;
pub const PREVIEW_HEIGHT: u32 = 150;

/// Encodes a top-down tightly packed RGBA8 buffer into a 24-bit bottom-up BMP.
/// Returns `None` on invalid dimensions, integer overflow, or truncated input.
pub fn encode_rgba_top_down_to_bmp24(rgba: &[u8], width: u32, height: u32) -> Option<Vec<u8>> {
    if width == 0 || height == 0 || width > 16384 || height > 16384 {
        return None;
    }
    let w = width as usize;
    let h = height as usize;
    let expected = w.checked_mul(h)?.checked_mul(4)?;
    if rgba.len() < expected {
        return None;
    }

    let row_bytes = w.checked_mul(3)?;
    let row_stride = (row_bytes + 3) & !3;
    let pixel_bytes = row_stride.checked_mul(h)?;
    let file_size = 54usize.checked_add(pixel_bytes)?;
    let file_size_u32 = u32::try_from(file_size).ok()?;
    let pixel_bytes_u32 = u32::try_from(pixel_bytes).ok()?;
    let w_i32 = i32::try_from(width).ok()?;
    let h_i32 = i32::try_from(height).ok()?;

    let mut out = vec![0u8; file_size];
    // BITMAPFILEHEADER (14 bytes)
    out[0] = b'B';
    out[1] = b'M';
    out[2..6].copy_from_slice(&file_size_u32.to_le_bytes());
    out[10..14].copy_from_slice(&54u32.to_le_bytes());

    // BITMAPINFOHEADER (40 bytes)
    out[14..18].copy_from_slice(&40u32.to_le_bytes());
    out[18..22].copy_from_slice(&w_i32.to_le_bytes());
    out[22..26].copy_from_slice(&h_i32.to_le_bytes()); // positive = bottom-up
    out[26..28].copy_from_slice(&1u16.to_le_bytes()); // planes
    out[28..30].copy_from_slice(&24u16.to_le_bytes()); // bpp
    out[34..38].copy_from_slice(&pixel_bytes_u32.to_le_bytes());

    // Write rows bottom-up, converting RGBA -> BGR
    for dst_y in 0..h {
        let src_y = h - 1 - dst_y;
        let src_row = src_y * w * 4;
        let dst_row = 54 + dst_y * row_stride;
        for x in 0..w {
            let s = src_row + x * 4;
            let d = dst_row + x * 3;
            out[d] = rgba[s + 2];     // B
            out[d + 1] = rgba[s + 1]; // G
            out[d + 2] = rgba[s];     // R
        }
    }
    Some(out)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn encodes_valid_bottom_up_bgr_and_odd_padding() {
        // 3x2 top-down RGBA: top row = red, green, blue; bottom row = white, black, amber
        let rgba = [
            255, 0, 0, 255,   0, 255, 0, 255,   0, 0, 255, 255,
            255, 255, 255, 255, 0, 0, 0, 255,  255, 128, 32, 255,
        ];
        let bmp = encode_rgba_top_down_to_bmp24(&rgba, 3, 2).unwrap();
        // row stride for 3px = (9 + 3) & !3 = 12 bytes -> 54 + 24 = 78 bytes
        assert_eq!(bmp.len(), 78);
        assert_eq!(&bmp[0..2], b"BM");
        assert_eq!(u32::from_le_bytes(bmp[2..6].try_into().unwrap()), 78);
        assert_eq!(i32::from_le_bytes(bmp[18..22].try_into().unwrap()), 3);
        assert_eq!(i32::from_le_bytes(bmp[22..26].try_into().unwrap()), 2);
        assert_eq!(u16::from_le_bytes(bmp[28..30].try_into().unwrap()), 24);
        // Bottom row first (dst_y=0 -> src_y=1): white, black, amber (BGR) + 3 pad bytes
        assert_eq!(&bmp[54..63], &[255, 255, 255, 0, 0, 0, 32, 128, 255]);
        assert_eq!(&bmp[63..66], &[0, 0, 0]);
        // Top row second (dst_y=1 -> src_y=0): red, green, blue (BGR)
        assert_eq!(&bmp[66..75], &[0, 0, 255, 0, 255, 0, 255, 0, 0]);
    }

    #[test]
    fn rejects_invalid_inputs() {
        assert!(encode_rgba_top_down_to_bmp24(&[], 0, 10).is_none());
        assert!(encode_rgba_top_down_to_bmp24(&[], 10, 0).is_none());
        assert!(encode_rgba_top_down_to_bmp24(&[0; 10], 4, 4).is_none());
        assert!(encode_rgba_top_down_to_bmp24(&[0; 16], 20000, 1).is_none());
    }

    #[test]
    fn preview_dimensions_match_contract() {
        let rgba = vec![12u8; (PREVIEW_WIDTH * PREVIEW_HEIGHT * 4) as usize];
        let bmp = encode_rgba_top_down_to_bmp24(&rgba, PREVIEW_WIDTH, PREVIEW_HEIGHT).unwrap();
        assert_eq!(bmp.len(), 54 + (PREVIEW_WIDTH * 3 * PREVIEW_HEIGHT) as usize);
    }
}
