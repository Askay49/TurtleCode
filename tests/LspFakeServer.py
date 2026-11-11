"""Deterministic LSP peer for protocol regression checks (no network)."""
import json
import sys
import threading
import re
from pathlib import Path
from urllib.parse import urlparse, unquote
import time

lock = threading.Lock()
documents = {}
versions = {}
saved = False
configured = False
semantic_methods = []
language_ids = {}
configuration_result = None
initialization = {}

def send(message, delayed=False):
    payload = json.dumps(message, ensure_ascii=False).encode()
    def write():
        # Fragment a lower-case header and body to exercise stream framing.
        frame = b'content-length: ' + str(len(payload)).encode() + b'\r\n\r\n' + payload
        with lock:
            for chunk in (frame[:7], frame[7:25], frame[25:]):
                sys.stdout.buffer.write(chunk)
                sys.stdout.buffer.flush()
    if delayed:
        threading.Timer(0.12, write).start()
    else:
        write()

def response(identifier, result, delayed=False):
    send({'jsonrpc': '2.0', 'id': identifier, 'result': result}, delayed)

def offset(text, pos):
    lines = text.splitlines(keepends=True)
    line = pos['line']
    prefix = ''.join(lines[:line])
    current = lines[line] if line < len(lines) else ''
    # UTF-16 columns must not split surrogate pairs.
    column = len(current.encode('utf-16-le')[:pos['character'] * 2].decode('utf-16-le'))
    return len(prefix) + column

