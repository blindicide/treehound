#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Opt-in real directory-heavy watch benchmark; preserves fixture/index/report.
Usage: watches.py BUILD OUTPUT [directories=20000] [index-name=baseline]
"""
import hashlib,json,os,pathlib,platform,signal,socket,struct,subprocess,sys,time
build=pathlib.Path(sys.argv[1]).resolve();base=pathlib.Path(sys.argv[2]).resolve()
count=int(sys.argv[3]) if len(sys.argv)>3 else 20000
name=sys.argv[4] if len(sys.argv)>4 else 'baseline'
assert name.isalnum() and count>=1000
base.mkdir(exist_ok=True,parents=True);root=base/'files';root.mkdir(exist_ok=True)
assert not (base/(name+'.db')).exists(), 'use a fresh index name; retain previous evidence'
for i in range(count):
    d=root/f'dir-{i:06}';d.mkdir(exist_ok=True)
    (d/'marker').write_bytes(b'x')
cfg=base/'config';cfg.write_text(f'root = {root}\nwatch = true\nreconcile_interval_hours = 0\n')
endpoint=str(base/'run'/'socket')
log=open(base/(name+'-daemon.log'),'w')
start=time.monotonic()
p=subprocess.Popen([str(build/'treehoundd'),'--config',str(cfg),'--database',str(base/(name+'.db')),'--socket',endpoint],stderr=log)
def call(cmd,**args):
    with socket.socket(socket.AF_UNIX) as s:
        s.settimeout(10);s.connect(endpoint)
        data=json.dumps(dict(v=1,cmd=cmd,**args)).encode();s.sendall(struct.pack('!I',len(data))+data)
        def read(n):
            b=b''
            while len(b)<n:
                part=s.recv(n-len(b));assert part;b+=part
            return b
        return json.loads(read(struct.unpack('!I',read(4))[0]))
def idle():
    deadline=time.monotonic()+300
    while time.monotonic()<deadline:
        assert p.poll() is None
        try:
            s=call('status')
            if not s['busy'] and not s['queued']:return s
        except (FileNotFoundError,ConnectionRefusedError):pass
        time.sleep(.02)
    raise AssertionError('scan deadline')
def ticks():
    f=pathlib.Path(f'/proc/{p.pid}/stat').read_text().rsplit(')',1)[1].split()
    return int(f[11])+int(f[12])
try:
    status=idle();scan=time.monotonic()-start
    assert status['entries']==2*count+1,status
    before=call('roots')['roots'][0]
    assert before['status']=='verified'
    watches=sum(t.count('inotify wd:') for f in pathlib.Path(f'/proc/{p.pid}/fdinfo').iterdir() for t in [f.read_text()])
    assert watches==count+1,watches
    cpu=ticks();start=time.monotonic()
    os.kill(p.pid,signal.SIGSTOP)
    try:
        for i in range(count-1000,count): (root/f'dir-{i:06}'/'marker').write_bytes(b'xx')
    finally:os.kill(p.pid,signal.SIGCONT)
    deadline=time.monotonic()+300
    while time.monotonic()<deadline:
        result=call('search',query='marker',min_size=2,limit=2000)
        assert result['ok'],result
        if len(result['items'])==1000:break
        time.sleep(.02)
    else:raise AssertionError('event convergence deadline')
    idle();burst=time.monotonic()-start
    after=call('roots')['roots'][0]
    assert after['status']=='verified' and after['size']-before['size']==1000,(before,after)
    report=dict(measured_at_utc=time.strftime('%Y-%m-%dT%H:%M:%SZ',time.gmtime()),head=subprocess.check_output(['git','rev-parse','HEAD'],text=True).strip(),working_tree_dirty=bool(subprocess.check_output(['git','status','--porcelain'],text=True)),binary_sha256=hashlib.sha256((build/'treehoundd').read_bytes()).hexdigest(),version=subprocess.check_output([str(build/'treehoundd'),'--version'],text=True).strip(),platform=platform.platform(),filesystem=subprocess.check_output(['stat','-f','-c','%T',str(root)],text=True).strip(),directories=count,indexed_entries=status['entries'],kernel_watches=watches,initial_scan_seconds=scan,root_logical_size_before=before['size'],root_logical_size_after=after['size'],burst_files=1000,burst_seconds=burst,burst_cpu_seconds=(ticks()-cpu)/os.sysconf('SC_CLK_TCK'),memory={l.split(':')[0]:int(l.split()[1])*1024 for l in pathlib.Path(f'/proc/{p.pid}/status').read_text().splitlines() if l.startswith(('VmRSS:','VmHWM:'))})
    (base/(name+'-results.json')).write_text(json.dumps(report,indent=2)+'\n');print(json.dumps(report,indent=2),flush=True)
finally:
    p.terminate();p.wait(timeout=15);log.close()
