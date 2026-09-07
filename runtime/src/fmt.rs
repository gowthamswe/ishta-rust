//! f-string format-spec runtime formatter — the shared-renderer path for
//! the specifiers `snprintf` can't express (binary `b`, center align `^`,
//! and custom fill chars). Phase-8 stdlib floor follow-up.
//!
//! ## Why this exists
//!
//! Codegen renders most `f"{x:spec}"` holes inline via `snprintf`
//! (`FormatSpec::to_printf` → `%04lld` etc.), which matches the
//! interpreter's `apply_*` byte-for-byte for the printf-expressible subset.
//! But printf has no binary conversion, no center alignment, and no custom
//! (non-space) fill char. For those three, codegen instead compiles the raw
//! spec string + the value into a call to one of the `karac_runtime_fmt_*`
//! entrypoints below, which parse the spec and render it through the SAME
//! `crate::format_spec::FormatSpec::apply_*` helpers the interpreter calls.
//! One renderer, one source of truth → `karac run` == `karac build`.
//!
//! ## Single source of truth
//!
//! `format_spec.rs` lives in the compiler crate (the interpreter needs it in
//! non-`llvm` builds, where `karac-runtime` isn't even a dependency). It is
//! freestanding — std-only, no `use crate::` — so we compile the *same file*
//! into the runtime crate via `#[path]` rather than duplicate it. Editing the
//! parser or an `apply_*` helper updates both the interpreter and this
//! formatter at once. `#[allow(dead_code)]`: the runtime uses `apply_*` and
//! `parse` but not the codegen-only `to_printf` / `int_conv`.
//!
//! ## ABI
//!
//! Each entrypoint takes the raw spec bytes (`spec_ptr` / `spec_len`), the
//! value, and a caller-provided output buffer (`out_buf` / `out_cap`). It
//! writes the rendered UTF-8 bytes into the buffer and returns the byte
//! length written (never exceeding `out_cap`). Codegen sizes `out_buf` to the
//! spec's guaranteed maximum output (`max(width, 72)` for numerics; `width`
//! for the string pad branch, which it only enters when the source is
//! shorter than `width`), so no truncation occurs in practice; the `out_cap`
//! bound is a hard safety net against overflow regardless.

#[allow(dead_code)]
#[path = "../../src/format_spec.rs"]
mod format_spec;

use format_spec::FormatSpec;

/// Parse the raw spec bytes into a `FormatSpec`, falling back to the default
/// (no-op) spec if the bytes are somehow invalid. The typechecker validated
/// the spec at compile time, so the fallback never fires in practice — it
/// just keeps the runtime total rather than panicking across the FFI edge.
unsafe fn parse_spec(spec_ptr: *const u8, spec_len: i64) -> FormatSpec {
    unsafe {
        let default = || FormatSpec {
            fill: None,
            align: None,
            zero_pad: false,
            width: None,
            precision: None,
            radix: format_spec::Radix::Dec,
        };
        if spec_ptr.is_null() || spec_len < 0 {
            return default();
        }
        let raw = std::slice::from_raw_parts(spec_ptr, spec_len as usize);
        match std::str::from_utf8(raw) {
            Ok(s) => FormatSpec::parse(s).unwrap_or_else(|_| default()),
            Err(_) => default(),
        }
    }
}

/// Copy `s`'s bytes into `out_buf` bounded by `out_cap`, returning the number
/// of bytes written.
unsafe fn write_out(s: &str, out_buf: *mut u8, out_cap: i64) -> i64 {
    unsafe {
        if out_buf.is_null() || out_cap <= 0 {
            return 0;
        }
        let bytes = s.as_bytes();
        let n = std::cmp::min(bytes.len(), out_cap as usize);
        std::ptr::copy_nonoverlapping(bytes.as_ptr(), out_buf, n);
        n as i64
    }
}

/// Render an integer hole. `is_unsigned != 0` selects `apply_uint` (never
/// negative, and the value's bits are the magnitude); otherwise `apply_int`.
///
/// # Safety
///
/// `spec_ptr`/`out_buf` must satisfy the ABI in the module doc.
#[no_mangle]
pub unsafe extern "C" fn karac_runtime_fmt_int(
    spec_ptr: *const u8,
    spec_len: i64,
    value: i64,
    is_unsigned: i32,
    out_buf: *mut u8,
    out_cap: i64,
) -> i64 {
    unsafe {
        let fs = parse_spec(spec_ptr, spec_len);
        let rendered = if is_unsigned != 0 {
            fs.apply_uint(value as u64)
        } else {
            fs.apply_int(value)
        };
        write_out(&rendered, out_buf, out_cap)
    }
}

