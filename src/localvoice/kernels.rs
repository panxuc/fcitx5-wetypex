// Adapted from WowType d145a93b1aac9fb1d464de0646f62374cd4ebb4d; see NOTICE.md.
//! 数值内核：int8 动态量化矩阵乘（aarch64 sdot / x86_64 AVX2 / 标量兜底）与 f32 点积
use super::parallel;

/// 按行动态量化到 int8（对称，每行一个 scale）
pub struct QAct {
    pub q: Vec<i8>,
    pub s: Vec<f32>,
    pub k: usize,
}

pub fn quant(x: &[f32], k: usize) -> QAct {
    let t = x.len() / k;
    let mut q = vec![0i8; t * k];
    let mut s = vec![0f32; t];
    for ((q, s), x) in q.chunks_mut(k).zip(s.iter_mut()).zip(x.chunks(k)) {
        let m = x.iter().fold(0f32, |a, v| a.max(v.abs()));
        let sc = if m > 0.0 { m / 127.0 } else { 1.0 };
        let inv = 1.0 / sc;
        for (q, v) in q.iter_mut().zip(x) {
            *q = (v * inv).round().clamp(-127.0, 127.0) as i8;
        }
        *s = sc;
    }
    QAct { q, s, k }
}

/// out[t×n] = (x · wᵀ) * scale + bias，w 为 [n, k] 行主序 int8
#[allow(clippy::needless_range_loop)] // 行/列索引运算，不是简单遍历
pub fn qgemm(x: &QAct, w: &[i8], n: usize, ws: f32, bias: Option<&[f32]>) -> Vec<f32> {
    let k = x.k;
    let t = x.s.len();
    debug_assert_eq!(w.len(), n * k);
    let mut out = vec![0f32; t * n];
    // 用 usize 传递基址：裸指针不是 Send，闭包按字段捕获时会被拒
    let base = out.as_mut_ptr() as usize;
    const TILE: usize = 32;
    parallel::for_each_index(n.div_ceil(TILE), move |ti| {
        let n0 = ti * TILE;
        let n1 = (n0 + TILE).min(n);
        let mut j = n0;
        while j < n1 {
            let rows = (n1 - j).min(4);
            let mut i = 0;
            while i < t {
                let r2 = (t - i).min(2);
                let acc = dot(&x.q[i * k..], r2, &w[j * k..], rows, k);
                for a in 0..r2 {
                    for b in 0..rows {
                        let v =
                            acc[a][b] as f32 * x.s[i + a] * ws + bias.map_or(0.0, |bb| bb[j + b]);
                        unsafe { *((base as *mut f32).add((i + a) * n + j + b)) = v };
                    }
                }
                i += r2;
            }
            j += rows;
        }
    });
    out
}

/// xr 行（≤2）× wr 行（≤4）的 int8 点积；不足的行用重复行指针补齐
#[inline]
fn dot(x: &[i8], xr: usize, w: &[i8], wr: usize, k: usize) -> [[i32; 4]; 2] {
    let xp = [x.as_ptr(), x[(xr - 1) * k..].as_ptr()];
    let wp: [*const i8; 4] = std::array::from_fn(|b| w[b.min(wr - 1) * k..].as_ptr());
    #[cfg(target_arch = "aarch64")]
    if k % 16 == 0 {
        return unsafe { neon::dot2x4(xp, wp, k) };
    }
    #[cfg(target_arch = "x86_64")]
    if k % 32 == 0 {
        match x86_level() {
            #[cfg(wetype_vnni)]
            X86::Vnni => return unsafe { avx::dot2x4_vnni(xp, wp, k) },
            X86::Avx2 => return unsafe { avx::dot2x4(xp, wp, k) },
            X86::None => {}
        }
    }
    let mut r = [[0i32; 4]; 2];
    for a in 0..xr {
        for b in 0..wr {
            r[a][b] = dot1(&x[a * k..(a + 1) * k], &w[b * k..(b + 1) * k]);
        }
    }
    r
}

