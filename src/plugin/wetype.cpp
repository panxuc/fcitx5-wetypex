#include "../common/appearance.hpp"
#include "../common/json.hpp"
#include "config.hpp"
#include "english_dictionary.hpp"
#include "wechat_emoji.hpp"
#include <algorithm>
#include <cerrno>
#include <cctype>
#include <clipboard_public.h>
#include <csignal>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <ctime>
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
#include <linux/input-event-codes.h>
#include <map>
#include <optional>
#include <poll.h>
#include <queue>
#include <set>
#include <spawn.h>
#include <spell_public.h>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <tuple>
#include <type_traits>
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
template <typename Owner, typename = void>
struct HasCustomImInfo : std::false_type {};
template <typename Owner>
struct HasCustomImInfo<Owner, std::void_t<decltype(
    std::declval<Owner &>().showCustomInputMethodInformation(
        nullptr, std::string{}))>>
    : std::true_type {};
template <typename Owner>
void showModeInfo(Owner *instance, InputContext *ic, const std::string &text) {
  if constexpr (HasCustomImInfo<Owner>::value)
    instance->showCustomInputMethodInformation(ic, text);
  else
    instance->showInputMethodInformation(ic);
}
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
  bool applicationForwarded = false;
};
enum class VMenu { None, Main, Clipboard, Hotwords, SymbolGroups, Symbols };
struct State : InputContextProperty {
  uint64_t id, epoch = 1, seq = 0, applied = 0, revision = 0, inputSeq = 0;
  uint64_t appliedUpdate = 0;
  uint64_t initialPanelDeadline = 0;
  uint64_t initialPanelInputSeq = 0;
  bool initialPanelShown = false;
  uint64_t cloudPollUntil = 0, lastCloudPoll = 0;
  int candidatePage = 0, candidateCursor = 0;
  wire::Json candidates, cloudFlags;
  std::string preedit;
  std::string englishWord;
  int englishCandidateCursor = 0;
  size_t cursor = 0;
  std::string displayPreedit;
  size_t displayCursor = 0, editableBegin = 0;
  std::vector<size_t> cursorStops;
  char pendingPairKey = 0;
  std::string pendingPairRight;
  uint8_t symbolAutoState = 0;
  bool validateNumericState = false;
  std::string numericPreview;
  bool doubleQuoteLeft = true, singleQuoteLeft = true;
  bool english = false, fullWidth = false, traditional = false,
       vMode = false, vCalculator = false;
  std::optional<bool> englishPunctuation;
  bool languageInitialized = false;
  uint32_t physicalModifiers = 0;
  std::set<KeySym> heldModifierKeys;
  uint8_t pressedShifts = 0;
  int pendingCursorSteps = 0;
  uint64_t cursorDeadline = 0;
  uint64_t cursorReplayAt = 0;
  int64_t cursorTarget = -1;
  std::string cursorExpected, cursorPrefix;
  size_t cursorViewHash = 0;
  unsigned cursorViewCursor = 0, cursorViewAnchor = 0;
  bool cursorHadView = false;
  bool cursorResetPending = false;
  bool cursorMoveReset = false;
  bool cursorViewAcknowledged = false;
  std::vector<DeferredKey> cursorKeys;
  std::vector<Key> forwardedKeys;
  bool pendingClientKey = false;
  std::string clientExpected;
  size_t clientTextHash = 0;
  unsigned clientCursor = 0, clientAnchor = 0;
  bool clientHadText = false, clientCursorReplay = false;
  bool clientRequireChange = false;
  bool clientResetPending = false;
  bool selectionPending = false;
  std::vector<DeferredKey> deferredKeys;
  VMenu vMenu = VMenu::None;
  uint64_t vMenuGeneration = 0;
  std::string vMenuTitle;
  std::vector<std::pair<std::string, std::string>> vMenuEntries;
  bool active = false;
  Key modifierCandidate;
  char32_t lastCommitTail = 0;
  size_t spacingTextHash = 0;
  unsigned spacingCursor = 0, spacingAnchor = 0;
  bool spacingHadSurrounding = false;
  uint64_t lastCommitAt = 0;
  // Whether this client has ever supplied a real surrounding-text view. Set
  // once and never cleared, so rules that need more than a neighbour can skip
  // themselves on frontends that only ever give the cached tail.
  bool sawWindow = false;
  TrackableObjectReference<InputContext> ic;
  State(uint64_t n, InputContext &context) : id(n), ic(context.watch()) {}
};
static void clearDisplayState(State *state) {
  state->candidates.reset();
  state->cloudFlags.reset();
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
class EnglishCompletionWord : public CandidateWord {
  WeType *engine_;
  std::string word_;

public:
  EnglishCompletionWord(WeType *engine, std::string word)
      : CandidateWord(Text(word)), engine_(engine), word_(std::move(word)) {}
  void select(InputContext *ic) const override;
  void select(InputContext *ic, bool trailingSpace) const;
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
class WeType : public InputMethodEngineV2 {
  friend class VMenuWord;
  Instance *instance_;
  wetype::WeChatEmoticons wechatEmoticons_{WETYPE_WECHAT_EMOTICONS};
  bool wechatFont_ = wetype_emoji::originalFontAvailable();
  uint64_t nextId_ = 0;
  FactoryFor<State> factory_;
  SimpleAction settingsAction_;
  Connection settingsConnection_;
  Connection commitFilterConnection_;
  bool spacingBypass_ = false;
  template <typename T> struct ScopedRestore {
    T &value;
    T previous;
    explicit ScopedRestore(T &target) : value(target), previous(target) {}
    ScopedRestore(const ScopedRestore &) = delete;
    ScopedRestore &operator=(const ScopedRestore &) = delete;
    ~ScopedRestore() { value = previous; }
  };
  struct TextBatch {
    TrackableObjectReference<InputContext> context;
    std::string text;
  };
  TextBatch *textBatch_ = nullptr;
  struct ReplayView {
    InputContext *context = nullptr;
    std::string text;
    unsigned cursor = 0;
  };
  ReplayView *replayView_ = nullptr;
  std::map<uint64_t, TrackableObjectReference<InputContext>> contexts_;
  pid_t child_ = -1;
  int read_ = -1, write_ = -1;
  std::unique_ptr<EventSourceIO> reader_, writer_;
  std::unique_ptr<EventSourceTime> timer_, cursorTimer_;
  std::unique_ptr<HandlerTableEntry<EventHandler>> languageModifierWatcher_;
  std::unique_ptr<HandlerTableEntry<EventHandler>> surroundingWatcher_;
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
  bool replayingKeys_ = false, replayingCursor_ = false;
  const KeyEvent *currentKeyEvent_ = nullptr;
  bool replayingForwarded_ = false;
  struct MaintenanceKey {
    TrackableObjectReference<InputContext> context;
    uint64_t epoch;
    DeferredKey event;
  };
  std::vector<MaintenanceKey> maintenanceKeys_;
  enum class RuntimePhase { Idle, Draining, Applying };
  RuntimePhase runtimePhase_ = RuntimePhase::Idle;
  pid_t runtimeWorker_ = -1, runtimeSync_ = -1;
  uint64_t runtimePollAt_ = 0, runtimeRetryAt_ = 0, lastKeyAt_ = 0;
  uint64_t runtimeDrainAt_ = 0;
  bool runtimePending_ = false;
  bool runtimeConfigApply_ = false;
  std::filesystem::path runtimeRoot() const {
    const char *data = getenv("XDG_DATA_HOME"), *home = getenv("HOME");
    return (data ? std::filesystem::path(data)
                 : std::filesystem::path(home ? home : "") / ".local/share") /
           "fcitx5-wetypex";
  }
  static bool waylandV2(InputContext *ic) {
    return std::string_view(ic->frontend()) == "wayland_v2";
  }
  static bool virtualKeyboardCursor(InputContext *ic) {
    // KWin uses the v1 frontend ("wayland"). Both native protocols and
    // GNOME's IBus bridge need ordering against the physical modifiers and
    // the client's asynchronous cursor updates.
    const std::string_view frontend = ic->frontend();
    return frontend == "wayland" || frontend == "wayland_v2" ||
           frontend == "ibus";
  }
  static bool legacyClientKeys(InputContext *ic) {
    return std::string_view(ic->frontend()) == "dbus" &&
           ic->capabilityFlags().test(CapabilityFlag::KeyEventOrderFix) &&
           !ic->capabilityFlags().test(CapabilityFlag::SurroundingText);
  }
  template <typename System>
  static bool modifiersHeld(System *instance, InputContext *ic, const State *s,
                             bool maintenance = false) {
    if constexpr (HasXkbMask<System>::value) {
      if (const auto mask = instance->xkbStateMask(ic->display()))
        return std::get<0>(*mask) || std::get<1>(*mask);
    }
    (void)instance;
    (void)ic;
    // Old frameworks expose no live mask. A modifier reported on a character
    // event may remain stale after a compositor-only modifiers update.
    return maintenance ? !s->heldModifierKeys.empty() : s->physicalModifiers != 0;
  }
  static bool samePhysicalKey(const Key &a, const Key &b) {
    return a.code() && b.code() ? a.code() == b.code() : a.sym() == b.sym();
  }
  static bool releaseForwardedKey(InputContext *ic, State *s, const Key &key) {
    const auto it = std::find_if(
        s->forwardedKeys.begin(), s->forwardedKeys.end(),
        [&](const Key &pressed) { return samePhysicalKey(pressed, key); });
    if (it == s->forwardedKeys.end())
      return false;
    const auto pressed = *it;
    s->forwardedKeys.erase(it);
    ic->forwardKey(Key(key.sym(), key.states(), pressed.code()), true);
    return true;
  }
  static void releaseForwardedKeys(InputContext *ic, State *s) {
    auto keys = std::move(s->forwardedKeys);
    s->forwardedKeys.clear();
    for (auto it = keys.rbegin(); it != keys.rend(); ++it)
      ic->forwardKey(
          Key(it->sym(), KeyStates(s->physicalModifiers), it->code()), true);
  }
  void forwardReplayedKey(InputContext *ic, const Key &key, bool release,
                          int time) {
    if (std::string_view(ic->frontend()) == "ibus") {
      auto *s = state(ic);
      if (release && releaseForwardedKey(ic, s, key))
        return;
      if (!release &&
          std::none_of(s->forwardedKeys.begin(), s->forwardedKeys.end(),
                       [&](const Key &pressed) {
                         return samePhysicalKey(pressed, key);
                       }))
        s->forwardedKeys.push_back(key);
    }
    ic->forwardKey(key, release, time);
  }
  static void clearCursorMove(State *s) {
    s->pendingCursorSteps = 0;
    s->pendingClientKey = false;
    s->clientExpected.clear();
    s->clientCursorReplay = false;
    s->clientRequireChange = false;
    s->clientResetPending = false;
    s->cursorDeadline = 0;
    s->cursorReplayAt = 0;
    s->cursorTarget = -1;
    s->cursorExpected.clear();
    s->cursorPrefix.clear();
    s->cursorHadView = false;
    s->cursorResetPending = false;
    s->cursorMoveReset = false;
    s->cursorViewAcknowledged = false;
    s->cursorKeys.clear();
  }
  void scheduleCursorMove() {
    if (!cursorTimer_)
      return;
    cursorTimer_->setTime(now(CLOCK_MONOTONIC) + 1000);
    cursorTimer_->setEnabled(true);
  }
  // Native application keys and IM commits can use separate client queues.
  // Prefer its text acknowledgement; clients without one get a client turn.
  void waitForClient(InputContext *ic, State *s,
                     const std::string &expected = {},
                     bool cursorReplay = false, bool requireChange = false) {
    flushTextBatch(ic);
    const auto &text = ic->surroundingText();
    s->pendingClientKey = true;
    s->clientExpected = expected;
    s->clientCursorReplay = cursorReplay;
    s->clientRequireChange = requireChange;
    s->clientResetPending = false;
    s->clientHadText =
        ic->capabilityFlags().test(CapabilityFlag::SurroundingText) &&
        text.isValid();
    s->clientCursor = text.cursor();
    s->clientAnchor = text.anchor();
    s->clientTextHash = std::hash<std::string>{}(text.text());
    s->cursorReplayAt =
        now(CLOCK_MONOTONIC) + (expected.empty() ? 16000 : 250000);
    scheduleCursorMove();
  }
  void forwardCursor(InputContext *ic, State *s, int steps,
                     const std::string &expected) {
    if (!steps)
      return;
    const auto prefix = textBatch_ && textBatch_->context.get() == ic
                            ? textBatch_->text
                            : std::string{};
    const auto &view = ic->surroundingText();
    const bool hadView =
        ic->capabilityFlags().test(CapabilityFlag::SurroundingText);
    const auto viewCursor = view.cursor(), viewAnchor = view.anchor();
    const auto viewHash = std::hash<std::string>{}(view.text());
    flushTextBatch(ic);
    clearSpacing(s);
    // Order native cursor events after the client applies the insertion.
    // Virtual-keyboard protocols also need the physical modifiers released;
    // Wayland v1 keysym events carry their own modifier mask.
    if (virtualKeyboardCursor(ic) || legacyClientKeys(ic)) {
      s->pendingClientKey = false;
      s->clientExpected.clear();
      s->pendingCursorSteps = steps;
      s->cursorExpected = expected;
      s->cursorPrefix = prefix;
      s->cursorHadView = hadView;
      s->cursorViewCursor = viewCursor;
      s->cursorViewAnchor = viewAnchor;
      s->cursorViewHash = viewHash;
      s->cursorDeadline = 0;
      s->cursorReplayAt = 0;
      s->cursorTarget = -1;
      s->cursorResetPending = s->cursorMoveReset = false;
      s->cursorViewAcknowledged = steps > 0 && prefix.empty();
      scheduleCursorMove();
      return;
    }
    emitCursor(ic, steps);
  }
  static void emitCursor(InputContext *ic, int steps) {
    const bool ibus = std::string_view(ic->frontend()) == "ibus";
    // GNOME forwards IBus events through a Wayland virtual keyboard, which
    // needs a real keycode, not only a keysym. Fcitx's IBus frontend subtracts
    // the XKB offset (8) before sending the Linux evdev code to the client.
    const auto code = ibus ? (steps < 0 ? KEY_LEFT : KEY_RIGHT) + 8 : 0;
    const Key key(steps < 0 ? FcitxKey_Left : FcitxKey_Right, KeyStates(), code);
    for (int i = 0; i < std::abs(steps); ++i) {
      ic->forwardKey(key);
      if (ibus)
        ic->forwardKey(key, true);
    }
  }
  void recordReplayCommit(InputContext *ic, const std::string &text) {
    if (!replayView_ || replayView_->context != ic)
      return;
    auto &view = *replayView_;
    const auto length = text.empty() ? 0 : utf8::lengthValidated(text);
    if (length == utf8::INVALID_LENGTH) {
      view.context = nullptr;
      return;
    }
    const auto byte = view.cursor
                          ? utf8::ncharByteLength(view.text.begin(), view.cursor)
                          : 0;
    view.text.insert(size_t(byte), text);
    view.cursor += length;
  }
  void commitText(InputContext *ic, const std::string &text) {
    if (!textBatch_ || textBatch_->context.get() != ic) {
      recordReplayCommit(ic, text);
      ic->commitString(text);
      return;
    }
    auto *s = state(ic);
    auto value = text;
    if (!spacingBypass_ && autoSpaceEnabled(ic, s) && utf8::validate(value))
      value =
          applyAutoSpace(value, textBatch_->text.empty() ? beforeWindow(ic, s)
                                                         : textBatch_->text);
    textBatch_->text += value;
    recordReplayCommit(ic, value);
  }
  void flushTextBatch(InputContext *ic) {
    if (!textBatch_ || textBatch_->context.get() != ic ||
        textBatch_->text.empty())
      return;
    auto text = std::move(textBatch_->text);
    textBatch_->text.clear();
    auto *s = state(ic);
    if (!s->active || !ic->hasFocus())
      return;
    ScopedRestore<bool> restore(spacingBypass_);
    spacingBypass_ = true; // Each fragment was formatted when queued.
    if (!text.empty()) {
      ic->commitString(text);
      rememberSpacing(ic, s, lastScalar(text));
    }
  }
  void replayDeferredKeys(InputContext *ic, std::vector<DeferredKey> keys,
                          bool cursorReplay = false) {
    flushTextBatch(ic);
    TextBatch batch{ic->watch(), {}};
    ScopedRestore<TextBatch *> restoreBatch(textBatch_);
    ScopedRestore<bool> restoreReplay(replayingKeys_);
    ScopedRestore<bool> restoreForwarded(replayingForwarded_);
    ScopedRestore<bool> restoreCursor(replayingCursor_);
    replayingCursor_ = cursorReplay;
    // A virtual-keyboard replay can produce several immediate text commits.
    // Publish them together so GNOME does not keep only the last digit.
    if (cursorReplay && virtualKeyboardCursor(ic))
      textBatch_ = &batch;
    replayingKeys_ = true;
    const auto &ref = batch.context;
    for (const auto &key : keys) {
      auto *target = ref.get();
      if (!target || !target->hasFocus() ||
          !target->propertyFor(&factory_)->active ||
          target->capabilityFlags().test(CapabilityFlag::Password))
        break;
      replayingForwarded_ = key.applicationForwarded;
      KeyEvent event(target, key.key, key.release, key.time);
      const bool handled = target->keyEvent(event);
      if (key.release && handled &&
          std::string_view(target->frontend()) == "ibus")
        releaseForwardedKey(target, state(target), key.key);
      if (!handled && !key.applicationForwarded) {
        const auto scalar = Key::keySymToUnicode(key.key.sym());
        const auto commands = key.key.states().toInteger() &
                              uint32_t(KeyState::SimpleMask) &
                              ~uint32_t(KeyState::Shift);
        if (cursorReplay && !commands && textBatch_ == &batch &&
            !batch.text.empty() &&
            (key.key.sym() == FcitxKey_BackSpace ||
             key.key.sym() == FcitxKey_Delete)) {
          if (!key.release) {
            if (key.key.sym() == FcitxKey_BackSpace) {
              const auto length = utf8::length(batch.text);
              if (replayView_ && replayView_->context == target && replayView_->cursor) {
                auto &view = *replayView_;
                const auto last = utf8::ncharByteLength(view.text.begin(), view.cursor);
                const auto first = utf8::ncharByteLength(view.text.begin(), view.cursor - 1);
                view.text.erase(size_t(first), size_t(last - first));
                --view.cursor;
              }
              batch.text.resize(
                  utf8::ncharByteLength(batch.text.begin(), length - 1));
            } else {
              // Apply the buffered insertion first. Forward Delete only
              // after the client acknowledges that caret, then let the
              // application perform its ordinary edit and undo handling.
              const auto expected = batch.text;
              flushTextBatch(target);
              waitForClient(target, state(target),
                            target->capabilityFlags().test(
                                CapabilityFlag::SurroundingText)
                                ? expected
                                : std::string{});
              state(target)->cursorKeys.push_back(key);
              continue;
            }
            state(target)->symbolAutoState = !batch.text.empty() &&
                                                     batch.text.back() >= '0' &&
                                                     batch.text.back() <= '9'
                                                 ? 3
                                                 : 0;
          }
        } else if (cursorReplay && virtualKeyboardCursor(target) && !commands &&
                   scalar >= 0x20 && scalar != 0x7f) {
          // The original uppercase symbol must survive replay after Shift
          // has been released; native forwardKey cannot restore its mask.
          if (!key.release)
            commitText(target, utf8::UCS4ToUTF8(scalar));
        } else {
          flushTextBatch(target);
          // A deferred chord may outlive the physical Shift release. Restore
          // it for this one key and release it in the same operation.
          const bool restoreShift =
              !key.release && !key.key.isModifier() &&
              virtualKeyboardCursor(target) &&
              key.key.states().test(KeyState::Shift) &&
              !(state(target)->physicalModifiers & uint32_t(KeyState::Shift));
          const Key shift(
              FcitxKey_Shift_L, key.key.states().unset(KeyState::Shift),
              std::string_view(target->frontend()) == "ibus" ? KEY_LEFTSHIFT + 8
                                                             : 0);
          if (restoreShift)
            target->forwardKey(shift);
          forwardReplayedKey(target, key.key, key.release, key.time);
          if (restoreShift)
            target->forwardKey(shift, true);
          if (!key.release && !key.key.isModifier() &&
              (commands || scalar <= 0x20 || scalar == 0x7f) &&
              !state(target)->pendingClientKey)
            waitForClient(target, state(target));
        }
      }
    }
    if (auto *target = ref.get())
      flushTextBatch(target);
  }
  bool resolveCursorMove(InputContext *ic, State *s) {
    if (!s->pendingCursorSteps && !s->pendingClientKey)
      return false;
    if (!s->active || !ic->hasFocus() ||
        ic->capabilityFlags().test(CapabilityFlag::Password) ||
        ic->capabilityFlags().test(CapabilityFlag::Sensitive)) {
      releaseForwardedKeys(ic, s);
      clearCursorMove(s);
      return false;
    }
    const auto time = now(CLOCK_MONOTONIC);
    const auto &surrounding = ic->surroundingText();
    if (s->pendingClientKey) {
      const bool viewChanged =
          s->clientHadText && surrounding.isValid() &&
          (surrounding.cursor() != s->clientCursor ||
           surrounding.anchor() != s->clientAnchor ||
           std::hash<std::string>{}(surrounding.text()) != s->clientTextHash);
      const bool acknowledged =
          !s->clientExpected.empty()
              ? previousTextIs(ic, s->clientExpected) &&
                    (!s->clientRequireChange || viewChanged)
              : viewChanged;
      if (!acknowledged && time < s->cursorReplayAt)
        return true;
      if (!acknowledged && s->clientResetPending) {
        clearCursorMove(s);
        return false;
      }
      auto keys = std::move(s->cursorKeys);
      const auto cursorReplay = s->clientCursorReplay;
      clearCursorMove(s);
      replayDeferredKeys(ic, std::move(keys), cursorReplay);
      return s->pendingCursorSteps || s->pendingClientKey;
    }
    if (s->cursorReplayAt) {
      const auto context = symbolContext(ic);
      const bool sameText = s->cursorHadView &&
          context.valid && context.selected.empty() &&
          std::hash<std::string>{}(surrounding.text()) == s->cursorViewHash;
      const bool acknowledged = sameText && s->cursorTarget >= 0 &&
          int64_t(surrounding.cursor()) == s->cursorTarget;
      if (!acknowledged && time < s->cursorReplayAt)
        return true;
      if (!acknowledged && s->cursorHadView && !s->cursorMoveReset &&
          time < s->cursorDeadline)
        return true;
      ReplayView moved;
      if (acknowledged) {
        moved = {ic, surrounding.text(), surrounding.cursor()};
      } else if (s->cursorMoveReset && sameText && s->cursorTarget >= 0 &&
          surrounding.cursor() == s->cursorViewCursor &&
          uint64_t(s->cursorTarget) <= utf8::length(surrounding.text())) {
        // Some GTK clients reset after our Left key, but only publish that
        // new caret on the next physical key. Scope the confirmed text and
        // our own ordered movement to this replay; never alter the client's
        // surrounding-text cache or infer context for an opaque client.
        moved = {ic, surrounding.text(), unsigned(s->cursorTarget)};
      } else if (!acknowledged && s->cursorResetPending && s->cursorHadView) {
        clearCursorMove(s);
        return false;
      }
      auto keys = std::move(s->cursorKeys);
      clearCursorMove(s);
      ScopedRestore<ReplayView *> restoreView(replayView_);
      if (moved.context)
        replayView_ = &moved;
      replayDeferredKeys(ic, std::move(keys), true);
      return s->pendingCursorSteps || s->pendingClientKey;
    }
    // Only virtual keyboards inherit the physical modifier mask. Wayland
    // v1 and Qt D-Bus cursor events carry an explicit empty mask, including
    // the Qt fallback used before a blank document has surrounding text.
    const std::string_view frontend = ic->frontend();
    if ((frontend == "wayland_v2" || frontend == "ibus") &&
        modifiersHeld(instance_, ic, s))
      return true;
    if (!s->cursorDeadline)
      s->cursorDeadline = time + 250000;
    // Matching the old closing bracket is not an acknowledgement of the
    // buffered insertion before it. Confirm that insertion before moving.
    const bool viewChanged =
        !s->cursorHadView || !surrounding.isValid() ||
        surrounding.cursor() != s->cursorViewCursor ||
        surrounding.anchor() != s->cursorViewAnchor ||
        std::hash<std::string>{}(surrounding.text()) != s->cursorViewHash;
    const bool prefixApplied =
        !s->cursorHadView ||
        !ic->capabilityFlags().test(CapabilityFlag::SurroundingText) ||
        (surrounding.isValid() &&
         (s->cursorPrefix.empty() || previousTextIs(ic, s->cursorPrefix)) &&
         ((s->pendingCursorSteps >= 0 && s->cursorPrefix.empty()) ||
          viewChanged));
    if (!prefixApplied && time < s->cursorDeadline)
      return true;
    // Terminals can support native input without exposing surrounding text.
    // With no text to compare, preserve the ordered cursor-key fallback.
    const bool hasText =
        ic->capabilityFlags().test(CapabilityFlag::SurroundingText) &&
        surrounding.isValid() && !surrounding.text().empty();
    bool matches = prefixApplied &&
                   (!hasText || surrounding.cursor() == surrounding.anchor());
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
      s->cursorHadView = hasText;
      s->cursorViewHash = std::hash<std::string>{}(surrounding.text());
      s->cursorViewCursor = surrounding.cursor();
      s->cursorMoveReset = false;
      s->cursorViewAcknowledged = false;
      s->cursorDeadline = time + 250000;
      emitCursor(ic, steps);
      // Cursor keys and IM commits use different Wayland protocols. Give
      // the client a turn to apply the key before replaying nested pairs or
      // text. Surrounding-text acknowledgement can finish this earlier.
      s->cursorTarget = hasText ? int64_t(surrounding.cursor()) + steps : -1;
      s->cursorReplayAt = time + 16000;
      return true;
    }
    auto keys = std::move(s->cursorKeys);
    const bool reset = s->cursorResetPending;
    clearCursorMove(s);
    if (!reset)
      replayDeferredKeys(ic, std::move(keys), true);
    return s->pendingCursorSteps || s->pendingClientKey;
  }
  void acknowledgeCursorView(InputContext *ic, State *s) {
    if (!s->pendingCursorSteps || replayView_)
      return;
    const auto context = symbolContext(ic);
    if (!context.valid || !context.selected.empty())
      return;
    const auto &view = ic->surroundingText();
    const auto hash = std::hash<std::string>{}(view.text());
    if (s->cursorReplayAt) {
      if (s->cursorTarget >= 0 && int64_t(view.cursor()) == s->cursorTarget &&
          hash == s->cursorViewHash)
        s->cursorViewAcknowledged = true;
    } else if (!s->cursorExpected.empty()) {
      const bool changed = view.cursor() != s->cursorViewCursor ||
                           view.anchor() != s->cursorViewAnchor ||
                           hash != s->cursorViewHash;
      const bool matches = s->pendingCursorSteps < 0
                               ? previousTextIs(ic, s->cursorExpected)
                               : nextSurroundingTextIs(ic, s->cursorExpected) &&
                                     (s->cursorPrefix.empty() ||
                                      previousTextIs(ic, s->cursorPrefix));
      if (matches &&
          (changed || (s->pendingCursorSteps > 0 && s->cursorPrefix.empty())))
        s->cursorViewAcknowledged = true;
    }
  }
  void startSync() {
    if (runtimePhase_ != RuntimePhase::Idle)
      return;
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
  wire::Json engineSettings_;
  using IdentityStamp =
      std::tuple<dev_t, ino_t, off_t, time_t, long, time_t, long>;
  std::optional<IdentityStamp> backendIdentity_;
  std::optional<IdentityStamp> identityStamp() const {
    const char *directory = getenv("WETYPE_ACCOUNT_STATE_DIR");
    const auto path = (directory ? std::filesystem::path(directory)
                                 : runtimeRoot() / "account") / "identity.json";
    struct stat info{};
    if (stat(path.c_str(), &info) == 0)
      return IdentityStamp{info.st_dev, info.st_ino, info.st_size,
                           info.st_mtim.tv_sec, info.st_mtim.tv_nsec,
                           info.st_ctim.tv_sec, info.st_ctim.tv_nsec};
    if (errno == ENOENT)
      return IdentityStamp{};
    return std::nullopt;
  }
  bool frontendOnly(const wetype_config::WeTypeConfig &before) const {
    if (before == config_ || child_ <= 0 || failed_ ||
        runtimePhase_ != RuntimePhase::Idle || !backendIdentity_ ||
        backendIdentity_ != identityStamp())
      return false;
    auto comparable = config_;
    auto *appearance = comparable.appearance.mutableValue();
    appearance->pageSize.setValue(*before.appearance->pageSize);
    appearance->vertical.setValue(*before.appearance->vertical);
    appearance->preeditPosition.setValue(*before.appearance->preeditPosition);
    appearance->labelStyle.setValue(*before.appearance->labelStyle);
    appearance->candidateSize.setValue(*before.appearance->candidateSize);
    appearance->theme.setValue(*before.appearance->theme);
    auto *input = comparable.input.mutableValue();
    input->chinesePunctuation.setValue(*before.input->chinesePunctuation);
    input->rememberPunctuation.setValue(*before.input->rememberPunctuation);
    comparable.voice.mutableValue()->provider.setValue(*before.voice->provider);
    comparable.voice.mutableValue()->localModel.setValue(*before.voice->localModel);
    return comparable == before;
  }
  void refreshPresentation() {
    for (const auto &[id, ref] : contexts_)
      if (auto *ic = ref.get()) {
        auto *s = ic->propertyFor(&factory_);
        if (!s->active || ic->capabilityFlags().test(CapabilityFlag::Password))
          continue;
        s->candidatePage = s->candidateCursor / pageSize_;
        if (s->vMenu != VMenu::None)
          updateVMenu(ic, s);
        else
          updateCandidates(ic, s);
        panel(ic, s);
        ic->updateUserInterface(UserInterfaceComponent::StatusArea);
      }
  }
  std::optional<wetype::EnglishDictionary> englishDictionary_;
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
  bool nextSurroundingTextIs(InputContext *ic,
                             const std::string &expected) const {
    const auto context = symbolContext(ic);
    return context.valid && context.selected.empty() &&
           context.after.compare(0, expected.size(), expected) == 0;
  }
  struct SymbolContext {
    bool valid = false;
    std::string_view before, selected, after;
  };
  SymbolContext symbolContext(InputContext *ic) const {
    const auto &view = ic->surroundingText();
    const bool replay = replayView_ && replayView_->context == ic;
    if (!ic->capabilityFlags().test(CapabilityFlag::SurroundingText) ||
        (!replay && !view.isValid()))
      return {};
    const auto cursor = replay ? replayView_->cursor : view.cursor();
    const auto anchor = replay ? cursor : view.anchor();
    const auto &value = replay ? replayView_->text : view.text();
    const auto begin = std::min(cursor, anchor);
    const auto end = std::max(cursor, anchor);
    const auto length = value.empty() ? 0 : utf8::lengthValidated(value);
    if (length == utf8::INVALID_LENGTH || end > length)
      return {};
    const auto first = begin ? utf8::ncharByteLength(value.begin(), begin) : 0;
    const auto last = end ? utf8::ncharByteLength(value.begin(), end) : 0;
    const std::string_view text(value);
    return {true, text.substr(0, first), text.substr(first, last - first),
            text.substr(last)};
  }
  static bool symbolWhitespace(char32_t c) {
    // Foundation CharacterSet.whitespacesAndNewlines, used by the original
    // isCursorEndOrSelected before enabling automatic symbol pairing.
    return (c >= 0x9 && c <= 0xd) || c == 0x20 || c == 0x85 || c == 0xa0 ||
           c == 0x1680 || (c >= 0x2000 && c <= 0x200a) || c == 0x2028 ||
           c == 0x2029 || c == 0x202f || c == 0x205f || c == 0x3000;
  }
  static bool symbolPairPosition(const SymbolContext &context) {
    return !context.valid || !context.selected.empty() || context.after.empty() ||
           symbolWhitespace(utf8::getChar(context.after.begin(), context.after.end()));
  }
  static bool quoteOpens(const SymbolContext &context, const std::string &left,
                         const std::string &right, bool fallback) {
    if (!context.valid)
      return fallback;
    // Original CharMapHelper searches backwards on the current line. An
    // existing opening quote selects the closing form, which is inserted
    // literally even when an automatically inserted closer follows it.
    const auto opening = utf8::getChar(left.begin(), left.end());
    const auto closing = utf8::getChar(right.begin(), right.end());
    for (size_t end = context.before.size(); end;) {
      auto begin = end - 1;
      while (begin && (static_cast<unsigned char>(context.before[begin]) & 0xc0) == 0x80)
        --begin;
      const auto c = utf8::getChar(context.before.begin() + begin,
                                   context.before.begin() + end);
      if ((c >= 0xa && c <= 0xd) || c == 0x85 || c == 0x2028 || c == 0x2029)
        return true;
      // outputRightOfPair in the original retains a pending single opening
      // quote. A new line overrides it; selections use that pending state
      // without searching the text being replaced.
      if (!context.selected.empty() || !fallback)
        return fallback;
      else if (c == closing)
        return true;
      else if (c == opening)
        return false;
      end = begin;
    }
    return true;
  }
  // Confirm a committed suffix at the actual, unselected caret.
  bool previousTextIs(InputContext *ic, const std::string &expected) const {
    const auto context = symbolContext(ic);
    return context.valid && context.selected.empty() &&
           context.before.size() >= expected.size() &&
           context.before.compare(context.before.size() - expected.size(),
                                  expected.size(), expected) == 0;
  }
  static bool numericKeyTransport(InputContext *ic) {
    // KeyEventOrderFix also appears in GTK clients such as Chromium, whose
    // GTK bridge cannot insert forwarded Unicode punctuation. Keep clients
    // without surrounding-text support on the commit path for Unicode.
    return std::string_view(ic->frontend()) == "dbus" &&
           ic->capabilityFlags().test(CapabilityFlag::KeyEventOrderFix) &&
           ic->capabilityFlags().test(CapabilityFlag::SurroundingText);
  }
  void deliverNumericText(InputContext *ic, State *s,
                          const std::string &text) {
    // Preserve the original native key and its layout/modifier state for
    // ASCII. A manufactured colon without Shift becomes a semicolon in GTK.
    const bool orderedAscii =
        (legacyClientKeys(ic) || numericKeyTransport(ic)) && currentKeyEvent_ &&
        currentKeyEvent_->inputContext() == ic && text.size() == 1 &&
        static_cast<unsigned char>(text.front()) >= 0x20 &&
        static_cast<unsigned char>(text.front()) <= 0x7e &&
        Key::keySymToUnicode(currentKeyEvent_->rawKey().sym()) ==
            static_cast<unsigned char>(text.front());
    if (!numericKeyTransport(ic) && !orderedAscii) {
      // Forwarded keys bypass Instance::CommitFilter, so only this branch
      // needs to place the space itself; commitString is filtered upstream.
      commitText(ic, text);
    } else {
      std::string payload = text;
      if (s && autoSpaceEnabled(ic, s) && utf8::validate(text))
        payload = applyAutoSpace(text, beforeWindow(ic, s));
      // A formatted gap has no matching physical key. Commit the complete
      // insertion, allowing the normal commit filter to format it once.
      if (orderedAscii && payload != text) {
        commitText(ic, text);
        return;
      }
      if (orderedAscii) {
        ic->forwardKey(currentKeyEvent_->rawKey());
        ic->forwardKey(currentKeyEvent_->rawKey(), true);
      } else {
        // Qt's D-Bus client can temporarily discard its surrounding-text cache
        // while still reporting the document. Keep this path on ordered keys.
        std::string_view remaining(payload);
        while (!remaining.empty()) {
          const auto scalar = utf8::getChar(remaining.begin(), remaining.end());
          const Key key(Key::keySymFromUnicode(scalar));
          ic->forwardKey(key);
          ic->forwardKey(key, true);
          remaining.remove_prefix(firstUtf8Size(remaining));
        }
      }
      if (s && !text.empty()) {
        rememberSpacing(ic, s, lastScalar(payload));
        if (legacyClientKeys(ic))
          waitForClient(ic, s, {}, replayingCursor_);
      }
    }
  }
  void flushNumericPreview(InputContext *ic, State *s, bool commit = true) {
    if (s->numericPreview.empty())
      return;
    auto text = std::move(s->numericPreview);
    s->numericPreview.clear();
    s->symbolAutoState = 0;
    ic->inputPanel().setClientPreedit(Text());
    ic->updatePreedit();
    if (commit)
      commitText(ic, text);
  }
  void finishNumericPreviews() {
    for (const auto &[id, ref] : contexts_)
      if (auto *ic = ref.get()) {
        auto *s = ic->propertyFor(&factory_);
        flushNumericPreview(ic, s, s->active && ic->hasFocus() &&
            !ic->capabilityFlags().test(CapabilityFlag::Password) &&
            !ic->capabilityFlags().test(CapabilityFlag::Sensitive));
      }
  }
  std::string mappedSymbol(State *s, char ascii, bool asciiMode) const {
    const auto *entry = punctuationEntry(ascii);
    if (!entry)
      return std::string(1, ascii);
    // Original CharMapHelper chooses full-width symbols by input language.
    // The punctuation preference applies only in the default character form.
    if (s->fullWidth)
      return asciiMode ? entry->englishFull : entry->full;
    const bool useEnglish = asciiMode || englishPunctuation(s);
    if (ascii == '/' && slashPunctuation_ && !useEnglish)
      return "、";
    if (useEnglish)
      return entry->englishHalf;
    return entry->normal;
  }
  static bool isCjk(char32_t c) {
    return c == 0x3007 || (c >= 0x3400 && c <= 0x4dbf) ||
           (c >= 0x4e00 && c <= 0x9fff) || (c >= 0xf900 && c <= 0xfaff) ||
           (c >= 0x20000 && c <= 0x2ee5f) || (c >= 0x2f800 && c <= 0x2fa1f) ||
           (c >= 0x30000 && c <= 0x323af);
  }
  bool isNeedSpace(char32_t previous, char32_t current) const {
    if (isCjk(previous) == isCjk(current))
      return false;
    const auto ascii = isCjk(previous) ? current : previous;
    if ((ascii >= 'a' && ascii <= 'z') || (ascii >= 'A' && ascii <= 'Z'))
      return *config_.appearance->autoSpaceEnglish;
    return ascii >= '0' && ascii <= '9' && *config_.appearance->autoSpaceDigit;
  }
  bool autoSpaceEnabled(InputContext *ic, State *s) const {
    constexpr uint64_t constrained = uint64_t(CapabilityFlag::Password) |
        uint64_t(CapabilityFlag::Sensitive) | uint64_t(CapabilityFlag::Url) |
        uint64_t(CapabilityFlag::Email) | uint64_t(CapabilityFlag::Number) |
        uint64_t(CapabilityFlag::Digit) | uint64_t(CapabilityFlag::Dialable);
    return *config_.appearance->autoSpace && !spacingBypass_ && !s->fullWidth &&
           !(ic->capabilityFlags().toInteger() & constrained);
  }
  static void clearSpacing(State *s) {
    s->lastCommitTail = 0;
    s->lastCommitAt = 0;
    s->spacingHadSurrounding = false;
  }
  static char32_t lastScalar(const std::string &text) {
    if (text.empty())
      return 0;
    size_t at = text.size() - 1;
    while (at && (static_cast<unsigned char>(text[at]) & 0xc0) == 0x80)
      --at;
    const auto scalar = utf8::getChar(text.begin() + at, text.end());
    return utf8::isValidChar(scalar) ? scalar : 0;
  }
  static char32_t firstScalar(const std::string &text) {
    if (text.empty())
      return 0;
    const auto scalar = utf8::getChar(text);
    return utf8::isValidChar(scalar) ? scalar : 0;
  }
  static char32_t lastScalarBefore(const std::string &text, size_t cursor) {
    if (text.empty())
      return 0;
    const auto bytes = utf8::ncharByteLength(text.begin(), std::min(cursor, utf8::length(text)));
    return bytes > 0 && size_t(bytes) <= text.size() ? lastScalar(text.substr(0, bytes)) : 0;
  }
  void rememberSpacing(InputContext *ic, State *s, char32_t tail) {
    if (!*config_.appearance->autoSpace) {
      clearSpacing(s);
      return;
    }
    s->lastCommitTail = tail;
    s->lastCommitAt = now(CLOCK_MONOTONIC);
    const auto &surrounding = ic->surroundingText();
    s->spacingHadSurrounding = surrounding.isValid();
    if (surrounding.isValid()) {
      s->sawWindow = true;
      s->spacingTextHash = std::hash<std::string>{}(surrounding.text());
      s->spacingCursor = surrounding.cursor();
      s->spacingAnchor = surrounding.anchor();
    }
  }
  std::string beforeWindow(InputContext *ic, State *s) const {
    const auto &surrounding = ic->surroundingText();
    const bool recent = s->lastCommitTail && now(CLOCK_MONOTONIC) - s->lastCommitAt < 10000000;
    if (surrounding.isValid()) {
      s->sawWindow = true;
      // Until the client reports our last commit, its document still describes
      // the preceding state. Reuse the predicted tail only for that exact view.
      if (recent && s->spacingHadSurrounding && surrounding.cursor() == surrounding.anchor() &&
          surrounding.cursor() == s->spacingCursor && surrounding.anchor() == s->spacingAnchor &&
          std::hash<std::string>{}(surrounding.text()) == s->spacingTextHash)
        return utf8::UCS4ToUTF8(s->lastCommitTail);
      const auto &text = surrounding.text();
      const auto cursor = std::min(surrounding.cursor(), surrounding.anchor());
      if (!utf8::validate(text) || cursor > utf8::length(text))
        return {};
      const auto tail = lastScalarBefore(text, cursor);
      return tail ? utf8::UCS4ToUTF8(tail) : std::string{};
    }
    return !s->sawWindow && recent ? utf8::UCS4ToUTF8(s->lastCommitTail) : std::string{};
  }
  std::string applyAutoSpace(const std::string &text, const std::string &before) const {
    std::string output;
    output.reserve(text.size() + 8);
    char32_t previous = lastScalar(before);
    for (size_t offset = 0; offset < text.size();) {
      const auto width = std::min(firstUtf8Size(std::string_view(text).substr(offset)), text.size() - offset);
      const auto current = utf8::getChar(text.begin() + offset, text.end());
      if (previous && isNeedSpace(previous, current))
        output += ' ';
      output.append(text, offset, width);
      previous = current;
      offset += width;
    }
    return output;
  }
  void trackPassedKey(InputContext *ic, State *s, const KeyEvent &event) {
    // Remember only physical presses that Wayland v2 will pass to the
    // client. Buffered presses are committed as text and own no held key.
    if (waylandV2(ic) && !replayingKeys_ && !event.isRelease() &&
        !event.key().isModifier() && !event.filtered() && !event.accepted() &&
        std::none_of(s->forwardedKeys.begin(), s->forwardedKeys.end(),
                     [&](const Key &key) {
                       return samePhysicalKey(key, event.rawKey());
                     }))
      s->forwardedKeys.push_back(event.rawKey());
    const auto modifiers = event.key().states().toInteger() &
                           uint32_t(KeyState::SimpleMask) &
                           ~uint32_t(KeyState::Shift);
    const auto scalar = Key::keySymToUnicode(event.key().sym());
    if (!event.isRelease() && !event.key().isModifier() && !event.filtered() &&
        !event.accepted() && (modifiers || scalar < 0x20 || scalar == 0x7f)) {
      s->symbolAutoState = 0;
    }
    if (!event.isRelease() && !event.key().isModifier() && !event.filtered() &&
        !event.accepted() && !replayingForwarded_ && legacyClientKeys(ic) &&
        (modifiers || scalar <= 0x20 || scalar == 0x7f))
      waitForClient(ic, s);
    if (!*config_.appearance->autoSpace) {
      clearSpacing(s);
      return;
    }
    if (event.isRelease() || event.key().isModifier() || event.filtered() || event.accepted())
      return;
    // A key handled by the application may edit or move its caret. Printable
    // keys without command modifiers are the only pass-through text we know.
    if (!modifiers && scalar >= 0x20 && scalar != 0x7f)
      rememberSpacing(ic, s, scalar);
    else
      clearSpacing(s);
  }
  struct PassedKeyTracker {
    WeType *owner;
    InputContext *ic;
    State *state;
    const KeyEvent &event;
    ~PassedKeyTracker() { owner->trackPassedKey(ic, state, event); }
  };
  template <typename Context, typename Flag = CapabilityFlag>
  void commitStringAtCursor(Context *ic, const std::string &text,
                                   size_t cursor) {
    if (textBatch_ && textBatch_->context.get() == ic &&
        !textBatch_->text.empty()) {
      if (replayView_ && replayView_->context == ic) {
        auto &view = *replayView_;
        const auto byte = utf8::ncharByteLength(view.text.begin(), view.cursor);
        const auto &prefix = textBatch_->text;
        if (size_t(byte) >= prefix.size() &&
            view.text.compare(size_t(byte) - prefix.size(), prefix.size(), prefix) == 0) {
          view.text.erase(size_t(byte) - prefix.size(), prefix.size());
          view.cursor -= utf8::length(prefix);
        }
      }
      auto combined = std::move(textBatch_->text);
      textBatch_->text.clear();
      const auto position = utf8::length(combined) + cursor;
      combined += text;
      ScopedRestore<bool> restore(spacingBypass_);
      spacingBypass_ = true;
      commitStringAtCursor(ic, combined, position);
      return;
    }
    const auto length = utf8::length(text);
    cursor = std::min(cursor, length);
    if constexpr (HasCursorCommit<Context, Flag>::value) {
      if (ic->capabilityFlags().test(Flag::CommitStringWithCursor)) {
        recordReplayCommit(ic, text);
        if (replayView_ && replayView_->context == ic)
          replayView_->cursor -= length - cursor;
        ic->commitStringWithCursor(text, cursor);
        // The caret now sits inside the pair, so the character before it, not
        // the pair's last character, is what the next commit follows.
        rememberSpacing(ic, state(ic), lastScalarBefore(text, cursor));
        return;
      }
    }

    // XIM and some Wayland text-input clients cannot express a cursor inside
    // committed text. Commit the complete pair first, then reproduce the
    // original desktop behavior with ordinary cursor movement.
    recordReplayCommit(ic, text);
    ic->commitString(text);
    forwardCursor(ic, state(ic), -int(length - cursor), text);
    rememberSpacing(ic, state(ic), lastScalarBefore(text, cursor));
  }
  static void rememberPair(State *s, char opening,
                           const std::string &right) {
    s->pendingPairKey = closingKey(opening);
    s->pendingPairRight = right;
  }
  static void finishPair(State *s) {
    s->pendingPairKey = 0;
    s->pendingPairRight.clear();
  }
  bool commitSymbol(InputContext *ic, State *s, char ascii, bool asciiMode) {
    const auto *entry = punctuationEntry(ascii);
    if (!entry)
      return false;
    const auto context = symbolContext(ic);
    if (!asciiMode && !entry->pairType && s->pendingPairKey == ascii &&
        !s->pendingPairRight.empty() &&
        mappedSymbol(s, ascii, false) == s->pendingPairRight &&
        nextSurroundingTextIs(ic, s->pendingPairRight)) {
      forwardCursor(ic, s, 1, s->pendingPairRight);
      finishPair(s);
      s->symbolAutoState = 0;
      return true;
    }
    // Chinese CharMapHelper remembers one most recently generated right
    // symbol and clears it when processing another mapped Chinese symbol.
    if (!asciiMode)
      finishPair(s);
    const bool autoChangeActive = symbolAutoChange_ && !asciiMode &&
                                  !englishPunctuation(s) &&
                                  !s->fullWidth;
    const bool numeric = autoChangeActive && s->symbolAutoState;
    auto emit = [&](const std::string &value) {
      if (numeric || (!asciiMode && !s->fullWidth && numericKeyTransport(ic)))
        deliverNumericText(ic, s, value);
      else
        commitText(ic, value);
      // Keep the auto-space boundary state current: punctuation must not let
      // a later candidate think the previous commit ended with 我.
      if (!value.empty())
        rememberSpacing(ic, s, lastScalar(value));
    };
    // A client without native preedit cannot own a temporary separator.
    // Keep the established literal ASCII fallback instead of rewriting text.
    const bool numericAscii =
        autoChangeActive && s->symbolAutoState == 3 &&
        (!ic->capabilityFlags().test(CapabilityFlag::Preedit) ||
         !ic->isPreeditEnabled()) &&
        (ascii == '.' || ascii == ',' || ascii == ':');
    std::string mapped = numericAscii ? std::string(1, ascii)
                                      : mappedSymbol(s, ascii, asciiMode);
    if (autoChangeActive && s->symbolAutoState == 3 &&
        ic->capabilityFlags().test(CapabilityFlag::Preedit) &&
        ic->isPreeditEnabled() &&
        (ascii == '.' || ascii == ',' || ascii == ':')) {
      // Native preedit owns the tentative separator. The next key commits
      // its final form once; no timer deletes previously committed text.
      flushTextBatch(ic);
      s->numericPreview = mapped;
      Text preview(mapped);
      preview.setCursor(mapped.size());
      ic->inputPanel().setClientPreedit(preview);
      ic->updatePreedit();
      s->symbolAutoState = 0;
      return true;
    }
    auto [left, right] = splitPair(mapped);
    if (entry->pairType && !right.empty()) {
      if (asciiMode) {
        // getAppositeCharInEngMode returns the first mapped character only.
        emit(left);
      } else if (*config_.input->symbolAutoPair && symbolPairPosition(context)) {
        mapped = left + std::string(context.selected) + right;
        rememberPair(s, ascii, right);
        if (numeric && numericKeyTransport(ic)) {
          emit(mapped);
          ic->forwardKey(Key(FcitxKey_Left));
          ic->forwardKey(Key(FcitxKey_Left), true);
        } else {
          commitStringAtCursor(ic, mapped, utf8::length(mapped) - 1);
        }
      } else if (entry->pairType == 2) {
        bool &leftNext =
            ascii == '\"' ? s->doubleQuoteLeft : s->singleQuoteLeft;
        const bool opening = quoteOpens(context, left, right, leftNext);
        emit(opening ? left : right);
        leftNext = !opening;
        // Fast physical keys can arrive before a Wayland client applies the
        // previous quote. Use its updated text for the next decision, even
        // if the old suffix happens to be the same ASCII quote character.
        if (context.valid && (!replayView_ || replayView_->context != ic) &&
            (virtualKeyboardCursor(ic) || replayingKeys_))
          waitForClient(ic, s, opening ? left : right, replayingCursor_, true);
      } else {
        emit(left);
      }
    } else {
      emit(mapped);
    }

    s->symbolAutoState = 0;
    return true;
  }
  static std::filesystem::path configPath() {
    const char *config = getenv("XDG_CONFIG_HOME"), *home = getenv("HOME");
    return (config && *config ? std::filesystem::path(config)
                             : std::filesystem::path(home ? home : "") / ".config") /
           "fcitx5/wetypex.json";
  }
  static bool storeSettings(json_object *settings) {
    const auto path = configPath();
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    if (error)
      return false;
    const auto bytes = wire::dump(settings) + '\n';
    if (bytes.size() > 16384)
      return false;
    auto temporary = path.string() + ".XXXXXX";
    const int fd = mkostemp(temporary.data(), O_CLOEXEC);
    if (fd < 0)
      return false;
    size_t offset = 0;
    while (offset < bytes.size()) {
      const auto count = ::write(fd, bytes.data() + offset, bytes.size() - offset);
      if (count > 0)
        offset += size_t(count);
      else if (count < 0 && errno == EINTR)
        continue;
      else
        break;
    }
    const bool closed = close(fd) == 0;
    if (offset == bytes.size() && closed &&
        ::rename(temporary.c_str(), path.c_str()) == 0)
      return true;
    unlink(temporary.c_str());
    return false;
  }
  void resetPunctuationOverrides() {
    for (const auto &[id, ref] : contexts_)
      if (auto *ic = ref.get()) {
        auto *s = ic->propertyFor(&factory_);
        s->englishPunctuation.reset();
        s->symbolAutoState = 0;
        ic->updateUserInterface(UserInterfaceComponent::StatusArea);
      }
  }
  bool rememberPunctuation(bool chinese) {
    // Patch only this preference; settings edited by another UI stay intact.
    std::ifstream file(configPath());
    std::string bytes(16385, '\0');
    file.read(bytes.data(), bytes.size());
    bytes.resize(file.gcount());
    auto settings = bytes.size() > 16384 ? wire::Json{} : wire::parse(bytes);
    if (!settings || !json_object_is_type(settings.get(), json_type_object))
      return false;
    wire::put(settings.get(), "chinese_punctuation", chinese);
    if (!storeSettings(settings.get()))
      return false;
    chinesePunctuation_ = chinese;
    config_.input.mutableValue()->chinesePunctuation.setValue(chinese);
    resetPunctuationOverrides();
    return true;
  }
  void reloadSettings() {
    finishNumericPreviews();
    std::ifstream f(configPath());
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
    const bool previousPunctuation = chinesePunctuation_;
    const bool remember = boolSetting("remember_punctuation", false);
    auto *punctuation = wire::get(j.get(), "chinese_punctuation");
    chinesePunctuation_ = !punctuation || json_object_get_boolean(punctuation);
    if (previousPunctuation != chinesePunctuation_ ||
        remember != *config_.input->rememberPunctuation)
      resetPunctuationOverrides();
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
    input->englishCompletion.setValue(boolSetting("english_completion", false));
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
    input->chinesePunctuation.setValue(chinesePunctuation_);
    input->rememberPunctuation.setValue(remember);
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
    appearance->autoSpace.setValue(boolSetting("auto_space", false));
    appearance->autoSpaceEnglish.setValue(
        boolSetting("auto_space_english", true));
    appearance->autoSpaceDigit.setValue(boolSetting("auto_space_digit", true));
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
    voice->copyToClipboard.setValue(
        boolSetting("voice_copy_to_clipboard", true));
    auto stringSetting = [&](const char *name, const char *fallback) {
      auto value = wire::str(j.get(), name);
      return value.empty() ? std::string(fallback) : value;
    };
    auto voiceKeys = [&](const char *name, const char *fallback) {
      auto keys = Key::keyListFromString(wire::get(j.get(), name)
                                           ? wire::str(j.get(), name)
                                           : std::string(fallback));
      for (auto &key : keys)
        key = key.normalize();
      return keys;
    };
    voice->launchKey.setValue(voiceKeys("voice_launch_key", "Control+Super+Shift_L"));
    voice->holdKey.setValue(voiceKeys("voice_hold_key", "Control+Super_L"));
    voice->provider.setValue(wire::str(j.get(), "voice_provider") == "local" ? wetype_config::VoiceProviderMode::Local : wetype_config::VoiceProviderMode::Original);
    voice->localModel.setValue(wire::str(j.get(), "local_voice_model", "wetype"));
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
      auto keys = *option;
      for (auto &key : keys)
        key = key.normalize();
      option.setValue(std::move(keys));
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
    for (const char *key : {"page_size", "vertical", "preedit_position",
                            "candidate_label_style", "candidate_size",
                            "theme_mode", "chinese_punctuation",
                            "remember_punctuation", "voice_provider", "local_voice_model"})
      if (j && json_object_is_type(j.get(), json_type_object))
        json_object_object_del(j.get(), key);
    engineSettings_ = std::move(j);
  }
  void saveNativeConfig() {
    const auto path = configPath();
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
    wire::put(json.get(), "english_completion", bool(*input.englishCompletion));
    wire::put(json.get(), "emoji_recommend", bool(*input.emojiRecommend));
    wire::put(json.get(), "wechat_emoji", bool(*input.wechatEmoji));
    wire::put(json.get(), "normal_emoji", bool(*input.normalEmoji));
    wire::put(json.get(), "kaomoji", bool(*input.kaomoji));
    wire::put(json.get(), "large_emoji", bool(*input.largeEmoji));
    wire::put(json.get(), "symbol_emoji", bool(*input.symbolEmoji));
    wire::put(json.get(), "chinese_punctuation", bool(*input.chinesePunctuation));
    wire::put(json.get(), "remember_punctuation", bool(*input.rememberPunctuation));
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
    wire::put(json.get(), "auto_space", bool(*appearance.autoSpace));
    wire::put(json.get(), "auto_space_english",
              bool(*appearance.autoSpaceEnglish));
    wire::put(json.get(), "auto_space_digit", bool(*appearance.autoSpaceDigit));
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
    wire::put(json.get(), "voice_provider", std::string(*voice.provider == wetype_config::VoiceProviderMode::Local ? "local" : "original"));
    wire::put(json.get(), "local_voice_model", *voice.localModel);
    wire::put(json.get(), "voice_launch_shortcut", bool(*voice.launchShortcut));
    wire::put(json.get(), "voice_hold_shortcut", bool(*voice.holdShortcut));
    wire::put(json.get(), "voice_smart_polish", bool(*voice.smartPolish));
    wire::put(json.get(), "voice_copy_to_clipboard",
              bool(*voice.copyToClipboard));
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
    storeSettings(json.get());
    reloadSettings();
  }
  void applyAppearance(bool themeChanged, bool sizeChanged) {
    if (!themeChanged && !sizeChanged)
      return;
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
    if ((std::filesystem::exists(path, error) && !input) || error)
      return;
    const std::string text((std::istreambuf_iterator<char>(input)), {});
    if (input.bad())
      return;
    const auto &appearance = *config_.appearance;
    std::map<std::string, std::string> values;
    if (themeChanged) {
      values = {
          {"Theme", *appearance.theme == wetype_config::ThemeMode::Dark
                        ? "wetypex-dark"
                        : "wetypex-light"},
          {"DarkTheme", "wetypex-dark"},
          {"UseDarkTheme", *appearance.theme == wetype_config::ThemeMode::System
                               ? "True"
                               : "False"},
          {"UseAccentColor", "False"},
          {"PreferTextIcon", "False"}};
    }
    if (sizeChanged)
      values["Font"] = wetype::candidateFontWithSize(
          wetype::classicUiValue(text, "Font").value_or(""),
          *appearance.candidateSize);
    const auto updated = wetype::updateClassicUi(text, values);
    if (updated == text)
      return;
    auto temporary = path;
    temporary += ".tmp";
    std::ofstream output(temporary, std::ios::trunc);
    output << updated;
    output.close();
    if (!output)
      return;
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
    if (std::string_view(operation) == "start" &&
        (wire::number(control.get(), "voice_protocol") != 2 || wire::str(control.get(), "voice_revision") != WETYPE_VOICE_REVISION))
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
        peer.uid != getuid() || peer.pid != wire::number(control.get(), "pid")) {
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
    wire::put(message.get(), "provider", std::string(*config_.voice->provider == wetype_config::VoiceProviderMode::Local ? "local" : "original"));
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
  bool voiceAvailable() const {
    return networkEnabled_ || *config_.voice->provider == wetype_config::VoiceProviderMode::Local;
  }
  void primeVoice() {
    voiceWarmTick_ = now(CLOCK_MONOTONIC);
    if (voiceAvailable() &&
        ((*config_.voice->launchShortcut && !config_.voice->launchKey->empty()) ||
         (*config_.voice->holdShortcut && !config_.voice->holdKey->empty())) &&
        !voiceControl("status"))
      startProcess({WETYPE_VOICE, "status"});
  }
  bool flushBeforeVoice(InputContext *ic, KeyEvent &event) {
    auto *s = state(ic);
    commitEnglishWord(ic, s);
    if (s->vMode || s->vMenu != VMenu::None ||
        (s->preedit.empty() && s->inputSeq <= s->applied))
      return false;
    const auto fallback = s->preedit;
    if (send(ic, "raw")) {
      s->selectionPending = true;
      // Re-enter the shortcut after the queued letters have committed, just
      // as later typing waits behind a candidate selection. Do not discard
      // unfinished spelling when dictation starts.
      s->deferredKeys.push_back({event.rawKey(), event.isRelease(), event.time(), replayingForwarded_});
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
    flushNumericPreview(ic, s);
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
    if (!voiceControl("start", s->traditional)) {
      if (access(WETYPE_VOICE, X_OK) != 0) {
        cancelVoice(false);
        voiceSession_ = s->id;
        voiceError_ = "语音组件不可用，请重启输入法以加载当前安装版本";
        voiceStatusUntil_ = now(CLOCK_MONOTONIC) + 6000000;
      } else {
        startProcess({WETYPE_VOICE, "start", std::to_string(voiceSession_),
                      std::to_string(voiceEpoch_), voiceToken_,
                      s->traditional ? "1" : "0"});
      }
    }
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
      if (phase == "final" && !text.empty() && *config_.voice->copyToClipboard)
        voiceControl("copy");
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
      if (phase == "final" && !text.empty()) {
        spacingBypass_ = true;
        ic->commitString(text);
        spacingBypass_ = false;
      }
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
    s->candidatePage = s->candidateCursor = 0;
    updateVMenu(ic, s);
    panel(ic, s);
  }
  void updateVMenu(InputContext *ic, State *s) {
    auto list = std::make_unique<CommonCandidateList>();
    list->setPageSize(pageSize_);
    list->setCursorPositionAfterPaging(CursorPositionAfterPaging::ResetToFirst);
    const bool nativeLabels = *config_.appearance->labelStyle ==
                              wetype_config::CandidateLabelStyle::Fcitx;
    list->setSelectionKey(nativeLabels && s->vMenu != VMenu::Main
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
      if (!nativeLabels && s->vMenu != VMenu::Main)
        display = std::to_string(i % pageSize_ + 1) + display;
      list->append<VMenuWord>(this, std::move(display), i, s->vMenuGeneration,
                              s->epoch, s->id);
    }
    if (list->totalSize()) {
      s->candidateCursor = std::clamp(s->candidateCursor, 0, list->totalSize() - 1);
      s->candidatePage = s->candidateCursor / pageSize_;
      list->setPage(s->candidatePage);
      list->setGlobalCursorIndex(s->candidateCursor);
    }
    ic->inputPanel().setCandidateList(std::move(list));
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
      spacingBypass_ = true;
      ic->commitString(value);
      spacingBypass_ = false;
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
  bool bareVEnabled() const {
    return *config_.shortcuts->vMode &&
           *config_.input->mode == wetype_config::InputMode::Pinyin &&
           Key(FcitxKey_v).checkKeyList(*config_.shortcuts->vModeKeys);
  }
  bool pendingVPrefix(const State *s) const {
    if (!bareVEnabled() || s->english || s->vMode || s->vMenu != VMenu::None ||
        s->preedit.empty() || s->preedit.front() != 'v' ||
        s->cursor != s->preedit.size())
      return false;
    return std::all_of(s->preedit.begin() + 1, s->preedit.end(), [](char c) {
      return (c >= '0' && c <= '9') || c == '.' || c == '+' || c == '-' ||
             c == '(' || c == ')';
    });
  }
  bool enterVMode(InputContext *ic, State *s, const std::string &prefix,
                   bool tools) {
    // Re-arm the original wrapper when a numeric prefix has already been
    // entered as ordinary text. Nothing from that prefix has been committed.
    if (prefix.size() > 1) {
      ++s->epoch;
      if (!send(ic, "reset"))
        return false;
    }
    if ((prefix.empty() || prefix.size() > 1) && !send(ic, "key", "v"))
      return false;
    if (!send(ic, "vmode"))
      return false;
    s->vMode = true;
    s->vCalculator = !tools;
    s->preedit.clear();
    s->cursor = 0;
    s->symbolAutoState = 0;
    clearDisplayState(s);
    ic->inputPanel().setCandidateList(nullptr);
    for (size_t i = 1; i < prefix.size(); ++i) {
      s->preedit += prefix[i];
      ++s->cursor;
      if (!send(ic, "key", std::string(1, prefix[i])))
        return false;
    }
    panel(ic, s);
    if (tools && !startVModeWindow(ic)) {
      cancelVMode(ic);
      return false;
    }
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
  bool englishPunctuation(const State *s) const {
    return s->englishPunctuation.value_or(!chinesePunctuation_);
  }
  State *state(InputContext *ic) {
    auto *s = ic->propertyFor(&factory_);
    contexts_[s->id] = ic->watch();
    return s;
  }
  bool inlinePreeditEnabled(InputContext *ic) const {
    return *config_.appearance->preeditPosition !=
               wetype_config::PreeditPosition::Candidate &&
           ic->capabilityFlags().test(CapabilityFlag::Preedit) &&
           ic->isPreeditEnabled();
  }
  static bool nativeEnglishField(InputContext *ic) {
    if (ic->capabilityFlags().test(CapabilityFlag::NoSpellCheck))
      return true;
    // Electron's X11 IM bridge does not propagate the editor's spellcheck=false
    // hint. Keep native key events for VS Code so its own quote pairing and
    // IntelliSense trigger characters can run.
    auto program = ic->program();
    const auto slash = program.find_last_of('/');
    if (slash != std::string::npos)
      program.erase(0, slash + 1);
    std::transform(program.begin(), program.end(), program.begin(),
                   [](unsigned char c) { return char(std::tolower(c)); });
    constexpr std::string_view desktop = ".desktop";
    if (program.size() > desktop.size() &&
        program.compare(program.size() - desktop.size(), desktop.size(),
                        desktop) == 0)
      program.resize(program.size() - desktop.size());
    return program == "code" || program == "code-insiders" ||
           program == "code-oss" || program == "vscode" ||
           program == "codium" || program == "codium-insiders" ||
           program == "com.visualstudio.code" ||
           program == "com.visualstudio.code.insiders" ||
           program == "com.vscodium.codium" ||
           program == "com.vscodium.codium-insiders";
  }
  static bool englishCompletionField(InputContext *ic) {
    constexpr auto blocked =
        uint64_t(CapabilityFlag::Password) | uint64_t(CapabilityFlag::Sensitive) |
        uint64_t(CapabilityFlag::NoSpellCheck) | uint64_t(CapabilityFlag::Url) |
        uint64_t(CapabilityFlag::Email) | uint64_t(CapabilityFlag::Digit) |
        uint64_t(CapabilityFlag::Number) | uint64_t(CapabilityFlag::Dialable) |
        uint64_t(CapabilityFlag::Terminal);
    return !(ic->capabilityFlags().toInteger() & blocked) &&
           !nativeEnglishField(ic);
  }
  bool englishCompletionEnabled(InputContext *ic, const State *s) const {
    return *config_.input->englishCompletion && !s->fullWidth &&
           englishCompletionField(ic);
  }
  static const KeyList &englishSelectionKeys() {
    static const auto keys = Key::keyListFromString(
        "1 2 3 4 5 6 7 8 9 "
        "Alt+1 Alt+2 Alt+3 Alt+4 Alt+5 Alt+6 Alt+7 Alt+8 Alt+9");
    return keys;
  }
  AddonInstance *spellAddon() {
    return instance_->addonManager().addon("spell", true);
  }
  const wetype::EnglishDictionary &englishDictionary() {
    if (!englishDictionary_)
      englishDictionary_.emplace();
    return *englishDictionary_;
  }
  void moveHorizontalCandidate(CommonCandidateList &list, bool next) const {
    if (!list.totalSize())
      return;
    const int target = std::clamp(std::max(list.globalCursorIndex(), 0) +
                                      (next ? 1 : -1),
                                  0, list.totalSize() - 1);
    list.setPage(target / pageSize_);
    list.setGlobalCursorIndex(target);
  }
  void updateEnglishCandidates(InputContext *ic, State *s) {
    if (!englishCompletionEnabled(ic, s) || s->englishWord.size() < 2) {
      s->englishCandidateCursor = 0;
      ic->inputPanel().setCandidateList(nullptr);
      return;
    }
    const auto &prefix = s->englishWord;
    auto words = englishDictionary().complete(prefix, size_t(pageSize_));
    if (words.empty()) {
      if (auto *spell = spellAddon())
        words = spell->call<ISpell::hint>("en", prefix, size_t(2 * pageSize_));
    }
    if (words.size() == 1 && words.front() == prefix) {
      ic->inputPanel().setCandidateList(nullptr);
      return;
    }
    auto list = std::make_unique<CommonCandidateList>();
    list->setPageSize(pageSize_);
    // Display plain indices independently of the selection shortcut.
    list->setSelectionKey(Key::keyListFromString("1 2 3 4 5 6 7 8 9"));
    list->setCursorIncludeUnselected(false);
    list->setCursorPositionAfterPaging(CursorPositionAfterPaging::ResetToFirst);
    list->setLayoutHint(vertical_ ? CandidateLayoutHint::Vertical
                                  : CandidateLayoutHint::Horizontal);
    for (auto &word : words) {
      if (word.compare(0, prefix.size(), prefix))
        continue;
      list->append<EnglishCompletionWord>(this, std::move(word));
      if (list->totalSize() == pageSize_)
        break;
    }
    if (list->totalSize()) {
      s->englishCandidateCursor =
          std::clamp(s->englishCandidateCursor, 0, list->totalSize() - 1);
      list->setGlobalCursorIndex(s->englishCandidateCursor);
    } else {
      s->englishCandidateCursor = 0;
      list.reset();
    }
    ic->inputPanel().setCandidateList(std::move(list));
  }
  void commitEnglishWord(InputContext *ic, State *s) {
    if (!s->englishWord.empty())
      commitText(ic, std::exchange(s->englishWord, {}));
    s->englishCandidateCursor = 0;
  }
  bool handleEnglishCompletion(InputContext *ic, State *s, KeyEvent &event,
                               KeySym sym, bool commandModifiers) {
    if (!englishCompletionEnabled(ic, s)) {
      if (!s->englishWord.empty()) {
        s->englishWord.clear();
        panel(ic, s);
      }
      return false;
    }
    const bool letter = (sym >= FcitxKey_a && sym <= FcitxKey_z) ||
                        (sym >= FcitxKey_A && sym <= FcitxKey_Z);
    const bool apostrophe = sym == FcitxKey_apostrophe &&
                            !s->englishWord.empty() &&
                            s->englishWord.back() != '\'';
    if (!commandModifiers && (letter || apostrophe)) {
      if (s->englishWord.empty() && !englishDictionary().available()) {
        auto *spell = spellAddon();
        if (!spell || !spell->call<ISpell::checkDict>("en"))
          return false;
      }
      s->englishWord.push_back(char(sym));
      s->englishCandidateCursor = 0;
      if (s->englishWord.size() >= 64)
        commitEnglishWord(ic, s);
      panel(ic, s);
      return true;
    }
    if (s->englishWord.empty())
      return false;
    const auto &key = event.key();
    if (key.check(FcitxKey_BackSpace)) {
      s->englishWord.pop_back();
      s->englishCandidateCursor = 0;
      panel(ic, s);
      return true;
    }
    const Key selectionKey(sym, key.states());
    const auto candidates = ic->inputPanel().candidateList();
    auto *list = dynamic_cast<CommonCandidateList *>(candidates.get());
    if (list && list->size()) {
      const bool previous = selectionKey.check(FcitxKey_Left);
      const bool next = selectionKey.check(FcitxKey_Right);
      const bool pageUp = selectionKey.check(FcitxKey_Page_Up) ||
          selectionKey.checkKeyList(instance_->globalConfig().defaultPrevPage());
      const bool pageDown = selectionKey.check(FcitxKey_Page_Down) ||
          selectionKey.checkKeyList(instance_->globalConfig().defaultNextPage());
      if (previous || next || pageUp || pageDown) {
        if (previous || next)
          moveHorizontalCandidate(*list, next);
        else if (pageUp && list->hasPrev())
          list->prev();
        else if (pageDown && list->hasNext())
          list->next();
        s->englishCandidateCursor = std::max(list->globalCursorIndex(), 0);
        ic->updateUserInterface(UserInterfaceComponent::InputPanel);
        return true;
      }
    }
    const bool selectWithSpace = selectionKey.check(FcitxKey_space);
    const int highlighted = candidates ? std::max(candidates->cursorIndex(), 0) : 0;
    // Plain digits and their Alt variants select the same nine positions.
    const int index = selectWithSpace || selectionKey.check(FcitxKey_Tab)
                          ? highlighted
                          : selectionKey.keyListIndex(englishSelectionKeys()) % 9;
    if (index >= 0 && candidates && index < candidates->size()) {
      const auto *word = dynamic_cast<const EnglishCompletionWord *>(
          &candidates->candidate(index));
      if (word) {
        word->select(ic, selectWithSpace);
        return true;
      }
    }
    if (s->englishWord.empty())
      return false;
    if (!commandModifiers && sym >= 0x20 && sym <= 0x7e) {
      const char ascii = char(sym);
      const auto *entry = punctuationEntry(ascii);
      if (entry && (entry->pairType || s->pendingPairKey == ascii)) {
        // Keep the word and mapped punctuation in one client transaction.
        TextBatch batch{ic->watch(), {}};
        ScopedRestore<TextBatch *> restore(textBatch_);
        if (!textBatch_)
          textBatch_ = &batch;
        commitEnglishWord(ic, s);
        commitSymbol(ic, s, ascii, true);
        panel(ic, s);
      } else {
        s->englishWord.push_back(ascii);
        commitEnglishWord(ic, s);
        panel(ic, s);
      }
      return true;
    }
    const auto word = s->englishWord;
    commitEnglishWord(ic, s);
    panel(ic, s);
    if (virtualKeyboardCursor(ic)) {
      waitForClient(ic, s, ic->capabilityFlags().test(CapabilityFlag::SurroundingText)
                              ? word
                              : std::string{});
      s->cursorKeys.push_back({event.rawKey(), false, event.time()});
      return true;
    }
    return false;
  }
  void panel(InputContext *ic, State *s) {
    // The insertion must reach the client before its preedit is replaced.
    flushTextBatch(ic);
    const bool inlinePreedit = !s->numericPreview.empty() || inlinePreeditEnabled(ic);
    const bool englishPreedit = !s->englishWord.empty();
    if (englishPreedit)
      updateEnglishCandidates(ic, s);
    const bool voice = voicePending_ && voiceSession_ == s->id &&
                       voiceEpoch_ == s->epoch;
    const bool doublePinyin =
        *config_.input->mode == wetype_config::InputMode::DoublePinyin;
    const auto candidates = ic->inputPanel().candidateList();
    if (s->preedit.empty() || (inlinePreedit && !doublePinyin) || voice || s->vMode ||
        s->vMenu != VMenu::None) {
      s->initialPanelDeadline = 0;
      s->initialPanelShown = false;
    } else if ((doublePinyin ||
                (!s->initialPanelShown && (!candidates || !candidates->size()))) &&
               s->inputSeq > s->applied && !failed_ &&
               s->initialPanelInputSeq != s->inputSeq) {
      const auto time = now(CLOCK_MONOTONIC);
      if (!s->initialPanelDeadline) {
        // Double-pinyin segmentation is supplied by the core. Preserve its
        // previous frame until the reply, rather than flashing raw spelling
        // between two segmented strings. Do not restart this deadline on
        // each key: slow/startup paths still get feedback within one frame.
        s->initialPanelDeadline = time + 16000;
        if (timer_) {
          timer_->setAccuracy(1000);
          timer_->setTime(std::min(timer_->time(), s->initialPanelDeadline));
        }
      }
      if (time < s->initialPanelDeadline)
        return;
      s->initialPanelInputSeq = s->inputSeq;
      s->initialPanelShown = true;
      s->initialPanelDeadline = 0;
    } else {
      s->initialPanelShown = true;
      s->initialPanelDeadline = 0;
    }
    Text aux;
    if (voice)
      aux = Text("🎙 语音输入 " + std::string(voiceLevel_ / 12, '|'));
    else if (s->vMenu != VMenu::None)
      aux = Text(s->vMenuTitle);
    else if (s->vCalculator)
      aux = Text("计算：输入表达式，空格选取结果");
    else if (s->preedit == "v" && pendingVPrefix(s))
      aux = Text("空格打开快捷工具，继续输入英文或计算式；Enter 输入 v");
    ic->inputPanel().setAuxUp(aux);
    const std::string displayed =
        voice ? voiceText_
        : !s->numericPreview.empty() ? s->numericPreview
        : englishPreedit ? s->englishWord
        : !s->displayPreedit.empty() ? s->displayPreedit
        : !s->vMode && *config_.input->mode == wetype_config::InputMode::Pinyin
            ? formatPinyinPreedit(s->preedit)
            : s->preedit;
    // Text("") contains one empty segment and is not empty to Fcitx UI.
    Text text;
    if (!displayed.empty()) {
      // A partial transcript is a preview. Fcitx commits ordinary client
      // preedit before focus-out reaches deactivate(), which would otherwise
      // accept the partial even though the voice session is cancelled.
      text.append(displayed, voice ? TextFormatFlag::DontCommit
                                   : TextFormatFlag::NoFlag);
      text.setCursor(
          voice || englishPreedit || !s->numericPreview.empty() ? displayed.size()
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
    else if (!englishPreedit && s->numericPreview.empty() &&
             !displayed.empty() && (failed_ || !ready_))
      status.append(failed_ ? "WeTypeX 核心正在恢复…" : "WeTypeX 核心启动中…");
    ic->inputPanel().setAuxDown(status);
    if (s->preedit.empty() && !englishPreedit && !s->vMode)
      ic->inputPanel().setCandidateList(nullptr);
    ic->updatePreedit();
    ic->updateUserInterface(UserInterfaceComponent::InputPanel);
  }
  bool pollRuntimeUpdate(uint64_t time) {
    if (runtimePhase_ == RuntimePhase::Idle) {
      if (time < runtimePollAt_)
        return false;
      runtimePollAt_ = time + 1000000;
      // A fresh heartbeat lets settings distinguish a live-upgrade capable
      // frontend from a pre-upgrade Fcitx process still using the old module.
      std::error_code clientError;
      std::filesystem::create_directories(runtimeRoot(), clientError);
      if (!clientError) {
        auto report = wire::object();
        wire::put(report.get(), "protocol", 1);
        wire::put(report.get(), "pid", static_cast<int>(getpid()));
        wire::put(report.get(), "updated_at", static_cast<int64_t>(std::time(nullptr)));
        const auto path = runtimeRoot() / "runtime-client.json";
        const auto temporary = runtimeRoot() / (".runtime-client-" + std::to_string(getpid()));
        std::ofstream output(temporary, std::ios::trunc);
        chmod(temporary.c_str(), 0600);
        output << wire::dump(report.get()) << '\n';
        output.close();
        if (output)
          std::filesystem::rename(temporary, path, clientError);
      }
      std::ifstream file(runtimeRoot() / "runtime-pending.json");
      std::string bytes(16385, '\0');
      file.read(bytes.data(), bytes.size());
      bytes.resize(file.gcount());
      auto pending = bytes.size() <= 16384 ? wire::parse(bytes) : wire::Json{};
      runtimePending_ = pending && !wire::str(pending.get(), "generation").empty();
      if (!runtimePending_ || time < runtimeRetryAt_ ||
          !wire::str(pending.get(), "error").empty() || voicePending_ ||
          time < lastKeyAt_ + 2000000 || !outbound_.empty())
        return false;
      for (const auto &[id, ref] : contexts_)
        if (auto *ic = ref.get()) {
          const auto *s = ic->propertyFor(&factory_);
          if (!s->preedit.empty() || !s->englishWord.empty() ||
              !s->numericPreview.empty() || s->vMode ||
              s->vMenu != VMenu::None ||
              s->seq != s->applied || s->selectionPending ||
              !s->deferredKeys.empty() || !s->cursorKeys.empty() ||
              s->pendingCursorSteps || s->pendingClientKey ||
              !s->forwardedKeys.empty() ||
              modifiersHeld(instance_, ic, s, true))
            return false;
        }
      runtimePhase_ = RuntimePhase::Draining;
      runtimeDrainAt_ = time;
      (void)voiceControl("quiesce");
      if (syncChild_ > 0) {
        runtimeSync_ = syncChild_;
        syncChild_ = -1;
        kill(runtimeSync_, SIGTERM);
      }
      reader_.reset();
      writer_.reset();
      if (write_ >= 0)
        close(write_);
      if (read_ >= 0)
        close(read_);
      read_ = write_ = -1;
      ready_ = false;
      restartAt_ = 0;
    }
    if (runtimePhase_ == RuntimePhase::Draining) {
      if (child_ > 0) {
        if (waitpid(child_, nullptr, WNOHANG) == 0) {
          if (time >= runtimeDrainAt_ + 10000000)
            kill(child_, SIGKILL);
          else if (time >= runtimeDrainAt_ + 5000000)
            kill(child_, SIGTERM);
          return true;
        }
        child_ = -1;
      }
      if (runtimeSync_ > 0) {
        if (waitpid(runtimeSync_, nullptr, WNOHANG) == 0) {
          if (time >= runtimeDrainAt_ + 10000000)
            kill(runtimeSync_, SIGKILL);
          return true;
        }
        runtimeSync_ = -1;
      }
      const char *override = getenv("WETYPE_SETUP_LAUNCHER");
      std::string launcher = override && *override ? override : WETYPE_SETUP;
      std::string operation = "--activate-pending";
      char *arguments[] = {launcher.data(), operation.data(), nullptr};
      posix_spawn_file_actions_t actions;
      posix_spawn_file_actions_init(&actions);
      posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
      posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, "/dev/null", O_WRONLY, 0);
      posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
      const int result = posix_spawn(&runtimeWorker_, launcher.c_str(), &actions,
                                     nullptr, arguments, environ);
      posix_spawn_file_actions_destroy(&actions);
      if (result)
        runtimeWorker_ = -1;
      runtimePhase_ = RuntimePhase::Applying;
    }
    if (runtimeWorker_ > 0 && waitpid(runtimeWorker_, nullptr, WNOHANG) == 0)
      return true;
    runtimeWorker_ = -1;
    runtimePhase_ = RuntimePhase::Idle;
    std::error_code runtimeError;
    const bool stillPending = std::filesystem::exists(
        runtimeRoot() / "runtime-pending.json", runtimeError);
    runtimeRetryAt_ = stillPending || runtimeError ? time + 60000000 : 0;
    inbound_.clear();
    outbound_.clear();
    restartNeedsOpen_ = true;
    for (const auto &[id, ref] : contexts_)
      if (auto *ic = ref.get()) {
        auto *s = ic->propertyFor(&factory_);
        ++s->epoch;
        s->seq = s->applied = s->inputSeq = s->appliedUpdate = 0;
        s->initialPanelInputSeq = 0;
        s->initialPanelDeadline = 0;
        s->cloudPollUntil = 0;
        if (s->active && ic->hasFocus()) {
          ic->inputPanel().reset();
          ic->updateUserInterface(UserInterfaceComponent::InputPanel);
        }
      }
    if (!start()) {
      failed_ = true;
      scheduleRestart();
    } else {
      // Open the new sessions before replaying any input. A late open on the
      // ready reply would otherwise discard keys queued behind a selection.
      restartNeedsOpen_ = false;
      for (const auto &[id, ref] : contexts_)
        if (auto *ic = ref.get(); ic && ic->hasFocus() && state(ic)->active &&
            !ic->capabilityFlags().test(CapabilityFlag::Password))
          send(ic, "open");
    }
    if (runtimeConfigApply_ && networkEnabled_)
      applyDeviceFunctions();
    runtimeConfigApply_ = false;
    if (networkEnabled_)
      startSync();
    auto keys = std::move(maintenanceKeys_);
    maintenanceKeys_.clear();
    for (size_t i = 0; i < keys.size();) {
      const auto &queued = keys[i];
      auto *ic = queued.context.get();
      if (!ic || state(ic)->epoch != queued.epoch + 1) {
        ++i;
        continue;
      }
      // Validate the captured context once. An intentional Escape/reset
      // inside this batch may advance its epoch before the next queued word.
      std::vector<DeferredKey> batch;
      const auto epoch = queued.epoch;
      do {
        batch.push_back(keys[i++].event);
      } while (i < keys.size() && keys[i].context.get() == ic && keys[i].epoch == epoch);
      replayDeferredKeys(ic, std::move(batch));
    }
    return false;
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
    invalidateSessions(true);
  }
  void invalidateSessions(bool preserveEnglish = false) {
    cancelVoice();
    restartNeedsOpen_ = true;
    for (auto &[id, ref] : contexts_)
      if (auto *ic = ref.get()) {
        auto *s = ic->propertyFor(&factory_);
        // English completion is local to the frontend. A backend failure
        // must not commit an unfinished word or discard its candidates.
        const bool keepEnglish = preserveEnglish && s->active && ic->hasFocus() &&
                                 s->english && englishCompletionEnabled(ic, s);
        if (!keepEnglish) {
          if (s->active && ic->hasFocus() && englishCompletionField(ic))
            commitEnglishWord(ic, s);
          s->englishWord.clear();
        }
        releaseForwardedKeys(ic, s);
        clearCursorMove(s);
        s->physicalModifiers = s->pressedShifts = 0;
        s->heldModifierKeys.clear();
        ++s->epoch;
        s->seq = s->applied = s->inputSeq = 0;
        s->initialPanelInputSeq = 0;
        s->appliedUpdate = 0;
        s->initialPanelDeadline = 0;
        s->initialPanelShown = false;
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
    if (runtimePhase_ != RuntimePhase::Idle)
      return true;
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
    const auto identity = identityStamp();
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
    backendIdentity_ = identity;
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
        std::string_view(op) == "delete" || std::string_view(op) == "move" ||
        std::string_view(op) == "reconfigure")
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
    wire::put(json.get(), "chinese_punctuation", int64_t(!englishPunctuation(s)));
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
    const std::string_view operation(op);
    const auto spellingLength =
        s->preedit.size() - std::min(s->editableBegin, s->preedit.size());
    const bool wubiMayCommit =
        *inputConfig.mode == wetype_config::InputMode::Wubi &&
        (operation == "key" || operation == "backspace" ||
         operation == "delete" || operation == "move") &&
        ((*inputConfig.wubiUniqueCommit && spellingLength == 4) ||
         (*inputConfig.wubiNextCommit && spellingLength == 5));
    // These replies decide whether a composition remains. Later digits must
    // not select from its old candidates or overtake its pending insertion.
    // Wubi's four/five-code auto-commit boundaries need the same ordering.
    if (operation == "select" || operation == "punctuation" ||
        operation == "raw" || operation == "reconfigure" || wubiMayCommit)
      s->selectionPending = true;
    if (operation == "punctuation") {
      ic->inputPanel().setCandidateList(nullptr);
      ic->updateUserInterface(UserInterfaceComponent::InputPanel);
    }
    activity_ = now(CLOCK_MONOTONIC);
    flush();
    return child_ > 0 && write_ >= 0;
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
    auto cursorCommit = wire::str(j.get(), "cursor_commit");
    auto cursorCommitPosition =
        wire::number(j.get(), "cursor_commit_position", -1);
    const bool hasCursorCommit = !cursorCommit.empty() &&
        cursorCommitPosition >= 0 &&
        size_t(cursorCommitPosition) <= utf8::length(cursorCommit);
    const bool mergeCursorCommit =
        hasCursorCommit && virtualKeyboardCursor(ic);
    std::string cursorPrefix;
    const bool previousSpacingBypass = spacingBypass_;
    spacingBypass_ |= json_object_get_boolean(wire::get(j.get(), "preserve_spacing"));
    if (commits && json_object_is_type(commits, json_type_array))
      for (size_t i = 0; i < json_object_array_length(commits); i++) {
        auto *v = json_object_array_get_idx(commits, i);
        if (json_object_is_type(v, json_type_string)) {
          if (mergeCursorCommit)
            cursorPrefix += json_object_get_string(v);
          else
            ic->commitString(json_object_get_string(v));
        }
      }
    if (!mergeCursorCommit)
      spacingBypass_ = previousSpacingBypass;
    if (hasCursorCommit) {
      // Wayland clients can replace or recommit pending preedit when
      // multiple commits arrive before the next done event. Submit the
      // selected word and its bracket pair together, with the caret offset
      // adjusted for the prefix, so neither part can overwrite the other.
      const auto position =
          utf8::length(cursorPrefix) + size_t(cursorCommitPosition);
      cursorPrefix += cursorCommit;
      commitStringAtCursor(ic, cursorPrefix, position);
    }
    spacingBypass_ = previousSpacingBypass;
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
    s->candidates.reset(json_object_get(wire::get(j.get(), "candidates")));
    s->cloudFlags.reset(json_object_get(wire::get(j.get(), "cloud")));
    updateCandidates(ic, s);
    panel(ic, s);
    auto deferred = std::move(s->deferredKeys);
    s->deferredKeys.clear();
    replayDeferredKeys(ic, std::move(deferred));
  }

  void updateCandidates(InputContext *ic, State *s) {
    auto list = std::make_unique<CommonCandidateList>();
    list->setPageSize(pageSize_);
    list->setCursorPositionAfterPaging(CursorPositionAfterPaging::ResetToFirst);
    const bool nativeLabels = *config_.appearance->labelStyle ==
                              wetype_config::CandidateLabelStyle::Fcitx;
    list->setSelectionKey(
        nativeLabels ? Key::keyListFromString("1 2 3 4 5 6 7 8 9") : KeyList{});
    list->setLayoutHint(vertical_ ? CandidateLayoutHint::Vertical
                                  : CandidateLayoutHint::Horizontal);
    auto *candidates = s->candidates.get();
    auto *cloudFlags = s->cloudFlags.get();
    const bool wechat = wetype::WeChatEmoticons::isWeChat(ic->program());
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
          const auto *emoticon = wechat ? wechatEmoticons_.find(text) : nullptr;
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
    commitFilterConnection_ =
        instance_->connect<Instance::CommitFilter>(
            [this](InputContext *ic, std::string &orig) {
              auto *entry = ic ? instance_->inputMethodEntry(ic) : nullptr;
              if (!entry || entry->uniqueName() != "wetypex" || !ic->hasFocus() ||
                  ic->capabilityFlags().test(CapabilityFlag::Password) || orig.empty())
                return;
              auto *s = state(ic);
              if (!s->active)
                return;
              if (!*config_.appearance->autoSpace) {
                clearSpacing(s);
                return;
              }
              if (!utf8::validate(orig)) {
                clearSpacing(s);
                return;
              }
              if (autoSpaceEnabled(ic, s))
                orig = applyAutoSpace(orig, beforeWindow(ic, s));
              rememberSpacing(ic, s, lastScalar(orig));
            });
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
          if (!languageModifier && !languageSwitchKey(key.key()) &&
              !pendingSwitch && !compositionShortcut)
            return;
          if (key.key().isModifier() && !replayingKeys_) {
            // Apply our switch logic without accepting the physical event.
            // Filtering alone skips Fcitx's global trigger and lets the
            // frontend deliver the original press/release to the client.
            // forwardKey() may synthesize an immediate release on Wayland.
            auto modifier = key;
            keyEvent(*entry, modifier);
            key.filter();
          } else {
            // Replayed modifiers already reached the client physically.
            keyEvent(*entry, key);
            key.filterAndAccept();
          }
        });
    surroundingWatcher_ = instance_->watchEvent(
        EventType::InputContextSurroundingTextUpdated, EventWatcherPhase::Default,
        [this](Event &event) {
          auto *ic = static_cast<InputContextEvent &>(event).inputContext();
          const auto *entry = instance_->inputMethodEntry(ic);
          if (entry && entry->uniqueName() == "wetypex")
            acknowledgeCursorView(ic, state(ic));
          if (entry && entry->uniqueName() == "wetypex" &&
              (state(ic)->pendingClientKey || state(ic)->pendingCursorSteps))
            scheduleCursorMove();
        });
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
          if (pollRuntimeUpdate(time)) {
            source->setTime(time + 10000);
            source->setEnabled(true);
            return true;
          }
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
          if (focused && voiceAvailable() && !runtimePending_ && time >= voiceWarmTick_ + 60000000)
            primeVoice();
          pollVModeWindow();
          if (*config_.update->autoUpdate &&
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
  std::string subMode(const InputMethodEntry &, InputContext &ic) override {
    return modeDescription(state(&ic));
  }
  std::string subModeIconImpl(const InputMethodEntry &, InputContext &ic) override {
    const auto *s = state(&ic);
    return s->english ? "fcitx5-wetypex-en"
                      : s->traditional ? "fcitx5-wetypex-traditional"
                                       : "fcitx5-wetypex-zh";
  }
  std::string subModeLabelImpl(const InputMethodEntry &, InputContext &ic) override {
    const auto *s = state(&ic);
    return s->english ? "英" : s->traditional ? "繁" : "中";
  }
  const Configuration *
  getConfigForInputMethod(const InputMethodEntry &) const override {
    return &config_;
  }
  void setConfig(const RawConfig &raw) override {
    finishNumericPreviews();
    const auto before = config_;
    const auto oldTheme = *config_.appearance->theme;
    const auto oldSize = *config_.appearance->candidateSize;
    config_.load(raw, true);
    const bool themeChanged = *config_.appearance->theme != oldTheme;
    const bool sizeChanged = *config_.appearance->candidateSize != oldSize;
    saveNativeConfig();
    applyAppearance(themeChanged, sizeChanged);
    if (*before.voice->provider != *config_.voice->provider ||
        *before.voice->localModel != *config_.voice->localModel)
      cancelVoice();
    if (frontendOnly(before)) {
      refreshPresentation();
      return;
    }
    if (runtimePhase_ != RuntimePhase::Idle) {
      runtimeConfigApply_ = true;
      return;
    }
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
    const auto before = config_;
    auto oldEngineSettings = std::move(engineSettings_);
    reloadSettings();
    if (*before.voice->provider != *config_.voice->provider ||
        *before.voice->localModel != *config_.voice->localModel)
      cancelVoice();
    if (frontendOnly(before) && oldEngineSettings && engineSettings_ &&
        json_object_equal(oldEngineSettings.get(), engineSettings_.get())) {
      refreshPresentation();
      return;
    }
    cancelVoice();
    englishDictionary_.reset();
    if (networkEnabled_)
      startSync();
    else
      stopSync();
    if (runtimePhase_ != RuntimePhase::Idle) {
      return;
    }
    stop();
    invalidateSessions();
    if (!start()) {
      failed_ = true;
      scheduleRestart();
    }
  }
  ~WeType() override {
    for (auto &[id, ref] : contexts_)
      if (auto *ic = ref.get())
        releaseForwardedKeys(ic, ic->propertyFor(&factory_));
    cancelVoice();
    languageModifierWatcher_.reset();
    surroundingWatcher_.reset();
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
    // Focus round trips preserve a deliberate language choice. Explicitly
    // entering this input method starts in the configured default language.
    if (!current->languageInitialized ||
        e.type() == EventType::InputContextSwitchInputMethod) {
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
    modeChanged(ic, current, false);
  }
  void reset(const InputMethodEntry &, InputContextEvent &e) override {
    auto *ic = e.inputContext();
    auto *s = state(ic);
    const auto time = now(CLOCK_MONOTONIC);
    const bool editing = s->active && ic->hasFocus() &&
        !ic->capabilityFlags().test(CapabilityFlag::Password) &&
        !ic->capabilityFlags().test(CapabilityFlag::Sensitive) &&
        s->preedit.empty() && s->englishWord.empty() && !s->selectionPending;
    // After the client acknowledges our edit, a later reset is a new
    // cancellation (for example a mouse selection), not its commit echo.
    const bool cursorPending = editing && s->pendingCursorSteps &&
        !s->cursorViewAcknowledged &&
        (!s->cursorDeadline || time <= s->cursorDeadline);
    const bool clientPending = editing && s->pendingClientKey && s->clientHadText &&
        ic->capabilityFlags().test(CapabilityFlag::SurroundingText) &&
        time < s->cursorReplayAt &&
        (!s->clientExpected.empty() || s->symbolAutoState);
    const auto numericState = editing ? s->symbolAutoState : 0;
    if (!cursorPending)
      releaseForwardedKeys(ic, s);
    if (!clientPending && !cursorPending)
      clearCursorMove(s);
    else if (clientPending) {
      s->clientResetPending = true;
      s->clientRequireChange = true;
    }
    if (cursorPending) {
      s->cursorResetPending = true;
      if (s->cursorReplayAt)
        s->cursorMoveReset = true;
    } else {
      s->physicalModifiers = s->pressedShifts = 0;
      s->heldModifierKeys.clear();
    }
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
    s->numericPreview.clear();
    s->preedit.clear();
    s->englishWord.clear();
    s->englishCandidateCursor = 0;
    s->cursor = 0;
    clearDisplayState(s);
    if (!cursorPending) {
      s->pendingPairKey = 0;
      s->pendingPairRight.clear();
    }
    s->symbolAutoState = numericState;
    s->validateNumericState = numericState != 0;
    s->vMode = false;
    s->vCalculator = false;
    s->cloudPollUntil = 0;
    // The caret may have moved, so whatever was committed last no longer sits
    // before the next character.
    clearSpacing(s);
    ic->inputPanel().reset();
    if (child_ > 0)
      send(ic, "reset");
    ic->updatePreedit();
    ic->updateUserInterface(UserInterfaceComponent::InputPanel);
  }
  void deactivate(const InputMethodEntry &entry,
                  InputContextEvent &e) override {
    auto *ic = e.inputContext();
    auto *s = state(ic);
    // Fcitx or the client commits inline preedit when focus leaves.
    if (e.type() != EventType::InputContextFocusOut)
      flushNumericPreview(ic, s);
    if (englishCompletionField(ic) &&
        (e.type() != EventType::InputContextFocusOut || !inlinePreeditEnabled(ic)))
      commitEnglishWord(ic, s);
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
  void selectEnglishCompletion(InputContext *ic, std::string word,
                               bool trailingSpace) {
    auto *s = state(ic);
    if (!s->active || !ic->hasFocus() || !s->english ||
        !englishCompletionEnabled(ic, s) || s->englishWord.empty() ||
        word.compare(0, s->englishWord.size(), s->englishWord))
      return;
    s->englishWord.clear();
    s->englishCandidateCursor = 0;
    if (trailingSpace)
      word.push_back(' ');
    commitText(ic, word);
    panel(ic, s);
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
  std::string modeDescription(const State *s) const {
    return std::string(s->english ? "英文" : "中文") +
           (s->traditional ? " · 繁体" : " · 简体") +
           (s->fullWidth ? " · 全角" : " · 半角") +
           (s->english || (!s->fullWidth && englishPunctuation(s))
                ? " · 英文标点" : " · 中文标点");
  }
  void modeChanged(InputContext *ic, State *s, bool notify = true) {
    ic->updateUserInterface(UserInterfaceComponent::StatusArea);
    if (notify && s->active && ic->hasFocus() &&
        !ic->capabilityFlags().test(CapabilityFlag::Password))
      showModeInfo(instance_, ic, modeDescription(s));
  }
  void switchLanguage(InputContext *ic, State *s) {
    flushNumericPreview(ic, s);
    if (s->vMode)
      cancelVMode(ic);
    commitEnglishWord(ic, s);
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
    s->modifierCandidate = Key();
    ic->inputPanel().reset();
    panel(ic, s);
    modeChanged(ic, s);
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
    switch (sym) {
    case FcitxKey_KP_Space:
      sym = FcitxKey_space;
      break;
    case FcitxKey_KP_Tab:
      sym = FcitxKey_Tab;
      break;
    case FcitxKey_KP_Home:
      sym = FcitxKey_Home;
      break;
    case FcitxKey_KP_End:
      sym = FcitxKey_End;
      break;
    case FcitxKey_KP_Left:
      sym = FcitxKey_Left;
      break;
    case FcitxKey_KP_Right:
      sym = FcitxKey_Right;
      break;
    case FcitxKey_KP_Up:
      sym = FcitxKey_Up;
      break;
    case FcitxKey_KP_Down:
      sym = FcitxKey_Down;
      break;
    case FcitxKey_KP_Prior:
      sym = FcitxKey_Page_Up;
      break;
    case FcitxKey_KP_Next:
      sym = FcitxKey_Page_Down;
      break;
    case FcitxKey_KP_Delete:
      sym = FcitxKey_Delete;
      break;
    default:
      break;
    }
    bool keypadOperator = true;
    switch (sym) {
    case FcitxKey_KP_Add:
      sym = FcitxKey_plus;
      break;
    case FcitxKey_KP_Subtract:
      sym = FcitxKey_minus;
      break;
    case FcitxKey_KP_Multiply:
      sym = FcitxKey_asterisk;
      break;
    case FcitxKey_KP_Divide:
      sym = FcitxKey_slash;
      break;
    case FcitxKey_KP_Decimal:
      sym = FcitxKey_period;
      break;
    case FcitxKey_KP_Separator:
      sym = FcitxKey_comma;
      break;
    case FcitxKey_KP_Equal:
      sym = FcitxKey_equal;
      break;
    default:
      keypadOperator = false;
      break;
    }
    if (!s->active || ic->capabilityFlags().test(CapabilityFlag::Password))
      return;
    ScopedRestore<const KeyEvent *> restoreKey(currentKeyEvent_);
    currentKeyEvent_ = &e;
    PassedKeyTracker track{this, ic, s, e};
    if (!replayingKeys_) {
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
      if (e.isRelease() && !waylandV2(ic))
        releaseForwardedKey(ic, s, e.rawKey());
    }
    // A passed press belongs to the client even if an IM transaction has
    // started since. Release it immediately instead of buffering/dropping
    // its up event and leaving the client's repeat timer running.
    if (!replayingKeys_ && waylandV2(ic) && e.isRelease() &&
        releaseForwardedKey(ic, s, e.rawKey())) {
      e.filterAndAccept();
      return;
    }
    if (!replayingKeys_) {
      if (key.isModifier()) {
        if (e.isRelease())
          s->heldModifierKeys.erase(sym);
        else
          s->heldModifierKeys.insert(sym);
      }
      lastKeyAt_ = now(CLOCK_MONOTONIC);
      if (runtimePhase_ != RuntimePhase::Idle) {
        const auto commands = key.states().toInteger() &
                              uint32_t(KeyState::SimpleMask) &
                              ~uint32_t(KeyState::Shift);
        const bool shift = sym == FcitxKey_Shift_L || sym == FcitxKey_Shift_R;
        const bool inputShortcut = languageSwitchKey(key) || compositionShortcutKey(key) ||
            // Earlier queued keys may switch language or half/full width.
            // Replay decides whether this chord selects an English word.
            (*config_.input->englishCompletion && englishCompletionField(ic) &&
             Key(sym, key.states()).keyListIndex(englishSelectionKeys()) >= 0) ||
            (*config_.shortcuts->halfFull && key.checkKeyList(*config_.shortcuts->halfFullKeys)) ||
            (*config_.shortcuts->selectCtrl && bareModifier(key) &&
             (sym == FcitxKey_Control_L || sym == FcitxKey_Control_R)) ||
            (*config_.shortcuts->vMode && !s->english &&
             *config_.input->mode == wetype_config::InputMode::Pinyin &&
             key.checkKeyList(*config_.shortcuts->vModeKeys)) ||
            (networkEnabled_ && *config_.shortcuts->aiAssistant && key.checkKeyList(*config_.shortcuts->aiAssistantKeys)) ||
            (voiceAvailable() &&
             ((*config_.voice->launchShortcut && key.checkKeyList(*config_.voice->launchKey)) ||
              (*config_.voice->holdShortcut && key.checkKeyList(*config_.voice->holdKey))));
        // Application commands run against the text already committed. Keep
        // their position in the IME event stream too, so a Ctrl+S between a
        // modifier press/release cannot turn into bare-Ctrl candidate choice.
        const bool forwarded = (commands || (key.isModifier() && !shift)) &&
                               !inputShortcut;
        maintenanceKeys_.push_back({ic->watch(), s->epoch,
                                   {e.rawKey(), e.isRelease(), e.time(), forwarded}});
        if (forwarded)
          return;
        ic->inputPanel().setCandidateList(nullptr);
        ic->inputPanel().setAuxUp(Text("正在应用更新，输入将自动继续"));
        ic->updateUserInterface(UserInterfaceComponent::InputPanel);
        e.filterAndAccept();
        return;
      }
    }
    if (!s->numericPreview.empty() &&
        (!symbolAutoChange_ || s->english || englishPunctuation(s) || s->fullWidth))
      flushNumericPreview(ic, s);
    if (!s->numericPreview.empty() && !e.isRelease() && !key.isModifier()) {
      s->modifierCandidate = Key();
      const auto commands = key.states().toInteger() &
          uint32_t(KeyState::SimpleMask) & ~uint32_t(KeyState::Shift);
      if (!commands && sym >= FcitxKey_0 && sym <= FcitxKey_9) {
        const auto value = s->numericPreview == "。" ? "." :
                           s->numericPreview == "，" ? "," : ":";
        const auto text = std::string(value) + char(sym);
        flushNumericPreview(ic, s, false);
        commitText(ic, text);
        s->symbolAutoState = 3;
        e.filterAndAccept();
        return;
      }
      if (!commands && sym == FcitxKey_BackSpace) {
        flushNumericPreview(ic, s, false);
        e.filterAndAccept();
        return;
      }
      if (!commands && sym == FcitxKey_space) {
        const auto value = s->numericPreview + " ";
        flushNumericPreview(ic, s, false);
        commitText(ic, value);
        e.filterAndAccept();
        return;
      }
      flushNumericPreview(ic, s);
    }
    const auto scalar = Key::keySymToUnicode(sym);
    const auto commandState = key.states().toInteger() &
                              uint32_t(KeyState::SimpleMask) &
                              ~uint32_t(KeyState::Shift);
    const bool nativeNumericKey =
        s->preedit.empty() && !s->english && !s->fullWidth &&
        !englishPunctuation(s) && symbolAutoChange_ &&
        Key::keySymToUnicode(e.rawKey().sym()) == scalar &&
        ((sym >= FcitxKey_0 && sym <= FcitxKey_9) ||
         (s->symbolAutoState == 3 &&
          (sym == FcitxKey_period || sym == FcitxKey_comma ||
           sym == FcitxKey_colon || sym == FcitxKey_space)) ||
         (replayingCursor_ && s->pendingPairKey &&
          scalar == s->pendingPairKey));
    // Preserve the application's own native events while no IM text is
    // waiting. Only text that could overtake them needs to be queued.
    const bool nativeClientKey =
        s->pendingClientKey && legacyClientKeys(ic) &&
        (e.isRelease() || key.isModifier() || commandState ||
         nativeNumericKey || scalar < 0x20 || scalar == 0x7f) &&
        std::none_of(s->cursorKeys.begin(), s->cursorKeys.end(),
                     [](const DeferredKey &key) {
                       const auto c = Key::keySymToUnicode(key.key.sym());
                       return !key.release && c >= 0x20 && c != 0x7f &&
                              !key.key.isModifier();
                     });
    if (!nativeClientKey && s->pendingClientKey) {
      if (!e.isRelease())
        s->modifierCandidate = Key();
      s->cursorKeys.push_back({e.rawKey(), e.isRelease(), e.time(), replayingForwarded_});
      e.filterAndAccept();
      return;
    }
    if (s->pendingCursorSteps) {
      scheduleCursorMove();
      if (!key.isModifier()) {
        const auto commands = key.states().toInteger() &
                              uint32_t(KeyState::SimpleMask) &
                              ~uint32_t(KeyState::Shift);
        const auto scalar = Key::keySymToUnicode(sym);
        const bool queuedText =
            std::any_of(s->cursorKeys.begin(), s->cursorKeys.end(),
                        [](const DeferredKey &key) {
                          const auto c = Key::keySymToUnicode(key.key.sym());
                          return !key.release && c >= 0x20 && c != 0x7f;
                        });
        const bool enter = sym == FcitxKey_Return || sym == FcitxKey_KP_Enter;
        const bool orderedEdit =
            (enter &&
             (queuedText || !e.rawKey().states().test(KeyState::Shift))) ||
            (queuedText &&
             (sym == FcitxKey_BackSpace || sym == FcitxKey_Delete ||
              sym == FcitxKey_Escape));
        if (!commands && ((scalar >= 0x20 && scalar != 0x7f) || orderedEdit)) {
          // This is a chord even if its printable key is replayed later.
          // Otherwise releasing Shift can switch language before a queued
          // nested bracket is processed.
          if (!e.isRelease())
            s->modifierCandidate = Key();
          s->cursorKeys.push_back({e.rawKey(), e.isRelease(), e.time(), replayingForwarded_});
          e.filterAndAccept();
          return;
        }
        // Explicit application shortcuts and navigation override automatic
        // caret placement. Preserve earlier text before processing that key.
        auto keys = std::move(s->cursorKeys);
        clearCursorMove(s);
        replayDeferredKeys(ic, std::move(keys), true);
      }
    }
    if (voiceAvailable() && e.rawKey().states().test(KeyState::Repeat) &&
        ((*config_.voice->launchShortcut &&
          key.checkKeyList(*config_.voice->launchKey)) ||
         (*config_.voice->holdShortcut &&
          key.checkKeyList(*config_.voice->holdKey)))) {
      e.filterAndAccept();
      return;
    }
    // A later key must not operate on the preedit awaiting a select reply.
    // Re-enter normal event processing after that reply has committed it.
    if (s->selectionPending && (sym != FcitxKey_Escape || replayingKeys_)) {
      s->deferredKeys.push_back({e.rawKey(), e.isRelease(), e.time(), replayingForwarded_});
      e.filterAndAccept();
      return;
    }
    if (replayingForwarded_) {
      s->modifierCandidate = Key();
      s->symbolAutoState = 0;
      clearSpacing(s);
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
    if (!voiceAvailable() &&
        ((*config_.voice->launchShortcut && key.checkKeyList(*config_.voice->launchKey)) ||
         (*config_.voice->holdShortcut && key.checkKeyList(*config_.voice->holdKey)))) {
      showModeInfo(instance_, ic, "单机模式已开启；请在语音设置中选择本地离线识别，或关闭单机模式");
      e.filterAndAccept();
      return;
    }
    if (voiceAvailable() && *config_.voice->launchShortcut &&
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
    if (voiceAvailable() && *config_.voice->holdShortcut &&
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
    if (!s->english && *config_.shortcuts->punctuationSwitch &&
        key.checkKeyList(*config_.shortcuts->punctuationSwitchKeys)) {
      if (e.rawKey().states().test(KeyState::Repeat)) {
        e.filterAndAccept();
        return;
      }
      s->englishPunctuation = !englishPunctuation(s);
      s->symbolAutoState = 0;
      const bool saved = !*config_.input->rememberPunctuation ||
                         rememberPunctuation(!*s->englishPunctuation);
      modeChanged(ic, s, saved);
      if (!saved)
        showModeInfo(instance_, ic, modeDescription(s) + "（未能保存标点偏好）");
      e.filterAndAccept();
      return;
    }
    if (*config_.shortcuts->traditionalSwitch &&
        key.checkKeyList(*config_.shortcuts->traditionalSwitchKeys)) {
      if (e.rawKey().states().test(KeyState::Repeat)) {
        e.filterAndAccept();
        return;
      }
      s->traditional = !s->traditional;
      if (s->vMode)
        cancelVMode(ic);
      if (child_ > 0)
        send(ic, "reconfigure");
      modeChanged(ic, s);
      e.filterAndAccept();
      return;
    }
    if (*config_.shortcuts->halfFull &&
        key.checkKeyList(*config_.shortcuts->halfFullKeys)) {
      if (e.rawKey().states().test(KeyState::Repeat)) {
        e.filterAndAccept();
        return;
      }
      if (!s->englishWord.empty()) {
        commitEnglishWord(ic, s);
        panel(ic, s);
      }
      s->fullWidth = !s->fullWidth;
      s->symbolAutoState = 0;
      modeChanged(ic, s);
      e.filterAndAccept();
      return;
    }
    const bool commandModifiers =
        key.states().toInteger() & uint32_t(KeyState::SimpleMask) &
        ~uint32_t(KeyState::Shift);
    if (s->english && handleEnglishCompletion(ic, s, e, sym, commandModifiers)) {
      e.filterAndAccept();
      return;
    }
    const bool hasComposition = !s->preedit.empty() || s->vMenu != VMenu::None;
    const bool vModeKey = *config_.shortcuts->vMode &&
                          *config_.input->mode ==
                              wetype_config::InputMode::Pinyin &&
                          key.checkKeyList(*config_.shortcuts->vModeKeys);
    const bool idleShortcut =
        (!s->english && s->vMode && vModeKey) ||
        (!hasComposition &&
        ((!s->english && vModeKey) ||
         (networkEnabled_ && *config_.shortcuts->aiAssistant &&
          key.checkKeyList(*config_.shortcuts->aiAssistantKeys))));
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
      if (!s->fullWidth && nativeEnglishField(ic))
        return;
      if (sym >= 0x20 && sym <= 0x7e) {
        const char ascii = char(sym);
        const auto *entry = punctuationEntry(ascii);
        if (entry && s->fullWidth) {
          commitSymbol(ic, s, ascii, true);
          e.filterAndAccept();
        } else if (s->fullWidth) {
          uint32_t full = sym == FcitxKey_space ? 0x3000 : sym + 0xfee0;
          commitText(ic, utf8::UCS4ToUTF8(full));
          e.filterAndAccept();
        } else {
          if (autoSpaceEnabled(ic, s) &&
              isNeedSpace(lastScalar(beforeWindow(ic, s)), char32_t(sym))) {
            // Commit the first letter and its gap together. Mixing a separate
            // commit with a pass-through key can reorder them in GTK/Qt.
            commitText(ic, std::string(1, ascii));
            e.filterAndAccept();
          }
        }
      }
      return;
    }
    static constexpr std::string_view vAliases[] = {
        "vriqi", "vrq", "vdate", "vshijian", "vsj", "vtime"};
    if (!commandModifiers && (sym == FcitxKey_space ||
        (sym >= FcitxKey_1 && sym <= FcitxKey_9)) && bareVEnabled() &&
        !s->english && !s->vMode && s->cursor == s->preedit.size() &&
        std::find(std::begin(vAliases), std::end(vAliases), s->preedit) != std::end(vAliases)) {
      const auto prefix = s->preedit;
      if (enterVMode(ic, s, prefix, false)) {
        s->vCalculator = false;
        selectByKey(ic, sym == FcitxKey_space ? -1 : int(sym - FcitxKey_1));
      }
      e.filterAndAccept();
      return;
    }
    if (s->vMode && (sym == FcitxKey_Escape || vModeKey)) {
      cancelVMode(ic);
      e.filterAndAccept();
      return;
    }
    const bool vPrefix = pendingVPrefix(s) && !commandModifiers;
    if (vPrefix && s->preedit == "v" && sym == FcitxKey_space) {
      const auto prefix = s->preedit;
      enterVMode(ic, s, prefix, true);
      e.filterAndAccept();
      return;
    }
    if (vPrefix && sym < 0x80 && std::string_view("+-*/%^()=").find(char(sym)) !=
            std::string_view::npos &&
        std::any_of(s->preedit.begin() + 1, s->preedit.end(),
                    [](char c) { return c >= '0' && c <= '9'; })) {
      const auto prefix = s->preedit;
      if (!enterVMode(ic, s, prefix, false)) {
        e.filterAndAccept();
        return;
      }
    }
    bool composing = !s->preedit.empty() || s->vMenu != VMenu::None;
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
    if (vPrefix && !s->vMode &&
        ((sym >= FcitxKey_0 && sym <= FcitxKey_9) ||
         sym == FcitxKey_period || sym == FcitxKey_plus ||
         sym == FcitxKey_minus || sym == FcitxKey_parenleft ||
         sym == FcitxKey_parenright)) {
      typeCharacter(char(sym));
      return;
    }
    if (s->vMode && s->vMenu == VMenu::None && !commandModifiers && !vModeKey &&
        sym >= 0x20 && sym <= 0x7e) {
      if (vModeSession_ == s->id)
        stopVModeWindow();
      s->vCalculator = true;
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
        (!keypadOperator && *config_.shortcuts->pageMinusEqual &&
         sym == FcitxKey_equal) ||
        (*config_.shortcuts->pageBrackets && sym == FcitxKey_bracketright) ||
        (!keypadOperator && *config_.shortcuts->pageCommaPeriod &&
         sym == FcitxKey_period) ||
        (*config_.shortcuts->pageShiftTab &&
         !key.states().test(KeyState::Shift) && sym == FcitxKey_Tab);
    bool pageUp =
        sym == FcitxKey_Page_Up ||
        key.checkKeyList(instance_->globalConfig().defaultPrevPage()) ||
        key.checkKeyList(*config_.shortcuts->previousPageKeys) ||
        (!keypadOperator && *config_.shortcuts->pageMinusEqual &&
         sym == FcitxKey_minus) ||
        (*config_.shortcuts->pageBrackets && sym == FcitxKey_bracketleft) ||
        (!keypadOperator && *config_.shortcuts->pageCommaPeriod &&
         sym == FcitxKey_comma) ||
        (*config_.shortcuts->pageShiftTab &&
         key.states().test(KeyState::Shift) && sym == FcitxKey_Tab);
    auto *list = dynamic_cast<CommonCandidateList *>(
        ic->inputPanel().candidateList().get());
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
    const bool literalV = sym == FcitxKey_v &&
        !(key.states().toInteger() & uint32_t(KeyState::SimpleMask));
    if (!composing && !s->vMode && vModeKey && !literalV) {
      enterVMode(ic, s, {}, true);
      e.filterAndAccept();
      return;
    }
    if (s->vMode && s->vMenu == VMenu::None && s->preedit.empty() &&
        (sym == FcitxKey_Return || sym == FcitxKey_KP_Enter)) {
      e.filterAndAccept();
      return;
    }
    if (s->vMode && s->vMenu == VMenu::None && s->preedit.empty() &&
        (sym == FcitxKey_BackSpace || sym == FcitxKey_Delete)) {
      if (sym == FcitxKey_BackSpace) {
        cancelVMode(ic);
        if (bareVEnabled())
          typeCharacter('v');
      }
      e.filterAndAccept();
      return;
    }

    const Key navigationKey(sym, key.states());
    const bool horizontalNavigation = !s->vMode &&
        (navigationKey.check(FcitxKey_Left) || navigationKey.check(FcitxKey_Right));
    if (composing && horizontalNavigation && s->inputSeq > s->applied) {
      // Wait for this spelling's candidates before moving their highlight.
      // Later keys, including Space, must follow the queued arrow too.
      s->deferredKeys.push_back({e.rawKey(), e.isRelease(), e.time(), replayingForwarded_});
      s->selectionPending = true;
      e.filterAndAccept();
      return;
    }
    // Match the original controller: at the first candidate Left edits the
    // spelling; Right edits until the caret reaches its end, then selects.
    // Wubi navigates candidates directly because it has no pinyin caret.
    const bool candidateNavigation = composing && horizontalNavigation &&
        list && list->size() &&
        (*config_.input->mode == wetype_config::InputMode::Wubi ||
         (sym == FcitxKey_Left && list->globalCursorIndex() > 0) ||
         (sym == FcitxKey_Right && s->cursor >= s->preedit.size()));
    if (candidateNavigation) {
      moveHorizontalCandidate(*list, sym == FcitxKey_Right);
      s->candidatePage = list->currentPage();
      s->candidateCursor = list->globalCursorIndex();
      ic->updateUserInterface(UserInterfaceComponent::InputPanel);
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
        const auto next = std::upper_bound(s->cursorStops.begin(),
                                           s->cursorStops.end(), target);
        target = next != s->cursorStops.end()
                     ? *next
                     : target + firstUtf8Size(std::string_view(s->preedit).substr(target));
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

    // Keypad arithmetic is literal text, not a candidate page shortcut or
    // Chinese punctuation. Consume the current composition before its symbol
    // so a raw forwarded key cannot overtake the asynchronous candidate commit.
    // Configured shortcuts and V-calculator input have already been handled.
    if (keypadOperator) {
      s->symbolAutoState = 0;
      const auto literal = s->fullWidth
                               ? utf8::UCS4ToUTF8(uint32_t(sym) + 0xfee0)
                               : std::string(1, asciiChar);
      if (composing)
        send(ic, "punctuation", std::string(1, asciiChar), 0, -1, -1, literal);
      else {
        deliverNumericText(ic, s, literal);
      }
      e.filterAndAccept();
      return;
    }

    // SymbolAutoChangedHelper in the desktop implementation delays numeric
    // punctuation correction. Numeric punctuation is displayed in
    // Chinese form first; only another digit changes it back to ASCII.
    if (!composing && s->validateNumericState) {
      s->validateNumericState = false;
      const auto context = symbolContext(ic);
      if (!context.valid || !context.selected.empty() ||
          context.before.empty() || context.before.back() < '0' ||
          context.before.back() > '9')
        s->symbolAutoState = 0;
    }
    const bool autoChangeActive = symbolAutoChange_ && !composing &&
                                  !englishPunctuation(s) &&
                                  !s->fullWidth;
    if (autoChangeActive && sym == FcitxKey_space && s->symbolAutoState) {
      s->symbolAutoState = 0;
      deliverNumericText(ic, s, " ");
      e.filterAndAccept();
      return;
    }
    if (!composing && digit && autoChangeActive) {
      s->symbolAutoState = 3;
      deliverNumericText(ic, s, std::string(1, asciiChar));
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
      commitText(ic, utf8::UCS4ToUTF8(full));
      e.filterAndAccept();
      return;
    }
    if (!composing && digit && numericKeyTransport(ic)) {
      // Use the same native event queue as Chinese punctuation even with
      // auto-conversion disabled; an unhandled digit can overtake its commit.
      deliverNumericText(ic, s, std::string(1, asciiChar));
      e.filterAndAccept();
      return;
    }
    if (!composing && entry) {
      if (commitSymbol(ic, s, asciiChar, false))
        e.filterAndAccept();
      return;
    }
    if (composing && entry && !separator) {
      finishPair(s);
      std::string mapped = mappedSymbol(s, asciiChar, false);
      int pairCursor = -1;
      auto [left, right] = splitPair(mapped);
      if (entry->pairType && !right.empty()) {
        auto context = symbolContext(ic);
        // The selected candidate replaces the application's selection first.
        // Do not wrap that old selection again after committing the word.
        context.selected = {};
        if (*config_.input->symbolAutoPair && symbolPairPosition(context)) {
          pairCursor = utf8::length(mapped) - 1;
          rememberPair(s, asciiChar, right);
        } else if (entry->pairType == 2) {
          bool &leftNext =
              asciiChar == '"' ? s->doubleQuoteLeft : s->singleQuoteLeft;
          const bool opening = quoteOpens(context, left, right, leftNext);
          mapped = opening ? left : right;
          leftNext = !opening;
        } else {
          mapped = left;
        }
      }
      send(ic, "punctuation", std::string(1, asciiChar), 0, -1, -1, mapped,
           pairCursor);
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
void EnglishCompletionWord::select(InputContext *ic) const {
  select(ic, false);
}
void EnglishCompletionWord::select(InputContext *ic, bool trailingSpace) const {
  engine_->selectEnglishCompletion(ic, word_, trailingSpace);
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
