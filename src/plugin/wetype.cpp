#include "../common/json.hpp"
#include "config.hpp"
#include <algorithm>
#include <cerrno>
#include <clipboard_public.h>
#include <csignal>
#include <fcitx-utils/capabilityflags.h>
#include <fcitx-utils/event.h>
#include <fcitx-utils/key.h>
#include <fcitx-utils/keysym.h>
#include <fcitx-utils/misc.h>
#include <fcitx-utils/utf8.h>
#include <fcitx/action.h>
#include <fcitx/addonfactory.h>
#include <fcitx/addonmanager.h>
#include <fcitx/candidatelist.h>
#include <fcitx/event.h>
#include <fcitx/globalconfig.h>
#include <fcitx/inputcontext.h>
#include <fcitx/inputcontextmanager.h>
#include <fcitx/inputcontextproperty.h>
#include <fcitx/inputmethodengine.h>
#include <fcitx/inputpanel.h>
#include <fcitx/instance.h>
#include <fcitx/statusarea.h>
#include <fcitx/text.h>
#include <fcitx/userinterfacemanager.h>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <libime/pinyin/pinyinencoder.h>
#include <map>
#include <queue>
#include <spawn.h>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>
extern char **environ;
namespace {
using namespace fcitx;
struct PunctuationEntry {
  char ascii;
  const char *full;
  const char *englishFull;
  const char *normal;
  const char *chineseHalf;
  const char *englishHalf;
  uint8_t pairType;
};
// Recovered from macOS 2.2.3.657 Contents/Resources/punctuation.json.
static constexpr PunctuationEntry kPunctuation[] = {
    {' ', "　", "　", " ", " ", " ", 0},
    {'!', "！", "！", "！", "!", "!", 0},
    {'\"', "“”", "＂＂", "“”", "“”", "\"\"", 2},
    {'#', "＃", "＃", "#", "#", "#", 0},
    {'$', "＄", "＄", "¥", "¥", "$", 0},
    {'%', "％", "％", "%", "%", "%", 0},
    {'&', "＆", "＆", "&", "&", "&", 0},
    {'\'', "‘’", "＇＇", "‘’", "‘’", "''", 2},
    {'(', "（）", "（）", "（）", "()", "()", 1},
    {')', "）", "）", "）", ")", ")", 0},
    {'*', "＊", "＊", "*", "*", "*", 0},
    {'+', "＋", "＋", "+", "+", "+", 0},
    {',', "，", "，", "，", ",", ",", 0},
    {'-', "－", "－", "-", "-", "-", 0},
    {'.', "。", "．", "。", ".", ".", 0},
    {'/', "／", "／", "/", "/", "/", 0},
    {':', "：", "：", "：", ":", ":", 0},
    {';', "；", "；", "；", ";", ";", 0},
    {'<', "《》", "＜＞", "《》", "《》", "<>", 1},
    {'=', "＝", "＝", "=", "=", "=", 0},
    {'>', "》", "＞", "》", "》", ">", 0},
    {'?', "？", "？", "？", "?", "?", 0},
    {'@', "＠", "＠", "@", "@", "@", 0},
    {'[', "［］", "［］", "【】", "[]", "[]", 1},
    {'\\', "、", "＼", "、", "、", "\\", 0},
    {']', "］", "］", "】", "]", "]", 0},
    {'^', "＾", "＾", "……", "……", "^", 0},
    {'_', "＿", "＿", "——", "——", "_", 0},
    {'`', "·", "｀", "·", "·", "`", 0},
    {'{', "「」", "｛｝", "「」", "「」", "{}", 1},
    {'|', "｜", "｜", "｜", "|", "|", 0},
    {'}', "」", "｝", "」", "」", "}", 0},
    {'~', "～", "～", "～", "~", "~", 0},
};
static const PunctuationEntry *punctuationEntry(char ascii) {
  auto it =
      std::find_if(std::begin(kPunctuation), std::end(kPunctuation),
                   [ascii](const auto &entry) { return entry.ascii == ascii; });
  return it == std::end(kPunctuation) ? nullptr : it;
}
static size_t firstUtf8Size(std::string_view text) {
  if (text.empty())
    return 0;
  const auto byte = static_cast<unsigned char>(text.front());
  return byte < 0x80 ? 1 : byte < 0xe0 ? 2 : byte < 0xf0 ? 3 : 4;
}
static std::pair<std::string, std::string> splitPair(const std::string &text) {
  const auto first = std::min(firstUtf8Size(text), text.size());
  return {text.substr(0, first), text.substr(first)};
}
static std::string formatPinyinPreedit(const std::string &raw) {
  auto begin = std::find_if(raw.begin(), raw.end(), [](unsigned char c) {
    return (c >= 'a' && c <= 'z') || c == '\'';
  });
  if (begin == raw.end())
    return raw;
  const size_t prefixSize = begin - raw.begin();
  std::string input(begin, raw.end());
  if (input.size() < 2 || input.find('\'') != std::string::npos ||
      !std::all_of(input.begin(), input.end(),
                   [](unsigned char c) { return c >= 'a' && c <= 'z'; }))
    return raw;
  auto graph = libime::PinyinEncoder::parseUserPinyin(
      input, libime::PinyinFuzzyFlag::None);
  using Node = libime::SegmentGraphNode;
  std::queue<const Node *> pending;
  std::map<const Node *, const Node *> previous;
  pending.push(&graph.start());
  previous[&graph.start()] = nullptr;
  while (!pending.empty() && !previous.count(&graph.end())) {
    auto *node = pending.front();
    pending.pop();
    for (const auto &next : node->nexts())
      if (!previous.count(&next)) {
        previous[&next] = node;
        pending.push(&next);
      }
  }
  if (!previous.count(&graph.end()))
    return raw;
  std::vector<size_t> boundaries;
  for (auto *node = &graph.end(); node != &graph.start(); node = previous[node])
    boundaries.push_back(node->index());
  std::reverse(boundaries.begin(), boundaries.end());
  if (boundaries.size() < 2)
    return raw;
  std::vector<size_t> displayBoundaries;
  size_t start = 0;
  for (size_t i = 0; i + 1 < boundaries.size(); ++i) {
    auto segment = std::string_view(input).substr(start, boundaries[i] - start);
    if (segment.size() == 1 && segment != "a" && segment != "e" &&
        segment != "o")
      break; // Keep the unfinished suffix together, e.g. zhong'we.
    displayBoundaries.push_back(boundaries[i]);
    start = boundaries[i];
  }
  if (displayBoundaries.empty())
    return raw; // Preserve smart-spelling input instead of inventing breaks.
  displayBoundaries.push_back(input.size());
  std::string result = raw.substr(0, prefixSize);
  start = 0;
  for (size_t i = 0; i < displayBoundaries.size(); ++i) {
    if (i)
      result += '\'';
    result.append(input, start, displayBoundaries[i] - start);
    start = displayBoundaries[i];
  }
  return result;
}
static size_t displayCursorForRaw(const std::string &raw,
                                  const std::string &displayed,
                                  size_t rawCursor) {
  rawCursor = std::min(rawCursor, raw.size());
  size_t source = 0, display = 0;
  while (source < rawCursor && display < displayed.size()) {
    if (displayed[display] == '\'' &&
        (source >= raw.size() || raw[source] != '\'')) {
      ++display;
      continue;
    }
    ++source;
    ++display;
  }
  while (display < displayed.size() && displayed[display] == '\'' &&
         (source >= raw.size() || raw[source] != '\''))
    ++display;
  return display;
}
class WeType;
struct State : InputContextProperty {
  uint64_t id, epoch = 1, seq = 0, applied = 0, revision = 0;
  uint64_t cloudPollUntil = 0, lastCloudPoll = 0;
  int candidatePage = 0, candidateCursor = 0;
  std::string preedit;
  size_t cursor = 0;
  std::string displayPreedit;
  size_t displayCursor = 0, editableBegin = 0;
  std::vector<size_t> cursorStops;
  char pendingPairKey = 0;
  std::string pendingPairRight;
  uint8_t symbolAutoState = 0;
  bool doubleQuoteLeft = true, singleQuoteLeft = true;
  bool english = false, fullWidth = false, traditional = false,
       englishPunctuation = false, vMode = false;
  KeySym modifierCandidate = FcitxKey_None;
  TrackableObjectReference<InputContext> ic;
  State(uint64_t n, InputContext &context) : id(n), ic(context.watch()) {}
};
static void clearDisplayState(State *state) {
  state->displayPreedit.clear();
  state->displayCursor = 0;
  state->editableBegin = 0;
  state->cursorStops.clear();
}
class Word : public CandidateWord {
  WeType *engine_;
  unsigned index_;
  uint64_t revision_;

public:
  Word(WeType *e, std::string text, unsigned index, uint64_t revision)
      : CandidateWord(Text(text)), engine_(e), index_(index),
        revision_(revision) {}
  void select(InputContext *ic) const override;
};
class WeType : public InputMethodEngine {
  Instance *instance_;
  uint64_t nextId_ = 0;
  FactoryFor<State> factory_;
  SimpleAction settingsAction_;
  Connection settingsConnection_;
  std::map<uint64_t, TrackableObjectReference<InputContext>> contexts_;
  pid_t child_ = -1;
  int read_ = -1, write_ = -1;
  std::unique_ptr<EventSourceIO> reader_, writer_;
  std::unique_ptr<EventSourceTime> timer_;
  bool ready_ = false, failed_ = false;
  uint64_t activity_ = 0;
  uint64_t restartAt_ = 0, restartBackoff_ = 250000;
  bool restartNeedsOpen_ = false;
  uint64_t syncTick_ = 0, updateTick_ = 0;
  uint64_t lastVoiceVersion_ = 0;
  uint64_t lastVModeVersion_ = 0;
  std::string inbound_, outbound_;
  bool voiceRecording_ = false, voiceHold_ = false;
  pid_t syncChild_ = -1;
  void startSync() {
    if (syncChild_ > 0 && waitpid(syncChild_, nullptr, WNOHANG) == 0)
      return;
    syncChild_ = -1;
    const char *override = getenv("WETYPE_SYNCD_LAUNCHER");
    std::string launcher = override && *override ? override : WETYPE_SYNCD;
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null",
                                     O_RDONLY, 0);
    posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, "/dev/null",
                                     O_WRONLY, 0);
    posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null",
                                     O_WRONLY, 0);
    char *argv[] = {launcher.data(), nullptr};
    if (posix_spawn(&syncChild_, launcher.c_str(), &actions, nullptr, argv,
                    environ))
      syncChild_ = -1;
    posix_spawn_file_actions_destroy(&actions);
  }
  void stopSync() {
    if (syncChild_ <= 0)
      return;
    kill(syncChild_, SIGTERM);
    while (waitpid(syncChild_, nullptr, 0) < 0 && errno == EINTR) {
    }
    syncChild_ = -1;
  }
  int pageSize_ = 5;
  int keyboard_ = 0;
  bool vertical_ = false, chinesePunctuation_ = true, slashPunctuation_ = true,
       symbolAutoChange_ = true, clipboardEnabled_ = false,
       networkEnabled_ = true;
  wetype_config::WeTypeConfig config_;
  std::filesystem::path stateDirectory() const {
    const char *state = getenv("WETYPE_STATE_DIR"),
               *data = getenv("XDG_DATA_HOME"), *home = getenv("HOME");
    if (state)
      return state;
    auto directory =
        data ? std::filesystem::path(data)
             : std::filesystem::path(home ? home : "") / ".local/share";
    return directory / "fcitx5-wetypex/state";
  }
  static char closingKey(char opening) {
    switch (opening) {
    case '(':
      return ')';
    case '[':
      return ']';
    case '{':
      return '}';
    case '<':
      return '>';
    case '\"':
    case '\'':
      return opening;
    default:
      return 0;
    }
  }
  static bool nextSurroundingTextIs(InputContext *ic,
                                    const std::string &expected) {
    const auto &surrounding = ic->surroundingText();
    if (!surrounding.isValid())
      return false;
    const auto &text = surrounding.text();
    auto byte = utf8::ncharByteLength(text.begin(), surrounding.cursor());
    return byte >= 0 && size_t(byte) <= text.size() &&
           text.compare(size_t(byte), expected.size(), expected) == 0;
  }
  std::string mappedSymbol(State *s, char ascii, bool asciiMode) const {
    const auto *entry = punctuationEntry(ascii);
    if (!entry)
      return std::string(1, ascii);
    const bool useEnglish =
        asciiMode || s->englishPunctuation || !chinesePunctuation_;
    if (ascii == '/' && slashPunctuation_ && !useEnglish && !s->fullWidth)
      return "、";
    if (s->fullWidth)
      return useEnglish ? entry->englishFull : entry->full;
    if (useEnglish)
      return entry->englishHalf;
    return entry->normal;
  }
  static void commitStringAtCursor(InputContext *ic, const std::string &text,
                                   size_t cursor) {
    const auto length = utf8::length(text);
    cursor = std::min(cursor, length);
    if (ic->capabilityFlags().test(CapabilityFlag::CommitStringWithCursor)) {
      ic->commitStringWithCursor(text, cursor);
      return;
    }

    // XIM and some Wayland text-input clients cannot express a cursor inside
    // committed text. Commit the complete pair first, then reproduce the
    // original desktop behavior with ordinary cursor movement.
    ic->commitString(text);
    for (size_t i = cursor; i < length; ++i)
      ic->forwardKey(Key(FcitxKey_Left));
  }
  bool commitSymbol(InputContext *ic, State *s, char ascii, bool asciiMode) {
    const auto *entry = punctuationEntry(ascii);
    if (!entry)
      return false;
    if (s->pendingPairKey == ascii && !s->pendingPairRight.empty() &&
        nextSurroundingTextIs(ic, s->pendingPairRight)) {
      ic->forwardKey(Key(FcitxKey_Right));
      s->pendingPairKey = 0;
      s->pendingPairRight.clear();
      s->symbolAutoState = 0;
      return true;
    }
    std::string mapped = mappedSymbol(s, ascii, asciiMode);
    auto [left, right] = splitPair(mapped);
    if (entry->pairType && !right.empty()) {
      if (*config_.input->symbolAutoPair) {
        commitStringAtCursor(ic, mapped, 1);
        s->pendingPairKey = closingKey(ascii);
        s->pendingPairRight = right;
      } else if (entry->pairType == 2) {
        bool &leftNext =
            ascii == '\"' ? s->doubleQuoteLeft : s->singleQuoteLeft;
        ic->commitString(leftNext ? left : right);
        leftNext = !leftNext;
      } else {
        ic->commitString(left);
      }
    } else {
      ic->commitString(mapped);
    }
    const bool autoChangeActive = symbolAutoChange_ && !asciiMode &&
                                  !s->englishPunctuation &&
                                  chinesePunctuation_ && !s->fullWidth;
    if (autoChangeActive && s->symbolAutoState == 3 && ascii == ':')
      s->symbolAutoState = 1;
    else if (autoChangeActive && s->symbolAutoState == 3 && ascii == ',')
      s->symbolAutoState = 2;
    else
      s->symbolAutoState = 0;
    return true;
  }
  void reloadSettings() {
    const char *config = getenv("XDG_CONFIG_HOME"), *home = getenv("HOME");
    std::string path =
        config ? config : std::string(home ? home : "") + "/.config";
    const auto newPath = path + "/fcitx5/wetypex.json";
    std::ifstream f(newPath);
    std::string bytes(16385, '\0');
    f.read(bytes.data(), bytes.size());
    bytes.resize(f.gcount());
    auto j = bytes.size() > 16384 ? wire::Json{} : wire::parse(bytes);
    auto boolSetting = [&](const char *name, bool fallback) {
      auto *value = wire::get(j.get(), name);
      return value ? bool(json_object_get_boolean(value)) : fallback;
    };
    pageSize_ = std::clamp(int(wire::number(j.get(), "page_size", 5)), 3, 9);
    keyboard_ = wire::number(j.get(), "keyboard", 0) == 5 ? 5 : 0;
    vertical_ = json_object_get_boolean(wire::get(j.get(), "vertical"));
    auto *punctuation = wire::get(j.get(), "chinese_punctuation");
    chinesePunctuation_ = !punctuation || json_object_get_boolean(punctuation);
    auto *slash = wire::get(j.get(), "slash_punctuation");
    slashPunctuation_ = !slash || json_object_get_boolean(slash);
    auto *symbolChange = wire::get(j.get(), "symbol_auto_change");
    symbolAutoChange_ = !symbolChange || json_object_get_boolean(symbolChange);
    clipboardEnabled_ =
        json_object_get_boolean(wire::get(j.get(), "clipboard_enabled"));
    networkEnabled_ =
        !json_object_get_boolean(wire::get(j.get(), "standalone"));
    auto *input = config_.input.mutableValue();
    auto mode = wire::str(j.get(), "input_mode");
    input->mode.setValue(mode == "wubi" ? wetype_config::InputMode::Wubi
                         : mode == "double_pinyin"
                             ? wetype_config::InputMode::DoublePinyin
                             : wetype_config::InputMode::Pinyin);
    input->wubi.setValue(static_cast<wetype_config::WubiScheme>(
        std::clamp(int(wire::number(j.get(), "wubi_solution", 0)), 0, 2)));
    input->wubiPinyin.setValue(boolSetting("wubi_pinyin", false));
    input->wubiUniqueCommit.setValue(boolSetting("wubi_unique_commit", false));
    input->wubiNextCommit.setValue(boolSetting("wubi_next_commit", false));
    input->wubiWildcardComment.setValue(
        boolSetting("wubi_wildcard_comment", false));
    input->doublePinyin.setValue(
        static_cast<wetype_config::DoublePinyinScheme>(std::clamp(
            int(wire::number(j.get(), "double_pinyin_scheme", 0)), 0, 6)));
    input->smartInput.setValue(
        !wire::get(j.get(), "smart_input") ||
        json_object_get_boolean(wire::get(j.get(), "smart_input")));
    input->emojiRecommend.setValue(
        !wire::get(j.get(), "emoji_recommend") ||
        json_object_get_boolean(wire::get(j.get(), "emoji_recommend")));
    input->wechatEmoji.setValue(boolSetting("wechat_emoji", true));
    input->normalEmoji.setValue(boolSetting("normal_emoji", true));
    input->kaomoji.setValue(boolSetting("kaomoji", true));
    input->largeEmoji.setValue(boolSetting("large_emoji", true));
    input->symbolEmoji.setValue(boolSetting("symbol_emoji", true));
    input->slashPunctuation.setValue(slashPunctuation_);
    input->symbolAutoChange.setValue(
        !wire::get(j.get(), "symbol_auto_change") ||
        json_object_get_boolean(wire::get(j.get(), "symbol_auto_change")));
    input->symbolAutoPair.setValue(
        !wire::get(j.get(), "symbol_auto_pair") ||
        json_object_get_boolean(wire::get(j.get(), "symbol_auto_pair")));
    input->standalone.setValue(!networkEnabled_);
    input->defaultLanguage.setValue(
        wire::str(j.get(), "default_language") == "english"
            ? wetype_config::DefaultLanguage::English
            : wetype_config::DefaultLanguage::Chinese);
    input->fuzzyNl.setValue(
        json_object_get_boolean(wire::get(j.get(), "fuzzy_nl")));
    input->fuzzyRl.setValue(
        json_object_get_boolean(wire::get(j.get(), "fuzzy_rl")));
    input->fuzzyHf.setValue(
        json_object_get_boolean(wire::get(j.get(), "fuzzy_hf")));
    input->fuzzyGk.setValue(
        json_object_get_boolean(wire::get(j.get(), "fuzzy_gk")));
    input->fuzzyAnAng.setValue(
        json_object_get_boolean(wire::get(j.get(), "fuzzy_an_ang")));
    input->fuzzyIanIang.setValue(
        json_object_get_boolean(wire::get(j.get(), "fuzzy_ian_iang")));
    input->fuzzyUanUang.setValue(
        json_object_get_boolean(wire::get(j.get(), "fuzzy_uan_uang")));
    input->fuzzyCCh.setValue(
        json_object_get_boolean(wire::get(j.get(), "fuzzy_c_ch")));
    input->fuzzySSh.setValue(
        json_object_get_boolean(wire::get(j.get(), "fuzzy_s_sh")));
    input->fuzzyZZh.setValue(
        json_object_get_boolean(wire::get(j.get(), "fuzzy_z_zh")));
    input->fuzzyHuiFei.setValue(
        json_object_get_boolean(wire::get(j.get(), "fuzzy_hui_fei")));
    input->fuzzyEnEng.setValue(
        json_object_get_boolean(wire::get(j.get(), "fuzzy_en_eng")));
    input->fuzzyInIng.setValue(
        json_object_get_boolean(wire::get(j.get(), "fuzzy_in_ing")));
    input->fuzzyOnOng.setValue(
        json_object_get_boolean(wire::get(j.get(), "fuzzy_on_ong")));
    input->fuzzyHuangWang.setValue(
        json_object_get_boolean(wire::get(j.get(), "fuzzy_huang_wang")));
    input->fuzzyUnOng.setValue(
        json_object_get_boolean(wire::get(j.get(), "fuzzy_un_ong")));
    input->fuzzyUnIong.setValue(
        json_object_get_boolean(wire::get(j.get(), "fuzzy_un_iong")));
    input->fuzzyAnAi.setValue(
        json_object_get_boolean(wire::get(j.get(), "fuzzy_an_ai")));
    input->fuzzyEngOng.setValue(
        json_object_get_boolean(wire::get(j.get(), "fuzzy_eng_ong")));
    auto *phrases = config_.phrases.mutableValue();
    phrases->clipboard.setValue(clipboardEnabled_);
    auto *appearance = config_.appearance.mutableValue();
    appearance->pageSize.setValue(pageSize_);
    appearance->candidateSize.setValue(
        std::clamp(int(wire::number(j.get(), "candidate_size", 13)), 10, 18));
    appearance->vertical.setValue(vertical_);
    appearance->theme.setValue(static_cast<wetype_config::ThemeMode>(
        std::clamp(int(wire::number(j.get(), "theme_mode", 0)), 0, 2)));
    auto *devices = config_.devices.mutableValue();
    devices->clipboardSync.setValue(
        json_object_get_boolean(wire::get(j.get(), "device_clipboard_sync")));
    devices->dictionarySync.setValue(
        json_object_get_boolean(wire::get(j.get(), "device_dictionary_sync")));
    devices->phraseSync.setValue(
        json_object_get_boolean(wire::get(j.get(), "device_phrase_sync")));
    auto *voice = config_.voice.mutableValue();
    voice->launchShortcut.setValue(boolSetting("voice_launch_shortcut", true));
    voice->holdShortcut.setValue(boolSetting("voice_hold_shortcut", true));
    voice->smartPolish.setValue(boolSetting("voice_smart_polish", true));
    auto stringSetting = [&](const char *name, const char *fallback) {
      auto value = wire::str(j.get(), name);
      return value.empty() ? std::string(fallback) : value;
    };
    voice->launchKey.setValue(Key::keyListFromString(
        stringSetting("voice_launch_key", "Control+Super+Shift_L")));
    voice->holdKey.setValue(Key::keyListFromString(
        stringSetting("voice_hold_key", "Control+Super_L")));
    voice->microphone.setValue(stringSetting("voice_microphone", "自动检测"));
    const auto voicePunctuation =
        stringSetting("voice_punctuation", "智能标点");
    voice->punctuation.setValue(
        voicePunctuation == "添加完整标点"
            ? wetype_config::VoicePunctuationMode::Full
        : voicePunctuation == "句末不加句号"
            ? wetype_config::VoicePunctuationMode::NoPeriod
        : voicePunctuation == "空格替换标点"
            ? wetype_config::VoicePunctuationMode::Spaces
            : wetype_config::VoicePunctuationMode::Smart);
    auto *shortcuts = config_.shortcuts.mutableValue();
    shortcuts->shiftSwitch.setValue(boolSetting("shift_switch", true));
    shortcuts->ctrlSwitch.setValue(boolSetting("ctrl_switch", false));
    shortcuts->aiAssistant.setValue(boolSetting("ai_assistant", true));
    shortcuts->vMode.setValue(boolSetting("v_mode", true));
    shortcuts->halfFull.setValue(boolSetting("half_full_switch", false));
    shortcuts->punctuationSwitch.setValue(
        boolSetting("punctuation_switch", true));
    shortcuts->traditionalSwitch.setValue(
        boolSetting("traditional_switch", false));
    shortcuts->pageMinusEqual.setValue(boolSetting("page_minus_equal", true));
    shortcuts->pageBrackets.setValue(boolSetting("page_brackets", true));
    shortcuts->pageCommaPeriod.setValue(
        boolSetting("page_comma_period", false));
    shortcuts->pageShiftTab.setValue(boolSetting("page_shift_tab", false));
    shortcuts->selectSemicolonQuote.setValue(
        boolSetting("select_semicolon_quote", false));
    shortcuts->selectCtrl.setValue(boolSetting("select_ctrl", false));
    auto loadKeys = [&](auto &option, const char *name) {
      auto value = wire::str(j.get(), name);
      if (!value.empty())
        option.setValue(Key::keyListFromString(value));
    };
    loadKeys(shortcuts->languageSwitchKeys, "language_switch_keys");
    loadKeys(shortcuts->aiAssistantKeys, "ai_assistant_keys");
    loadKeys(shortcuts->vModeKeys, "v_mode_keys");
    loadKeys(shortcuts->halfFullKeys, "half_full_keys");
    loadKeys(shortcuts->punctuationSwitchKeys, "punctuation_switch_keys");
    loadKeys(shortcuts->traditionalSwitchKeys, "traditional_switch_keys");
    loadKeys(shortcuts->previousPageKeys, "previous_page_keys");
    loadKeys(shortcuts->nextPageKeys, "next_page_keys");
    loadKeys(shortcuts->secondCandidateKeys, "second_candidate_keys");
    loadKeys(shortcuts->thirdCandidateKeys, "third_candidate_keys");
    config_.update.mutableValue()->autoUpdate.setValue(
        boolSetting("auto_update", true));
  }
  void saveNativeConfig() {
    const char *configHome = getenv("XDG_CONFIG_HOME"), *home = getenv("HOME");
    std::filesystem::path directory =
        configHome ? configHome
                   : std::filesystem::path(home ? home : "") / ".config";
    directory /= "fcitx5";
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    if (error)
      return;
    auto path = directory / "wetypex.json";
    std::ifstream oldFile(path);
    std::string oldBytes((std::istreambuf_iterator<char>(oldFile)), {});
    auto json = wire::parse(oldBytes);
    if (!json || !json_object_is_type(json.get(), json_type_object))
      json = wire::object();
    const auto &input = *config_.input;
    auto mode = *input.mode == wetype_config::InputMode::Wubi ? "wubi"
                : *input.mode == wetype_config::InputMode::DoublePinyin
                    ? "double_pinyin"
                    : "pinyin";
    wire::put(json.get(), "input_mode", std::string(mode));
    wire::put(json.get(), "keyboard",
              int64_t(*input.mode == wetype_config::InputMode::Wubi ? 5 : 0));
    wire::put(json.get(), "wubi_solution", int64_t(*input.wubi));
    wire::put(json.get(), "wubi_pinyin", bool(*input.wubiPinyin));
    wire::put(json.get(), "wubi_unique_commit", bool(*input.wubiUniqueCommit));
    wire::put(json.get(), "wubi_next_commit", bool(*input.wubiNextCommit));
    wire::put(json.get(), "wubi_wildcard_comment",
              bool(*input.wubiWildcardComment));
    wire::put(json.get(), "double_pinyin_scheme", int64_t(*input.doublePinyin));
    wire::put(json.get(), "smart_input", bool(*input.smartInput));
    wire::put(json.get(), "emoji_recommend", bool(*input.emojiRecommend));
    wire::put(json.get(), "wechat_emoji", bool(*input.wechatEmoji));
    wire::put(json.get(), "normal_emoji", bool(*input.normalEmoji));
    wire::put(json.get(), "kaomoji", bool(*input.kaomoji));
    wire::put(json.get(), "large_emoji", bool(*input.largeEmoji));
    wire::put(json.get(), "symbol_emoji", bool(*input.symbolEmoji));
    wire::put(json.get(), "slash_punctuation", bool(*input.slashPunctuation));
    wire::put(json.get(), "symbol_auto_change", bool(*input.symbolAutoChange));
    wire::put(json.get(), "symbol_auto_pair", bool(*input.symbolAutoPair));
    wire::put(json.get(), "standalone", bool(*input.standalone));
    wire::put(json.get(), "default_language",
              std::string(*input.defaultLanguage ==
                                  wetype_config::DefaultLanguage::English
                              ? "english"
                              : "chinese"));
    wire::put(json.get(), "fuzzy_nl", bool(*input.fuzzyNl));
    wire::put(json.get(), "fuzzy_rl", bool(*input.fuzzyRl));
    wire::put(json.get(), "fuzzy_hf", bool(*input.fuzzyHf));
    wire::put(json.get(), "fuzzy_gk", bool(*input.fuzzyGk));
    wire::put(json.get(), "fuzzy_an_ang", bool(*input.fuzzyAnAng));
    wire::put(json.get(), "fuzzy_ian_iang", bool(*input.fuzzyIanIang));
    wire::put(json.get(), "fuzzy_uan_uang", bool(*input.fuzzyUanUang));
    wire::put(json.get(), "fuzzy_c_ch", bool(*input.fuzzyCCh));
    wire::put(json.get(), "fuzzy_s_sh", bool(*input.fuzzySSh));
    wire::put(json.get(), "fuzzy_z_zh", bool(*input.fuzzyZZh));
    wire::put(json.get(), "fuzzy_hui_fei", bool(*input.fuzzyHuiFei));
    wire::put(json.get(), "fuzzy_en_eng", bool(*input.fuzzyEnEng));
    wire::put(json.get(), "fuzzy_in_ing", bool(*input.fuzzyInIng));
    wire::put(json.get(), "fuzzy_on_ong", bool(*input.fuzzyOnOng));
    wire::put(json.get(), "fuzzy_huang_wang", bool(*input.fuzzyHuangWang));
    wire::put(json.get(), "fuzzy_un_ong", bool(*input.fuzzyUnOng));
    wire::put(json.get(), "fuzzy_un_iong", bool(*input.fuzzyUnIong));
    wire::put(json.get(), "fuzzy_an_ai", bool(*input.fuzzyAnAi));
    wire::put(json.get(), "fuzzy_eng_ong", bool(*input.fuzzyEngOng));
    const auto &appearance = *config_.appearance;
    wire::put(json.get(), "page_size", int64_t(*appearance.pageSize));
    wire::put(json.get(), "candidate_size", int64_t(*appearance.candidateSize));
    wire::put(json.get(), "vertical", bool(*appearance.vertical));
    wire::put(json.get(), "theme_mode", int64_t(*appearance.theme));
    wire::put(json.get(), "clipboard_enabled",
              bool(config_.phrases->clipboard.value()));
    const auto &devices = *config_.devices;
    wire::put(json.get(), "device_clipboard_sync",
              bool(*devices.clipboardSync));
    wire::put(json.get(), "device_dictionary_sync",
              bool(*devices.dictionarySync));
    wire::put(json.get(), "device_phrase_sync", bool(*devices.phraseSync));
    const auto &voice = *config_.voice;
    wire::put(json.get(), "voice_launch_shortcut", bool(*voice.launchShortcut));
    wire::put(json.get(), "voice_hold_shortcut", bool(*voice.holdShortcut));
    wire::put(json.get(), "voice_smart_polish", bool(*voice.smartPolish));
    wire::put(json.get(), "voice_launch_key",
              Key::keyListToString(*voice.launchKey));
    wire::put(json.get(), "voice_hold_key",
              Key::keyListToString(*voice.holdKey));
    wire::put(json.get(), "voice_microphone", *voice.microphone);
    const char *punctuation =
        *voice.punctuation == wetype_config::VoicePunctuationMode::Full
            ? "添加完整标点"
        : *voice.punctuation == wetype_config::VoicePunctuationMode::NoPeriod
            ? "句末不加句号"
        : *voice.punctuation == wetype_config::VoicePunctuationMode::Spaces
            ? "空格替换标点"
            : "智能标点";
    wire::put(json.get(), "voice_punctuation", std::string(punctuation));
    const auto &shortcuts = *config_.shortcuts;
    wire::put(json.get(), "shift_switch", bool(*shortcuts.shiftSwitch));
    wire::put(json.get(), "ctrl_switch", bool(*shortcuts.ctrlSwitch));
    wire::put(json.get(), "ai_assistant", bool(*shortcuts.aiAssistant));
    wire::put(json.get(), "v_mode", bool(*shortcuts.vMode));
    wire::put(json.get(), "half_full_switch", bool(*shortcuts.halfFull));
    wire::put(json.get(), "punctuation_switch",
              bool(*shortcuts.punctuationSwitch));
    wire::put(json.get(), "traditional_switch",
              bool(*shortcuts.traditionalSwitch));
    wire::put(json.get(), "page_minus_equal", bool(*shortcuts.pageMinusEqual));
    wire::put(json.get(), "page_brackets", bool(*shortcuts.pageBrackets));
    wire::put(json.get(), "page_comma_period",
              bool(*shortcuts.pageCommaPeriod));
    wire::put(json.get(), "page_shift_tab", bool(*shortcuts.pageShiftTab));
    wire::put(json.get(), "select_semicolon_quote",
              bool(*shortcuts.selectSemicolonQuote));
    wire::put(json.get(), "select_ctrl", bool(*shortcuts.selectCtrl));
    auto saveKeys = [&](const char *name, const auto &option) {
      wire::put(json.get(), name, Key::keyListToString(*option));
    };
    saveKeys("language_switch_keys", shortcuts.languageSwitchKeys);
    saveKeys("ai_assistant_keys", shortcuts.aiAssistantKeys);
    saveKeys("v_mode_keys", shortcuts.vModeKeys);
    saveKeys("half_full_keys", shortcuts.halfFullKeys);
    saveKeys("punctuation_switch_keys", shortcuts.punctuationSwitchKeys);
    saveKeys("traditional_switch_keys", shortcuts.traditionalSwitchKeys);
    saveKeys("previous_page_keys", shortcuts.previousPageKeys);
    saveKeys("next_page_keys", shortcuts.nextPageKeys);
    saveKeys("second_candidate_keys", shortcuts.secondCandidateKeys);
    saveKeys("third_candidate_keys", shortcuts.thirdCandidateKeys);
    wire::put(json.get(), "auto_update", bool(*config_.update->autoUpdate));
    auto temporary = path;
    temporary += ".tmp";
    std::ofstream output(temporary, std::ios::trunc);
    output << wire::dump(json.get()) << '\n';
    output.close();
    chmod(temporary.c_str(), 0600);
    std::filesystem::rename(temporary, path, error);
    reloadSettings();
  }
  void applyAppearance() {
    const char *configHome = getenv("XDG_CONFIG_HOME"), *home = getenv("HOME");
    std::filesystem::path path =
        configHome ? configHome
                   : std::filesystem::path(home ? home : "") / ".config";
    path /= "fcitx5/conf/classicui.conf";
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    if (error)
      return;
    std::ifstream input(path);
    std::vector<std::string> lines;
    std::string line;
    while (std::getline(input, line))
      lines.push_back(line);
    const auto &appearance = *config_.appearance;
    std::map<std::string, std::string> values = {
        {"Theme", *appearance.theme == wetype_config::ThemeMode::Dark
                      ? "wetypex-dark"
                      : "wetypex-light"},
        {"DarkTheme", "wetypex-dark"},
        {"UseDarkTheme", *appearance.theme == wetype_config::ThemeMode::System
                             ? "True"
                             : "False"},
        {"UseAccentColor", "False"},
        {"PreferTextIcon", "False"},
        {"Font",
         "Noto Sans CJK SC " + std::to_string(*appearance.candidateSize)}};
    for (auto &[key, value] : values) {
      bool found = false;
      for (auto &existing : lines)
        if (existing.starts_with(key + "=")) {
          existing = key + "=" + value;
          found = true;
          break;
        }
      if (!found)
        lines.push_back(key + "=" + value);
    }
    auto temporary = path;
    temporary += ".tmp";
    std::ofstream output(temporary, std::ios::trunc);
    for (auto &entry : lines)
      output << entry << '\n';
    output.close();
    chmod(temporary.c_str(), 0600);
    std::filesystem::rename(temporary, path, error);
    if (!error)
      instance_->reloadAddonConfig("classicui");
  }
  void applyDeviceFunctions() {
    std::ifstream input(stateDirectory() / "sync-state.json");
    std::string bytes((std::istreambuf_iterator<char>(input)), {});
    auto state = wire::parse(bytes);
    auto group = wire::number(state.get(), "group_id");
    if (group <= 0)
      return;
    const auto &devices = *config_.devices;
    int mask = (*devices.clipboardSync ? 1 : 0) |
               (*devices.phraseSync ? 2 : 0) |
               (*devices.dictionarySync ? 4 : 0);
    startProcess({WETYPE_ACCOUNT_TOOL, "set-functions", std::to_string(group),
                  std::to_string(mask)});
  }
  void recordClipboard(InputContext *ic) {
    auto *addon = instance_->addonManager().addon("clipboard", true);
    if (!addon)
      return;
    auto value = addon->call<IClipboard::clipboard>(ic);
    if (value.empty() || value.size() > 16384)
      return;
    auto directory = stateDirectory();
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    if (error)
      return;
    chmod(directory.c_str(), 0700);
    if (clipboardEnabled_) {
      auto path = directory / "clipboard-history.json";
      std::vector<std::string> entries{value};
      std::ifstream input(path);
      std::string bytes((std::istreambuf_iterator<char>(input)), {});
      if (bytes.size() <= 1048576) {
        auto old = wire::parse(bytes);
        if (old && json_object_is_type(old.get(), json_type_array))
          for (size_t i = 0;
               i < json_object_array_length(old.get()) && entries.size() < 20;
               ++i) {
            auto *item = json_object_array_get_idx(old.get(), i);
            if (!json_object_is_type(item, json_type_string))
              continue;
            std::string text = json_object_get_string(item);
            if (text.size() <= 16384 &&
                std::find(entries.begin(), entries.end(), text) ==
                    entries.end())
              entries.push_back(std::move(text));
          }
      }
      auto array = wire::Json(json_object_new_array());
      for (const auto &entry : entries)
        json_object_array_add(array.get(), json_object_new_string_len(
                                               entry.data(), entry.size()));
      auto temporary = path;
      temporary += ".tmp";
      std::ofstream output(temporary, std::ios::trunc);
      output << wire::dump(array.get()) << '\n';
      output.close();
      chmod(temporary.c_str(), 0600);
      std::filesystem::rename(temporary, path, error);
    }
  }
  void startVoice(InputContext *ic, bool hold) {
    if (voiceRecording_)
      return;
    auto *s = state(ic);
    if (!s->preedit.empty()) {
      send(ic, "reset");
      s->preedit.clear();
      s->cursor = 0;
      clearDisplayState(s);
      ic->inputPanel().reset();
      panel(ic, s);
    }
    voiceRecording_ = true;
    voiceHold_ = hold;
    startProcess({WETYPE_VOICE, "start"});
  }
  void stopVoice() {
    if (!voiceRecording_)
      return;
    voiceRecording_ = false;
    voiceHold_ = false;
    startProcess({WETYPE_VOICE, "stop"});
  }
  void receiveVoice() {
    std::ifstream input(stateDirectory() / "voice-inbox.json");
    std::string bytes((std::istreambuf_iterator<char>(input)), {});
    if (bytes.empty() || bytes.size() > 65536)
      return;
    auto message = wire::parse(bytes);
    auto version = uint64_t(wire::number(message.get(), "version"));
    auto text = wire::str(message.get(), "text");
    if (!version || version <= lastVoiceVersion_ || text.empty())
      return;
    lastVoiceVersion_ = version;
    for (auto &[id, ref] : contexts_)
      if (auto *ic = ref.get(); ic && ic->hasFocus()) {
        ic->commitString(text);
        break;
      }
  }
  void receiveVModeAction() {
    auto path = stateDirectory() / "vmode-action.json";
    std::ifstream input(path);
    std::string bytes((std::istreambuf_iterator<char>(input)), {});
    if (bytes.empty() || bytes.size() > 65536)
      return;
    auto message = wire::parse(bytes);
    const auto version = uint64_t(wire::number(message.get(), "version"));
    const auto session = uint64_t(wire::number(message.get(), "session"));
    if (!version || version <= lastVModeVersion_)
      return;
    lastVModeVersion_ = version;
    std::error_code error;
    std::filesystem::remove(path, error);
    auto it = contexts_.find(session);
    if (it == contexts_.end())
      return;
    auto *ic = it->second.get();
    if (!ic || !ic->hasFocus())
      return;
    const auto action = wire::str(message.get(), "action");
    if (action != "commit")
      return;
    const auto text = wire::str(message.get(), "text");
    if (text.empty() || text.size() > 65536)
      return;
    auto *s = state(ic);
    send(ic, "reset");
    s->vMode = false;
    s->preedit.clear();
    s->cursor = 0;
    clearDisplayState(s);
    ic->inputPanel().reset();
    ic->commitString(text);
    panel(ic, s);
  }
  State *state(InputContext *ic) {
    auto *s = ic->propertyFor(&factory_);
    contexts_[s->id] = ic->watch();
    return s;
  }
  void panel(InputContext *ic, State *s) {
    ic->inputPanel().setAuxUp(Text());
    const std::string displayed =
        !s->displayPreedit.empty() ? s->displayPreedit
        : !s->vMode && *config_.input->mode == wetype_config::InputMode::Pinyin
            ? formatPinyinPreedit(s->preedit)
            : s->preedit;
    Text text(displayed);
    text.setCursor(!s->displayPreedit.empty()
                       ? std::min(s->displayCursor, displayed.size())
                       : displayCursorForRaw(s->preedit, displayed, s->cursor));
    ic->inputPanel().setClientPreedit(text);
    // Windows 2.1.3.18 shows composition inline in the target application;
    // the candidate bar contains candidates only.
    ic->inputPanel().setPreedit(Text());
    ic->inputPanel().setAuxDown(Text(failed_   ? "WeTypeX 核心正在恢复…"
                                     : !ready_ ? "WeTypeX 核心启动中…"
                                               : ""));
    ic->updatePreedit();
    ic->updateUserInterface(UserInterfaceComponent::InputPanel);
  }
  void stop() {
    reader_.reset();
    writer_.reset();
    if (read_ >= 0)
      close(read_);
    if (write_ >= 0)
      close(write_);
    read_ = write_ = -1;
    if (child_ > 0) {
      kill(child_, SIGKILL);
      while (waitpid(child_, nullptr, 0) < 0 && errno == EINTR) {
      }
      child_ = -1;
    }
    ready_ = false;
    restartAt_ = 0;
    outbound_.clear();
    inbound_.clear();
  }
  void scheduleRestart() {
    restartAt_ = now(CLOCK_MONOTONIC) + restartBackoff_;
    restartBackoff_ = std::min<uint64_t>(restartBackoff_ * 2, 30000000);
  }
  void fail() {
    stop();
    failed_ = true;
    restartNeedsOpen_ = true;
    scheduleRestart();
    for (auto &[id, ref] : contexts_)
      if (auto *ic = ref.get(); ic && ic->hasFocus()) {
        auto *s = state(ic);
        ++s->epoch;
        s->seq = s->applied = 0;
        s->preedit.clear();
        s->cursor = 0;
        clearDisplayState(s);
        ic->inputPanel().setCandidateList(nullptr);
        panel(ic, s);
      }
  }
  bool start() {
    if (child_ > 0)
      return true;
    int to[2], from[2];
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, to) < 0)
      return false;
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, from) < 0) {
      close(to[0]);
      close(to[1]);
      return false;
    }
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, to[0], STDIN_FILENO);
    posix_spawn_file_actions_adddup2(&actions, from[1], STDOUT_FILENO);
    posix_spawn_file_actions_addclose(&actions, to[1]);
    posix_spawn_file_actions_addclose(&actions, from[0]);
    const char *override = getenv("WETYPE_BACKEND_LAUNCHER");
    std::string launcher = override ? override : WETYPE_LAUNCHER;
    char *argv[] = {launcher.data(), nullptr};
    int result = posix_spawn(&child_, launcher.c_str(), &actions, nullptr, argv,
                             environ);
    posix_spawn_file_actions_destroy(&actions);
    close(to[0]);
    close(from[1]);
    if (result) {
      close(to[1]);
      close(from[0]);
      child_ = -1;
      return false;
    }
    write_ = to[1];
    read_ = from[0];
    fcntl(write_, F_SETFL, fcntl(write_, F_GETFL) | O_NONBLOCK);
    fcntl(read_, F_SETFL, fcntl(read_, F_GETFL) | O_NONBLOCK);
    failed_ = false;
    restartAt_ = 0;
    activity_ = now(CLOCK_MONOTONIC);
    reader_ = instance_->eventLoop().addIOEvent(
        read_, IOEventFlag::In, [this](auto *, int fd, auto) {
          char buf[8192];
          for (;;) {
            ssize_t n = read(fd, buf, sizeof(buf));
            if (n > 0) {
              inbound_.append(buf, n);
              if (inbound_.size() > 1048576) {
                fail();
                return true;
              }
            } else if (n == 0) {
              fail();
              return true;
            } else if (errno == EINTR)
              continue;
            else if (errno == EAGAIN)
              break;
            else {
              fail();
              return true;
            }
          }
          for (;;) {
            auto end = inbound_.find('\n');
            if (end == std::string::npos)
              break;
            auto line = inbound_.substr(0, end);
            inbound_.erase(0, end + 1);
            receive(line);
          }
          return true;
        });
    writer_ = instance_->eventLoop().addIOEvent(write_, IOEventFlag::Out,
                                                [this](auto *, int, auto) {
                                                  flush();
                                                  return true;
                                                });
    writer_->setEnabled(false);
    return true;
  }
  void flush() {
    while (!outbound_.empty() && write_ >= 0) {
      ssize_t n =
          ::send(write_, outbound_.data(), outbound_.size(), MSG_NOSIGNAL);
      if (n > 0)
        outbound_.erase(0, n);
      else if (n < 0 && errno == EINTR)
        continue;
      else if (n < 0 && errno == EAGAIN)
        break;
      else {
        fail();
        return;
      }
    }
    if (writer_)
      writer_->setEnabled(!outbound_.empty());
  }
  void send(InputContext *ic, const char *op, const std::string &key = "",
            int index = 0, int64_t revision = -1, int64_t cursor = -1,
            const std::string &mapped = {}, int pairCursor = -1) {
    auto *s = state(ic);
    // Candidate page/cursor state belongs to the current composition.
    // Cloud-result polls refresh that same list and must preserve the user's
    // position; every actual engine operation starts again from the first
    // candidate, as on the native Windows/macOS clients.
    if (std::string_view(op) != "poll")
      s->candidatePage = s->candidateCursor = 0;
    if (child_ <= 0 && !start()) {
      failed_ = true;
      scheduleRestart();
      panel(ic, s);
      return;
    }
    auto json = wire::object();
    wire::put(json.get(), "session", int64_t(s->id));
    wire::put(json.get(), "seq", int64_t(++s->seq));
    wire::put(json.get(), "epoch", int64_t(s->epoch));
    wire::put(json.get(), "op", std::string(op));
    wire::put(json.get(), "key", key);
    wire::put(json.get(), "index", int64_t(index));
    wire::put(json.get(), "revision", revision);
    wire::put(json.get(), "cursor", cursor);
    wire::put(json.get(), "mapped", mapped);
    wire::put(json.get(), "pair_cursor", int64_t(pairCursor));
    wire::put(json.get(), "before", op == std::string("predict") ? key : "");
    wire::put(json.get(), "chinese_punctuation", int64_t(chinesePunctuation_));
    wire::put(json.get(), "keyboard", int64_t(keyboard_));
    const auto &inputConfig = *config_.input;
    static const int doubleSchemes[] = {5, 1, 2, 3, 4, 6, 7};
    wire::put(
        json.get(), "double_scheme",
        int64_t(*inputConfig.mode == wetype_config::InputMode::DoublePinyin
                    ? doubleSchemes[int(*inputConfig.doublePinyin)]
                    : 0));
    wire::put(json.get(), "wubi_solution", int64_t(*inputConfig.wubi));
    wire::put(json.get(), "wubi_pinyin", bool(*inputConfig.wubiPinyin));
    wire::put(json.get(), "wubi_unique_commit",
              bool(*inputConfig.wubiUniqueCommit));
    wire::put(json.get(), "wubi_next_commit",
              bool(*inputConfig.wubiNextCommit));
    wire::put(json.get(), "wubi_wildcard_comment",
              bool(*inputConfig.wubiWildcardComment));
    wire::put(json.get(), "smart_input", bool(*inputConfig.smartInput));
    wire::put(json.get(), "emoji_recommend", bool(*inputConfig.emojiRecommend));
    wire::put(json.get(), "wechat_emoji", bool(*inputConfig.wechatEmoji));
    wire::put(json.get(), "normal_emoji", bool(*inputConfig.normalEmoji));
    wire::put(json.get(), "kaomoji", bool(*inputConfig.kaomoji));
    wire::put(json.get(), "large_emoji", bool(*inputConfig.largeEmoji));
    wire::put(json.get(), "symbol_emoji", bool(*inputConfig.symbolEmoji));
    wire::put(json.get(), "v_mode", bool(*config_.shortcuts->vMode));
    wire::put(json.get(), "fuzzy_nl", bool(*inputConfig.fuzzyNl));
    wire::put(json.get(), "fuzzy_rl", bool(*inputConfig.fuzzyRl));
    wire::put(json.get(), "fuzzy_hf", bool(*inputConfig.fuzzyHf));
    wire::put(json.get(), "fuzzy_gk", bool(*inputConfig.fuzzyGk));
    wire::put(json.get(), "fuzzy_an_ang", bool(*inputConfig.fuzzyAnAng));
    wire::put(json.get(), "fuzzy_ian_iang", bool(*inputConfig.fuzzyIanIang));
    wire::put(json.get(), "fuzzy_uan_uang", bool(*inputConfig.fuzzyUanUang));
    wire::put(json.get(), "fuzzy_c_ch", bool(*inputConfig.fuzzyCCh));
    wire::put(json.get(), "fuzzy_s_sh", bool(*inputConfig.fuzzySSh));
    wire::put(json.get(), "fuzzy_z_zh", bool(*inputConfig.fuzzyZZh));
    wire::put(json.get(), "fuzzy_hui_fei", bool(*inputConfig.fuzzyHuiFei));
    wire::put(json.get(), "fuzzy_en_eng", bool(*inputConfig.fuzzyEnEng));
    wire::put(json.get(), "fuzzy_in_ing", bool(*inputConfig.fuzzyInIng));
    wire::put(json.get(), "fuzzy_on_ong", bool(*inputConfig.fuzzyOnOng));
    wire::put(json.get(), "fuzzy_huang_wang",
              bool(*inputConfig.fuzzyHuangWang));
    wire::put(json.get(), "fuzzy_un_ong", bool(*inputConfig.fuzzyUnOng));
    wire::put(json.get(), "fuzzy_un_iong", bool(*inputConfig.fuzzyUnIong));
    wire::put(json.get(), "fuzzy_an_ai", bool(*inputConfig.fuzzyAnAi));
    wire::put(json.get(), "fuzzy_eng_ong", bool(*inputConfig.fuzzyEngOng));
    wire::put(json.get(), "traditional", int64_t(s->traditional));
    outbound_ += wire::dump(json.get()) + '\n';
    if (outbound_.size() > 262144) {
      fail();
      return;
    }
    activity_ = now(CLOCK_MONOTONIC);
    flush();
  }
  void receive(const std::string &line) {
    auto j = wire::parse(line);
    if (!j)
      return;
    if (wire::str(j.get(), "event") == "ready") {
      ready_ = true;
      failed_ = false;
      restartBackoff_ = 250000;
      activity_ = now(CLOCK_MONOTONIC);
      if (restartNeedsOpen_) {
        restartNeedsOpen_ = false;
        for (auto &[id, ref] : contexts_)
          if (auto *ic = ref.get();
              ic && ic->hasFocus() &&
              !ic->capabilityFlags().test(CapabilityFlag::Password) &&
              !ic->capabilityFlags().test(CapabilityFlag::Sensitive))
            send(ic, "open");
      }
      return;
    }
    auto id = wire::number(j.get(), "session");
    auto it = contexts_.find(id);
    if (it == contexts_.end())
      return;
    auto *ic = it->second.get();
    if (!ic) {
      contexts_.erase(it);
      return;
    }
    auto *s = state(ic);
    auto seq = uint64_t(wire::number(j.get(), "seq"));
    if (seq <= s->applied)
      return;
    s->applied = seq;
    activity_ = now(CLOCK_MONOTONIC);
    if (uint64_t(wire::number(j.get(), "epoch")) != s->epoch || !ic->hasFocus())
      return;
    auto *commits = wire::get(j.get(), "commits");
    if (commits && json_object_is_type(commits, json_type_array))
      for (size_t i = 0; i < json_object_array_length(commits); i++) {
        auto *v = json_object_array_get_idx(commits, i);
        if (json_object_is_type(v, json_type_string))
          ic->commitString(json_object_get_string(v));
      }
    auto cursorCommit = wire::str(j.get(), "cursor_commit");
    auto cursorCommitPosition =
        wire::number(j.get(), "cursor_commit_position", -1);
    if (!cursorCommit.empty() && cursorCommitPosition >= 0 &&
        size_t(cursorCommitPosition) <= utf8::length(cursorCommit))
      commitStringAtCursor(ic, cursorCommit, size_t(cursorCommitPosition));
    if (seq != s->seq)
      return;
    s->preedit = wire::str(j.get(), "preedit");
    s->cursor =
        std::min<size_t>(std::max<int64_t>(0, wire::number(j.get(), "cursor",
                                                           s->preedit.size())),
                         s->preedit.size());
    s->editableBegin = std::min<size_t>(
        std::max<int64_t>(0, wire::number(j.get(), "editable_begin")),
        s->preedit.size());
    s->displayPreedit = wire::str(j.get(), "display_preedit");
    s->displayCursor = std::min<size_t>(
        std::max<int64_t>(0, wire::number(j.get(), "display_cursor")),
        s->displayPreedit.size());
    s->cursorStops.clear();
    auto *cursorStops = wire::get(j.get(), "cursor_stops");
    if (cursorStops && json_object_is_type(cursorStops, json_type_array))
      for (size_t i = 0; i < json_object_array_length(cursorStops); ++i) {
        auto *value = json_object_array_get_idx(cursorStops, i);
        if (json_object_is_type(value, json_type_int)) {
          const auto stop = json_object_get_int64(value);
          if (stop >= 0 && size_t(stop) <= s->preedit.size() &&
              (s->cursorStops.empty() || size_t(stop) > s->cursorStops.back()))
            s->cursorStops.push_back(size_t(stop));
        }
      }
    s->revision = wire::number(j.get(), "revision");
    auto list = std::make_unique<CommonCandidateList>();
    list->setPageSize(pageSize_);
    list->setCursorPositionAfterPaging(CursorPositionAfterPaging::ResetToFirst);
    // The Windows candidate bar renders the page-local digit as part of each
    // candidate ("1测试"), without Fcitx's default "1. 测试" label.
    list->setSelectionKey({});
    list->setLayoutHint(vertical_ ? CandidateLayoutHint::Vertical
                                  : CandidateLayoutHint::Horizontal);
    auto *candidates = wire::get(j.get(), "candidates");
    if (candidates && json_object_is_type(candidates, json_type_array))
      for (size_t i = 0;
           i < std::min(size_t(50), json_object_array_length(candidates));
           i++) {
        auto *v = json_object_array_get_idx(candidates, i);
        if (json_object_is_type(v, json_type_string)) {
          std::string display = std::to_string(i % pageSize_ + 1);
          display += json_object_get_string(v);
          list->append<Word>(this, std::move(display), i, s->revision);
        }
      }
    if (list->totalSize()) {
      s->candidatePage =
          std::clamp(s->candidatePage, 0, std::max(0, list->totalPages() - 1));
      const int pageBegin = s->candidatePage * pageSize_;
      const int pageEnd = std::min(pageBegin + pageSize_, list->totalSize());
      s->candidateCursor = std::clamp(s->candidateCursor, pageBegin,
                                      std::max(pageBegin, pageEnd - 1));
      list->setPage(s->candidatePage);
      list->setGlobalCursorIndex(s->candidateCursor);
      ic->inputPanel().setCandidateList(std::move(list));
    } else
      ic->inputPanel().setCandidateList(nullptr);
    panel(ic, s);
  }