fn dot1(a: &[i8], b: &[i8]) -> i32 {
    let mut acc = [0i32; 16];
    let (ca, cb) = (a.chunks_exact(16), b.chunks_exact(16));
    let (ra, rb) = (ca.remainder(), cb.remainder());
    for (x, y) in ca.zip(cb) {
        for i in 0..16 {
            acc[i] += x[i] as i32 * y[i] as i32;
        }
    }
    acc.iter().sum::<i32>()
        + ra.iter()
            .zip(rb)
            .map(|(&x, &y)| x as i32 * y as i32)
            .sum::<i32>()
}

#[cfg(target_arch = "x86_64")]
#[derive(Clone, Copy, PartialEq, Eq)]
enum X86 {
    None,
    Avx2,
    /// AVX-VNNI（Intel 12 代、AMD Zen 5 起）：一条 vpdpbusd 顶 maddubs + madd 两条
    #[cfg(wetype_vnni)]
    Vnni,
}

#[cfg(target_arch = "x86_64")]
fn x86_level() -> X86 {
    static F: std::sync::OnceLock<X86> = std::sync::OnceLock::new();
    *F.get_or_init(|| {
        if std::env::var("WETYPE_LOCAL_VOICE_CPU").as_deref() == Ok("scalar") {
            return X86::None;
        }
        #[cfg(wetype_vnni)]
        if std::env::var("WETYPE_LOCAL_VOICE_CPU").as_deref() != Ok("avx2")
            && std::is_x86_feature_detected!("avx2")
            && std::is_x86_feature_detected!("avxvnni")
        {
            return X86::Vnni;
        }
        if std::is_x86_feature_detected!("avx2") {
            X86::Avx2
        } else {
            X86::None
        }
    })
}

#[cfg(target_arch = "aarch64")]
mod neon {
    use std::arch::aarch64::*;

    /// 用 stable 的 vmull_s8 + vpadalq_s16 累加：
    /// vdotq_s32 需要 nightly 的 stdarch_neon_dotprod，这里换成等价写法。
    #[inline]
    unsafe fn add4(x: [*const i8; 2], w: *const i8, i: usize, a: &mut [int32x4_t; 8], b: usize) {
        let wv = vld1q_s8(w.add(i));
        let wl = vget_low_s8(wv);
        let wh = vget_high_s8(wv);
        for (k, xp) in x.into_iter().enumerate() {
            let xv = vld1q_s8(xp.add(i));
            let lo = vmull_s8(vget_low_s8(xv), wl);
            let hi = vmull_s8(vget_high_s8(xv), wh);
            let slot = &mut a[k * 4 + b];
            *slot = vpadalq_s16(*slot, lo);
            *slot = vpadalq_s16(*slot, hi);
        }
    }

    #[inline]
    unsafe fn lane_sum(v: int32x4_t) -> i32 {
        vgetq_lane_s32(v, 0) + vgetq_lane_s32(v, 1) + vgetq_lane_s32(v, 2) + vgetq_lane_s32(v, 3)
    }

    #[allow(clippy::needless_range_loop)] // 列索引与累加器槽位一一对应
    pub unsafe fn dot2x4(x: [*const i8; 2], w: [*const i8; 4], k: usize) -> [[i32; 4]; 2] {
        let mut a = [vdupq_n_s32(0); 8];
        let mut i = 0;
        while i < k {
            for b in 0..4 {
                add4(x, w[b], i, &mut a, b);
            }
            i += 16;
        }
        let mut r = [[0; 4]; 2];
        for b in 0..4 {
            r[0][b] = lane_sum(a[b]);
            r[1][b] = lane_sum(a[4 + b]);
        }
        r
    }
}

#[cfg(target_arch = "x86_64")]
mod avx {
    use std::arch::x86_64::*;

    #[target_feature(enable = "avx2")]
    unsafe fn hsum(v: __m256i) -> i32 {
        let s = _mm_add_epi32(_mm256_castsi256_si128(v), _mm256_extracti128_si256(v, 1));
        let s = _mm_add_epi32(s, _mm_shuffle_epi32(s, 0b01_00_11_10));
        let s = _mm_add_epi32(s, _mm_shuffle_epi32(s, 0b10_11_00_01));
        _mm_cvtsi128_si32(s)
    }