/// Render a float hole (`apply_float`).
///
/// # Safety
///
/// `spec_ptr`/`out_buf` must satisfy the ABI in the module doc.
#[no_mangle]
pub unsafe extern "C" fn karac_runtime_fmt_float(
    spec_ptr: *const u8,
    spec_len: i64,
    value: f64,
    out_buf: *mut u8,
    out_cap: i64,
) -> i64 {
    unsafe {
        let fs = parse_spec(spec_ptr, spec_len);
        let rendered = fs.apply_float(value);
        write_out(&rendered, out_buf, out_cap)
    }
}

/// Render a string hole (`apply_str` — width padding + align + fill). The
/// source bytes are borrowed read-only; codegen only calls this on the
/// pad branch (source shorter than `width`), so the result fits in a
/// `width`-sized buffer.
///
/// # Safety
///
/// `spec_ptr`/`s_ptr`/`out_buf` must satisfy the ABI in the module doc.
#[no_mangle]
pub unsafe extern "C" fn karac_runtime_fmt_str(
    spec_ptr: *const u8,
    spec_len: i64,
    s_ptr: *const u8,
    s_len: i64,
    out_buf: *mut u8,
    out_cap: i64,
) -> i64 {
    unsafe {
        let fs = parse_spec(spec_ptr, spec_len);
        let s = if s_ptr.is_null() || s_len < 0 {
            ""
        } else {
            let bytes = std::slice::from_raw_parts(s_ptr, s_len as usize);
            std::str::from_utf8(bytes).unwrap_or("")
        };
        let rendered = fs.apply_str(s);
        write_out(&rendered, out_buf, out_cap)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    unsafe fn call_int(spec: &str, v: i64, unsigned: bool) -> String {
        unsafe {
            let mut buf = [0u8; 128];
            let n = karac_runtime_fmt_int(
                spec.as_ptr(),
                spec.len() as i64,
                v,
                unsigned as i32,
                buf.as_mut_ptr(),
                buf.len() as i64,
            );
            String::from_utf8(buf[..n as usize].to_vec()).unwrap()
        }
    }
    unsafe fn call_float(spec: &str, v: f64) -> String {
        unsafe {
            let mut buf = [0u8; 128];
            let n = karac_runtime_fmt_float(
                spec.as_ptr(),
                spec.len() as i64,
                v,
                buf.as_mut_ptr(),
                buf.len() as i64,
            );
            String::from_utf8(buf[..n as usize].to_vec()).unwrap()
        }
    }
    unsafe fn call_str(spec: &str, s: &str) -> String {
        unsafe {
            let mut buf = [0u8; 128];
            let n = karac_runtime_fmt_str(
                spec.as_ptr(),
                spec.len() as i64,
                s.as_ptr(),
                s.len() as i64,
                buf.as_mut_ptr(),
                buf.len() as i64,
            );
            String::from_utf8(buf[..n as usize].to_vec()).unwrap()
        }
    }

    #[test]
    fn runtime_matches_apply_helpers() {
        unsafe {
            // Binary radix (the printf-inexpressible case).
            assert_eq!(call_int("b", 5, false), "101");
            assert_eq!(call_int("08b", 5, false), "00000101");
            assert_eq!(call_int("b", 255, true), "11111111");
            // Center align.
            assert_eq!(call_int("^6", 42, false), "  42  ");
            assert_eq!(call_str("^7", "hi"), "  hi   ");
            assert_eq!(call_float("^8.2", 1.5), "  1.50  ");
            // Custom fill.
            assert_eq!(call_str("*^7", "hi"), "**hi***");
            assert_eq!(call_str("*<7", "hi"), "hi*****");
            assert_eq!(call_int("*>7", 42, false), "*****42");
            assert_eq!(call_float("*^8.2", 1.5), "**1.50**");
        }
    }

    #[test]
    fn runtime_agrees_with_direct_apply() {
        // The FFI wrappers must return exactly what a direct `apply_*` call
        // returns — the property that guarantees run==build. Cross-check a
        // spread of specs against the shared helpers directly.
        unsafe {
            for (spec, v) in [("b", 42i64), ("^10", -7), ("=^9b", 6), ("*>12", 100)] {
                let fs = FormatSpec::parse(spec).unwrap();
                assert_eq!(call_int(spec, v, false), fs.apply_int(v), "int spec {spec}");
            }
            for spec in ["^9", "*^11", "*<8", ">6"] {
                let fs = FormatSpec::parse(spec).unwrap();
                assert_eq!(
                    call_str(spec, "kara"),
                    fs.apply_str("kara"),
                    "str spec {spec}"
                );
            }
        }
    }

    #[test]
    fn out_cap_never_overflows() {
        // A buffer smaller than the rendered output truncates rather than
        // overflowing (the hard safety net).
        unsafe {
            let mut buf = [0xAAu8; 4];
            let n =
                karac_runtime_fmt_int("b".as_ptr(), 1, 255, 0, buf.as_mut_ptr(), buf.len() as i64);
            assert_eq!(n, 4);
            assert_eq!(&buf, b"1111");
        }
    }

    /// ORACLE AGREEMENT for the allocation-free fast path.
    ///
    /// `karac_runtime_i64_fmt` reproduces `FormatSpec::apply_int` /
    /// `apply_uint` from PRE-DECODED constants instead of parsing the spec and
    /// building a `String`. Those two are the interpreter's path, so the fast
    /// path is only correct insofar as it matches them byte for byte — and the
    /// failure mode is silent (a wrong pad or a dropped sign still prints
    /// something plausible). So this asserts the whole cross product rather
    /// than a handful of eyeballed cases.
    ///
    /// Specs `needs_runtime_formatter()` diverts (binary radix, center align,
    /// non-space fill) are excluded: codegen never routes them here.
    #[test]
    fn i64_fmt_fast_path_matches_apply_int_over_a_matrix() {
        unsafe fn fast(fs: &FormatSpec, raw: u64, signed: bool) -> String {
            unsafe {
                let mut buf = [0u8; 256];
                let n = crate::karac_runtime_i64_fmt(
                    raw,
                    signed as i32,
                    fs.fast_radix_code(),
                    fs.zero_pad as i32,
                    fs.width.unwrap_or(0) as i64,
                    fs.numeric_align_left() as i32,
                    buf.as_mut_ptr(),
                    buf.len() as i64,
                );
                String::from_utf8(buf[..n as usize].to_vec()).unwrap()
            }
        }

        // Every spec shape the fast path can actually receive.
        let specs = [
            "", "5", "1", "20", "<5", ">5", "<12", ">12", "05", "020", "08", "x", "X", "o", "5x",
            "5X", "5o", "05x", "05X", "05o", "<8x", ">8X", "<8o", "020x",
        ];
        // Boundaries first: i64::MIN is where a naive negate wraps, and the
        // powers of ten/two are where digit counts cross the width.
        let signed_vals: [i64; 14] = [
            0,
            1,
            -1,
            7,
            -7,
            9,
            -9,
            10,
            -10,
            99999,
            -99999,
            i64::MAX,
            i64::MIN,
            i64::MIN + 1,
        ];
        let unsigned_vals: [u64; 8] = [0, 1, 9, 10, 255, u64::MAX, u64::MAX - 1, 1 << 63];

        for raw in specs {
            let fs = FormatSpec::parse(raw).expect("spec parses");
            if fs.needs_runtime_formatter() {
                continue;
            }
            for v in signed_vals {
                let want = fs.apply_int(v);
                let got = unsafe { fast(&fs, v as u64, true) };
                assert_eq!(
                    got, want,
                    "signed mismatch: spec {raw:?} value {v} -> fast {got:?} vs apply_int {want:?}"
                );
            }
            for v in unsigned_vals {
                let want = fs.apply_uint(v);
                let got = unsafe { fast(&fs, v, false) };
                assert_eq!(
                    got, want,
                    "unsigned mismatch: spec {raw:?} value {v} -> fast {got:?} vs apply_uint {want:?}"
                );
            }
        }
    }

    /// The fast path must TRUNCATE rather than write past a short buffer.
    /// Codegen sizes the buffer as `max(64, width + 2)` so this is a guard, not
    /// an expected path — but it is the one bug in a hand-rolled renderer that
    /// corrupts memory instead of printing wrong.
    #[test]
    fn i64_fmt_fast_path_truncates_into_a_short_buffer() {
        unsafe {
            let mut buf = [0xAAu8; 8];
            let n =
                crate::karac_runtime_i64_fmt(1234567890123u64, 1, 10, 0, 0, 0, buf.as_mut_ptr(), 4);
            assert_eq!(n, 4, "should stop at the cap");
            assert_eq!(&buf[..4], b"1234");
            assert_eq!(&buf[4..], &[0xAA; 4], "must not write past buf_len");

            // null / non-positive cap write nothing
            assert_eq!(
                crate::karac_runtime_i64_fmt(1, 1, 10, 0, 0, 0, std::ptr::null_mut(), 8),
                0
            );
            assert_eq!(
                crate::karac_runtime_i64_fmt(1, 1, 10, 0, 0, 0, buf.as_mut_ptr(), 0),
                0
            );
        }
    }
}
