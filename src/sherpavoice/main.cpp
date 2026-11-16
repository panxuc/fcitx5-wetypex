// Optional native CPU ASR. The pinned runtime and weights are installed on demand.
#include "c-api.h"
#include <dlfcn.h>
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#define ASR_API(X) \
 X(GetVersionStr) X(CreateOnlineRecognizer) X(DestroyOnlineRecognizer) \
 X(CreateOnlineStream) X(DestroyOnlineStream) X(OnlineStreamAcceptWaveform) \
 X(IsOnlineStreamReady) X(DecodeOnlineStream) X(GetOnlineStreamResult) \
 X(DestroyOnlineRecognizerResult) X(OnlineStreamInputFinished) \
 X(OnlineStreamReset) X(OnlineStreamIsEndpoint) X(CreateOfflineRecognizer) \
 X(DestroyOfflineRecognizer) X(CreateOfflineStream) X(DestroyOfflineStream) \
 X(AcceptWaveformOffline) X(DecodeOfflineStream) X(GetOfflineStreamResult) \
 X(DestroyOfflineRecognizerResult) X(CreateVoiceActivityDetector) \
 X(DestroyVoiceActivityDetector) X(VoiceActivityDetectorAcceptWaveform) \
 X(VoiceActivityDetectorEmpty) X(VoiceActivityDetectorFront) \
 X(DestroySpeechSegment) X(VoiceActivityDetectorPop) \
 X(VoiceActivityDetectorReset) X(VoiceActivityDetectorFlush)