while True:
    headers = {}
    while True:
        line = sys.stdin.buffer.readline()
        if not line:
            sys.exit(0)
        if line == b'\r\n':
            break
        key, value = line.decode().split(':', 1)
        headers[key.lower()] = value.strip()
    message = json.loads(sys.stdin.buffer.read(int(headers['content-length'])))
    method = message.get('method')
    params = message.get('params') or {}
    identifier = message.get('id')
    if method == 'initialize':
        initialization = params
        assert params['capabilities']['general']['positionEncodings'] == ['utf-16']
        assert not params['capabilities']['textDocument']['completion']['completionItem']['snippetSupport']
        if '--init-error' in sys.argv:
            send({'jsonrpc': '2.0', 'id': identifier, 'error': {'code': -32603, 'message': 'test initialization error'}})
            continue
        response(identifier, {'capabilities': {'positionEncoding': 'utf-16', 'textDocumentSync': {'openClose': True, 'change': 2, 'save': {'includeText': True}}, 'documentSymbolProvider': '--no-outline' not in sys.argv, 'documentFormattingProvider': False if '--no-format' in sys.argv else {}, 'completionProvider': {}, 'hoverProvider': True, 'definitionProvider': True, 'signatureHelpProvider': {}, 'renameProvider': {'prepareProvider': True}, 'semanticTokensProvider': {'legend': {'tokenTypes': ['variable', 'parameter', 'type'], 'tokenModifiers': ['declaration', 'static']}, 'full': '--range-only' not in sys.argv, 'range': '--semantic-range' in sys.argv or '--range-only' in sys.argv}}})
    elif method == 'initialized':
        send({'jsonrpc': '2.0', 'id': 'config-request', 'method': 'workspace/configuration', 'params': {'items': ([{'section': 'custom'}, {'section': 'custom.nested'}, {}, {'section': 'missing'}] if '--any-language' in sys.argv else [{'section': 'custom'}])}})
    elif identifier == 'config-request':
        configuration_result = message.get('result')
        if '--any-language' not in sys.argv:
            assert configuration_result == [{}]
        configured = True
        send({'jsonrpc': '2.0', 'id': 'semantic-refresh', 'method': 'workspace/semanticTokens/refresh'})
    elif identifier == 'semantic-refresh':
        assert message.get('result', 'missing') is None
    elif method == 'textDocument/didOpen':
        doc = params['textDocument']
        if '--any-language' not in sys.argv:
            assert doc['languageId'] in ('custom', 'cpp')
        language_ids[doc['uri']] = doc['languageId']
        documents[doc['uri']] = doc['text']
        versions[doc['uri']] = doc['version']
    elif method == 'textDocument/didChange':
        doc = params['textDocument']; uri = doc['uri']
        assert doc['version'] > versions[uri]
        for edit in params['contentChanges']:
            assert 'range' in edit, 'incremental sync was negotiated'
            a = offset(documents[uri], edit['range']['start'])
            b = offset(documents[uri], edit['range']['end'])
            documents[uri] = documents[uri][:a] + edit['text'] + documents[uri][b:]
        versions[uri] = doc['version']
    elif method == 'textDocument/didSave':
        uri = params['textDocument']['uri']; assert params['text'] == documents[uri]; saved = True
    elif method == 'textDocument/didClose':
        uri = params['textDocument']['uri']; documents.pop(uri); versions.pop(uri)
    elif method == 'textDocument/documentSymbol':
        assert initialization['capabilities']['textDocument']['documentSymbol']['hierarchicalDocumentSymbolSupport']
        uri = params['textDocument']['uri']
        text = documents[uri]
        start = {'line': 0, 'character': 4}
        rng = {'start': start, 'end': {'line': 0, 'character': 9}}
        if 'outline_empty' in text:
            response(identifier, [])
        elif 'outline_error' in text:
            send({'jsonrpc': '2.0', 'id': identifier, 'error': {'code': -32603, 'message': 'test outline error'}})
        elif uri.endswith('.py'):
            response(identifier, [{'name': 'python_value', 'kind': 13, 'location': {'uri': uri, 'range': rng}}], delayed=True)
        else:
            response(identifier, [{'name': 'Sample', 'detail': 'class', 'kind': 5, 'range': rng,
                'selectionRange': {'start': {'line': 0, 'character': 0}, 'end': start},
                'children': [{'name': 'value', 'detail': 'int', 'kind': 8, 'range': rng, 'selectionRange': rng}]}], delayed=True)
    elif method == 'textDocument/formatting':
        assert params['options'] == {'tabSize': 4, 'insertSpaces': True}
        uri = params['textDocument']['uri']
        text = documents[uri]
        if '--format-error' in sys.argv:
            send({'jsonrpc': '2.0', 'id': identifier, 'error': {'code': -32603, 'message': 'test formatting error'}})
            continue
        if '--format-null' in sys.argv:
            response(identifier, None)
            continue
        formatted = text.replace('int  value=1;', 'int value = 1;').replace('\r\n', '\n')
        lines = text.split('\n')
        edits = [] if formatted == text else [{'range': {'start': {'line': 0, 'character': 0},
                   'end': {'line': len(lines)-1, 'character': len(lines[-1].encode('utf-16-le')) // 2}},
                   'newText': formatted}]
        if '--format-invalid' in sys.argv:
            edits = [{'range': {'start': {'line': 999999, 'character': 0}, 'end': {'line': 999999, 'character': 1}}, 'newText': 'bad'}]
        response(identifier, edits, delayed=True)
    elif method in ('textDocument/prepareRename', 'textDocument/rename'):
        uri = params['textDocument']['uri']
        text = documents[uri]
        pos = offset(text, params['position'])
        symbol = next((m for m in re.finditer(r'\w+', text) if m.start() <= pos <= m.end()), None)
        if not symbol:
            response(identifier, None)
            continue
        if method == 'textDocument/prepareRename':
            start = text.rfind('\n', 0, symbol.start()) + 1
            line = text[:start].count('\n')
            a = len(text[start:symbol.start()].encode('utf-16-le')) // 2
            b = a + len(symbol.group().encode('utf-16-le')) // 2
            response(identifier, {'range': {'start': {'line': line, 'character': a}, 'end': {'line': line, 'character': b}}, 'placeholder': symbol.group()})
        else:
            targets = dict(documents)
            if '--rename-other' in sys.argv:
                other = Path(sys.argv[sys.argv.index('--rename-other') + 1])
                targets.setdefault(other.as_uri(), other.read_text())
            changes = []
            for target, content in targets.items():
                edits = []
                for line, row in enumerate(content.split('\n')):
                    for match in re.finditer(r'\b' + re.escape(symbol.group()) + r'\b', row):
                        a = len(row[:match.start()].encode('utf-16-le')) // 2
                        b = a + len(match.group().encode('utf-16-le')) // 2
                        edits.append({'range': {'start': {'line': line, 'character': a}, 'end': {'line': line, 'character': b}}, 'newText': params['newName']})
                if edits:
                    changes.append({'textDocument': {'uri': target, 'version': versions.get(target)}, 'edits': edits})
            response(identifier, {'documentChanges': changes})
    elif method in ('textDocument/semanticTokens/full', 'textDocument/semanticTokens/range'):
        semantic_methods.append(method)
        requested = params.get('range')
        text = documents[params['textDocument']['uri']]
        data = []
        previous_line = previous_start = 0
        for line, row in enumerate(text.split('\n')):
            if requested and not requested['start']['line'] <= line <= requested['end']['line']:
                continue
            for match in re.finditer(r'\w+', row):
                start = len(row[:match.start()].encode('utf-16-le')) // 2
                length = len(match.group().encode('utf-16-le')) // 2
                data.extend([line - previous_line, start if line != previous_line else start - previous_start, length, 2 if match.group() == 'int' else 1, 1])
                previous_line, previous_start = line, start
        response(identifier, {'data': data}, delayed=True)
    elif method == 'textDocument/completion':
        uri = params['textDocument']['uri']
        response(identifier, [{'label': documents[uri], 'detail': str(versions[uri])}], delayed=True)
    elif method == 'textDocument/definition':
        response(identifier, {'uri': params['textDocument']['uri'], 'range': {'start': {'line': 1, 'character': 0}, 'end': {'line': 1, 'character': 0}}})
    elif method == 'textDocument/declaration':
        response(identifier, [{'targetUri': params['textDocument']['uri'], 'targetRange': {'start': {'line': 0, 'character': 0}, 'end': {'line': 0, 'character': 14}}, 'targetSelectionRange': {'start': {'line': 0, 'character': 4}, 'end': {'line': 0, 'character': 9}}}])
    elif method == 'textDocument/typeDefinition':
        response(identifier, {'uri': params['textDocument']['uri'], 'range': {'start': {'line': 0, 'character': 0}, 'end': {'line': 0, 'character': 3}}})
    elif method == 'textDocument/references':
        assert params['context']['includeDeclaration'] is True
        assert params['position']['character'] == 5
        uri = params['textDocument']['uri']
        response(identifier, [{'uri': uri, 'range': {'start': {'line': 0, 'character': col}, 'end': {'line': 0, 'character': col + 1}}} for col in [4, 10]])
    elif method == 'textDocument/hover':
        uri = params['textDocument']['uri']
        response(identifier, {'contents': {'kind': 'plaintext', 'value': documents[uri][:1000] if '--compact-hover' in sys.argv else documents[uri]}, 'saved': saved, 'configured': configured, 'semanticMethods': semantic_methods, 'languageId': language_ids[uri], 'configuration': configuration_result, 'initialization': initialization})
        if '--log-flood' in sys.argv:
            send({'jsonrpc':'2.0', 'method':'window/showMessage', 'params':{'type':1,'message':'PRESERVED_TEST_ERROR'}})
            for i in range(300):
                send({'jsonrpc':'2.0', 'method':'window/logMessage', 'params':{'type':3,'message':str(i) + ' события ' * 100}})
            sys.stderr.write('stderr-tail ' * 20000)
            sys.stderr.flush()
        send({'jsonrpc': '2.0', 'method': 'textDocument/publishDiagnostics', 'params': {'uri': uri, 'version': versions[uri]-1, 'diagnostics': [{'message': 'stale'}]}})
        send({'jsonrpc': '2.0', 'method': 'textDocument/publishDiagnostics', 'params': {'uri': uri, 'version': versions[uri], 'diagnostics': [{'message': 'current'}]}})
    elif method == 'test/logs':
        send({'jsonrpc':'2.0', 'method':'window/showMessage', 'params':{'type':1,'message':'PRESERVED_TEST_ERROR'}})
        for i in range(300):
            send({'jsonrpc':'2.0', 'method':'window/logMessage', 'params':{'type':3,'message':str(i) + ' события ' * 100}})
        sys.stderr.write('stderr-tail ' * 20000)
        sys.stderr.flush()
        response(identifier, None)
    elif method == 'shutdown':
        response(identifier, None)
    elif method == 'exit':
        break
