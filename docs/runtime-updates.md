# Runtime updates and ARM64

WeTypeX separates the Linux adapter from the original input engine. Install the
adapter using your distribution's package manager. The setup tool downloads
original components directly from the official WeChat Input download service,
checks their SHA-256 hashes, and prepares the slice for your CPU architecture.
Original binaries are stored in your user directory and are not redistributed
inside WeTypeX packages.

## Install or upgrade

Open **升级和反馈** in WeTypeX Settings and select **下载并更新原版核心**.
The download and checks continue if you close Settings; reopen it to see the
result. The equivalent command is:

```sh
fcitx5-wetypex-setup --update --download --accept-upstream-license
```

Accept the original software license before downloading. For an offline
installation, supply the official ZIP with `--archive /path/to/WeType.zip`.
Profiles that combine components from different upstream releases may require
more than one archive. Import each with `--cache-archive FILE`, then run `--update`, or
allow missing components to download with `--download`.

Existing 2.2.3 installations are recognized by the core image's hash, including
installations without version metadata. Their original directory, account,
custom themes and user dictionaries are preserved during migration. When first
installing an adapter that supports live updates, restart Fcitx5 once to load its
new plugin (`fcitx5 -r -d`). Subsequent prepared updates are applied after input,
voice input and outstanding requests finish. An open transfer or account
operation defers activation through a shared runtime lease. Keep typing normally;
Settings reports a prepared update until it can be applied.

The automatic-update setting controls both compatible core preparation and
adapter release checks, including in standalone mode. It checks at most once a
day, retries failed checks after an hour, and never automatically downgrades an
explicitly selected newer beta. Adapter package installation uses the normal
privilege prompt. Nix users update the adapter through their Nix configuration;
compatible user-directory core updates work through the same setup tool.

The adapter only activates versions in its shipped compatibility catalog. The
website's newest version is discovered dynamically, then validated before being
added to that catalog. A newly published, unvalidated upstream binary cannot
silently replace a working engine.

```sh
fcitx5-wetypex-setup --status
fcitx5-wetypex-setup --query-upstream
fcitx5-wetypex-setup --check --transport
```

The current default is core **3.0.1.684**. The compatible transport, voice assets
and UI resources remain pinned to **2.2.3.657**. The newer transport frameworks
also use Objective-C and Swift interfaces; they require separate compatibility
work. The version shown in Settings identifies the input core.

## Rollback and storage

Select **回退上个版本**, or run:

```sh
fcitx5-wetypex-setup --rollback
```

Rollback retains the current pairing identity. For profiles with the same
validated user-data format, it keeps the latest learned words and common phrases.
Across data formats, the saved snapshot restores the previous data format.
After an explicit rollback, automatic core updates pause for that catalog's
default version; a later supported release or a manual update can advance it.

A completed update automatically keeps the current generation, one rollback
generation, any prepared update, and the required data snapshot. Original ZIPs
are removed once complete installed generations no longer need them. Verified
engine images and dictionary files share read-only storage on the same
filesystem. Mutable themes and user data remain independent; filesystems with
copy-on-write cloning can also reduce snapshot overhead. The preserved original
legacy directory and unrecognized user files are left intact.

```sh
fcitx5-wetypex-setup --cleanup --dry-run
fcitx5-wetypex-setup --cleanup
```

Shared archive caches honor other installations' pending-update reservations.
An update interrupted by power loss recovers from its journal before activating
another generation. A failed download or compatibility check leaves the current
runtime selected. Background operation details are in
`$XDG_DATA_HOME/fcitx5-wetypex/runtime-operation.log` (by default,
`~/.local/share/fcitx5-wetypex/runtime-operation.log`).

## ARM64

The ARM64 adapter loads the official ordinary `arm64` slice and bridges Darwin's
calling conventions, thread-local storage, callbacks and C++ ABI. Its private
libc++ uses Apple's alternate string layout and is separate from the Qt
applications' system C++ runtime. `arm64e` images are rejected.

CPU features required by the original Apple Silicon binary are checked before
loading it. Supported CPUs execute it directly. Older CPUs can install the
optional distribution `qemu-user` package; automatic provider selection then
runs the same ARM64 binary through QEMU. `network-info` reports
`native_cpu_supported`, `execution_provider` and `execution_ready` without loading
the original engine. Keep `WETYPE_ENGINE_PROVIDER=auto` for normal use.

Release builds provide ARM64 Arch Linux, Debian/Ubuntu, legacy Ubuntu and Fedora
packages, plus an archive. The legacy Ubuntu build targets glibc 2.31 and Fcitx5
5.0.14; stock Ubuntu 20.04 repositories need a suitable Fcitx5 backport. Native
ARM64 CI covers package builds and actual core/TLS checks. QEMU validation on an
x86-64 machine supplements this; tablet desktop, physical keyboard and microphone
behavior still need device testing.

For a smaller Nix closure, `enableAI = false` omits QtWebEngine. ARM compatibility
emulation is opt-in through `enableCompatibilityProvider = true`, so capable
ARM64 machines do not pull QEMU into the default closure.

## Maintainer automation

Three workflows discover official stable releases, open an `upstream/runtime-*`
PR, and validate it on x86-64 and native ARM64 runners. Validation includes old
Linux package builds, input candidates, persisted common-phrase upgrade and
reverse compatibility, local TLS transfers, anonymous pairing and speech
recognition using independent temporary identities.

Automation is off by default. Set repository variable
`WETYPEX_UPSTREAM_AUTOMATION` to `pr` to enable discovery and validation while
keeping merging and publishing manual. Set it to `release` to additionally allow
validated generated updates to be squash-merged and tagged. Remove the variable
to disable it. GitHub Actions must be allowed to create pull requests.

The privileged completion job uses trusted `main` code to independently regenerate
the expected patch. It checks exact file contents, the head and base commits,
archive hashes and every verification job. Modified candidates, incomplete
checks and a moving base block publication. Its generated update scope is the
catalog and version metadata; changes to compatibility code require review.
The tag is checked against the merged tree, and the release workflow is explicitly
dispatched because tags created with `GITHUB_TOKEN` do not trigger workflows.