struct Api {
  void *library = nullptr;
#define MEMBER(n) decltype(&SherpaOnnx##n) n = nullptr;
  ASR_API(MEMBER)
#undef MEMBER
  explicit Api(const std::filesystem::path &runtime) {
    library = dlopen((runtime / "lib/libsherpa-onnx-c-api.so").c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!library) throw std::runtime_error(dlerror());
#define LOAD(n) n = reinterpret_cast<decltype(n)>(dlsym(library, "SherpaOnnx" #n)); \
    if (!n) throw std::runtime_error("离线运行组件缺少接口：" #n);
    ASR_API(LOAD)
#undef LOAD
    if (std::string(GetVersionStr()) != "2.0.1")
      throw std::runtime_error("离线运行组件版本不匹配，请修复模型");
  }
  ~Api() { if (library) dlclose(library); }
};
static std::string quoted(const std::string &value) {
  static const char hex[] = "0123456789abcdef";
  std::string out = "\"";
  for (unsigned char c : value) {
    if (c == '\\' || c == '"') { out += '\\'; out += c; }
    else if (c < 32) { out += "\\u00"; out += hex[c >> 4]; out += hex[c & 15]; }
    else out += c;
  }
  return out + '"';
}
static void emit(const char *kind, const std::string &token, const std::string &text) {
  std::cout << "{\"event\":" << quoted(kind) << ",\"token\":" << quoted(token)
            << ",\"text\":" << quoted(text) << ",\"message\":" << quoted(text) << "}" << std::endl;
  if (!std::cout) throw std::runtime_error("语音控制器已退出");
}
static std::string joined(std::string a, const std::string &b) {
  if (!a.empty() && !b.empty() && static_cast<unsigned char>(a.back()) < 128 &&
      static_cast<unsigned char>(b.front()) < 128 && std::isalnum(a.back()) && std::isalnum(b.front())) a += ' ';
  return a + b;
}
class Decoder {
  Api api;
  std::deque<std::string> paths;
  const SherpaOnnxOnlineRecognizer *online = nullptr;
  const SherpaOnnxOnlineStream *stream = nullptr;
  const SherpaOnnxOfflineRecognizer *offline = nullptr;
  const SherpaOnnxVoiceActivityDetector *vad = nullptr;
  std::string token, settled, shown;
  bool active = false;
  uint64_t samples = 0;
  const char *path(const std::filesystem::path &base, const char *name) {
    paths.push_back((base / name).string()); return paths.back().c_str();
  }
  void partial(const std::string &text) {
    if (text.size() > 65536) throw std::runtime_error("识别结果过长，请分段输入");
    if (text != shown) { shown = text; emit("partial", token, shown); }
  }
  std::string onlineText() {
    const auto *result = api.GetOnlineStreamResult(online, stream);
    if (!result) throw std::runtime_error("无法读取识别结果");
    std::string text = result->text ? result->text : "";
    api.DestroyOnlineRecognizerResult(result); return text;
  }
  void decodeOnline() {
    while (api.IsOnlineStreamReady(online, stream)) api.DecodeOnlineStream(online, stream);
  }
  void segments() {
    while (!api.VoiceActivityDetectorEmpty(vad)) {
      const auto *segment = api.VoiceActivityDetectorFront(vad);
      if (!segment) throw std::runtime_error("无法读取语音片段");
      const auto *utterance = api.CreateOfflineStream(offline);
      if (!utterance) { api.DestroySpeechSegment(segment); throw std::runtime_error("无法创建语音片段"); }
      api.AcceptWaveformOffline(utterance, 16000, segment->samples, segment->n);
      api.DestroySpeechSegment(segment);
      api.VoiceActivityDetectorPop(vad);
      api.DecodeOfflineStream(offline, utterance);
      const auto *result = api.GetOfflineStreamResult(utterance);
      if (result) { settled = joined(settled, result->text ? result->text : ""); api.DestroyOfflineRecognizerResult(result); }
      api.DestroyOfflineStream(utterance);
      partial(settled);
    }
  }
public:
  Decoder(const std::filesystem::path &model, const std::filesystem::path &runtime, const std::string &kind): api(runtime) {
    const auto threads = std::max(1u, std::min(4u, std::thread::hardware_concurrency() / 2));
    if (kind == "xasr") {
      SherpaOnnxOnlineRecognizerConfig c{};
      c.feat_config = {16000, 80}; c.decoding_method = "greedy_search";
      c.model_config.transducer.encoder = path(model, "encoder-160ms.onnx");
      c.model_config.transducer.decoder = path(model, "decoder-160ms.onnx");
      c.model_config.transducer.joiner = path(model, "joiner-160ms.onnx");
      c.model_config.tokens = path(model, "tokens.txt"); c.model_config.model_type = "zipformer2";
      c.model_config.provider = "cpu"; c.model_config.num_threads = threads;
      c.enable_endpoint = 1; c.rule1_min_trailing_silence = 2.4f;
      c.rule2_min_trailing_silence = .8f; c.rule3_min_utterance_length = 20.f;
      online = api.CreateOnlineRecognizer(&c);
      if (!online) throw std::runtime_error("无法加载 X-ASR 模型");
    } else {
      SherpaOnnxOfflineRecognizerConfig c{};
      c.feat_config = {16000, 80}; c.decoding_method = "greedy_search";
      auto &m = c.model_config; m.provider = "cpu"; m.num_threads = threads;
      if (kind == "qwen3") {
        m.qwen3_asr.conv_frontend = path(model, "conv_frontend.onnx");
        m.qwen3_asr.encoder = path(model, "encoder.int8.onnx");
        m.qwen3_asr.decoder = path(model, "decoder.int8.onnx");
        m.qwen3_asr.tokenizer = path(model, "tokenizer");
        m.qwen3_asr.max_total_len = 512; m.qwen3_asr.max_new_tokens = 256;
      } else if (kind == "funasr") {
        m.funasr_nano.encoder_adaptor = path(model, "encoder_adaptor.int8.onnx");
        m.funasr_nano.llm = path(model, "llm.int8.onnx");
        m.funasr_nano.embedding = path(model, "embedding.int8.onnx");
        m.funasr_nano.tokenizer = path(model, "Qwen3-0.6B");
        m.funasr_nano.max_new_tokens = 256;
      } else if (kind == "firered") {
        m.fire_red_asr_ctc.model = path(model, "model.int8.onnx"); m.tokens = path(model, "tokens.txt");
      } else throw std::runtime_error("不支持的离线模型");
      offline = api.CreateOfflineRecognizer(&c);
      if (!offline) throw std::runtime_error("无法加载离线模型");
      SherpaOnnxVadModelConfig v{};
      v.silero_vad.model = path(runtime, "silero_vad.onnx");
      v.silero_vad.threshold = .5f; v.silero_vad.min_silence_duration = .45f;
      v.silero_vad.min_speech_duration = .15f; v.silero_vad.max_speech_duration = 12.f;
      v.silero_vad.window_size = 512; v.sample_rate = 16000; v.num_threads = 1; v.provider = "cpu";
      vad = api.CreateVoiceActivityDetector(&v, 30.f);
      if (!vad) throw std::runtime_error("无法加载语音分段组件");
    }
  }
  ~Decoder() {
    if (stream) api.DestroyOnlineStream(stream);
    if (online) api.DestroyOnlineRecognizer(online);
    if (vad) api.DestroyVoiceActivityDetector(vad);
    if (offline) api.DestroyOfflineRecognizer(offline);
  }
  void serve() {
    char op;
    while (std::cin.read(&op, 1)) {
      unsigned char length[4];
      if (!std::cin.read(reinterpret_cast<char *>(length), 4)) break;
      uint32_t size = length[0] | uint32_t(length[1]) << 8 | uint32_t(length[2]) << 16 | uint32_t(length[3]) << 24;
      if (size > 65536) throw std::runtime_error("语音帧过大");
      std::string data(size, '\0'); if (!std::cin.read(data.data(), size)) break;
      if (op == 'S') {
        token = data; settled.clear(); shown.clear(); samples = 0; active = true;
        if (stream) api.DestroyOnlineStream(stream);
        stream = online ? api.CreateOnlineStream(online) : nullptr;
        if (online && !stream) throw std::runtime_error("无法开始语音识别");
        if (vad) api.VoiceActivityDetectorReset(vad);
        emit("started", token, "");
      } else if (op == 'C') { active = false; }
      else if (op == 'P' && active) {
        if (size % 2) throw std::runtime_error("语音帧不完整");
        std::vector<float> pcm(size / 2);
        for (size_t i = 0; i < pcm.size(); ++i) {
          auto n = uint16_t(static_cast<unsigned char>(data[i*2])) | uint16_t(static_cast<unsigned char>(data[i*2+1])) << 8;
          pcm[i] = static_cast<int16_t>(n) / 32768.f;
        }
        samples += pcm.size();
        if (online) {
          api.OnlineStreamAcceptWaveform(stream, 16000, pcm.data(), pcm.size()); decodeOnline();
          auto text = onlineText(); partial(joined(settled, text));
          if (api.OnlineStreamIsEndpoint(online, stream)) { settled = joined(settled, text); api.OnlineStreamReset(online, stream); }
        } else { api.VoiceActivityDetectorAcceptWaveform(vad, pcm.data(), pcm.size()); segments(); }
        if (samples >= 16000ULL * 600) {
          std::cout << "{\"event\":\"state\",\"token\":" << quoted(token) << ",\"state\":2,\"limit\":true}" << std::endl;
          finish(false);
        }
      } else if ((op == 'T' || op == 'F') && active) finish(op == 'F');
    }
  }
  void finish(bool immediate) {
    if (!immediate || shown.empty()) {
      if (online) {
        std::vector<float> tail(16000, 0.f);
        api.OnlineStreamAcceptWaveform(stream, 16000, tail.data(), tail.size());
        api.OnlineStreamInputFinished(stream); decodeOnline(); shown = joined(settled, onlineText());
      } else { api.VoiceActivityDetectorFlush(vad); segments(); shown = settled; }
    }
    active = false; emit("final", token, shown);
  }
};
int main(int argc, char **argv) {
  try {
    if (argc != 5 || (std::string(argv[1]) != "--model" && std::string(argv[1]) != "--check"))
      throw std::runtime_error("wetypex-sherpa-voice --model|--check MODEL_DIR RUNTIME_DIR xasr|qwen3|firered|funasr");
    Decoder decoder(argv[2], argv[3], argv[4]); emit("ready", "", "");
    if (std::string(argv[1]) == "--model") decoder.serve();
  } catch (const std::exception &error) {
    emit("error", "", error.what()); return 1;
  }
}
