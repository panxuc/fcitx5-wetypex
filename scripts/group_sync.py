"""Forward device-group snapshots to the running original input core."""
import json
import os
import socket
import time


def request_group_sync(state, group, timeout=10):
    def number(name):
        try:
            value = int(group.get(name, 0))
        except (TypeError, ValueError) as error:
            raise ValueError('设备组同步状态无效') from error
        if not 0 <= value < 2**63:
            raise ValueError('设备组同步状态无效')
        return value

    group_id = number('group_id')
    request = {'session': 1, 'seq': time.time_ns(), 'epoch': 1,
               'op': 'group_sync', 'group_id': group_id,
               'func_switch': number('func_switch') if group_id else 0,
               'hotword_version': number('hotword_version') if group_id else 0}
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as client:
        client.settimeout(timeout)
        directory = os.open(state, os.O_RDONLY | os.O_DIRECTORY)
        try:
            # State roots can exceed AF_UNIX's pathname limit.
            client.connect('/proc/self/fd/{}/control.sock'.format(directory))
        finally:
            os.close(directory)
        client.sendall(json.dumps(request).encode() + b'\n')
        reply = bytearray()
        while b'\n' not in reply:
            chunk = client.recv(4096)
            if not chunk or len(reply) + len(chunk) > 65536:
                raise ValueError('输入核心返回了无效同步响应')
            reply.extend(chunk)
    response = json.loads(reply.split(b'\n', 1)[0])
    if (not isinstance(response, dict) or
            response.get('seq') != request['seq'] or
            response.get('session') != request['session']):
        raise ValueError('输入核心返回了无效同步响应')
    if not response.get('requested'):
        raise ValueError(response.get('error', '输入核心未接受同步请求'))
    if any(response.get(name) != request[name]
           for name in ('group_id', 'func_switch', 'hotword_version')):
        raise ValueError('同步组件版本不匹配，请重启 Fcitx5 后重试')
    return response
