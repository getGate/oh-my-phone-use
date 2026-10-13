#!/usr/bin/env python3
"""Call the running PhoneUse service and return immediately when the tool finishes.

Example: python tools/phone_client.py phone_status
         python tools/phone_client.py phone_turn --args '{"call_id":"...","text":"你好"}'
Use this instead of polling a long-lived stdio terminal. Requires phone_service.py running.
"""
import argparse
import json
from pathlib import Path
import sys
from urllib.request import Request, urlopen


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('tool', choices=['phone_status','phone_start','phone_turn','phone_end','phone_result'])
    parser.add_argument('--args', default='{}', help='JSON tool arguments')
    parser.add_argument('--state-dir', default=str(Path(__file__).resolve().parents[1] / '.state' / 'phoneuse'))
    opts = parser.parse_args()
    connection = json.loads((Path(opts.state_dir) / '.connection.json').read_text())
    # Local connection metadata is never used to send a capability to a remote server.
    from urllib.parse import urlparse
    url = urlparse(connection['url'])
    if url.scheme != 'http' or url.hostname != '127.0.0.1' or url.path != '/mcp':
        raise ValueError('Expected a loopback PhoneUse service URL')
    body = {'jsonrpc':'2.0', 'id':1, 'method':'tools/call',
            'params':{'name':opts.tool,'arguments':json.loads(opts.args)}}
    request = Request(connection['url'], data=json.dumps(body).encode(),
                      headers={'Authorization':'Bearer '+connection['token'],'Content-Type':'application/json'})
    with urlopen(request, timeout=240) as response:
        result = json.load(response)
    output = result.get('result', result)
    print(json.dumps(output.get('structuredContent', output), ensure_ascii=False, indent=2))
    return 1 if result.get('error') or output.get('isError') else 0

if __name__ == '__main__':
    sys.exit(main())
