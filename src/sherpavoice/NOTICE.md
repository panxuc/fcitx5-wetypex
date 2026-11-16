# Optional ASR components

`c-api.h` is from sherpa-onnx v2.0.1, Copyright 2023 Xiaomi Corporation,
licensed under Apache-2.0: https://github.com/k2-fsa/sherpa-onnx/tree/v2.0.1.
Only the header is included in the source package. The matching native CPU runtime
is downloaded separately when an optional model is installed.

The runtime includes sherpa-onnx (Apache-2.0) and ONNX Runtime (MIT).
Silero VAD is MIT licensed: https://github.com/snakers4/silero-vad.
X-ASR, Qwen3-ASR, FireRedASR2 and Fun-ASR-Nano model sources, licenses and pinned
checksums are recorded in `optional-voice-models.json`.
