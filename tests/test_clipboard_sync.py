"""Exercise the sync daemon's first receive with isolated service/tool fixtures."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

FAKE_TOOL = r'''
import json, os
from pathlib import Path
import sys, time
kind = Path(sys.argv[0]).name
args = sys.argv[1:]
if args == ["--version"]:
    print("wl-clipboard 2.3.0")
elif kind == "clipboard-helper":
    print("zwlr_data_control_manager_v1")
elif kind == "account-tool":
    if args[0] != "group-info":
        with open(os.environ["TEST_SEND_LOG"], "a") as output:
            output.write(args[0] + "\n")
        print('{"ok":false}')
        raise SystemExit(1)
    print(json.dumps({"ok": True, "group_id": 1, "func_switch": 1,
        "self_uin": 10, "clipboard_from_uin": 20, "clipboard_version": 1,
        "clipboard_type": 1, "clipboard": "remote test message",
        "clipboard_expire_at": int(time.time()) + 300}))
elif kind == "wl-copy":
    Path(os.environ["TEST_CLIPBOARD_OUTPUT"]).write_bytes(sys.stdin.buffer.read())
elif kind == "wl-paste":
    mode = os.environ["TEST_CLIPBOARD_MODE"]
    if mode == "text":
        print("text/plain;charset=utf-8" if args == ["--list-types"] else "existing local text", end="\n" if args == ["--list-types"] else "")
    else:
        if mode == "bad-output":
            sys.stdout.write("unexpected output")
        sys.stderr.write("Nothing is copied\n" if mode in ("empty", "bad-output") else "Failed to connect to a Wayland server\n")
        raise SystemExit(1)
'''

RUNNER = r'''
import os, runpy, shutil, sys
original = shutil.which
def which(command, mode=os.F_OK | os.X_OK, path=None):
    if command in ("wl-copy", "wl-paste"):
        return os.path.join(os.environ["TEST_TOOLS"], command)
    return original(command, mode=mode, path=path)
shutil.which = which
sys.argv = [sys.argv[1], "--once"]
runpy.run_path(sys.argv[0], run_name="__main__")
'''


class FirstClipboardReceiveTest(unittest.TestCase):
    def run_case(self, mode):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            tools = root / "tools"
            tools.mkdir()
            for name in ["account-tool", "clipboard-helper", "wl-copy", "wl-paste"]:
                target = tools / name
                target.write_text("#!" + sys.executable + "\n" + FAKE_TOOL)
                target.chmod(0o700)
            output = root / "received"
            sends = root / "sent"
            state = root / "state"
            env = os.environ.copy()
            env.update(WETYPE_STATE_DIR=str(state),
                       WETYPE_ACCOUNT_TOOL=str(tools / "account-tool"),
                       WETYPE_CLIPBOARD_HELPER=str(tools / "clipboard-helper"),
                       WAYLAND_DISPLAY="test-display", TEST_TOOLS=str(tools),
                       TEST_CLIPBOARD_MODE=mode, TEST_CLIPBOARD_OUTPUT=str(output),
                       TEST_SEND_LOG=str(sends))
            script = Path(__file__).resolve().parents[1] / "scripts/fcitx5-wetypex-syncd.sh.in"
            subprocess.run([sys.executable, "-c", RUNNER, str(script)],
                           env=env, capture_output=True, check=True, timeout=10)
            self.assertFalse(sends.exists(), "first start must not upload the old local clipboard")
            runtime = json.loads((state / "sync-runtime.json").read_text())
            if mode in ("empty", "text"):
                self.assertEqual(output.read_text(), "remote test message")
                self.assertEqual(runtime["remote_version"], 1)
                self.assertFalse(runtime["clipboard_baseline_pending"])
            else:
                self.assertFalse(output.exists(), "read failures must not overwrite the clipboard")
                self.assertEqual(runtime["remote_version"], 0)
                self.assertTrue(runtime["clipboard_baseline_pending"])

    def test_empty_clipboard_can_receive_first_remote_message(self):
        self.run_case("empty")

    def test_existing_local_baseline_is_not_uploaded(self):
        self.run_case("text")

    def test_connection_failure_is_not_treated_as_empty(self):
        self.run_case("connection-error")

    def test_nonempty_error_output_is_not_treated_as_empty(self):
        self.run_case("bad-output")


if __name__ == "__main__":
    unittest.main()
