// Adapted from WowType d145a93b1aac9fb1d464de0646f62374cd4ebb4d; see NOTICE.md.
//! HTK 风格 FBank：39 维 log-mel + 对数能量，25ms 窗 / 10ms 帧移；减去最近 0.5 秒的滑动均值
use std::collections::VecDeque;

const WIN: usize = 400;
const HOP: usize = 160;
const NFFT: usize = 512;
const NCH: usize = 39;
pub const DIM: usize = NCH + 1;
/// 滑动均值窗口（帧），对应官方配置 cms_win=50；只减均值、不除方差：
/// 用 CMS.40.bin 的方差缩放会把笔记本麦克风这类底噪高、起伏小的声音压扁，模型大量漏字
const CMS_WIN: usize = 50;

struct Tables {
    ham: Vec<f64>,
    tw: Vec<(f64, f64)>,
    /// 每个频点 k（1..=256）：(低通道, 权重, 高通道, 权重)
    mel: Vec<(usize, f64, usize, f64)>,
}

fn tables() -> &'static Tables {
    static T: std::sync::OnceLock<Tables> = std::sync::OnceLock::new();
    T.get_or_init(|| {
        let pi = std::f64::consts::PI;
        let ham = (0..WIN)
            .map(|n| 0.54 - 0.46 * (2.0 * pi * n as f64 / (WIN - 1) as f64).cos())
            .collect();
        let tw = (0..NFFT / 2)
            .map(|i| {
                let a = -2.0 * pi * i as f64 / NFFT as f64;
                (a.cos(), a.sin())
            })
            .collect();
        let mel = |f: f64| 1127.0 * (1.0 + f / 700.0).ln();
        let hi = mel(8000.0);
        let cf: Vec<f64> = (1..=NCH + 1)
            .map(|i| hi * i as f64 / (NCH + 1) as f64)
            .collect();
        let bins = (1..=NFFT / 2)
            .map(|k| {
                let m = mel(k as f64 * 16000.0 / NFFT as f64);
                let c = cf.iter().take_while(|&&v| v < m).count();
                match c {
                    0 => (0, m / cf[0], 0, 0.0),
                    c if c < NCH => {
                        let w = (cf[c] - m) / (cf[c] - cf[c - 1]);
                        (c - 1, w, c, 1.0 - w)
                    }
                    c if c == NCH => (NCH - 1, (cf[NCH] - m) / (cf[NCH] - cf[NCH - 1]), 0, 0.0),
                    _ => (0, 0.0, 0, 0.0),
                }
            })
            .collect();
        Tables { ham, tw, mel: bins }
    })
}

fn fft(re: &mut [f64; NFFT], im: &mut [f64; NFFT], tw: &[(f64, f64)]) {
    let mut j = 0;
    for i in 1..NFFT {
        let mut b = NFFT >> 1;
        while j & b != 0 {
            j ^= b;
            b >>= 1;
        }
        j |= b;
        if i < j {
            re.swap(i, j);
            im.swap(i, j);
        }
    }
    let mut len = 2;
    while len <= NFFT {
        let step = NFFT / len;
        for s in (0..NFFT).step_by(len) {
            for k in 0..len / 2 {
                let (wr, wi) = tw[k * step];
                let (a, b) = (s + k, s + k + len / 2);
                let tr = re[b] * wr - im[b] * wi;
                let ti = re[b] * wi + im[b] * wr;
                re[b] = re[a] - tr;
                im[b] = im[a] - ti;
                re[a] += tr;
                im[a] += ti;
            }
        }
        len <<= 1;
    }
}

/// 抖动：每个采样加一点高斯噪声（标准差 1 个采样单位，同 Kaldi 的 dither=1）。
///
/// 数字静音（全 0）的对数谱是常数，和真实麦克风的底噪完全不一样：麦克风刚打开、
/// 或者软件静音时送来的就是全 0，滑动均值被它带偏，模型常把开头第一个字吞掉。
/// 合成语音前补 0.4 秒数字静音实测：不抖动字错率 4.9%，抖动后 1.6%。
pub struct Dither(u64);

