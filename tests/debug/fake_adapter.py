import json
import sys
import time

seq = 0
current_value = "42"
cortex_hex = False

def send(obj):
    global seq
    seq += 1
    obj['seq'] = seq
    data = json.dumps(obj, ensure_ascii=False).encode()
    wire = f'Content-Length: {len(data)}\r\n\r\n'.encode() + data
    # Exercise fragmented headers, bodies and UTF-8 with multiple frames in a read.
    for start in range(0, len(wire), 7):
        sys.stdout.buffer.write(wire[start:start + 7])
        sys.stdout.buffer.flush()

def event(name, body):
    send(dict(type='event', event=name, body=body))

while True:
    headers = {}
    while True:
        line = sys.stdin.buffer.readline()
        if not line:
            sys.exit(0)
        if line == b'\r\n':
            break
        k, v = line.decode().split(':', 1)
        headers[k.lower()] = v.strip()
    req = json.loads(sys.stdin.buffer.read(int(headers['content-length'])))
    cmd, body = req['command'], {}
    if cmd == 'initialize':
        import os
        if 'TURTLE_ADAPTER_TEST' in os.environ:
            event('turtle-test-environment', {'value': os.environ['TURTLE_ADAPTER_TEST']})
        event('turtle-test-initialize', req['arguments'])
        value_mode = os.environ.get('TURTLE_VALUE_TEST', '')
        body = dict(supportsConfigurationDoneRequest=True)
        if value_mode:
            body.update(supportsSetVariable=value_mode != 'none', supportsSetExpression=value_mode == 'all',
                        supportsValueFormattingOptions=value_mode not in ('none', 'cortex'))
    elif cmd in ('launch', 'attach'):
        cortex_hex = not req['arguments'].get('variableUseNaturalFormat', True) if value_mode == 'cortex' else False
        launch_command = cmd
        launch_args = req['arguments'].get('args', [])
        if 'gdbServerConsolePort' in req['arguments']:
            import socket
            with socket.create_connection(('127.0.0.1', req['arguments']['gdbServerConsolePort'])) as console:
                console.sendall(b'\x1b[32mfake OpenOCD log\x1b[0m\n')
        if '--fail-launch' in launch_args:
            send(dict(type='response', request_seq=req['seq'], command=cmd, success=False,
                      body=dict(error=dict(id=1, format='Cannot launch {program}', variables=dict(program='test fixture')))))
            continue
        event('turtle-test-launch', req['arguments'])
        program = req['arguments']['program']
        launch_seq = req['seq']
        event('initialized', {})
        continue
    elif cmd == 'setBreakpoints':
        body = dict(breakpoints=[dict(verified=True, line=b['line']) for b in req['arguments']['breakpoints']])
    elif cmd == 'configurationDone':
        send(dict(type='response', request_seq=launch_seq, command=launch_command, success=True))
    elif cmd == 'threads':
        body = dict(threads=[dict(id=1, name='main')])
    elif cmd == 'stackTrace':
        body = dict(stackFrames=[dict(id=10, name='тест', line=3, source=dict(path='/no/such/source.c' if '--missing-source' in launch_args else program))])
    elif cmd == 'scopes':
        body = dict(scopes=[dict(name='Locals', variablesReference=20)])
    elif cmd == 'variables':
        formatted = hex(int(current_value)) if value_mode and (cortex_hex or req['arguments'].get('format', {}).get('hex')) else current_value
        body = dict(variables=[dict(name='total', value=formatted, type='int', variablesReference=0)])
        if value_mode:
            body['variables'][0]['evaluateName'] = 'total'
            body['variables'] += [dict(name='opaque field', value=formatted, variablesReference=0),
                                  dict(name='readonly', value='7', evaluateName='readonly', variablesReference=0,
                                       presentationHint=dict(attributes=['readOnly']))]
            if os.environ.get('TURTLE_TREE_TEST'):
                ref = req['arguments']['variablesReference']
                if ref == 20:
                    body['variables'].append(dict(name='record', value='{...}', evaluateName='record', variablesReference=30))
                elif ref == 30:
                    body['variables'] = [dict(name='inner', value='{...}', evaluateName='record.inner', variablesReference=31)]
                elif ref == 31:
                    body['variables'] = [dict(name='leaf', value=formatted, evaluateName='record.inner.leaf', variablesReference=0)]
            event('turtle-test-values', dict(command=cmd, arguments=req['arguments']))
    elif cmd == 'evaluate':
        formatted = hex(int(current_value)) if value_mode and (cortex_hex or req['arguments'].get('format', {}).get('hex')) else current_value
        body = dict(result=formatted, variablesReference=0)
        if os.environ.get('TURTLE_TREE_TEST') and req['arguments']['expression'] == 'record':
            body = dict(result='{...}', variablesReference=30)
        if value_mode:
            event('turtle-test-values', dict(command=cmd, arguments=req['arguments']))
    elif cmd == 'set-var-format':
        cortex_hex = req['arguments']['hex']
        event('turtle-test-values', dict(command=cmd, arguments=req['arguments']))
    elif cmd in ('setExpression', 'setVariable'):
        event('turtle-test-values', dict(command=cmd, arguments=req['arguments']))
        if req['arguments']['value'] == 'reject':
            send(dict(type='response', request_seq=req['seq'], command=cmd, success=False, message='read-only fixture'))
            continue
        current_value = req['arguments']['value']
        body = dict(value=current_value, variablesReference=0)
    elif cmd == 'bad':
        sys.stdout.buffer.write(b'Content-Length: nope\r\n\r\n')
        sys.stdout.buffer.flush()
        time.sleep(1)
        continue
    send(dict(type='response', request_seq=req['seq'], command=cmd, success=True, body=body))
    if cmd in ('configurationDone', 'next', 'stepIn', 'stepOut', 'pause', 'set-var-format'):
        event('stopped', dict(reason='step', threadId=1))
    elif cmd == 'continue':
        event('continued', dict(threadId=1))
        event('output', dict(output='результат: 42; Done: 16\n'))
        event('terminated', {})
    elif cmd == 'disconnect':
        sys.exit(0)
