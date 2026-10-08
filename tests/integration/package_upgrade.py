#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Native package upgrade checkpoint, invoked before/after replacing binaries."""
import json,pathlib,socket,struct,subprocess,sys,time
phase,exe,output=sys.argv[1:];base=pathlib.Path(output).resolve();base.mkdir(exist_ok=True)
root=base/'files';root.mkdir(exist_ok=True)
if phase=='prepare':
    (root/'upgrade-preserved.txt').write_bytes(b'x'*42)
    (base/'config').write_text(f'root = {root}\nwatch = true\nreconcile_interval_hours = 0\n')
sock=str(base/'runtime'/'daemon.sock')
def call(cmd,**kw):
    with socket.socket(socket.AF_UNIX) as s:
        s.settimeout(5);s.connect(sock);data=json.dumps(dict(v=1,cmd=cmd,**kw)).encode();s.sendall(struct.pack('!I',len(data))+data)
        def read(n):
            b=b''
            while len(b)<n:
                part=s.recv(n-len(b));assert part;b+=part
            return b
        return json.loads(read(struct.unpack('!I',read(4))[0]))
proc=subprocess.Popen([exe,'--config',str(base/'config'),'--database',str(base/'index.db'),'--socket',sock])
try:
    for _ in range(300):
        assert proc.poll() is None
        try:
            status=call('status')
            if not status['busy'] and not status['queued']:break
        except (FileNotFoundError,ConnectionRefusedError):pass
        time.sleep(.02)
    else:raise RuntimeError('upgrade daemon deadline')
    roots=call('roots')['roots'];assert len(roots)==1 and roots[0]['status']=='verified',roots
    result=call('search',query='upgrade-preserved');assert result['ok'] and len(result['items'])==1 and result['items'][0]['size']==42,result
    history=call('history',root_id=roots[0]['id']);assert history['ok'] and history['snapshots'],history
    checkpoint=dict(entries=status['entries'],root=roots[0]['id'],entry=result['items'][0]['id'],snapshot=history['snapshots'][0]['id'])
    if phase=='prepare':(base/'checkpoint.json').write_text(json.dumps(checkpoint))
    else:assert checkpoint==json.loads((base/'checkpoint.json').read_text()),checkpoint
finally:
    proc.terminate();assert proc.wait(timeout=10)==0
print('native package upgrade',phase,'preserved indexed identity and history')
