// Adapted from WowType d145a93b1aac9fb1d464de0646f62374cd4ebb4d; see NOTICE.md.
//! 基于标准库的小型并行工具。
//!
//! 原实现用 rayon；这里换成 `std::thread::scope`，避免为几处并行引入额外依赖。
//! 线程数上限与上游一致（4）：再多的收益很小，调度开销反而明显。

use std::sync::atomic::{AtomicUsize, Ordering};

pub fn threads() -> usize {
    std::thread::available_parallelism()
        .map_or(1, |n| n.get())
        .min(4)
}

/// 按 `chunk` 切分后并行处理，回调拿到 (块序号, 块切片)。
///
/// `buf.len()` 不是 `chunk` 的整数倍时最后一块会更短，序号仍然连续。
pub fn for_each_chunk_mut<T: Send>(
    buf: &mut [T],
    chunk: usize,
    f: impl Fn(usize, &mut [T]) + Sync,
) {
    if chunk == 0 || buf.is_empty() {
        return;
    }
    let blocks = buf.len().div_ceil(chunk);
    let workers = threads().min(blocks);
    if workers <= 1 {
        for (i, c) in buf.chunks_mut(chunk).enumerate() {
            f(i, c);
        }
        return;
    }
    let per = blocks.div_ceil(workers);
    std::thread::scope(|s| {
        for (w, part) in buf.chunks_mut(per * chunk).enumerate() {
            let f = &f;
            s.spawn(move || {
                for (j, c) in part.chunks_mut(chunk).enumerate() {
                    f(w * per + j, c);
                }
            });
        }
    });
}

/// 并行遍历 `0..n`，任务从共享计数器动态领取，适合长度不均的分块。
pub fn for_each_index<F: Fn(usize) + Sync>(n: usize, f: F) {
    if n == 0 {
        return;
    }
    let workers = threads().min(n);
    if workers <= 1 {
        for i in 0..n {
            f(i);
        }
        return;
    }
    let next = AtomicUsize::new(0);
    std::thread::scope(|s| {
        for _ in 0..workers {
            let f = &f;
            let next = &next;
            s.spawn(move || loop {
                let i = next.fetch_add(1, Ordering::Relaxed);
                if i >= n {
                    break;
                }
                f(i);
            });
        }
    });
}
