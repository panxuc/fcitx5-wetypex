#!/usr/bin/env python3
"""Launch the engine in Fcitx's Flatpak using WeTypeX's sandbox arguments."""

import ctypes
import os
import pathlib
import signal
import stat
import sys


def main(arguments):
    if not pathlib.Path('/.flatpak-info').is_file():
        raise RuntimeError('the Flatpak engine launcher requires a Flatpak sandbox')
    mounts, environment = {}, {}
    offline, parent_death = False, False
    index = 0
    while index < len(arguments) and arguments[index].startswith('--'):
        option = arguments[index]
        index += 1
        if option == '--':
            break
        if option in ('--unshare-net', '--unshare-pid', '--die-with-parent'):
            offline |= option == '--unshare-net'
            parent_death |= option == '--die-with-parent'
            continue
        count = 2 if option in ('--ro-bind', '--ro-bind-try', '--bind',
                                '--setenv', '--symlink') else 1
        values = arguments[index:index + count]
        if len(values) != count:
            raise ValueError('incomplete engine sandbox option: ' + option)
        index += count
        if option in ('--ro-bind', '--ro-bind-try', '--bind'):
            source, destination = values
            if option == '--ro-bind-try' and not pathlib.Path(source).exists():
                continue
            if destination in ('/input', '/support', '/work', '/voice-vad'):
                mounts[destination] = pathlib.Path(source).resolve(strict=True)
            elif source != destination or not (
                    destination == '/usr' or destination == '/lib64' or
                    destination.startswith('/etc/')):
                raise ValueError('unsupported engine sandbox mount')
        elif option == '--setenv':
            key, value = values
            if not key or not all(c.isascii() and (c.isalnum() or c == '_')
                                  for c in key):
                raise ValueError('invalid engine environment name')
            environment[key] = value
        elif option == '--symlink':
            if values not in (['usr/lib', '/lib'], ['usr/bin', '/bin']):
                raise ValueError('unsupported engine sandbox symlink')
        elif option in ('--proc', '--dev', '--tmpfs', '--dir', '--chdir'):
            expected = {'--proc': '/proc', '--dev': '/dev', '--tmpfs': '/tmp',
                        '--dir': '/etc', '--chdir': '/work'}[option]
            if values[0] != expected:
                raise ValueError('unsupported engine sandbox directory')
        else:
            raise ValueError('unsupported engine sandbox option: ' + option)

    if not {'/input', '/support', '/work'} <= mounts.keys():
        raise ValueError('the engine runtime, support and workspace are required')
    command = arguments[index:]
    if len(command) < 3 or command[0] != '/support/wetypex-engine-host':
        raise ValueError('only the WeTypeX engine host can use this launcher')

    def translate(value):
        for alias, directory in sorted(mounts.items(), reverse=True):
            if value == alias or value.startswith(alias + '/'):
                relative = pathlib.PurePosixPath(value).relative_to(alias)
                if '..' in relative.parts:
                    raise ValueError('an engine path leaves its sandbox directory')
                return str(directory / relative)
        return value

    command[:2] = [translate(value) for value in command[:2]]
    path_variables = {'HOME', 'WETYPE_SUPPORT_DIR', 'WETYPE_WORK_DIR',
                      'WETYPE_IDENTITY_FILE', 'WETYPE_P2P_INFO_FILE',
                      'WETYPE_VOICE_FILE', 'WETYPE_SEND_CLIPBOARD_IMAGE',
                      'WETYPE_FLURRY_DOWNLOAD_DIR', 'WETYPE_FLURRY_TEMP_DIR',
                      'WETYPE_AUX_RUNTIME', 'WETYPE_FLURRY_REMOTE_CA_FILE',
                      'WETYPE_WXP2P_DISPATCH_FILE', 'WETYPE_VAD_DIR'}
    for key in path_variables & environment.keys():
        environment[key] = translate(environment[key])
    workspace, support = mounts['/work'], mounts['/support']
    environment['WETYPE_WORKSPACE'] = str(workspace)
    environment['HOME'] = str(workspace)
    environment.setdefault('PATH', '/usr/bin')
    environment['LD_LIBRARY_PATH'] = ':'.join(
        str(path) for path in (support / 'abi/lib', support.parent)
        if path.is_dir())
    os.chdir(workspace)
    if parent_death:
        parent = os.getppid()
        libc = ctypes.CDLL(None, use_errno=True)
        libc.prctl.argtypes = [ctypes.c_int] + [ctypes.c_ulong] * 4
        if libc.prctl(1, signal.SIGTERM, 0, 0, 0) or os.getppid() != parent:
            raise RuntimeError('the engine parent exited during startup')
    # Do not pass ambient display, D-Bus or network connections to the core.
    # Keep regular-file descriptors holding the runtime and account leases.
    leases = []
    for entry in pathlib.Path('/proc/self/fd').iterdir():
        fd = int(entry.name)
        if fd > 2:
            try:
                mode = os.fstat(fd).st_mode
                if stat.S_ISSOCK(mode):
                    os.close(fd)
                elif stat.S_ISREG(mode):
                    os.set_inheritable(fd, True)
                    leases.append(fd)
            except OSError:
                pass
    # The host Flatpak service creates this child sandbox: unlike nested
    # Bubblewrap, this works with Flatpak's namespace syscall restrictions.
    # Keep credentials in an inherited fd, outside the process argument list.
    environment_fd = os.memfd_create('wetypex-engine-environment', flags=0)
    os.write(environment_fd, b'\0'.join(os.fsencode(k + '=' + v)
                                     for k, v in environment.items()) + b'\0')
    os.lseek(environment_fd, 0, os.SEEK_SET)
    launch = ['/usr/bin/flatpak-spawn', '--sandbox', '--watch-bus', '--clear-env',
              '--env-fd=' + str(environment_fd), '--directory=' + str(workspace)]
    if offline:
        launch.append('--no-network')
    for alias, path in mounts.items():
        if alias == '/work':
            launch.append('--sandbox-expose-path=' + str(path))
        elif not path.is_relative_to('/app'):
            launch.append('--sandbox-expose-path-ro=' + str(path))
    launch.extend('--forward-fd=' + str(fd) for fd in leases)
    launch.extend(['--', *command])
    # Flatpak always exposes the caller's session bus at this sandbox path,
    # including when Fcitx is running on an independent verification bus.
    if not pathlib.Path('/run/flatpak/bus').is_socket():
        raise RuntimeError('the Flatpak session service socket is unavailable')
    os.execve(launch[0], launch, {'PATH': '/usr/bin',
              'DBUS_SESSION_BUS_ADDRESS': 'unix:path=/run/flatpak/bus',
              'XDG_RUNTIME_DIR': '/run/user/' + str(os.getuid())})


if __name__ == '__main__':
    try:
        main(sys.argv[1:])
    except (OSError, ValueError, RuntimeError) as error:
        print('WeTypeX Flatpak engine: ' + str(error), file=sys.stderr)
        raise SystemExit(78)
