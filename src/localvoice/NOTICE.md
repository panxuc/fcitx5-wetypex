# Offline decoder provenance

The XNET loader, FBank, Transformer, SIMD kernels and streaming decoder are
adapted from Gloridust/WowType, commit
`d145a93b1aac9fb1d464de0646f62374cd4ebb4d`, `voice/src/wtlocal`.
WowType declares its code dual-licensed under MIT or Apache-2.0 in its README;
this project uses the MIT option (see the installed MIT license).
The original inference implementation is by J3n5en/voicekey contributors;
WowType's NOTICE records its inclusion with the author's permission.

Sources:
- https://github.com/Gloridust/WowType/tree/d145a93b1aac9fb1d464de0646f62374cd4ebb4d/voice/src/wtlocal
- https://github.com/Gloridust/WowType/blob/d145a93b1aac9fb1d464de0646f62374cd4ebb4d/README.md#许可证
- https://github.com/J3n5en/voicekey/tree/main/crates/wtlocal

WeTypeX adds its bounded, cancellable process protocol, builds without Rust
package dependencies, and uses older-toolchain-compatible CPU dispatch.
No model weights are distributed. They remain Tencent resources, downloaded
only when requested from the official HTTPS CDN and verified before use.