    /// |x| (u8) × sign(w, x) (i8) 经 maddubs 成对累加到 i16（|x|≤127 不溢出），再 madd 到 i32
    #[target_feature(enable = "avx2")]
    pub unsafe fn dot2x4(x: [*const i8; 2], w: [*const i8; 4], k: usize) -> [[i32; 4]; 2] {
        let ones = _mm256_set1_epi16(1);
        let mut a = [_mm256_setzero_si256(); 8];
        let mut i = 0;
        while i < k {
            let x0 = _mm256_loadu_si256(x[0].add(i) as *const __m256i);
            let x1 = _mm256_loadu_si256(x[1].add(i) as *const __m256i);
            let (u0, u1) = (_mm256_sign_epi8(x0, x0), _mm256_sign_epi8(x1, x1));
            for b in 0..4 {
                let wv = _mm256_loadu_si256(w[b].add(i) as *const __m256i);
                let p0 =
                    _mm256_madd_epi16(_mm256_maddubs_epi16(u0, _mm256_sign_epi8(wv, x0)), ones);
                let p1 =
                    _mm256_madd_epi16(_mm256_maddubs_epi16(u1, _mm256_sign_epi8(wv, x1)), ones);
                a[b] = _mm256_add_epi32(a[b], p0);
                a[4 + b] = _mm256_add_epi32(a[4 + b], p1);
            }
            i += 32;
        }
        let mut r = [[0; 4]; 2];
        for b in 0..4 {
            r[0][b] = hsum(a[b]);
            r[1][b] = hsum(a[4 + b]);
        }
        r
    }

    /// acc += u8(a) × i8(b)，四个一组累加到 i32（AVX-VNNI 的 vpdpbusd）
    ///
    /// 用内联汇编而不是标准库的 intrinsic：有的发行版（如 Fedora 43）的 rustc
    /// 链接系统 LLVM，那个 intrinsic 的签名和标准库对不上，整个模块编不过。
    #[cfg(wetype_vnni)]
    #[target_feature(enable = "avx2,avxvnni")]
    #[inline]
    unsafe fn dpbusd(mut acc: __m256i, a: __m256i, b: __m256i) -> __m256i {
        std::arch::asm!(
            "{{vex}} vpdpbusd {acc}, {a}, {b}",
            acc = inout(ymm_reg) acc,
            a = in(ymm_reg) a,
            b = in(ymm_reg) b,
            options(pure, nomem, nostack, preserves_flags),
        );
        acc
    }

    /// 同 [`dot2x4`]，乘加用 AVX-VNNI 的 vpdpbusd（u8×i8 直接累加到 i32）
    #[cfg(wetype_vnni)]
    #[target_feature(enable = "avx2,avxvnni")]
    pub unsafe fn dot2x4_vnni(x: [*const i8; 2], w: [*const i8; 4], k: usize) -> [[i32; 4]; 2] {
        let mut a = [_mm256_setzero_si256(); 8];
        let mut i = 0;
        while i < k {
            let x0 = _mm256_loadu_si256(x[0].add(i) as *const __m256i);
            let x1 = _mm256_loadu_si256(x[1].add(i) as *const __m256i);
            let (u0, u1) = (_mm256_sign_epi8(x0, x0), _mm256_sign_epi8(x1, x1));
            for b in 0..4 {
                let wv = _mm256_loadu_si256(w[b].add(i) as *const __m256i);
                a[b] = dpbusd(a[b], u0, _mm256_sign_epi8(wv, x0));
                a[4 + b] = dpbusd(a[4 + b], u1, _mm256_sign_epi8(wv, x1));
            }
            i += 32;
        }
        let mut r = [[0; 4]; 2];
        for b in 0..4 {
            r[0][b] = hsum(a[b]);
            r[1][b] = hsum(a[4 + b]);
        }
        r
    }
}

/// f32 点积（8 路累加，便于自动向量化）
#[inline]
pub fn dotf(a: &[f32], b: &[f32]) -> f32 {
    let mut acc = [0f32; 8];
    let (ca, cb) = (a.chunks_exact(8), b.chunks_exact(8));
    let (ra, rb) = (ca.remainder(), cb.remainder());
    for (x, y) in ca.zip(cb) {
        for i in 0..8 {
            acc[i] += x[i] * y[i];
        }
    }
    acc.iter().sum::<f32>() + ra.iter().zip(rb).map(|(x, y)| x * y).sum::<f32>()
}