public:
  explicit WeType(Instance *instance)
      : instance_(instance), factory_([this](InputContext &ic) {
          return new State(++nextId_, ic);
        }) {
    reloadSettings();
    instance_->inputContextManager().registerProperty("wetypexState",
                                                      &factory_);
    settingsAction_.setShortText("WeTypeX 设置");
    settingsAction_.setIcon("fcitx5-wetypex");
    settingsAction_.registerAction("wetypex-settings",
                                   &instance_->userInterfaceManager());
    settingsConnection_ = settingsAction_.connect<SimpleAction::Activated>(
        [](InputContext *) { startProcess({WETYPE_SETTINGS}); });
    timer_ = instance_->eventLoop().addTimeEvent(
        CLOCK_MONOTONIC, now(CLOCK_MONOTONIC) + 100000, 0,
        [this](auto *source, uint64_t time) {
          bool pending = false;
          for (auto &[id, ref] : contexts_)
            if (auto *ic = ref.get()) {
              auto *s = ic->propertyFor(&factory_);
              pending |= s->seq > s->applied;
              if (networkEnabled_ && ready_ && ic->hasFocus() &&
                  !s->preedit.empty() && s->seq == s->applied &&
                  time < s->cloudPollUntil &&
                  time >= s->lastCloudPoll + 100000) {
                s->lastCloudPoll = time;
                send(ic, "poll");
              }
            }
          if (child_ > 0 && (!ready_ || pending) &&
              time >= activity_ + (ready_ ? 30000000 : 180000000))
            fail();
          if (child_ <= 0 && restartAt_ && time >= restartAt_) {
            restartAt_ = 0;
            if (!start())
              scheduleRestart();
          }
          if (time >= syncTick_ + 1000000) {
            syncTick_ = time;
            receiveVoice();
            receiveVModeAction();
          }
          if (networkEnabled_ && *config_.update->autoUpdate &&
              time >= updateTick_ + 3600000000ULL) {
            updateTick_ = time;
            startProcess({WETYPE_UPDATE});
          }
          source->setTime(time + 100000);
          source->setEnabled(true);
          return true;
        });
    if (!start()) {
      failed_ = true;
      scheduleRestart();
    }
  }
  const Configuration *getConfig() const override { return &config_; }
  const Configuration *
  getConfigForInputMethod(const InputMethodEntry &) const override {
    return &config_;
  }
  void setConfig(const RawConfig &raw) override {
    config_.load(raw, true);
    saveNativeConfig();
    applyAppearance();
    if (networkEnabled_) {
      applyDeviceFunctions();
      startSync();
    } else {
      stopSync();
    }
    stop();
    if (!start()) {
      failed_ = true;
      scheduleRestart();
    }
  }
  void setConfigForInputMethod(const InputMethodEntry &,
                               const RawConfig &raw) override {
    setConfig(raw);
  }
  void reloadConfig() override {
    reloadSettings();
    if (networkEnabled_)
      startSync();
    else
      stopSync();
    stop();
    if (!start()) {
      failed_ = true;
      scheduleRestart();
    }
  }
  ~WeType() override {
    timer_.reset();
    stop();
    stopSync();
  }
  void activate(const InputMethodEntry &, InputContextEvent &e) override {
    reloadSettings();
    if (networkEnabled_) {
      startSync();
    }
    auto *ic = e.inputContext();
    auto *current = state(ic);
    current->english = *config_.input->defaultLanguage ==
                       wetype_config::DefaultLanguage::English;
    current->traditional = false;
    ic->statusArea().addAction(StatusGroup::InputMethod, &settingsAction_);
    if (ic->capabilityFlags().test(CapabilityFlag::Password))
      return;
    send(ic, "open");
  }
  void reset(const InputMethodEntry &, InputContextEvent &e) override {
    auto *ic = e.inputContext();
    auto *s = state(ic);
    ++s->epoch;
    s->preedit.clear();
    s->cursor = 0;
    clearDisplayState(s);
    s->pendingPairKey = 0;
    s->pendingPairRight.clear();
    s->symbolAutoState = 0;
    s->vMode = false;
    s->cloudPollUntil = 0;
    ic->inputPanel().reset();
    if (child_ > 0)
      send(ic, "reset");
    ic->updatePreedit();
    ic->updateUserInterface(UserInterfaceComponent::InputPanel);
  }
  void deactivate(const InputMethodEntry &entry,
                  InputContextEvent &e) override {
    reset(entry, e);
    if (child_ > 0)
      send(e.inputContext(), "close");
  }
  void select(InputContext *ic, unsigned index, int64_t revision = -1) {
    auto *list = dynamic_cast<CommonCandidateList *>(
        ic->inputPanel().candidateList().get());
    if (!list || index >= unsigned(list->totalSize()))
      return;
    send(ic, "select", "", index, revision);
    // Keep the current client preedit until the asynchronous select response
    // commits the chosen text.  Clearing it here races React/contenteditable
    // clients (notably Perplexity) and can overwrite the later commit.
    // receive() will apply the response's empty preedit and refresh the panel.
  }
  void keyEvent(const InputMethodEntry &, KeyEvent &e) override {
    auto key = e.key();
    auto *ic = e.inputContext();
    auto *s = state(ic);
    auto sym = key.sym();
    const bool shiftModifier =
        sym == FcitxKey_Shift_L || sym == FcitxKey_Shift_R;
    const bool ctrlModifier =
        sym == FcitxKey_Control_L || sym == FcitxKey_Control_R;
    if (e.isRelease()) {
      auto releasedConfiguredKey = [&](const KeyList &keys) {
        return std::any_of(keys.begin(), keys.end(), [&](const Key &candidate) {
          return key.isReleaseOfModifier(candidate);
        });
      };
      if (voiceRecording_ && voiceHold_ &&
          releasedConfiguredKey(*config_.voice->holdKey)) {
        stopVoice();
        e.filterAndAccept();
        return;
      }
      if (!s->preedit.empty() && s->modifierCandidate == sym &&
          ((*config_.shortcuts->selectCtrl && ctrlModifier) ||
           releasedConfiguredKey(*config_.shortcuts->secondCandidateKeys) ||
           releasedConfiguredKey(*config_.shortcuts->thirdCandidateKeys))) {
        const bool third =
            sym == FcitxKey_Control_R ||
            releasedConfiguredKey(*config_.shortcuts->thirdCandidateKeys);
        auto *list = dynamic_cast<CommonCandidateList *>(
            ic->inputPanel().candidateList().get());
        const unsigned pageIndex = third ? 2 : 1;
        if (list && pageIndex < unsigned(list->size()))
          list->candidate(pageIndex).select(ic);
        s->modifierCandidate = FcitxKey_None;
        e.filterAndAccept();
        return;
      }
      const bool configuredLanguageSwitch =
          releasedConfiguredKey(*config_.shortcuts->languageSwitchKeys) &&
          ((!shiftModifier && !ctrlModifier) ||
           (shiftModifier && *config_.shortcuts->shiftSwitch) ||
           (ctrlModifier && *config_.shortcuts->ctrlSwitch));
      if (s->modifierCandidate == sym &&
          ((shiftModifier && *config_.shortcuts->shiftSwitch) ||
           (ctrlModifier && *config_.shortcuts->ctrlSwitch) ||
           configuredLanguageSwitch)) {
        if (!s->preedit.empty())
          send(ic, "reset");
        s->preedit.clear();
        s->cursor = 0;
        clearDisplayState(s);
        s->english = !s->english;
        s->symbolAutoState = 0;
        s->modifierCandidate = FcitxKey_None;
        ic->inputPanel().reset();
        panel(ic, s);
        e.filterAndAccept();
      }
      return;
    }
    if (networkEnabled_ && *config_.voice->launchShortcut &&
        key.checkKeyList(*config_.voice->launchKey)) {
      if (!voiceRecording_)
        startVoice(ic, false);
      else
        voiceHold_ = false;
      e.filterAndAccept();
      return;
    }
    if (networkEnabled_ && !voiceRecording_ && *config_.voice->holdShortcut &&
        key.checkKeyList(*config_.voice->holdKey)) {
      startVoice(ic, true);
      e.filterAndAccept();
      return;
    }
    if (voiceRecording_ && !key.isModifier()) {
      stopVoice();
      e.filterAndAccept();
      return;
    }
    if (key.isModifier()) {
      s->modifierCandidate = sym;
      return;
    }
    s->modifierCandidate = FcitxKey_None;
    if (*config_.shortcuts->punctuationSwitch &&
        key.checkKeyList(*config_.shortcuts->punctuationSwitchKeys)) {
      s->englishPunctuation = !s->englishPunctuation;
      s->symbolAutoState = 0;
      e.filterAndAccept();
      return;
    }
    if (*config_.shortcuts->traditionalSwitch &&
        key.checkKeyList(*config_.shortcuts->traditionalSwitchKeys)) {
      s->traditional = !s->traditional;
      if (child_ > 0) {
        send(ic, "close");
        send(ic, "open");
      }
      e.filterAndAccept();
      return;
    }
    if (*config_.shortcuts->halfFull &&
        key.checkKeyList(*config_.shortcuts->halfFullKeys)) {
      s->fullWidth = !s->fullWidth;
      s->symbolAutoState = 0;
      e.filterAndAccept();
      return;
    }
    if (key.states().test(KeyState::Ctrl) || key.states().test(KeyState::Alt) ||
        key.states().test(KeyState::Super))
      return;
    if (ic->capabilityFlags().test(CapabilityFlag::Password))
      return;
    if (networkEnabled_ && !s->vMode && s->preedit.empty() &&
        *config_.shortcuts->aiAssistant &&
        key.checkKeyList(*config_.shortcuts->aiAssistantKeys)) {
      const auto &surrounding = ic->surroundingText();
      if (surrounding.isValid() && surrounding.cursor()) {
        const auto &all = surrounding.text();
        auto byteCursor =
            utf8::ncharByteLength(all.begin(), surrounding.cursor());
        if (byteCursor > 0 && size_t(byteCursor) <= all.size()) {
          std::string question = all.substr(0, size_t(byteCursor));
          constexpr size_t MaxContextBytes = 4096;
          if (question.size() > MaxContextBytes) {
            size_t begin = question.size() - MaxContextBytes;
            while (begin < question.size() &&
                   (static_cast<unsigned char>(question[begin]) & 0xc0) == 0x80)
              ++begin;
            question.erase(0, begin);
          }
          while (!question.empty() &&
                 std::isspace(static_cast<unsigned char>(question.back())))
            question.pop_back();
          if (!question.empty()) {
            startProcess({WETYPE_AI, question});
            e.filterAndAccept();
            return;
          }
        }
      }
    }
    if (s->english) {
      if (sym >= 0x20 && sym <= 0x7e) {
        const char ascii = char(sym);
        const auto *entry = punctuationEntry(ascii);
        if (entry &&
            (s->fullWidth || entry->pairType || s->pendingPairKey == ascii)) {
          commitSymbol(ic, s, ascii, true);
          e.filterAndAccept();
        } else if (s->fullWidth) {
          uint32_t full = sym == FcitxKey_space ? 0x3000 : sym + 0xfee0;
          ic->commitString(utf8::UCS4ToUTF8(full));
          e.filterAndAccept();
        }
      }
      return;
    }
    bool composing = !s->preedit.empty();
    if (!composing)
      recordClipboard(ic);
    if (failed_) {
      if (composing && sym == FcitxKey_Return) {
        ic->commitString(s->preedit);
        s->preedit.clear();
        s->cursor = 0;
        clearDisplayState(s);
        ic->inputPanel().reset();
        panel(ic, s);
        e.filterAndAccept();
      }
      return;
    }
    auto *list = dynamic_cast<CommonCandidateList *>(
        ic->inputPanel().candidateList().get());
    if (!composing && !s->vMode && *config_.shortcuts->vMode &&
        key.checkKeyList(*config_.shortcuts->vModeKeys)) {
      // VModeV2 is armed by the core's pending-input callback for a literal
      // "v", then activated through SessionOptions 0x11.
      send(ic, "key", "v");
      send(ic, "vmode");
      s->vMode = true;
      s->preedit.clear();
      s->cursor = 0;
      clearDisplayState(s);
      ic->inputPanel().setCandidateList(nullptr);
      panel(ic, s);
      const auto &rect = ic->cursorRect();
      startProcess({WETYPE_VMODE, std::to_string(s->id),
                    std::to_string(rect.left()),
                    std::to_string(rect.bottom())});
      e.filterAndAccept();
      return;
    }
    if (s->vMode && !composing && sym == FcitxKey_Escape) {
      send(ic, "reset");
      s->vMode = false;
      e.filterAndAccept();
      return;
    }
    if (s->vMode && sym >= 0x20 && sym <= 0x7e) {
      if (sym == FcitxKey_space) {
        if (list && list->totalSize())
          list->candidate(std::max(list->cursorIndex(), 0)).select(ic);
        e.filterAndAccept();
        return;
      }
      std::string text(1, char(sym));
      s->cursor = std::min(s->cursor, s->preedit.size());
      s->preedit.insert(s->cursor, text);
      ++s->cursor;
      clearDisplayState(s);
      send(ic, "key", text);
      panel(ic, s);
      e.filterAndAccept();
      return;
    }
    if (composing && (sym == FcitxKey_Left || sym == FcitxKey_Right ||
                      sym == FcitxKey_Home || sym == FcitxKey_End)) {
      size_t target = s->cursor;
      if (sym == FcitxKey_Home) {
        target = s->editableBegin;
      } else if (sym == FcitxKey_End) {
        target = s->preedit.size();
      } else if (sym == FcitxKey_Left && target) {
        size_t previous = s->editableBegin;
        for (const auto stop : s->cursorStops) {
          if (stop >= target)
            break;
          if (stop >= s->editableBegin)
            previous = stop;
        }
        if (s->cursorStops.empty() && target > s->editableBegin) {
          previous = target - 1;
          while (previous > s->editableBegin &&
                 (static_cast<unsigned char>(s->preedit[previous]) & 0xc0) ==
                     0x80)
            --previous;
        }
        target = previous;
      } else if (sym == FcitxKey_Right && target < s->preedit.size()) {
        target += firstUtf8Size(std::string_view(s->preedit).substr(target));
      }
      target = std::min(target, s->preedit.size());
      if (target != s->cursor) {
        s->cursor = target;
        clearDisplayState(s);
        send(ic, "move", "", 0, -1, target);
        panel(ic, s);
      }
      e.filterAndAccept();
      return;
    }
    bool pageDown =
        sym == FcitxKey_Page_Down ||
        key.checkKeyList(instance_->globalConfig().defaultNextPage()) ||
        key.checkKeyList(*config_.shortcuts->nextPageKeys) ||
        (*config_.shortcuts->pageMinusEqual && sym == FcitxKey_equal) ||
        (*config_.shortcuts->pageBrackets && sym == FcitxKey_bracketright) ||
        (*config_.shortcuts->pageCommaPeriod && sym == FcitxKey_period) ||
        (*config_.shortcuts->pageShiftTab &&
         !key.states().test(KeyState::Shift) && sym == FcitxKey_Tab);
    bool pageUp =
        sym == FcitxKey_Page_Up ||
        key.checkKeyList(instance_->globalConfig().defaultPrevPage()) ||
        key.checkKeyList(*config_.shortcuts->previousPageKeys) ||
        (*config_.shortcuts->pageMinusEqual && sym == FcitxKey_minus) ||
        (*config_.shortcuts->pageBrackets && sym == FcitxKey_bracketleft) ||
        (*config_.shortcuts->pageCommaPeriod && sym == FcitxKey_comma) ||
        (*config_.shortcuts->pageShiftTab &&
         key.states().test(KeyState::Shift) && sym == FcitxKey_Tab);
    if (composing && (pageDown || pageUp)) {
      if (list) {
        if (pageDown) {
          if (list->hasNext())
            list->next();
        } else if (list->hasPrev())
          list->prev();
        s->candidatePage = list->currentPage();
        s->candidateCursor = list->globalCursorIndex();
        ic->updateUserInterface(UserInterfaceComponent::InputPanel);
      }
      e.filterAndAccept();
      return;
    }
    const bool previousCandidate =
        key.checkKeyList(instance_->globalConfig().defaultPrevCandidate()) ||
        sym == FcitxKey_Up;
    const bool nextCandidate =
        key.checkKeyList(instance_->globalConfig().defaultNextCandidate()) ||
        sym == FcitxKey_Down;
    if (composing && list && (previousCandidate || nextCandidate)) {
      if (previousCandidate)
        list->prevCandidate();
      else
        list->nextCandidate();
      s->candidatePage = list->currentPage();
      s->candidateCursor = list->globalCursorIndex();
      ic->updateUserInterface(UserInterfaceComponent::InputPanel);
      e.filterAndAccept();
      return;
    }
    const bool configuredSecond =
        key.checkKeyList(*config_.shortcuts->secondCandidateKeys);
    const bool configuredThird =
        key.checkKeyList(*config_.shortcuts->thirdCandidateKeys);
    if (composing &&
        (configuredSecond || configuredThird ||
         (*config_.shortcuts->selectSemicolonQuote &&
          (sym == FcitxKey_semicolon || sym == FcitxKey_apostrophe)))) {
      const unsigned pageIndex =
          configuredSecond || sym == FcitxKey_semicolon ? 1 : 2;
      if (list && pageIndex < unsigned(list->size()))
        list->candidate(pageIndex).select(ic);
      e.filterAndAccept();
      return;
    }
    if (composing && !s->vMode &&
        (sym == FcitxKey_space || (sym >= FcitxKey_1 && sym <= FcitxKey_9))) {
      const unsigned pageIndex =
          sym == FcitxKey_space ? 0 : unsigned(sym - FcitxKey_1);
      if (!list || pageIndex >= unsigned(list->size())) {
        if (sym != FcitxKey_space)
          return;
      }
      if (list && list->size()) {
        const int index = sym == FcitxKey_space
                              ? std::max(list->cursorIndex(), 0)
                              : int(pageIndex);
        if (index < list->size())
          list->candidate(index).select(ic);
      } else if (sym == FcitxKey_space) {
        select(ic, 0);
      }
      e.filterAndAccept();
      return;
    }
    if (composing && sym == FcitxKey_BackSpace) {
      s->cursor = std::min(s->cursor, s->preedit.size());
      if (s->cursor) {
        size_t pos = s->cursor - 1;
        while (pos &&
               (static_cast<unsigned char>(s->preedit[pos]) & 0xc0) == 0x80)
          --pos;
        s->preedit.erase(pos, s->cursor - pos);
        s->cursor = pos;
        clearDisplayState(s);
      }
      send(ic, "backspace");
      panel(ic, s);
      e.filterAndAccept();
      return;
    }
    if (composing && sym == FcitxKey_Delete) {
      s->cursor = std::min(s->cursor, s->preedit.size());
      if (s->cursor < s->preedit.size()) {
        const auto count =
            firstUtf8Size(std::string_view(s->preedit).substr(s->cursor));
        s->preedit.erase(s->cursor, count);
        clearDisplayState(s);
        send(ic, "delete");
        panel(ic, s);
      }
      e.filterAndAccept();
      return;
    }
    if (composing && (sym == FcitxKey_Escape || sym == FcitxKey_Return ||
                      sym == FcitxKey_KP_Enter)) {
      if (s->vMode && sym != FcitxKey_Escape && list && list->totalSize()) {
        list->candidate(std::max(list->cursorIndex(), 0)).select(ic);
        e.filterAndAccept();
        return;
      }
      send(ic, sym == FcitxKey_Escape ? "reset" : "raw");
      s->vMode = false;
      s->preedit.clear();
      s->cursor = 0;
      clearDisplayState(s);
      ic->inputPanel().setCandidateList(nullptr);
      panel(ic, s);
      e.filterAndAccept();
      return;
    }
    const bool lower = sym >= FcitxKey_a && sym <= FcitxKey_z;
    const bool upper = sym >= FcitxKey_A && sym <= FcitxKey_Z;
    const bool digit = sym >= FcitxKey_0 && sym <= FcitxKey_9;
    const bool ascii = sym >= 0x20 && sym <= 0x7e;
    const char asciiChar = ascii ? char(sym) : 0;
    const auto *entry = ascii ? punctuationEntry(asciiChar) : nullptr;

    // SymbolAutoChangedHelper in the desktop implementation delays numeric
    // punctuation correction.  A colon/comma after a digit is displayed in
    // Chinese form first; only another digit changes it back to ASCII.
    const bool autoChangeActive = symbolAutoChange_ && !composing &&
                                  !s->englishPunctuation &&
                                  chinesePunctuation_ && !s->fullWidth;
    if (!composing && digit && autoChangeActive) {
      if (s->symbolAutoState == 1 || s->symbolAutoState == 2) {
        const char replacement = s->symbolAutoState == 1 ? ':' : ',';
        ic->deleteSurroundingText(-1, 1);
        ic->commitString(std::string(1, replacement) + asciiChar);
        s->symbolAutoState = 3;
        e.filterAndAccept();
        return;
      }
      s->symbolAutoState = 3;
    } else if (!composing && (!entry || !autoChangeActive)) {
      s->symbolAutoState = 0;
    }

    // The original desktop treats apostrophe as a pinyin separator while a
    // composition exists. Outside a composition it follows the quote-pair
    // entry in punctuation.json.
    const bool separator = composing && sym == FcitxKey_apostrophe;
    if (!composing && s->fullWidth &&
        (upper || digit || sym == FcitxKey_space)) {
      const uint32_t full = sym == FcitxKey_space ? 0x3000 : sym + 0xfee0;
      ic->commitString(utf8::UCS4ToUTF8(full));
      e.filterAndAccept();
      return;
    }
    if (!composing && upper) {
      ic->commitString(std::string(1, asciiChar));
      e.filterAndAccept();
      return;
    }
    if (!composing && entry) {
      if (commitSymbol(ic, s, asciiChar, false))
        e.filterAndAccept();
      return;
    }
    if (composing && entry && !separator) {
      std::string mapped = mappedSymbol(s, asciiChar, false);
      int pairCursor = -1;
      auto [left, right] = splitPair(mapped);
      if (entry->pairType && !right.empty()) {
        if (*config_.input->symbolAutoPair) {
          pairCursor = 1;
          s->pendingPairKey = closingKey(asciiChar);
          s->pendingPairRight = right;
        } else if (entry->pairType == 2) {
          bool &leftNext =
              asciiChar == '"' ? s->doubleQuoteLeft : s->singleQuoteLeft;
          mapped = leftNext ? left : right;
          leftNext = !leftNext;
        } else {
          mapped = left;
        }
      }
      send(ic, "punctuation", std::string(1, asciiChar), 0, -1, -1, mapped,
           pairCursor);
      e.filterAndAccept();
      return;
    }
    if (composing && upper) {
      send(ic, "punctuation", std::string(1, asciiChar), 0, -1, -1,
           std::string(1, asciiChar));
      e.filterAndAccept();
      return;
    }
    if (lower || separator) {
      s->symbolAutoState = 0;
      std::string text(1, asciiChar);
      s->cursor = std::min(s->cursor, s->preedit.size());
      s->preedit.insert(s->cursor, text);
      ++s->cursor;
      clearDisplayState(s);
      if (lower && networkEnabled_)
        s->cloudPollUntil = now(CLOCK_MONOTONIC) + 1500000;
      send(ic, "key", text);
      panel(ic, s);
      e.filterAndAccept();
    }
  }
};
void Word::select(InputContext *ic) const {
  engine_->select(ic, index_, revision_);
}
class Factory : public AddonFactory {
  AddonInstance *create(AddonManager *m) override {
    return new WeType(m->instance());
  }
};
} // namespace
FCITX_ADDON_FACTORY(Factory)
