#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Opt-in real filesystem/daemon benchmark; never part of routine CTest.
Usage: python3 tests/benchmarks/million.py build /path/to/isolated-output [file-count] [index-name]
Retains the actual fixture/index and JSON evidence for inspection. Test-only Python.
"""
import hashlib,json,os,pathlib,platform,socket,struct,subprocess,sys,time
build=pathlib.Path(sys.argv[1]).resolve();base=pathlib.Path(sys.argv[2]).resolve()
count=int(sys.argv[3]) if len(sys.argv)>3 else 1000000
base.mkdir(parents=True,exist_ok=True);root=base/'files';root.mkdir(exist_ok=True)
index_name=sys.argv[4] if len(sys.argv)>4 else 'index'
assert index_name.isalnum(), 'index name must be alphanumeric'
database=base/(index_name+'.db');initial_scan=not database.exists()
space=os.statvfs(base)
assert space.f_favail>count+2000 and space.f_bavail*space.f_frsize>count*2048, 'insufficient fixture space/inodes'
t0=time.monotonic()
for directory in range((count+999)//1000):
    path=root/f'dir-{directory:04}';path.mkdir(exist_ok=True)
    for i in range(directory*1000,min(count,(directory+1)*1000)):
        token=hashlib.sha256(str(i).encode()).hexdigest()[:12]
        p=path/f'item-{i:07}-{token}.dat'
        if not p.exists():
            with open(p,'wb') as f:
                if i%10<3:f.write(b'x')
    if directory%100==0: print('fixture files',min(count,(directory+1)*1000),flush=True)
fixture_seconds=time.monotonic()-t0
cfg=base/'config';cfg.write_text(f'root = {root}\nwatch = true\nreconcile_interval_hours = 0\n')
runtime=base/'runtime';runtime.mkdir(mode=0o700,exist_ok=True)
sock=str(runtime/'treehound'/'treehoundd.sock')
log=open(base/'daemon.log','w');start=time.monotonic()
daemon=subprocess.Popen([str(build/'treehoundd'),'--config',str(cfg),'--database',str(database),'--socket',sock],stderr=log)
def call(cmd,**kw):
    with socket.socket(socket.AF_UNIX) as s:
        s.settimeout(10);s.connect(sock);b=json.dumps(dict(v=1,cmd=cmd,**kw)).encode();s.sendall(struct.pack('!I',len(b))+b)
        def read(n):
            b=b''
            while len(b)<n:
                part=s.recv(n-len(b));assert part;b+=part
            return b
        return json.loads(read(struct.unpack('!I',read(4))[0]))
def memory():
    values={line.split(':')[0]:int(line.split()[1])*1024 for line in pathlib.Path(f'/proc/{daemon.pid}/status').read_text().splitlines() if line.startswith(('VmRSS:','VmHWM:'))}
    return values
report=dict(head=subprocess.check_output(['git','rev-parse','HEAD'],text=True).strip(),working_tree_dirty=bool(subprocess.check_output(['git','status','--porcelain'],text=True)),binary_sha256={name:hashlib.sha256((build/name).read_bytes()).hexdigest() for name in ('treehoundd','treehound')},version=subprocess.check_output([str(build/'treehoundd'),'--version'],text=True).strip(),count=count,platform=platform.platform(),fixture_seconds=fixture_seconds,filesystem=subprocess.check_output(['stat','-f','-c','%T',str(root)],text=True).strip(),cpu_count=os.cpu_count(),initial_scan=initial_scan)
try:
    peak=0
    while time.monotonic()-start<1800:
        assert daemon.poll() is None,'daemon exited during indexing'
        peak=max(peak,memory()['VmRSS'])
        try:
            status=call('status')
            if not status['busy'] and status['queued']==0:break
        except (FileNotFoundError,ConnectionRefusedError):pass
        time.sleep(.25)
    else:raise RuntimeError('indexing deadline')
    report.update(scan_seconds=time.monotonic()-start,indexed_entries=status['entries'],scan_peak_rss_bytes=peak,roots=call('roots')['roots'])
    assert status['entries']==count+(count+999)//1000+1,status
    assert report['roots'][0]['status']=='verified',report['roots']
    samples=[]
    queries=[hashlib.sha256(str(i*7919%count).encode()).hexdigest()[:length] for length in (3,6) for i in range(30)]
    for query in queries:
        call('search',query=query,limit=200)
        t0=time.perf_counter();result=call('search',query=query,limit=200);ms=(time.perf_counter()-t0)*1000
        assert result['ok'] and result['used_index'],result
        assert len(result['items'])<=200 and all(query.casefold() in item['name'].casefold() or query.casefold() in item['path'].casefold() for item in result['items']),result
        samples.append(dict(query=query,length=len(query),ms=ms,returned=len(result['items']),engine_ms=result.get('elapsed_ms')))
    report['search_samples']=samples
    report['search_p95_ms_by_length']={str(n): sorted(x['ms'] for x in samples if x['length']==n)[28] for n in (3,6)}
    broad=[]
    for query in ('item','dat','it'):
        t0=time.perf_counter();result=call('search',query=query,limit=200);broad.append(dict(query=query,ms=(time.perf_counter()-t0)*1000,ok=result['ok'],error=result.get('error'),returned=len(result.get('items',[]))))
    report['broad_searches']=broad
    pages=[];seen=set();report['pagination_samples']=pages
    for offset in (x for x in (0,200,400,10000) if x+200<=count):
        t0=time.perf_counter();result=call('search',query='item',limit=200,offset=offset,sort='name')
        pages.append(dict(offset=offset,returned=len(result.get('items',[])),ms=(time.perf_counter()-t0)*1000,ok=result['ok'],error=result.get('error')))
        if not result['ok']:continue
        assert len(result['items'])==200,result
        identifiers={item['id'] for item in result['items']}
        assert len(identifiers)==200 and not identifiers.intersection(seen), 'overlapping search pages'
        assert all('item' in item['name'] for item in result['items']), 'nonmatching search page'
        seen.update(identifiers)
    report['pagination_samples']=pages
    def ticks():
        fields=pathlib.Path(f'/proc/{daemon.pid}/stat').read_text().rsplit(')',1)[1].split();return int(fields[11])+int(fields[12])
    cpu0=ticks();t0=time.monotonic();time.sleep(10);elapsed=time.monotonic()-t0
    report.update(idle_window_seconds=elapsed,idle_cpu_seconds=(ticks()-cpu0)/os.sysconf('SC_CLK_TCK'),idle_memory=memory())
    report['database_bytes']=database.stat().st_size
    gui_samples=[]
    env=dict(os.environ,XDG_RUNTIME_DIR=str(runtime),XDG_CONFIG_HOME=str(base/'gui-config'),TREEHOUND_GUI_BENCHMARK='1',GTK_A11Y='none',GSK_RENDERER='cairo',GTK_USE_PORTAL='0',GSETTINGS_BACKEND='memory')
    for _ in range(10):
        t0=time.perf_counter();gui=subprocess.run(['xvfb-run','-a','dbus-run-session','--',str(build/'treehound')],env=env,capture_output=True,text=True,timeout=15)
        assert gui.returncode==0,(gui.returncode,gui.stderr)
        measurement=next(json.loads(line) for line in gui.stdout.splitlines() if line.startswith('{'))
        measurement['process_with_xvfb_ms']=(time.perf_counter()-t0)*1000;gui_samples.append(measurement)
    report['gui_samples']=gui_samples
    report['gui_indexed_listing_p95_ms']=sorted(x['indexed_listing_ms'] for x in gui_samples)[9]
    report['measured_at_utc']=time.strftime('%Y-%m-%dT%H:%M:%SZ',time.gmtime())
    (base/(index_name+'-results.json')).write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps({k:v for k,v in report.items() if k not in ('search_samples','roots')},indent=2),flush=True)
except Exception as error:
    report.update(incomplete=True,failure=str(error),measured_at_utc=time.strftime('%Y-%m-%dT%H:%M:%SZ',time.gmtime()))
    (base/(index_name+'-failed-results.json')).write_text(json.dumps(report,indent=2)+'\n')
    raise
finally:
    daemon.terminate();daemon.wait(timeout=15);log.close()
