#include "../common/json.hpp"
#include "config.hpp"
#include "wechat_emoji.hpp"
#include <algorithm>
#include <cerrno>
#include <clipboard_public.h>
#include <csignal>
#include <cstddef>
#include <cstdlib>
#include <cstring>
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
#include <fcitx/inputmethodentry.h>
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
#include <poll.h>
#include <queue>
#include <spawn.h>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <type_traits>
#include <tuple>
#include <unistd.h>
#include <utility>
#include <vector>
extern char **environ;
namespace {
using namespace fcitx;
template <typename Context, typename Flag, typename = void>
struct HasCursorCommit : std::false_type {};
template <typename Context, typename Flag>
struct HasCursorCommit<Context, Flag,
    std::void_t<decltype(Flag::CommitStringWithCursor),
                decltype(std::declval<Context &>().commitStringWithCursor(
                    std::declval<const std::string &>(), size_t{}))>>
    : std::true_type {};
template <typename System, typename = void>
struct HasXkbMask : std::false_type {};
template <typename System>
struct HasXkbMask<System, std::void_t<decltype(
    std::declval<const System &>().xkbStateMask(std::string{}))>>
    : std::true_type {};
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
  // Uppercase letters participate in the original core's mixed-case/English
  // composition. Keep their raw spelling while awaiting its display text.
  if (std::any_of(raw.begin(), raw.end(),
                  [](unsigned char c) { return c >= 'A' && c <= 'Z'; }))
    return raw;
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
struct DeferredKey {
  Key key;
  bool release;
  int time;
};
enum class VMenu { None, Main, Clipboard, Hotwords, SymbolGroups, Symbols };
struct State : InputContextProperty {
  uint64_t id, epoch = 1, seq = 0, applied = 0, revision = 0, inputSeq = 0;
  uint64_t appliedUpdate = 0;
  uint64_t initialPanelDeadline = 0;
  bool initialPanelShown = false;
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
  std::string pendingNumericText, pendingNumericReplacement;
  uint64_t pendingNumericUntil = 0;
  bool doubleQuoteLeft = true, singleQuoteLeft = true;
  bool english = false, fullWidth = false, traditional = false,
       englishPunctuation = false, vMode = false, vCalculator = false;
  bool languageInitialized = false;
  uint32_t physicalModifiers = 0;
  uint8_t pressedShifts = 0;
  int pendingCursorSteps = 0;
  uint64_t cursorDeadline = 0;
  uint64_t cursorReplayAt = 0;
  int64_t cursorTarget = -1;
  std::string cursorExpected;
  std::vector<DeferredKey> cursorKeys;
  bool selectionPending = false;
  std::vector<DeferredKey> deferredKeys;
  VMenu vMenu = VMenu::None;
  uint64_t vMenuGeneration = 0;
  std::string vMenuTitle;
  std::vector<std::pair<std::string, std::string>> vMenuEntries;
  bool active = false;
  Key modifierCandidate;
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
class VMenuWord : public CandidateWord {
  WeType *engine_;
  size_t index_;
  uint64_t generation_, epoch_, session_;

public:
  VMenuWord(WeType *engine, std::string text, size_t index, uint64_t generation,
            uint64_t epoch, uint64_t session)
      : CandidateWord(Text(text)), engine_(engine), index_(index),
        generation_(generation), epoch_(epoch), session_(session) {}
  void select(InputContext *ic) const override;
};
class WeType : public InputMethodEngine {
  friend class VMenuWord;
  Instance *instance_;
  wetype::WeChatEmoticons wechatEmoticons_{WETYPE_WECHAT_EMOTICONS};
  bool wechatFont_ = wetype_emoji::originalFontAvailable();
  uint64_t nextId_ = 0;
  FactoryFor<State> factory_;
  SimpleAction settingsAction_;
  Connection settingsConnection_;
  std::map<uint64_t, TrackableObjectReference<InputContext>> contexts_;
  pid_t child_ = -1;
  int read_ = -1, write_ = -1;
  std::unique_ptr<EventSourceIO> reader_, writer_;
  std::unique_ptr<EventSourceTime> timer_, numericTimer_, cursorTimer_;
  std::unique_ptr<HandlerTableEntry<EventHandler>> languageModifierWatcher_;
  std::unique_ptr<HandlerTableEntry<EventHandler>> numericWatcher_;
  bool ready_ = false, failed_ = false, backendUpdates_ = false;
  uint64_t activity_ = 0;
  uint64_t restartAt_ = 0, restartBackoff_ = 250000;
  bool restartNeedsOpen_ = false;
  uint64_t syncTick_ = 0, updateTick_ = 0;
  uint64_t voiceWarmTick_ = 0;
  uint64_t lastVoiceVersion_ = 0;
  pid_t vModeChild_ = -1;
  uint64_t vModeSession_ = 0, vModeEpoch_ = 0, vModeGeneration_ = 0;
  std::string vModeToken_;
  bool vModeNative_ = false;
  uint64_t vModeFocusDeadline_ = 0;
  bool vModeFallbackPending_ = false;
  std::string inbound_, outbound_;
  bool voiceRecording_ = false, voiceHold_ = false, voicePending_ = false;
  uint64_t voiceSession_ = 0, voiceEpoch_ = 0;
  uint64_t voiceStatusUntil_ = 0;
  pid_t voiceDaemonPid_ = 0;
  std::string voiceToken_, voiceText_, voicePhase_, voiceError_;
  int voiceLevel_ = 0;
  pid_t syncChild_ = -1;
  bool replayingCursorKeys_ = false;
  static bool waylandV2(InputContext *ic) {
    return std::string_view(ic->frontend()) == "wayland_v2";
  }
  template <typename System>
  static bool modifiersHeld(System *instance, InputContext *ic, State *s) {
    if constexpr (HasXkbMask<System>::value) {
      if (const auto mask = instance->xkbStateMask(ic->display()))
        return std::get<0>(*mask) || std::get<1>(*mask);
    }
    (void)instance;
    (void)ic;
    return s->physicalModifiers != 0;
  }
  static void clearCursorMove(State *s) {
    s->pendingCursorSteps = 0;
    s->cursorDeadline = 0;
    s->cursorReplayAt = 0;
    s->cursorTarget = -1;
    s->cursorExpected.clear();
    s->cursorKeys.clear();
  }
  void scheduleCursorMove() {
    if (!cursorTimer_)
      return;
    cursorTimer_->setTime(now(CLOCK_MONOTONIC) + 1000);
    cursorTimer_->setEnabled(true);
  }
  void forwardCursor(InputContext *ic, State *s, int steps,
                     const std::string &expected) {
    if (!steps)
      return;
    // Wayland-v2's virtual keyboard inherits physical modifiers even
    // when forwardKey requests none. Wait for their real release rather
    // than sending Shift+Left and selecting the closing bracket.
    if (waylandV2(ic)) {
      s->pendingCursorSteps = steps;
      s->cursorExpected = expected;
      s->cursorDeadline = 0;
      s->cursorReplayAt = 0;
      s->cursorTarget = -1;
      scheduleCursorMove();
      return;
    }
    emitCursor(ic, steps);
  }
  static void emitCursor(InputContext *ic, int steps) {
    for (int i = 0; i < std::abs(steps); ++i)
      ic->forwardKey(Key(steps < 0 ? FcitxKey_Left : FcitxKey_Right));
  }
  void replayCursorKeys(InputContext *ic, std::vector<DeferredKey> keys) {
    const bool previous = replayingCursorKeys_;
    replayingCursorKeys_ = true;
    const auto ref = ic->watch();
    for (const auto &key : keys) {
      auto *target = ref.get();
      if (!target || !target->hasFocus() ||
          !target->propertyFor(&factory_)->active ||
          target->capabilityFlags().test(CapabilityFlag::Password))
        break;
      KeyEvent event(target, key.key, key.release, key.time);
      if (!target->keyEvent(event)) {
        const auto scalar = Key::keySymToUnicode(key.key.sym());
        if (waylandV2(target) && scalar >= 0x20 && scalar != 0x7f) {
          // The original uppercase symbol must survive replay after Shift
          // has been released; native forwardKey cannot restore its mask.
          if (!key.release)
            target->commitString(utf8::UCS4ToUTF8(scalar));
        } else
          target->forwardKey(key.key, key.release, key.time);
      }
    }
    replayingCursorKeys_ = previous;
  }
  bool resolveCursorMove(InputContext *ic, State *s) {
    if (!s->pendingCursorSteps)
      return false;
    if (!s->active || !ic->hasFocus() ||
        ic->capabilityFlags().test(CapabilityFlag::Password)) {
      clearCursorMove(s);
      return false;
    }
    const auto time = now(CLOCK_MONOTONIC);
    const auto &surrounding = ic->surroundingText();
    if (s->cursorReplayAt) {
      const bool acknowledged = s->cursorTarget >= 0 && surrounding.isValid() &&
          surrounding.cursor() == surrounding.anchor() &&
          int64_t(surrounding.cursor()) == s->cursorTarget;
      if (!acknowledged && time < s->cursorReplayAt)
        return true;
      auto keys = std::move(s->cursorKeys);
      clearCursorMove(s);
      replayCursorKeys(ic, std::move(keys));
      return s->pendingCursorSteps != 0;
    }
    if (modifiersHeld(instance_, ic, s))
      return true;
    if (!s->cursorDeadline)
      s->cursorDeadline = time + 250000;
    // Terminals can support native input without exposing surrounding text.
    // With no text to compare, preserve the ordered cursor-key fallback.
    const bool hasText = surrounding.isValid() && !surrounding.text().empty();
    bool matches = !hasText || surrounding.cursor() == surrounding.anchor();
    if (matches && hasText) {
      const auto &text = surrounding.text();
      const auto byte = utf8::ncharByteLength(text.begin(), surrounding.cursor());
      matches = byte >= 0 && size_t(byte) <= text.size();
      if (matches && s->pendingCursorSteps < 0)
        matches = size_t(byte) >= s->cursorExpected.size() &&
                  text.compare(size_t(byte) - s->cursorExpected.size(),
                               s->cursorExpected.size(), s->cursorExpected) == 0;
      else if (matches)
        matches = text.compare(size_t(byte), s->cursorExpected.size(),
                               s->cursorExpected) == 0;
    }
    if (!matches && time < s->cursorDeadline)
      return true;
    const int steps = s->pendingCursorSteps;
    // A click, selection or context change must not receive a late movement.
    if (matches) {
      emitCursor(ic, steps);
      // Cursor keys and IM commits use different Wayland protocols. Give
      // the client a turn to apply the key before replaying nested pairs or
      // text. Surrounding-text acknowledgement can finish this earlier.
      s->cursorTarget = hasText ? int64_t(surrounding.cursor()) + steps : -1;
      s->cursorReplayAt = time + 16000;
      return true;
    }
    auto keys = std::move(s->cursorKeys);
    clearCursorMove(s);
    replayCursorKeys(ic, std::move(keys));
    return s->pendingCursorSteps != 0;
  }
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
  // Confirm the committed numeric suffix at the actual, unselected caret.
  static bool previousNumericTextIs(InputContext *ic,
                                    const std::string &expected) {
    const auto &surrounding = ic->surroundingText();
    if (!surrounding.isValid() || surrounding.cursor() != surrounding.anchor())
      return false;
    const auto &text = surrounding.text();
    auto byte = utf8::ncharByteLength(text.begin(), surrounding.cursor());
    return byte >= 0 && size_t(byte) <= text.size() &&
           size_t(byte) > expected.size() &&
           text[size_t(byte) - expected.size() - 1] >= '0' &&
           text[size_t(byte) - expected.size() - 1] <= '9' &&
           text.compare(size_t(byte) - expected.size(), expected.size(),
                        expected) == 0;
  }
  static const char *numericPunctuation(char ascii) {
    return ascii == ':' ? "：" : ascii == ',' ? "，" : "。";
  }
  static void clearNumericCorrection(State *s) {
    s->pendingNumericText.clear();
    s->pendingNumericReplacement.clear();
    s->pendingNumericUntil = 0;
  }
  static char numericReplacement(uint8_t state) {
    return state == 1 ? ':' : state == 2 ? ',' : state == 4 ? '.' : 0;
  }
  static void appendNumericCorrection(State *s, const std::string &text) {
    if (s->pendingNumericText.empty())
      return;
    if (s->pendingNumericText.size() + text.size() > 1024) {
      clearNumericCorrection(s);
      return;
    }
    s->pendingNumericText += text;
    s->pendingNumericReplacement += text;
  }
  static bool numericKeyTransport(InputContext *ic) {
    return std::string_view(ic->frontend()) == "dbus";
  }
  static void deliverNumericText(InputContext *ic, const std::string &text) {
    if (!numericKeyTransport(ic)) {
      ic->commitString(text);
      return;
    }
    // Qt's D-Bus client can temporarily discard its surrounding-text cache
    // while still reporting the document. Keep this path on ordered keys.
    std::string_view remaining(text);
    while (!remaining.empty()) {
      const auto scalar = utf8::getChar(remaining.begin(), remaining.end());
      const Key key(Key::keySymFromUnicode(scalar));
      ic->forwardKey(key);
      ic->forwardKey(key, true);
      remaining.remove_prefix(firstUtf8Size(remaining));
    }
  }
  void scheduleNumericCorrection() {
    if (!numericTimer_)
      return;
    const auto wake = now(CLOCK_MONOTONIC) + 1;
    if (!numericTimer_->isEnabled() || numericTimer_->time() > wake)
      numericTimer_->setTime(wake);
    numericTimer_->setEnabled(true);
  }
  void resolveNumericCorrection(InputContext *ic, State *s) {
    if (s->pendingNumericText.empty())
      return;
    if (!s->active || !ic->hasFocus() || !symbolAutoChange_ || s->english ||
        s->englishPunctuation || s->fullWidth || !chinesePunctuation_ ||
        voicePending_ || s->vMode || s->vMenu != VMenu::None ||
        !s->preedit.empty() ||
        ic->capabilityFlags().test(CapabilityFlag::Password) ||
        ic->capabilityFlags().test(CapabilityFlag::Sensitive) ||
        now(CLOCK_MONOTONIC) > s->pendingNumericUntil) {
      clearNumericCorrection(s);
      return;
    }
    const auto &expected = s->pendingNumericText;
    if (!previousNumericTextIs(ic, expected))
      return;
    const auto replacement = s->pendingNumericReplacement;
    const int length = int(utf8::length(expected));
    clearNumericCorrection(s);
    if (numericKeyTransport(ic)) {
      for (int i = 0; i < length; ++i) {
        ic->forwardKey(Key(FcitxKey_BackSpace));
        ic->forwardKey(Key(FcitxKey_BackSpace), true);
      }
    } else {
      ic->deleteSurroundingText(-length, length);
    }
    deliverNumericText(ic, replacement);
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
  template <typename Context, typename Flag = CapabilityFlag>
  void commitStringAtCursor(Context *ic, const std::string &text,
                                   size_t cursor) {
    const auto length = utf8::length(text);
    cursor = std::min(cursor, length);
    if constexpr (HasCursorCommit<Context, Flag>::value) {
      if (ic->capabilityFlags().test(Flag::CommitStringWithCursor)) {
        ic->commitStringWithCursor(text, cursor);
        return;
      }
    }

    // XIM and some Wayland text-input clients cannot express a cursor inside
    // committed text. Commit the complete pair first, then reproduce the
    // original desktop behavior with ordinary cursor movement.
    ic->commitString(text);
    forwardCursor(ic, state(ic), -int(length - cursor), text);
  }
  bool commitSymbol(InputContext *ic, State *s, char ascii, bool asciiMode) {
    const auto *entry = punctuationEntry(ascii);
    if (!entry)
      return false;
    if (s->pendingPairKey == ascii && !s->pendingPairRight.empty() &&
        nextSurroundingTextIs(ic, s->pendingPairRight)) {
      forwardCursor(ic, s, 1, s->pendingPairRight);
      s->pendingPairKey = 0;
      s->pendingPairRight.clear();
      s->symbolAutoState = 0;
      return true;
    }
    const bool autoChangeActive = symbolAutoChange_ && !asciiMode &&
                                  !s->englishPunctuation &&
                                  chinesePunctuation_ && !s->fullWidth;
    const bool numeric = autoChangeActive &&
                         (s->symbolAutoState || !s->pendingNumericText.empty());
    auto emit = [&](const std::string &value) {
      if (numeric)
        deliverNumericText(ic, value);
      else
        ic->commitString(value);
    };
    std::string mapped = mappedSymbol(s, ascii, asciiMode);
    if (autoChangeActive && !entry->pairType)
      appendNumericCorrection(s, mapped);
    auto [left, right] = splitPair(mapped);
    if (entry->pairType && !right.empty()) {
      if (*config_.input->symbolAutoPair) {
        if (numeric && numericKeyTransport(ic)) {
          emit(mapped);
          for (size_t i = 1; i < utf8::length(mapped); ++i) {
            ic->forwardKey(Key(FcitxKey_Left));
            ic->forwardKey(Key(FcitxKey_Left), true);
          }
        } else {
          commitStringAtCursor(ic, mapped, 1);
        }
        s->pendingPairKey = closingKey(ascii);
        s->pendingPairRight = right;
      } else if (entry->pairType == 2) {
        bool &leftNext =
            ascii == '\"' ? s->doubleQuoteLeft : s->singleQuoteLeft;
        emit(leftNext ? left : right);
        leftNext = !leftNext;
      } else {
        emit(left);
      }
    } else {
      emit(mapped);
    }

    if (autoChangeActive && s->symbolAutoState == 3 && ascii == ':')
      s->symbolAutoState = 1;
    else if (autoChangeActive && s->symbolAutoState == 3 && ascii == ',')
      s->symbolAutoState = 2;
    else if (autoChangeActive && s->symbolAutoState == 3 && ascii == '.')
      s->symbolAutoState = 4;
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
    input->mode.setValue((mode == "wubi" ||
                         (mode.empty() && wire::number(j.get(), "keyboard") == 5))
                            ? wetype_config::InputMode::Wubi
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
    appearance->preeditPosition.setValue(
        wire::str(j.get(), "preedit_position") == "candidate"
            ? wetype_config::PreeditPosition::Candidate
            : wetype_config::PreeditPosition::Client);
    appearance->labelStyle.setValue(
        wire::str(j.get(), "candidate_label_style") == "fcitx"
            ? wetype_config::CandidateLabelStyle::Fcitx
            : wetype_config::CandidateLabelStyle::WeType);
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
      auto *value = wire::get(j.get(), name);
      if (value && json_object_is_type(value, json_type_string))
        option.setValue(Key::keyListFromString(wire::str(j.get(), name)));
      else
        option.reset();
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
    wire::put(json.get(), "preedit_position",
              std::string(*appearance.preeditPosition ==
                                  wetype_config::PreeditPosition::Candidate
                              ? "candidate"
                              : "client"));
    wire::put(json.get(), "candidate_label_style",
              std::string(*appearance.labelStyle ==
                                  wetype_config::CandidateLabelStyle::Fcitx
                              ? "fcitx"
                              : "wetype"));
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
        if (existing.rfind(key + "=", 0) == 0) {
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
    uint64_t mask =
        (uint64_t(wire::number(state.get(), "func_switch")) & ~uint64_t(7)) |
        (*devices.clipboardSync ? 1 : 0) | (*devices.phraseSync ? 2 : 0) |
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
  bool voiceControl(const char *operation, bool traditional = false) {
    std::ifstream input(stateDirectory() / "voice/control.json");
    std::string bytes(1025, '\0');
    input.read(bytes.data(), bytes.size());
    bytes.resize(input.gcount());
    if (bytes.size() > 1024)
      return false;
    auto control = wire::parse(bytes);
    if (wire::number(control.get(), "version") != 1)
      return false;
    const auto address = wire::str(control.get(), "address");
    const auto prefix = "wetypex-voice-" + std::to_string(getuid()) + "-";
    sockaddr_un socketAddress{};
    if (address.compare(0, prefix.size(), prefix) != 0 ||
        address.size() + 1 > sizeof(socketAddress.sun_path))
      return false;
    const int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0)
      return false;
    socketAddress.sun_family = AF_UNIX;
    memcpy(socketAddress.sun_path + 1, address.data(), address.size());
    struct ucred peer{};
    socklen_t peerSize = sizeof(peer);
    if (::connect(fd, reinterpret_cast<sockaddr *>(&socketAddress),
                offsetof(sockaddr_un, sun_path) + 1 + address.size()) ||
        getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &peer, &peerSize) ||
        peer.uid != getuid()) {
      close(fd);
      return false;
    }
    auto message = wire::object();
    wire::put(message.get(), "op", std::string(operation));
    wire::put(message.get(), "token", voiceToken_);
    wire::put(message.get(), "session", int64_t(voiceSession_));
    wire::put(message.get(), "epoch", int64_t(voiceEpoch_));
    wire::put(message.get(), "pid", int64_t(getpid()));
    wire::put(message.get(), "traditional", traditional);
    const auto payload = wire::dump(message.get()) + '\n';
    bool delivered = ::send(fd, payload.data(), payload.size(),
                           MSG_NOSIGNAL | MSG_DONTWAIT) == ssize_t(payload.size());
    if (delivered) {
      pollfd wait{fd, POLLIN, 0};
      char ack;
      // Local control normally completes in less than a millisecond. Bound
      // the GUI wait and fall back to the helper if the daemon is restarting.
      delivered = ::poll(&wait, 1, 5) > 0 && ::recv(fd, &ack, 1, MSG_DONTWAIT) == 1;
    }
    close(fd);
    if (delivered && std::string_view(operation) == "start")
      voiceDaemonPid_ = peer.pid;
    return delivered;
  }
  void primeVoice() {
    voiceWarmTick_ = now(CLOCK_MONOTONIC);
    if (networkEnabled_ &&
        (*config_.voice->launchShortcut || *config_.voice->holdShortcut) &&
        !voiceControl("status"))
      startProcess({WETYPE_VOICE, "status"});
  }
  bool flushBeforeVoice(InputContext *ic, KeyEvent &event) {
    auto *s = state(ic);
    if (s->vMode || s->vMenu != VMenu::None ||
        (s->preedit.empty() && s->inputSeq <= s->applied))
      return false;
    const auto fallback = s->preedit;
    if (send(ic, "raw")) {
      s->selectionPending = true;
      // Re-enter the shortcut after the queued letters have committed, just
      // as later typing waits behind a candidate selection. Do not discard
      // unfinished spelling when dictation starts.
      s->deferredKeys.push_back({event.rawKey(), event.isRelease(), event.time()});
      ic->inputPanel().setCandidateList(nullptr);
      panel(ic, s);
      return true;
    }
    if (!fallback.empty())
      ic->commitString(fallback);
    return false;
  }
  void startVoice(InputContext *ic, bool hold) {
    if (voicePending_ || ic->capabilityFlags().test(CapabilityFlag::Password))
      return;
    auto *s = state(ic);
    if (s->vMode || s->vMenu != VMenu::None)
      cancelVMode(ic, "voice input started");
    ++s->epoch;
    s->selectionPending = false;
    s->deferredKeys.clear();
    send(ic, "reset");
    s->preedit.clear();
    s->cursor = 0;
    clearDisplayState(s);
    ic->inputPanel().reset();
    voiceRecording_ = true;
    voicePending_ = true;
    voiceHold_ = hold;
    voiceSession_ = s->id;
    voiceEpoch_ = s->epoch;
    voiceToken_ = std::to_string(getpid()) + "-" +
                  std::to_string(now(CLOCK_MONOTONIC));
    voiceText_.clear();
    voiceError_.clear();
    voicePhase_ = "recording";
    voiceLevel_ = 0;
    voiceStatusUntil_ = 0;
    voiceDaemonPid_ = 0;
    lastVoiceVersion_ = 0;
    if (!voiceControl("start", s->traditional))
      startProcess({WETYPE_VOICE, "start", std::to_string(voiceSession_),
                    std::to_string(voiceEpoch_), voiceToken_,
                    s->traditional ? "1" : "0"});
    panel(ic, s);
  }
  void stopVoice(bool immediate = false) {
    if (!voicePending_)
      return;
    voiceRecording_ = false;
    voiceHold_ = false;
    voicePhase_ = "recognizing";
    if (!voiceControl(immediate ? "finish" : "stop"))
      startProcess({WETYPE_VOICE, immediate ? "finish" : "stop", voiceToken_});
    auto it = contexts_.find(voiceSession_);
    if (it != contexts_.end())
      if (auto *ic = it->second.get(); ic && ic->hasFocus())
        panel(ic, ic->propertyFor(&factory_));
  }
  void cancelVoice(bool notify = true) {
    if (notify && voicePending_ && !voiceControl("cancel"))
      startProcess({WETYPE_VOICE, "cancel", voiceToken_});
    voicePending_ = voiceRecording_ = voiceHold_ = false;
    voiceText_.clear();
    voiceToken_.clear();
    voicePhase_.clear();
    voiceError_.clear();
    voiceStatusUntil_ = 0;
    voiceDaemonPid_ = 0;
    auto it = contexts_.find(voiceSession_);
    if (it != contexts_.end())
      if (auto *ic = it->second.get(); ic && ic->hasFocus())
        panel(ic, ic->propertyFor(&factory_));
  }
  void receiveVoice() {
    if (voicePending_ && voiceDaemonPid_ > 1 &&
        kill(voiceDaemonPid_, 0) < 0 && errno == ESRCH) {
      cancelVoice(false);
      voiceError_ = "语音服务已退出，请重新启动语音输入";
      voiceStatusUntil_ = now(CLOCK_MONOTONIC) + 4000000;
      auto it = contexts_.find(voiceSession_);
      if (it != contexts_.end())
        if (auto *ic = it->second.get(); ic && ic->hasFocus())
          panel(ic, ic->propertyFor(&factory_));
      return;
    }
    if (voiceStatusUntil_ && now(CLOCK_MONOTONIC) >= voiceStatusUntil_)
      cancelVoice();
    std::ifstream input(stateDirectory() / "voice-inbox.json");
    std::string bytes((std::istreambuf_iterator<char>(input)), {});
    if (bytes.empty() || bytes.size() > 65536)
      return;
    auto message = wire::parse(bytes);
    if (voiceToken_.empty() || wire::str(message.get(), "token") != voiceToken_)
      return;
    auto version = uint64_t(wire::number(message.get(), "version"));
    auto text = wire::str(message.get(), "text");
    if (!version || version <= lastVoiceVersion_)
      return;
    lastVoiceVersion_ = version;
    // The producer atomically replaces this file. Removing an older version
    // after reading it can otherwise delete a newer final result.
    const auto daemon = wire::number(message.get(), "daemon_pid");
    if (daemon > 1 && daemon <= INT_MAX)
      voiceDaemonPid_ = daemon;
    const auto session = uint64_t(wire::number(message.get(), "session"));
    const auto epoch = uint64_t(wire::number(message.get(), "epoch"));
    auto it = contexts_.find(session);
    if (it == contexts_.end())
      return;
    auto *ic = it->second.get();
    if (!ic || !ic->hasFocus() ||
        ic->capabilityFlags().test(CapabilityFlag::Password)) {
      cancelVoice();
      return;
    }
    auto *s = ic->propertyFor(&factory_);
    if (!s->active || s->epoch != epoch || session != voiceSession_ ||
        epoch != voiceEpoch_) {
      cancelVoice();
      return;
    }
    const auto phase = wire::str(message.get(), "state");
    voicePhase_ = phase;
    voiceText_ = text;
    voiceLevel_ = std::clamp<int>(wire::number(message.get(), "level"), 0, 100);
    if (phase != "recording")
      voiceRecording_ = voiceHold_ = false;
    if (phase == "final" || phase == "error" || phase == "cancelled") {
      voicePending_ = false;
      voiceText_.clear();
      voiceToken_.clear();
      voiceDaemonPid_ = 0;
      if (phase == "error") {
        voiceError_ = wire::str(message.get(), "error", "语音输入失败");
        voiceStatusUntil_ = now(CLOCK_MONOTONIC) + 4000000;
      } else
        voicePhase_.clear();
      panel(ic, s);
      if (phase == "final" && !text.empty())
        ic->commitString(text);
    } else
      panel(ic, s);
  }
  static void clearVMenu(State *s) {
    s->vMenu = VMenu::None;
    ++s->vMenuGeneration;
    s->vMenuEntries.clear();
    s->vMenuTitle.clear();
  }
  static wire::Json menuFile(const std::filesystem::path &path) {
    std::ifstream input(path);
    std::string bytes(1048577, '\0');
    input.read(bytes.data(), bytes.size());
    bytes.resize(input.gcount());
    return bytes.size() > 1048576 ? wire::Json{} : wire::parse(bytes);
  }
  void nativeVMenu(InputContext *ic, VMenu kind, std::string title,
                   std::vector<std::pair<std::string, std::string>> entries) {
    auto *s = state(ic);
    s->vMenu = kind;
    ++s->vMenuGeneration;
    s->vMenuTitle = std::move(title);
    s->vMenuEntries = std::move(entries);
    auto list = std::make_unique<CommonCandidateList>();
    list->setPageSize(pageSize_);
    list->setCursorPositionAfterPaging(CursorPositionAfterPaging::ResetToFirst);
    const bool nativeLabels = *config_.appearance->labelStyle ==
                              wetype_config::CandidateLabelStyle::Fcitx;
    list->setSelectionKey(nativeLabels && kind != VMenu::Main
                              ? Key::keyListFromString("1 2 3 4 5 6 7 8 9")
                              : KeyList{});
    list->setLayoutHint(vertical_ ? CandidateLayoutHint::Vertical
                                  : CandidateLayoutHint::Horizontal);
    for (size_t i = 0; i < std::min<size_t>(s->vMenuEntries.size(), 5000);
         ++i) {
      const auto &text = s->vMenuEntries[i].first;
      size_t end = std::min<size_t>(text.size(), 120);
      while (end < text.size() && end &&
             (static_cast<unsigned char>(text[end]) & 0xc0) == 0x80)
        --end;
      auto display = text.substr(0, end);
      std::replace(display.begin(), display.end(), '\n', ' ');
      std::replace(display.begin(), display.end(), '\r', ' ');
      if (end < text.size())
        display += "…";
      if (!nativeLabels && kind != VMenu::Main)
        display = std::to_string(i % pageSize_ + 1) + display;
      list->append<VMenuWord>(this, std::move(display), i, s->vMenuGeneration,
                              s->epoch, s->id);
    }
    s->candidatePage = s->candidateCursor = 0;
    if (list->totalSize()) {
      list->setPage(0);
      list->setGlobalCursorIndex(0);
    }
    ic->inputPanel().setCandidateList(std::move(list));
    panel(ic, s);
  }
  void mainVMenu(InputContext *ic) {
    nativeVMenu(ic, VMenu::Main, "快捷工具（数字计算，空格选择）",
                {{"计算", "calculator"},
                 {"剪贴板", "clipboard"},
                 {"常用语", "hotwords"},
                 {"符号", "symbols"}});
  }
  void symbolVMenu(InputContext *ic, int groupIndex = -1) {
    auto json = menuFile(WETYPE_SYMBOLS);
    std::vector<std::pair<std::string, std::string>> entries;
    std::string title = "符号";
    if (json && json_object_is_type(json.get(), json_type_array)) {
      if (groupIndex < 0) {
        for (size_t i = 0; i < json_object_array_length(json.get()); ++i) {
          auto *group = json_object_array_get_idx(json.get(), i);
          entries.emplace_back(wire::str(group, "groupName"),
                               std::to_string(i));
        }
      } else if (size_t(groupIndex) < json_object_array_length(json.get())) {
        auto *group = json_object_array_get_idx(json.get(), groupIndex);
        title = wire::str(group, "groupName");
        auto *subgroups = wire::get(group, "groupData");
        if (subgroups && json_object_is_type(subgroups, json_type_array))
          for (size_t i = 0; i < json_object_array_length(subgroups); ++i) {
            auto *rows = wire::get(json_object_array_get_idx(subgroups, i),
                                   "subGroupData");
            if (!rows || !json_object_is_type(rows, json_type_array))
              continue;
            for (size_t r = 0; r < json_object_array_length(rows); ++r) {
              auto *row = json_object_array_get_idx(rows, r);
              if (!row || !json_object_is_type(row, json_type_array))
                continue;
              for (size_t n = 0; n < json_object_array_length(row); ++n) {
                auto *symbol = json_object_array_get_idx(row, n);
                if (json_object_is_type(symbol, json_type_string)) {
                  std::string text = json_object_get_string(symbol);
                  entries.emplace_back(text, text);
                }
              }
            }
          }
      }
    }
    nativeVMenu(ic, groupIndex < 0 ? VMenu::SymbolGroups : VMenu::Symbols,
                title + "（退格返回）", std::move(entries));
  }
  void selectVMenu(InputContext *ic, size_t index, uint64_t generation,
                   uint64_t epoch, uint64_t session) {
    auto *s = state(ic);
    if (!s->active || !ic->hasFocus() || !s->vMode || s->epoch != epoch ||
        s->id != session || s->vMenu == VMenu::None ||
        s->vMenuGeneration != generation || index >= s->vMenuEntries.size() ||
        ic->capabilityFlags().test(CapabilityFlag::Password))
      return;
    const auto value = s->vMenuEntries[index].second;
    if (s->vMenu == VMenu::Main) {
      if (value == "calculator") {
        stopVModeWindow();
        s->vCalculator = true;
        ic->inputPanel().reset();
        panel(ic, s);
      } else if (value == "clipboard") {
        auto json = menuFile(stateDirectory() / "clipboard-history.json");
        std::vector<std::pair<std::string, std::string>> entries;
        if (json && json_object_is_type(json.get(), json_type_array))
          for (size_t i = 0; i < json_object_array_length(json.get()); ++i) {
            auto *item = json_object_array_get_idx(json.get(), i);
            if (json_object_is_type(item, json_type_string)) {
              std::string text = json_object_get_string(item);
              if (!text.empty() && text.size() <= 16384)
                entries.emplace_back(text, text);
            }
          }
        nativeVMenu(ic, VMenu::Clipboard, "剪贴板（退格返回）",
                    std::move(entries));
      } else if (value == "hotwords") {
        nativeVMenu(ic, VMenu::Hotwords, "常用语（退格返回）", {});
        send(ic, "hotword_list");
      } else if (value == "symbols")
        symbolVMenu(ic);
    } else if (s->vMenu == VMenu::SymbolGroups) {
      symbolVMenu(ic, int(index));
    } else if (!value.empty() && value.size() <= 65536) {
      cancelVMode(ic);
      ic->commitString(value);
    }
  }
  void stopVModeWindow(const char *reason = "toolbar closed") {
    if (vModeSession_) {
      const auto it = contexts_.find(vModeSession_);
      if (it != contexts_.end() && it->second.get())
        clearVMenu(it->second.get()->propertyFor(&factory_));
      std::ofstream log(stateDirectory() / "vmode.log", std::ios::app);
      log << "V toolbar closed: " << reason << '\n';
    }
    pid_t exited = -1;
    if (vModeChild_ > 0) {
      do {
        exited = waitpid(vModeChild_, nullptr, WNOHANG);
      } while (exited < 0 && errno == EINTR);
    }
    if (vModeChild_ > 0 && exited == 0) {
      kill(vModeChild_, SIGTERM);
      while (waitpid(vModeChild_, nullptr, 0) < 0 && errno == EINTR) {
      }
    }
    vModeChild_ = -1;
    vModeSession_ = vModeEpoch_ = 0;
    vModeToken_.clear();
    vModeNative_ = false;
    vModeFocusDeadline_ = 0;
    vModeFallbackPending_ = false;
  }
  void cancelVMode(InputContext *ic, const char *reason = "V mode cancelled") {
    auto *s = state(ic);
    if (vModeSession_ == s->id)
      stopVModeWindow(reason);
    ++s->epoch;
    send(ic, "reset");
    s->vMode = false;
    s->vCalculator = false;
    s->preedit.clear();
    s->cursor = 0;
    clearDisplayState(s);
    ic->inputPanel().reset();
    panel(ic, s);
  }
  bool startVModeWindow(InputContext *ic) {
    auto *s = state(ic);
    if (vModeSession_) {
      const auto previous = contexts_.find(vModeSession_);
      if (previous != contexts_.end() && previous->second.get())
        cancelVMode(previous->second.get());
      else
        stopVModeWindow();
    }
    const auto &rect = ic->cursorRect();
    const bool relative =
        ic->capabilityFlags().test(CapabilityFlag::RelativeRect);
    double scale = ic->scaleFactor();
    if (!(scale > 0.0))
      scale = 1.0;
    vModeSession_ = s->id;
    vModeEpoch_ = s->epoch;
    vModeToken_ = std::to_string(getpid()) + "-" +
                  std::to_string(now(CLOCK_MONOTONIC)) + "-" +
                  std::to_string(++vModeGeneration_);
    vModeFocusDeadline_ = 0;
    vModeFallbackPending_ = false;
    std::vector<std::string> args{
        WETYPE_VMODE, std::to_string(s->id), std::to_string(rect.left()),
        std::to_string(rect.bottom()), std::to_string(s->epoch), vModeToken_,
        relative ? "1" : "0", std::to_string(scale)};
    std::vector<char *> argv;
    for (auto &arg : args)
      argv.push_back(arg.data());
    argv.push_back(nullptr);
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null",
                                     O_RDONLY, 0);
    posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, "/dev/null",
                                     O_WRONLY, 0);
    const auto logPath = (stateDirectory() / "vmode.log").string();
    posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, logPath.c_str(),
                                     O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW,
                                     0600);
    pid_t child = -1;
    const int result = posix_spawn(&child, WETYPE_VMODE, &actions, nullptr,
                                   argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    if (result) {
      std::ofstream log(logPath, std::ios::app);
      log << "V toolbar spawn failed: " << std::strerror(result) << '\n';
      vModeChild_ = -1;
      vModeNative_ = true;
      mainVMenu(ic);
      return true;
    }
    vModeChild_ = child;
    std::ofstream log(logPath, std::ios::app);
    log << "V toolbar start: rect=" << rect.left() << ',' << rect.bottom()
        << " relative=" << relative << " scale=" << scale << '\n';
    return true;
  }
  void receiveVModeAction() {
    auto path = stateDirectory() / "vmode-action.json";
    std::ifstream input(path);
    std::string bytes((std::istreambuf_iterator<char>(input)), {});
    if (bytes.empty() || bytes.size() > 65536)
      return;
    auto message = wire::parse(bytes);
    const auto session = uint64_t(wire::number(message.get(), "session"));
    const auto epoch = uint64_t(wire::number(message.get(), "epoch"));
    std::error_code error;
    if (!session || session != vModeSession_ || epoch != vModeEpoch_ ||
        vModeToken_.empty() || wire::str(message.get(), "token") != vModeToken_) {
      std::filesystem::remove(path, error);
      return;
    }
    auto it = contexts_.find(session);
    auto *ic = it == contexts_.end() ? nullptr : it->second.get();
    if (!ic) {
      std::filesystem::remove(path, error);
      return;
    }
    const auto action = wire::str(message.get(), "action");
    auto *s = state(ic);
    const bool password =
        ic->capabilityFlags().test(CapabilityFlag::Password);
    if (password || !s->active || !s->vMode || s->epoch != epoch) {
      std::filesystem::remove(path, error);
      std::ofstream log(stateDirectory() / "vmode.log", std::ios::app);
      log << "V toolbar action dropped: " << action.substr(0, 32) << '\n';
      return;
    }
    if (action == "cancel") {
      std::filesystem::remove(path, error);
      cancelVMode(ic);
      return;
    }
    // Wait briefly for the original client to regain focus. A valid token
    // identifies the session, but never grants permission to commit unfocused.
    auto *focused = instance_->lastFocusedInputContext();
    if (!ic->hasFocus() || (focused && focused != ic)) {
      if (!focused && vModeFocusDeadline_ &&
          now(CLOCK_MONOTONIC) < vModeFocusDeadline_)
        return;
      std::filesystem::remove(path, error);
      cancelVMode(ic, "toolbar action target lost focus");
      return;
    }
    std::filesystem::remove(path, error);
    std::ofstream log(stateDirectory() / "vmode.log", std::ios::app);
    log << "V toolbar action: " << action.substr(0, 32) << '\n';
    if (action == "calculator") {
      // Keep the original core's armed VModeV2 session for keyboard input.
      stopVModeWindow();
      s->vCalculator = true;
      panel(ic, s);
    } else if (action == "commit") {
      const auto text = wire::str(message.get(), "text");
      if (text.empty() || text.size() > 65536)
        return;
      cancelVMode(ic);
      ic->commitString(text);
    }
  }
  void pollVModeWindow() {
    // Consume a button action before reaping the window that wrote it.
    receiveVModeAction();
    if (vModeChild_ <= 0 && !vModeNative_)
      return;
    const auto it = contexts_.find(vModeSession_);
    auto *ic = it == contexts_.end() ? nullptr : it->second.get();
    if (!ic) {
      stopVModeWindow();
      return;
    }
    auto *s = state(ic);
    auto *focused = instance_->lastFocusedInputContext();
    const bool otherFocus = focused && focused != ic;
    const auto exited =
        vModeChild_ > 0 ? waitpid(vModeChild_, nullptr, WNOHANG) : 0;
    const bool exitedNow =
        exited > 0 || (exited < 0 && errno == ECHILD);
    if (exited == vModeChild_ || (exited < 0 && errno == ECHILD))
      vModeChild_ = -1;
    vModeFallbackPending_ |= exitedNow;
    const bool password = ic->capabilityFlags().test(CapabilityFlag::Password);
    if (!otherFocus && s->active && s->vMode && s->epoch == vModeEpoch_ &&
        !password && !ic->hasFocus() && !vModeNative_ && vModeFocusDeadline_ &&
        now(CLOCK_MONOTONIC) < vModeFocusDeadline_)
      return;
    if (otherFocus || !ic->hasFocus() || !s->active || !s->vMode ||
        s->epoch != vModeEpoch_ || password) {
      cancelVMode(ic, "toolbar input context changed or focus grace expired");
      return;
    }
    vModeFocusDeadline_ = 0;
    if (vModeFallbackPending_) {
      vModeFallbackPending_ = false;
      vModeNative_ = true;
      mainVMenu(ic);
      return;
    }
    if (vModeChild_ <= 0 && !vModeNative_)
      cancelVMode(ic, "toolbar process exited");
  }
  State *state(InputContext *ic) {
    auto *s = ic->propertyFor(&factory_);
    contexts_[s->id] = ic->watch();
    return s;
  }
  void panel(InputContext *ic, State *s) {
    const bool inlinePreedit =
        *config_.appearance->preeditPosition !=
            wetype_config::PreeditPosition::Candidate &&
        ic->capabilityFlags().test(CapabilityFlag::Preedit) &&
        ic->isPreeditEnabled();
    const bool voice = voicePending_ && voiceSession_ == s->id &&
                       voiceEpoch_ == s->epoch;
    const auto candidates = ic->inputPanel().candidateList();
    if (s->preedit.empty() || inlinePreedit || voice || s->vMode ||
        s->vMenu != VMenu::None) {
      s->initialPanelDeadline = 0;
      s->initialPanelShown = false;
    } else if (!s->initialPanelShown &&
               (!candidates || !candidates->size()) &&
               s->inputSeq > s->applied && !failed_) {
      const auto time = now(CLOCK_MONOTONIC);
      if (!s->initialPanelDeadline) {
        // Usually the real core replies within a millisecond. Let that
        // reply populate the first popup instead of mapping a tiny window.
        // One frame bounds feedback for a slow or still-starting core.
        s->initialPanelDeadline = time + 16000;
        if (timer_) {
          timer_->setAccuracy(1000);
          timer_->setTime(std::min(timer_->time(), s->initialPanelDeadline));
        }
      }
      if (time < s->initialPanelDeadline) {
        const bool visible = !ic->inputPanel().empty();
        ic->inputPanel().reset();
        ic->updatePreedit();
        if (visible)
          ic->updateUserInterface(UserInterfaceComponent::InputPanel);
        return;
      }
      s->initialPanelShown = true;
      s->initialPanelDeadline = 0;
    } else {
      s->initialPanelShown = true;
      s->initialPanelDeadline = 0;
    }
    Text aux;
    if (voice)
      aux = Text("语音输入 " + std::string(voiceLevel_ / 12, '|'));
    else if (s->vMenu != VMenu::None)
      aux = Text(s->vMenuTitle);
    else if (s->vCalculator)
      aux = Text("计算：输入表达式，空格选取结果");
    ic->inputPanel().setAuxUp(aux);
    const std::string displayed =
        voice ? voiceText_
        : !s->displayPreedit.empty() ? s->displayPreedit
        : !s->vMode && *config_.input->mode == wetype_config::InputMode::Pinyin
            ? formatPinyinPreedit(s->preedit)
            : s->preedit;
    // Text("") contains one empty segment and is not empty to Fcitx UI.
    Text text;
    if (!displayed.empty()) {
      text.append(displayed);
      text.setCursor(
          voice ? displayed.size()
          : !s->displayPreedit.empty()
              ? std::min(s->displayCursor, displayed.size())
              : displayCursorForRaw(s->preedit, displayed, s->cursor));
    }
    ic->inputPanel().setClientPreedit(inlinePreedit ? text : Text());
    // Clients without inline preedit (including some terminal setups) need
    // to see the composition in the input panel.
    ic->inputPanel().setPreedit(inlinePreedit ? Text() : text);
    Text status;
    if (voice)
      status.append(voicePhase_ == "polishing"
                        ? "正在整理… Enter 立即上屏 · Esc 取消"
                    : voiceRecording_
                        ? "正在聆听… 空格结束 · Esc 取消"
                        : "正在识别… Enter 立即上屏 · Esc 取消");
    else if (voiceSession_ == s->id && voiceStatusUntil_)
      status.append(voiceError_);
    else if (!displayed.empty() && (failed_ || !ready_))
      status.append(failed_ ? "WeTypeX 核心正在恢复…" : "WeTypeX 核心启动中…");
    ic->inputPanel().setAuxDown(status);
    if (s->preedit.empty() && !s->vMode)
      ic->inputPanel().setCandidateList(nullptr);
    ic->updatePreedit();
    ic->updateUserInterface(UserInterfaceComponent::InputPanel);
  }
  void stop() {
    stopVModeWindow("input backend stopped");
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
    backendUpdates_ = false;
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
    invalidateSessions();
  }
  void invalidateSessions() {
    cancelVoice();
    restartNeedsOpen_ = true;
    for (auto &[id, ref] : contexts_)
      if (auto *ic = ref.get()) {
        auto *s = ic->propertyFor(&factory_);
        clearCursorMove(s);
        s->physicalModifiers = s->pressedShifts = 0;
        ++s->epoch;
        s->seq = s->applied = s->inputSeq = 0;
        s->appliedUpdate = 0;
        s->initialPanelDeadline = 0;
        s->initialPanelShown = false;
        clearNumericCorrection(s);
        s->selectionPending = false;
        s->deferredKeys.clear();
        s->modifierCandidate = Key();
        s->preedit.clear();
        s->cursor = 0;
        clearDisplayState(s);
        s->vMode = false;
        s->vCalculator = false;
        clearVMenu(s);
        s->cloudPollUntil = 0;
        s->pendingPairKey = 0;
        s->pendingPairRight.clear();
        s->symbolAutoState = 0;
        if (s->active) {
          ic->inputPanel().reset();
          if (ic->hasFocus())
            panel(ic, s);
        }
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
  bool send(InputContext *ic, const char *op, const std::string &key = "",
            int index = 0, int64_t revision = -1, int64_t cursor = -1,
            const std::string &mapped = {}, int pairCursor = -1) {
    auto *s = state(ic);
    // Candidate page/cursor state belongs to the current composition.
    // Cloud-result polls refresh that same list and must preserve the user's
    // position; every actual engine operation starts again from the first
    // candidate, as on the native Windows/macOS clients.
    if (std::string_view(op) != "poll")
      s->candidatePage = s->candidateCursor = 0;
    if (std::string_view(op) != "select" && std::string_view(op) != "poll") {
      s->selectionPending = false;
      s->deferredKeys.clear();
    }
    if (child_ <= 0 && !start()) {
      failed_ = true;
      scheduleRestart();
      panel(ic, s);
      return false;
    }
    auto json = wire::object();
    wire::put(json.get(), "session", int64_t(s->id));
    wire::put(json.get(), "seq", int64_t(++s->seq));
    if (std::string_view(op) == "key" || std::string_view(op) == "backspace" ||
        std::string_view(op) == "delete" || std::string_view(op) == "move")
      s->inputSeq = s->seq;
    wire::put(json.get(), "epoch", int64_t(s->epoch));
    wire::put(json.get(), "op", std::string(op));
    wire::put(json.get(), "candidate_updates", true);
    wire::put(json.get(), "key", key);
    wire::put(json.get(), "index", int64_t(index));
    wire::put(json.get(), "revision", revision);
    wire::put(json.get(), "cursor", cursor);
    wire::put(json.get(), "mapped", mapped);
    wire::put(json.get(), "pair_cursor", int64_t(pairCursor));
    wire::put(json.get(), "before", op == std::string("predict") ? key : "");
    wire::put(json.get(), "chinese_punctuation", int64_t(chinesePunctuation_));
    const auto &inputConfig = *config_.input;
    wire::put(json.get(), "keyboard",
              int64_t(*inputConfig.mode == wetype_config::InputMode::Wubi ? 5 : 0));
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
    const bool wechat = wetype::WeChatEmoticons::isWeChat(ic->program());
    wire::put(json.get(), "wechat_emoji",
              bool(*inputConfig.wechatEmoji) && wechat);
    wire::put(json.get(), "normal_emoji", bool(*inputConfig.normalEmoji));
    wire::put(json.get(), "kaomoji", bool(*inputConfig.kaomoji));
    wire::put(json.get(), "large_emoji",
              bool(*inputConfig.largeEmoji) && wechat);
    wire::put(json.get(), "symbol_emoji", bool(*inputConfig.symbolEmoji));
    wire::put(json.get(), "v_mode",
              bool(*config_.shortcuts->vMode) &&
                  *config_.input->mode == wetype_config::InputMode::Pinyin);
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
      return false;
    }
    activity_ = now(CLOCK_MONOTONIC);
    flush();
    return true;
  }
  void receive(const std::string &line) {
    auto j = wire::parse(line);
    if (!j)
      return;
    if (wire::str(j.get(), "event") == "ready") {
      ready_ = true;
      backendUpdates_ = wire::number(j.get(), "candidate_updates", 0);
      failed_ = false;
      restartBackoff_ = 250000;
      activity_ = now(CLOCK_MONOTONIC);
      if (restartNeedsOpen_) {
        restartNeedsOpen_ = false;
        for (auto &[id, ref] : contexts_)
          if (auto *ic = ref.get();
              ic && ic->hasFocus() && ic->propertyFor(&factory_)->active &&
              !ic->capabilityFlags().test(CapabilityFlag::Password))
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
    auto respEpoch = uint64_t(wire::number(j.get(), "epoch"));
    const auto update = uint64_t(wire::number(j.get(), "update_id"));
    const bool pushed = wire::str(j.get(), "event") == "update";
    if (respEpoch != s->epoch || seq < s->applied || seq > s->seq ||
        (seq == s->applied && (!pushed || update <= s->appliedUpdate)))
      return;
    s->applied = seq;
    s->appliedUpdate = update;
    activity_ = now(CLOCK_MONOTONIC);
    if (!s->active || !ic->hasFocus() ||
        ic->capabilityFlags().test(CapabilityFlag::Password))
      return;
    if (s->vMenu != VMenu::None) {
      auto *words = wire::get(j.get(), "hotwords");
      if (s->vMenu == VMenu::Hotwords && seq == s->seq && words &&
          json_object_is_type(words, json_type_array)) {
        std::vector<std::pair<std::string, std::string>> entries;
        for (size_t i = 0; i < json_object_array_length(words); ++i) {
          auto text = wire::str(json_object_array_get_idx(words, i), "words");
          if (!text.empty())
            entries.emplace_back(text, text);
        }
        nativeVMenu(ic, VMenu::Hotwords, "常用语（退格返回）",
                    std::move(entries));
      }
      return;
    }
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
    s->selectionPending = false;
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
    const bool nativeLabels = *config_.appearance->labelStyle ==
                              wetype_config::CandidateLabelStyle::Fcitx;
    list->setSelectionKey(
        nativeLabels ? Key::keyListFromString("1 2 3 4 5 6 7 8 9") : KeyList{});
    list->setLayoutHint(vertical_ ? CandidateLayoutHint::Vertical
                                  : CandidateLayoutHint::Horizontal);
    auto *candidates = wire::get(j.get(), "candidates");
    auto *cloudFlags = wire::get(j.get(), "cloud");
    if ((!s->preedit.empty() || s->vMode) && candidates &&
        json_object_is_type(candidates, json_type_array))
      for (size_t i = 0;
           i < std::min(size_t(50), json_object_array_length(candidates));
           i++) {
        auto *v = json_object_array_get_idx(candidates, i);
        if (json_object_is_type(v, json_type_string) &&
            json_object_get_string_len(v)) {
          bool cloud = false;
          if (cloudFlags && json_object_is_type(cloudFlags, json_type_array) &&
              i < json_object_array_length(cloudFlags)) {
            auto *flag = json_object_array_get_idx(cloudFlags, i);
            cloud = json_object_is_type(flag, json_type_boolean) &&
                    json_object_get_boolean(flag);
          }
          std::string display =
              nativeLabels ? std::string{}
                           : std::to_string(list->totalSize() % pageSize_ + 1);
          if (cloud)
            display += "☁";
          auto text = std::string_view(json_object_get_string(v),
                                       json_object_get_string_len(v));
          const auto *emoticon =
              wetype::WeChatEmoticons::isWeChat(ic->program())
                  ? wechatEmoticons_.find(text)
                  : nullptr;
          if (emoticon && wechatFont_)
            display += utf8::UCS4ToUTF8(emoticon->codepoint) + " ";
          display += text;
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
    auto deferred = std::move(s->deferredKeys);
    s->deferredKeys.clear();
    auto ref = ic->watch();
    for (const auto &key : deferred) {
      auto *target = ref.get();
      if (!target || !target->hasFocus() ||
          !target->propertyFor(&factory_)->active)
        break;
      KeyEvent event(target, key.key, key.release, key.time);
      if (!target->keyEvent(event))
        target->forwardKey(key.key, key.release, key.time);
    }
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
    // Fcitx global triggers run before the engine. Own explicitly configured
    // shortcuts and bare language modifiers so their ordered commits survive.
    languageModifierWatcher_ = instance_->watchEvent(
        EventType::InputContextKeyEvent, EventWatcherPhase::PreInputMethod,
        [this](Event &event) {
          auto &key = static_cast<KeyEvent &>(event);
          if (key.filtered())
            return;
          auto *ic = key.inputContext();
          auto *entry = instance_->inputMethodEntry(ic);
          if (!entry || entry->uniqueName() != "wetypex" ||
              !state(ic)->active ||
              ic->capabilityFlags().test(CapabilityFlag::Password))
            return;
          auto *s = state(ic);
          const auto sym = key.key().sym();
          const bool explicitGlobalModifier = std::any_of(
              instance_->globalConfig().altTriggerKeys().begin(),
              instance_->globalConfig().altTriggerKeys().end(),
              [&](const Key &candidate) {
                return candidate.isModifier() && !bareModifier(candidate) &&
                       (key.key().check(candidate) ||
                        key.key().isReleaseOfModifier(candidate));
              });
          // Fcitx 5.0 can match its bare Shift trigger on a Ctrl+Shift
          // release. Own those component events as well, while preserving
          // an explicitly configured global modifier chord.
          const bool languageModifier =
              !explicitGlobalModifier &&
              (((sym == FcitxKey_Shift_L || sym == FcitxKey_Shift_R) &&
                *config_.shortcuts->shiftSwitch) ||
               ((sym == FcitxKey_Control_L || sym == FcitxKey_Control_R) &&
                *config_.shortcuts->ctrlSwitch));
          const bool pendingSwitch =
              key.isRelease() && s->modifierCandidate.isModifier() &&
              languageSwitchKey(s->modifierCandidate) &&
              key.key().isReleaseOfModifier(s->modifierCandidate);
          const bool compositionShortcut =
              !s->english && (!s->preedit.empty() || s->vMenu != VMenu::None) &&
              !key.key().isModifier() && compositionShortcutKey(key.key());
          if (!languageModifier && !languageSwitchKey(key.key()) && !pendingSwitch &&
              !compositionShortcut)
            return;
          // Fcitx's global trigger must not see this modifier, but the client
          // still needs both halves of a chord such as Shift+Tab. Forward the
          // physical modifier event before consuming it from Fcitx.
          if (languageModifier ||
              (bareModifier(key.key()) && languageSwitchKey(key.key())))
            ic->forwardKey(key.key(), key.isRelease(), key.time());
          keyEvent(*entry, key);
          key.filterAndAccept();
        });
    numericWatcher_ = instance_->watchEvent(
        EventType::InputContextSurroundingTextUpdated, EventWatcherPhase::Default,
        [this](Event &event) {
          auto *ic = static_cast<InputContextEvent &>(event).inputContext();
          const auto *entry = instance_->inputMethodEntry(ic);
          if (entry && entry->uniqueName() == "wetypex" &&
              !state(ic)->pendingNumericText.empty())
            scheduleNumericCorrection();
        });
    // Old Fcitx emits deletes immediately but queues key-event commits.
    // Correct text outside that key transaction and retry while the client
    // acknowledges its recently inserted numeric text.
    numericTimer_ = instance_->eventLoop().addTimeEvent(
        CLOCK_MONOTONIC, now(CLOCK_MONOTONIC), 1000,
        [this](auto *source, uint64_t) {
          bool retry = false;
          for (auto &[id, ref] : contexts_)
            if (auto *ic = ref.get()) {
              auto *s = ic->propertyFor(&factory_);
              resolveNumericCorrection(ic, s);
              retry |= !s->pendingNumericText.empty();
            }
          if (retry)
            source->setTime(now(CLOCK_MONOTONIC) + 2000);
          source->setEnabled(retry);
          return true;
        });
    numericTimer_->setEnabled(false);
    cursorTimer_ = instance_->eventLoop().addTimeEvent(
        CLOCK_MONOTONIC, now(CLOCK_MONOTONIC), 1000,
        [this](auto *source, uint64_t time) {
          bool pending = false;
          for (auto &[id, ref] : contexts_)
            if (auto *ic = ref.get())
              pending |= resolveCursorMove(ic, ic->propertyFor(&factory_));
          if (pending)
            source->setTime(time + 2000);
          source->setEnabled(pending);
          return true;
        });
    cursorTimer_->setEnabled(false);
    timer_ = instance_->eventLoop().addTimeEvent(
        CLOCK_MONOTONIC, now(CLOCK_MONOTONIC) + 100000, 0,
        [this](auto *source, uint64_t time) {
          bool pending = false;
          bool focused = false;
          uint64_t panelWake = 0;
          for (auto &[id, ref] : contexts_)
            if (auto *ic = ref.get()) {
              auto *s = ic->propertyFor(&factory_);
              if (s->initialPanelDeadline) {
                if (!s->active || !ic->hasFocus() ||
                    ic->capabilityFlags().test(CapabilityFlag::Password)) {
                  s->initialPanelDeadline = 0;
                  s->initialPanelShown = false;
                } else if (time >= s->initialPanelDeadline)
                  panel(ic, s);
                if (s->initialPanelDeadline &&
                    (!panelWake || s->initialPanelDeadline < panelWake))
                  panelWake = s->initialPanelDeadline;
              }
              focused |= s->active && ic->hasFocus() &&
                         !ic->capabilityFlags().test(CapabilityFlag::Password);
              pending |= s->active && s->seq > s->applied;
              if (networkEnabled_ && !backendUpdates_ && ready_ && s->active && ic->hasFocus() &&
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
          if (time >= syncTick_ + (voicePending_ ? 40000 : 1000000)) {
            syncTick_ = time;
            receiveVoice();
          }
          if (focused && networkEnabled_ && time >= voiceWarmTick_ + 60000000)
            primeVoice();
          pollVModeWindow();
          if (networkEnabled_ && *config_.update->autoUpdate &&
              time >= updateTick_ + 3600000000ULL) {
            updateTick_ = time;
            startProcess({WETYPE_UPDATE});
          }
          const auto next = time + (voicePending_ ? 40000 : 100000);
          source->setAccuracy(panelWake ? 1000 : 0);
          source->setTime(panelWake ? std::min(next, panelWake) : next);
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
    invalidateSessions();
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
    cancelVoice();
    reloadSettings();
    if (networkEnabled_)
      startSync();
    else
      stopSync();
    stop();
    invalidateSessions();
    if (!start()) {
      failed_ = true;
      scheduleRestart();
    }
  }
  ~WeType() override {
    cancelVoice();
    languageModifierWatcher_.reset();
    numericWatcher_.reset();
    numericTimer_.reset();
    cursorTimer_.reset();
    timer_.reset();
    stop();
    stopSync();
  }
  void activate(const InputMethodEntry &, InputContextEvent &e) override {
    reloadSettings();
    if (networkEnabled_) {
      startSync();
    } else
      stopSync();
    auto *ic = e.inputContext();
    auto *current = state(ic);
    if (vModeSession_ == current->id && current->vMode && vModeFocusDeadline_ &&
        now(CLOCK_MONOTONIC) >= vModeFocusDeadline_)
      cancelVMode(ic, "toolbar focus grace expired before activation");
    if (vModeSession_ == current->id && current->vMode && current->active &&
        !ic->capabilityFlags().test(CapabilityFlag::Password)) {
      // The toolbar is still open after a focus round trip caused by the
      // layer surface itself. Do not send "open" again: it would reset the
      // backend V session the toolbar is still using.
      current->active = true;
      vModeFocusDeadline_ = 0;
      ic->statusArea().addAction(StatusGroup::InputMethod, &settingsAction_);
      panel(ic, current);
      return;
    }
    current->active = true;
    // activate() runs on every focus-in; seed language state only once.
    if (!current->languageInitialized) {
      current->english = *config_.input->defaultLanguage ==
                         wetype_config::DefaultLanguage::English;
      current->languageInitialized = true;
    }
    ic->statusArea().addAction(StatusGroup::InputMethod, &settingsAction_);
    if (ic->capabilityFlags().test(CapabilityFlag::Password))
      return;
    primeVoice();
    panel(ic, current);
    send(ic, "open");
  }
  void reset(const InputMethodEntry &, InputContextEvent &e) override {
    auto *ic = e.inputContext();
    auto *s = state(ic);
    clearCursorMove(s);
    s->physicalModifiers = s->pressedShifts = 0;
    if (voicePending_ && voiceSession_ == s->id)
      cancelVoice();
    if (vModeSession_ == s->id)
      stopVModeWindow(s->active ? "input context reset"
                                : "input method deactivated");
    clearVMenu(s);
    // A modifier release after a focus round trip must not toggle language.
    s->modifierCandidate = Key();
    s->selectionPending = false;
    s->deferredKeys.clear();
    ++s->epoch;
    s->initialPanelDeadline = 0;
    s->initialPanelShown = false;
    clearNumericCorrection(s);
    s->preedit.clear();
    s->cursor = 0;
    clearDisplayState(s);
    s->pendingPairKey = 0;
    s->pendingPairRight.clear();
    s->symbolAutoState = 0;
    s->vMode = false;
    s->vCalculator = false;
    s->cloudPollUntil = 0;
    ic->inputPanel().reset();
    if (child_ > 0)
      send(ic, "reset");
    ic->updatePreedit();
    ic->updateUserInterface(UserInterfaceComponent::InputPanel);
  }
  void deactivate(const InputMethodEntry &entry,
                  InputContextEvent &e) override {
    auto *s = state(e.inputContext());
    if (voiceSession_ == s->id)
      cancelVoice();
    if (e.type() == EventType::InputContextFocusOut && vModeSession_ == s->id &&
        s->vMode && !vModeNative_ &&
        instance_->lastFocusedInputContext() == nullptr) {
      if (!vModeFocusDeadline_)
        vModeFocusDeadline_ = now(CLOCK_MONOTONIC) + 500000;
      std::ofstream log(stateDirectory() / "vmode.log", std::ios::app);
      log << "V toolbar waiting up to 500ms for original input focus\n";
      return;
    }
    s->active = false;
    reset(entry, e);
    if (child_ > 0)
      send(e.inputContext(), "close");
  }
  void select(InputContext *ic, unsigned index, int64_t revision = -1) {
    auto *s = state(ic);
    if (s->selectionPending || !s->active || !ic->hasFocus() ||
        ic->capabilityFlags().test(CapabilityFlag::Password))
      return;
    auto *list = dynamic_cast<CommonCandidateList *>(
        ic->inputPanel().candidateList().get());
    if (revision >= 0 && (!list || index >= unsigned(list->totalSize())))
      return;
    if (revision < 0 && s->preedit.empty())
      return;
    if (!send(ic, "select", "", index, revision))
      return;
    s->selectionPending = true;
    s->vMode = false;
    s->vCalculator = false;
    ic->inputPanel().setCandidateList(nullptr);
    // Keep the current client preedit until the asynchronous select response
    // commits the chosen text.  Clearing it here races React/contenteditable
    // clients (notably Perplexity) and can overwrite the later commit.
    // Hide the stale candidate list immediately; receive() will apply the
    // response's empty preedit and refresh the panel.
    panel(ic, s);
  }
  bool selectByKey(InputContext *ic, int pageIndex = -1) {
    auto *s = state(ic);
    if (pageIndex >= pageSize_)
      return false;
    if (s->selectionPending)
      return true;
    // Preserve engine request order when a selection key follows typing
    // before its candidates arrive. The backend resolves the queued index
    // after processing those letters.
    if (s->inputSeq > s->applied) {
      const unsigned index =
          pageIndex < 0 ? unsigned(s->candidateCursor)
                        : unsigned(s->candidatePage * pageSize_ + pageIndex);
      select(ic, index);
      return true;
    }
    auto candidates = ic->inputPanel().candidateList();
    auto *list = dynamic_cast<CommonCandidateList *>(candidates.get());
    const int index = pageIndex < 0 && list ? std::max(list->cursorIndex(), 0)
                                            : std::max(pageIndex, 0);
    if (list && index < list->size()) {
      list->candidate(index).select(ic);
      return true;
    }
    // A composition with no candidates can still be committed as raw text.
    if (pageIndex < 0 && !s->preedit.empty()) {
      if (send(ic, "raw")) {
        s->selectionPending = true;
        ic->inputPanel().setCandidateList(nullptr);
        panel(ic, s);
      }
      return true;
    }
    return false;
  }
  static bool bareModifier(const Key &key) {
    return key.isModifier() &&
           !(key.states().toInteger() & uint32_t(KeyState::SimpleMask) &
             ~Key::keySymToStates(key.sym()).toInteger());
  }
  bool languageSwitchKey(const Key &key) const {
    if (bareModifier(key)) {
      if (key.sym() == FcitxKey_Shift_L || key.sym() == FcitxKey_Shift_R)
        return *config_.shortcuts->shiftSwitch;
      if (key.sym() == FcitxKey_Control_L || key.sym() == FcitxKey_Control_R)
        return *config_.shortcuts->ctrlSwitch;
    }
    return key.checkKeyList(*config_.shortcuts->languageSwitchKeys);
  }
  bool compositionShortcutKey(const Key &key) const {
    return key.checkKeyList(*config_.shortcuts->previousPageKeys) ||
           key.checkKeyList(*config_.shortcuts->nextPageKeys) ||
           key.checkKeyList(*config_.shortcuts->secondCandidateKeys) ||
           key.checkKeyList(*config_.shortcuts->thirdCandidateKeys) ||
           key.checkKeyList(instance_->globalConfig().defaultPrevPage()) ||
           key.checkKeyList(instance_->globalConfig().defaultNextPage()) ||
           key.checkKeyList(instance_->globalConfig().defaultPrevCandidate()) ||
           key.checkKeyList(instance_->globalConfig().defaultNextCandidate());
  }
  void switchLanguage(InputContext *ic, State *s) {
    if (s->vMode)
      cancelVMode(ic);
    // The queued raw commit must finish before later typing uses the new
    // language, even when the core has not returned the last letter yet.
    if (!s->preedit.empty() || s->inputSeq > s->applied) {
      if (send(ic, "raw")) {
        s->selectionPending = true;
        ic->inputPanel().setCandidateList(nullptr);
      } else {
        if (!s->preedit.empty())
          ic->commitString(s->preedit);
        ++s->epoch;
        s->selectionPending = false;
        s->deferredKeys.clear();
        s->preedit.clear();
        s->cursor = 0;
        clearDisplayState(s);
      }
    } else {
      s->preedit.clear();
      s->cursor = 0;
      clearDisplayState(s);
    }
    s->english = !s->english;
    s->symbolAutoState = 0;
    clearNumericCorrection(s);
    s->modifierCandidate = Key();
    ic->inputPanel().reset();
    panel(ic, s);
  }
  void keyEvent(const InputMethodEntry &, KeyEvent &e) override {
    auto key = e.key();
    auto *ic = e.inputContext();
    auto *s = state(ic);
    auto sym = key.sym();
    // Key::isDigit() only gained keypad support in Fcitx 5.0.20. Normalize
    // these symbols locally so the 5.0.14 baseline selects the same words.
    if (sym >= FcitxKey_KP_0 && sym <= FcitxKey_KP_9)
      sym = KeySym(FcitxKey_0 + (sym - FcitxKey_KP_0));
    if (!s->active || ic->capabilityFlags().test(CapabilityFlag::Password))
      return;
    if (!replayingCursorKeys_) {
      s->physicalModifiers = e.rawKey().states().toInteger() &
                             uint32_t(KeyState::SimpleMask);
      const uint8_t shift = sym == FcitxKey_Shift_L ? 1 :
                            sym == FcitxKey_Shift_R ? 2 : 0;
      if (shift) {
        if (e.isRelease())
          s->pressedShifts &= ~shift;
        else
          s->pressedShifts |= shift;
      }
      if (e.isRelease() && key.isModifier())
        s->physicalModifiers &= ~Key::keySymToStates(sym).toInteger();
      if (s->pressedShifts)
        s->physicalModifiers |= uint32_t(KeyState::Shift);
    }
    if (s->pendingCursorSteps) {
      scheduleCursorMove();
      if (!key.isModifier()) {
        const auto commands = key.states().toInteger() &
                              uint32_t(KeyState::SimpleMask) &
                              ~uint32_t(KeyState::Shift);
        const auto scalar = Key::keySymToUnicode(sym);
        const bool queuedLetters = !s->english && std::any_of(
            s->cursorKeys.begin(), s->cursorKeys.end(), [](const DeferredKey &key) {
              const auto c = Key::keySymToUnicode(key.key.sym());
              return !key.release && ((c >= 'a' && c <= 'z') ||
                                     (c >= 'A' && c <= 'Z'));
            });
        const bool enter = sym == FcitxKey_Return || sym == FcitxKey_KP_Enter;
        const bool orderedEdit =
            (enter && (queuedLetters ||
                       !e.rawKey().states().test(KeyState::Shift))) ||
            (queuedLetters && (sym == FcitxKey_BackSpace || sym == FcitxKey_Delete));
        if (!commands && ((scalar >= 0x20 && scalar != 0x7f) || orderedEdit)) {
          s->cursorKeys.push_back({e.rawKey(), e.isRelease(), e.time()});
          e.filterAndAccept();
          return;
        }
        // Explicit application shortcuts and navigation override automatic
        // caret placement. Preserve earlier text before processing that key.
        auto keys = std::move(s->cursorKeys);
        clearCursorMove(s);
        replayCursorKeys(ic, std::move(keys));
      }
    }
    if (!e.isRelease() && !s->pendingNumericText.empty()) {
      const auto commands = key.states().toInteger() &
                            uint32_t(KeyState::SimpleMask) &
                            ~uint32_t(KeyState::Shift);
      const bool numeric = sym >= FcitxKey_0 && sym <= FcitxKey_9;
      const auto *symbol = sym >= 0x20 && sym <= 0x7e
                               ? punctuationEntry(char(sym)) : nullptr;
      const bool continuation = (symbol && !symbol->pairType) || sym == FcitxKey_space ||
                                sym == FcitxKey_Shift_L || sym == FcitxKey_Shift_R;
      if (commands || (!numeric && !continuation))
        clearNumericCorrection(s);
    }
    if (networkEnabled_ && e.rawKey().states().test(KeyState::Repeat) &&
        ((*config_.voice->launchShortcut &&
          key.checkKeyList(*config_.voice->launchKey)) ||
         (*config_.voice->holdShortcut &&
          key.checkKeyList(*config_.voice->holdKey)))) {
      e.filterAndAccept();
      return;
    }
    // A later key must not operate on the preedit awaiting a select reply.
    // Re-enter normal event processing after that reply has committed it.
    if (s->selectionPending && sym != FcitxKey_Escape) {
      if (s->deferredKeys.size() < 64)
        s->deferredKeys.push_back({e.rawKey(), e.isRelease(), e.time()});
      e.filterAndAccept();
      return;
    }
    const bool ctrlModifier =
        sym == FcitxKey_Control_L || sym == FcitxKey_Control_R;
    if (e.isRelease()) {
      const auto pressed = s->modifierCandidate;
      const bool releasedModifier = pressed.isModifier() &&
                                    key.isReleaseOfModifier(pressed);
      auto releasedConfiguredKey = [&](const KeyList &keys) {
        return std::any_of(keys.begin(), keys.end(), [&](const Key &candidate) {
          return candidate.isModifier() ? key.isReleaseOfModifier(candidate)
              : sym == candidate.sym() ||
                    (candidate.states().toInteger() &
                     Key::keySymToStates(sym).toInteger());
        });
      };
      if (voiceRecording_ && voiceHold_ &&
          releasedConfiguredKey(*config_.voice->holdKey)) {
        stopVoice();
        e.filterAndAccept();
        return;
      }
      const bool selectCtrl = *config_.shortcuts->selectCtrl &&
                              bareModifier(pressed) &&
                              (pressed.sym() == FcitxKey_Control_L ||
                               pressed.sym() == FcitxKey_Control_R);
      if (!s->preedit.empty() && releasedModifier &&
          (selectCtrl ||
           pressed.checkKeyList(*config_.shortcuts->secondCandidateKeys) ||
           pressed.checkKeyList(*config_.shortcuts->thirdCandidateKeys))) {
        const bool third =
            (selectCtrl && pressed.sym() == FcitxKey_Control_R) ||
            pressed.checkKeyList(*config_.shortcuts->thirdCandidateKeys);
        const unsigned pageIndex = third ? 2 : 1;
        selectByKey(ic, pageIndex);
        s->modifierCandidate = Key();
        e.filterAndAccept();
        return;
      }
      // Releasing one Shift while the other remains down is not a bare
      // language-switch gesture. This also keeps deferred uppercase input
      // in the language in which it was typed.
      if ((sym == FcitxKey_Shift_L || sym == FcitxKey_Shift_R) &&
          s->pressedShifts) {
        s->modifierCandidate = Key();
        return;
      }
      if (releasedModifier && languageSwitchKey(pressed)) {
        switchLanguage(ic, s);
        e.filterAndAccept();
      }
      if (releasedModifier)
        s->modifierCandidate = Key();
      return;
    }
    if (networkEnabled_ && *config_.voice->launchShortcut &&
        key.checkKeyList(*config_.voice->launchKey)) {
      s->modifierCandidate = Key();
      if (voiceRecording_ && voiceHold_)
        voiceHold_ = false;
      else if (voicePending_)
        stopVoice(!voiceRecording_);
      else if (!flushBeforeVoice(ic, e))
        startVoice(ic, false);
      e.filterAndAccept();
      return;
    }
    if (networkEnabled_ && *config_.voice->holdShortcut &&
        key.checkKeyList(*config_.voice->holdKey)) {
      s->modifierCandidate = Key();
      if (!voicePending_ && !flushBeforeVoice(ic, e))
        startVoice(ic, true);
      e.filterAndAccept();
      return;
    }
    if (voicePending_ && !key.isModifier()) {
      if (sym == FcitxKey_Escape) {
        cancelVoice();
        e.filterAndAccept();
        return;
      }
      if (!key.states() &&
          (sym == FcitxKey_space || sym == FcitxKey_Return ||
           sym == FcitxKey_KP_Enter)) {
        stopVoice(sym != FcitxKey_space || !voiceRecording_);
        e.filterAndAccept();
        return;
      }
      // Keep the first typed key and application shortcuts when the user
      // leaves dictation, instead of silently swallowing them.
      cancelVoice();
    }
    if (key.isModifier()) {
      // A modifier chord is a shortcut only when explicitly configured.
      // Otherwise Ctrl+Shift must not arm the default bare-Shift switch.
      s->modifierCandidate =
          languageSwitchKey(key) ||
                  (*config_.shortcuts->selectCtrl && ctrlModifier &&
                   bareModifier(key)) ||
                  key.checkKeyList(*config_.shortcuts->secondCandidateKeys) ||
                  key.checkKeyList(*config_.shortcuts->thirdCandidateKeys)
              ? key
              : Key();
      return;
    }
    s->modifierCandidate = Key();
    if (languageSwitchKey(key)) {
      if (!e.rawKey().states().test(KeyState::Repeat))
        switchLanguage(ic, s);
      e.filterAndAccept();
      return;
    }
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
    const bool commandModifiers =
        key.states().toInteger() & uint32_t(KeyState::SimpleMask) &
        ~uint32_t(KeyState::Shift);
    const bool hasComposition = !s->preedit.empty() || s->vMenu != VMenu::None;
    const bool vModeKey = *config_.shortcuts->vMode &&
                          *config_.input->mode ==
                              wetype_config::InputMode::Pinyin &&
                          key.checkKeyList(*config_.shortcuts->vModeKeys);
    const bool idleShortcut =
        !hasComposition &&
        ((!s->english && vModeKey) ||
         (networkEnabled_ && *config_.shortcuts->aiAssistant &&
          key.checkKeyList(*config_.shortcuts->aiAssistantKeys)));
    if (commandModifiers && !idleShortcut &&
        !(hasComposition && compositionShortcutKey(key)))
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
    bool composing = hasComposition;
    auto typeCharacter = [&](char character) {
      s->symbolAutoState = 0;
      std::string text(1, character);
      s->cursor = std::min(s->cursor, s->preedit.size());
      s->preedit.insert(s->cursor, text);
      ++s->cursor;
      clearDisplayState(s);
      // New cores push asynchronous candidates; older ones retain polling.
      if (networkEnabled_ && !backendUpdates_ && character != '\'')
        s->cloudPollUntil = now(CLOCK_MONOTONIC) + 8000000;
      send(ic, "key", text);
      panel(ic, s);
      e.filterAndAccept();
    };
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
    // These original double-pinyin profiles assign ';' to ing. Give that
    // spelling key priority over punctuation and candidate shortcuts.
    const auto doubleScheme = *config_.input->doublePinyin;
    if (composing && !commandModifiers && !s->vMode && s->vMenu == VMenu::None &&
        sym == FcitxKey_semicolon &&
        *config_.input->mode == wetype_config::InputMode::DoublePinyin &&
        (doubleScheme == wetype_config::DoublePinyinScheme::Sogou ||
         doubleScheme == wetype_config::DoublePinyinScheme::Microsoft ||
         doubleScheme == wetype_config::DoublePinyinScheme::Ziguang)) {
      typeCharacter(';');
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
    auto *list = dynamic_cast<CommonCandidateList *>(
        ic->inputPanel().candidateList().get());
    if (s->vMode &&
        (sym == FcitxKey_Escape ||
         (vModeSession_ == s->id &&
          key.checkKeyList(*config_.shortcuts->vModeKeys)))) {
      cancelVMode(ic);
      e.filterAndAccept();
      return;
    }
    if (s->vMenu != VMenu::None) {
      if (list && list->totalSize() &&
          (sym == FcitxKey_Up || sym == FcitxKey_Down)) {
        if (sym == FcitxKey_Up)
          list->prevCandidate();
        else
          list->nextCandidate();
        s->candidatePage = list->currentPage();
        s->candidateCursor = list->globalCursorIndex();
        ic->updateUserInterface(UserInterfaceComponent::InputPanel);
        e.filterAndAccept();
        return;
      }
      if (sym == FcitxKey_BackSpace) {
        mainVMenu(ic);
        e.filterAndAccept();
        return;
      }
      if (sym == FcitxKey_space || sym == FcitxKey_Return ||
          sym == FcitxKey_KP_Enter ||
          (s->vMenu != VMenu::Main && sym >= FcitxKey_1 && sym <= FcitxKey_9)) {
        const int index = sym >= FcitxKey_1 && sym <= FcitxKey_9
                              ? int(sym - FcitxKey_1)
                          : list ? std::max(list->cursorIndex(), 0)
                                 : 0;
        if (list && index < list->size())
          list->candidate(index).select(ic);
        e.filterAndAccept();
        return;
      }
      if (sym >= 0x20 && sym <= 0x7e &&
          (s->vMenu == VMenu::Main || (!pageDown && !pageUp))) {
        clearVMenu(s);
        ic->inputPanel().reset();
        composing = !s->preedit.empty();
      }
    }
    if (!composing && !s->vMode && vModeKey) {
      // VModeV2 is armed by the core's pending-input callback for a literal
      // "v", then activated through SessionOptions 0x11.
      send(ic, "key", "v");
      send(ic, "vmode");
      s->vMode = true;
      s->vCalculator = false;
      s->preedit.clear();
      s->cursor = 0;
      clearDisplayState(s);
      ic->inputPanel().setCandidateList(nullptr);
      panel(ic, s);
      if (!startVModeWindow(ic))
        cancelVMode(ic);
      e.filterAndAccept();
      return;
    }
    if (s->vMode && s->vMenu == VMenu::None && sym >= 0x20 && sym <= 0x7e) {
      if (vModeSession_ == s->id)
        stopVModeWindow();
      if (sym == FcitxKey_space) {
        selectByKey(ic);
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
          configuredSecond ? 1 : configuredThird ? 2
                               : sym == FcitxKey_semicolon ? 1 : 2;
      selectByKey(ic, pageIndex);
      e.filterAndAccept();
      return;
    }
    if (composing && !s->vMode &&
        (sym == FcitxKey_space || (sym >= FcitxKey_1 && sym <= FcitxKey_9))) {
      const unsigned pageIndex =
          sym == FcitxKey_space ? 0 : unsigned(sym - FcitxKey_1);
      if (selectByKey(ic, sym == FcitxKey_space ? -1 : int(pageIndex)))
        e.filterAndAccept();
      return;
    }
    if (commandModifiers)
      return;
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
        selectByKey(ic);
        e.filterAndAccept();
        return;
      }
      send(ic, sym == FcitxKey_Escape ? "reset" : "raw");
      s->vMode = false;
      if (sym == FcitxKey_Escape) {
        s->preedit.clear();
        s->cursor = 0;
        clearDisplayState(s);
      }
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
    // punctuation correction. Numeric punctuation is displayed in
    // Chinese form first; only another digit changes it back to ASCII.
    const bool autoChangeActive = symbolAutoChange_ && !composing &&
                                  !s->englishPunctuation &&
                                  chinesePunctuation_ && !s->fullWidth;
    if (autoChangeActive && sym == FcitxKey_space &&
        (s->symbolAutoState || !s->pendingNumericText.empty())) {
      appendNumericCorrection(s, " ");
      s->symbolAutoState = 0;
      deliverNumericText(ic, " ");
      e.filterAndAccept();
      return;
    }
    if (!composing && digit && autoChangeActive) {
      const char replacement = numericReplacement(s->symbolAutoState);
      if (!s->pendingNumericText.empty()) {
        if (replacement) {
          const std::string previous = numericPunctuation(replacement);
          auto &text = s->pendingNumericReplacement;
          if (text.size() >= previous.size() &&
              text.compare(text.size() - previous.size(), previous.size(),
                           previous) == 0)
            text.replace(text.size() - previous.size(), previous.size(), 1,
                         replacement);
        }
        appendNumericCorrection(s, std::string(1, asciiChar));
      } else if (replacement) {
        const char *previous = numericPunctuation(replacement);
        // Wayland clients may acknowledge committed punctuation after the
        // next digit. Observe that exact suffix without delaying any keys.
        if ((ic->capabilityFlags().test(CapabilityFlag::SurroundingText) ||
             numericKeyTransport(ic)) &&
            !ic->capabilityFlags().test(CapabilityFlag::Sensitive) &&
            ic->surroundingText().isValid() &&
            ic->surroundingText().cursor() == ic->surroundingText().anchor()) {
          s->pendingNumericText = std::string(previous) + asciiChar;
          s->pendingNumericReplacement = std::string(1, replacement) + asciiChar;
          s->pendingNumericUntil = now(CLOCK_MONOTONIC) + 250000;
        }
      }
      s->symbolAutoState = 3;
      // Keep digits and punctuation on the same ordered text channel.
      // Raw key forwarding can otherwise overtake a Wayland commit.
      deliverNumericText(ic, std::string(1, asciiChar));
      if (!s->pendingNumericText.empty())
        scheduleNumericCorrection();
      e.filterAndAccept();
      return;
    } else if (!composing && (!entry || !autoChangeActive)) {
      s->symbolAutoState = 0;
    }

    // The original desktop treats apostrophe as a pinyin separator while a
    // composition exists. Outside a composition it follows the quote-pair
    // entry in punctuation.json.
    const bool separator = composing && sym == FcitxKey_apostrophe &&
                           *config_.input->mode != wetype_config::InputMode::Wubi;
    if (!composing && s->fullWidth &&
        (upper || digit || sym == FcitxKey_space)) {
      const uint32_t full = sym == FcitxKey_space ? 0x3000 : sym + 0xfee0;
      ic->commitString(utf8::UCS4ToUTF8(full));
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
      if (send(ic, "punctuation", std::string(1, asciiChar), 0, -1, -1, mapped,
               pairCursor) && pairCursor >= 0 && waylandV2(ic)) {
        // Keep later engine commits behind the paired cursor operation too,
        // including a burst typed before this asynchronous reply arrives.
        s->selectionPending = true;
      }
      e.filterAndAccept();
      return;
    }
    if (lower || upper || separator) {
      typeCharacter(asciiChar);
    }
  }
};
void Word::select(InputContext *ic) const {
  engine_->select(ic, index_, revision_);
}
void VMenuWord::select(InputContext *ic) const {
  engine_->selectVMenu(ic, index_, generation_, epoch_, session_);
}
class Factory : public AddonFactory {
  AddonInstance *create(AddonManager *m) override {
    return new WeType(m->instance());
  }
};
} // namespace
FCITX_ADDON_FACTORY(Factory)