impl Dither {
    pub fn new() -> Self {
        Dither(0x9E37_79B9_7F4A_7C15)
    }

    fn uniform(&mut self) -> f64 {
        // xorshift64*
        self.0 ^= self.0 >> 12;
        self.0 ^= self.0 << 25;
        self.0 ^= self.0 >> 27;
        ((self.0.wrapping_mul(0x2545_F491_4F6C_DD1D) >> 11) as f64 + 0.5) / (1u64 << 53) as f64
    }

    /// 标准正态（Box-Muller）
    fn gauss(&mut self) -> f64 {
        let (a, b) = (self.uniform(), self.uniform());
        (-2.0 * a.ln()).sqrt() * (2.0 * std::f64::consts::PI * b).cos()
    }
}

fn frame_with(x: &[i16], mut dither: Option<&mut Dither>) -> [f64; DIM] {
    let t = tables();
    let mut re = [0.0; NFFT];
    let mut im = [0.0; NFFT];
    let mut e = 0.0;
    let mut w = [0.0f64; WIN];
    for (n, v) in w.iter_mut().enumerate() {
        *v = x[n] as f64 + dither.as_deref_mut().map_or(0.0, Dither::gauss);
    }
    for n in 0..WIN {
        let v = w[n];
        e += v * v;
        let pe = if n == 0 {
            v * (1.0 - 0.97)
        } else {
            v - 0.97 * w[n - 1]
        };
        re[n] = pe * t.ham[n];
    }
    fft(&mut re, &mut im, &t.tw);
    let mut out = [0.0; DIM];
    for (k, &(lo, wl, hi, wh)) in t.mel.iter().enumerate() {
        let p = re[k + 1] * re[k + 1] + im[k + 1] * im[k + 1];
        out[lo] += p * wl;
        out[hi] += p * wh;
    }
    for v in &mut out[..NCH] {
        *v = v.max(1.0).ln();
    }
    out[NCH] = e.max(1e-10).ln();
    out
}

/// 静音帧，用于窗口末尾补齐；归一化空间的全 0 帧并不是静音，会让模型漏字。
/// 用抖动后的静音（和送进来的音频一样带底噪），不用对数谱为常数的数字静音
fn silence() -> [f64; DIM] {
    static S: std::sync::OnceLock<[f64; DIM]> = std::sync::OnceLock::new();
    *S.get_or_init(|| frame_with(&[0i16; WIN], Some(&mut Dither::new())))
}

/// 流式特征：喂 PCM，产出减去滑动均值后的 40 维帧
pub struct Fbank {
    buf: Vec<i16>,
    dither: Dither,
    hist: VecDeque<[f64; DIM]>,
    sum: [f64; DIM],
}

impl Fbank {
    pub fn new() -> Self {
        Fbank {
            buf: Vec::new(),
            dither: Dither::new(),
            hist: VecDeque::with_capacity(CMS_WIN),
            sum: [0.0; DIM],
        }
    }

    fn sub(&self, f: &[f64; DIM]) -> [f32; DIM] {
        let n = self.hist.len().max(1) as f64;
        std::array::from_fn(|i| (f[i] - self.sum[i] / n) as f32)
    }

    pub fn push(&mut self, pcm: &[i16], out: &mut Vec<[f32; DIM]>) {
        self.buf.extend_from_slice(pcm);
        let mut p = 0;
        while p + WIN <= self.buf.len() {
            let f = frame_with(&self.buf[p..p + WIN], Some(&mut self.dither));
            if self.hist.len() == CMS_WIN {
                let o = self.hist.pop_front().unwrap();
                (0..DIM).for_each(|i| self.sum[i] -= o[i]);
            }
            self.hist.push_back(f);
            (0..DIM).for_each(|i| self.sum[i] += f[i]);
            out.push(self.sub(&f));
            p += HOP;
        }
        self.buf.drain(..p);
    }

    /// 按当前滑动均值归一化的静音帧
    pub fn silence(&self) -> [f32; DIM] {
        self.sub(&silence())
    }
}
