import argparse,hashlib,importlib.util,json,pathlib,re,shutil,subprocess,tempfile,time,traceback
root=pathlib.Path('/mnt/c/Users/weyst/Documents/ChatGPT/ESP32S3VM/Project_ESP32S3VM');stage=root/'build-runtime-state/uart-continuation/lcd-ordinary-stage'
p=argparse.ArgumentParser();p.add_argument('--sdk',required=True,choices=('idf-5.5.5','idf-6.1'));args=p.parse_args()
old=pathlib.Path('/home/polar/.cache/esp32s3vm/lcd-cam-ordinary-live')/('i80-6ph0jm3j' if args.sdk=='idf-5.5.5' else 'i80-qusy2d0t')
manifest=json.loads((old/'runtime-manifest.json').read_text());qemu=pathlib.Path(manifest['qemu']['path']);flash=pathlib.Path(manifest['firmware'][0]['path'])
def digest(path):return hashlib.sha256(path.read_bytes()).hexdigest()
assert digest(qemu)==manifest['qemu']['sha256']=='d83145b84fada9da9d4e86b67bce8e40f4f5f7f6a057589aff6687b4996355c1';assert digest(flash)==manifest['firmware'][0]['sha256']
evidence=pathlib.Path.home()/'.cache/esp32s3vm/i80-counter-origin';evidence.mkdir(exist_ok=True);evidence=pathlib.Path(tempfile.mkdtemp(prefix=args.sdk+'-',dir=evidence))
graph=json.loads((old/'project.json').read_text());(evidence/'project.json').write_text(json.dumps(graph,indent=2)+'\n');scratch=evidence/'writable-flash.bin';shutil.copyfile(flash,scratch);scratch.chmod(0o600)
spec=importlib.util.spec_from_file_location('spi_qmp',root/'qemu-extensions/prototypes/spi/run-native-fixture.py');r=importlib.util.module_from_spec(spec);spec.loader.exec_module(r)
result={'status':'FAIL_DIAGNOSTIC','sdk':args.sdk,'qemu':str(qemu),'qemu_sha256':digest(qemu),'original_flash':str(flash),'original_flash_sha256':digest(flash),'gdb_script_sha256':digest(stage/'i80-counter-origin.gdb'),'boundary':'Read-only native GDB counter origin/load evidence, not a new panel/camera/peripheral qualification. No memory/input/IRQ writes or security changes.'};proc=None;q=None;transcript=[]
try:
 with tempfile.TemporaryDirectory(prefix='i80-origin-') as temp,(evidence/'gdb-console.log').open('wb') as out,(evidence/'stderr.log').open('wb') as err:
  sock=pathlib.Path(temp)/'qmp.sock';uart=evidence/'uart.log';command=[str(qemu),'-machine','esp32s3','-m','8M','-S','-nographic','-monitor','none','-serial','file:'+str(uart),'-drive','file='+str(scratch)+',if=mtd,format=raw,cache=writethrough','-qmp',f'unix:{sock},server=on,wait=off','-accel','tcg,thread=single','-icount','shift=0,align=off,sleep=off']
  debug=['gdb','-q','-batch','-x',str(stage/'i80-counter-origin.gdb'),'--args',*command];result['debug_command']=debug;result['qemu_command']=command
  proc=subprocess.Popen(debug,cwd=qemu.parent.parent,stdout=out,stderr=err);deadline=time.monotonic()+900
  while not sock.exists():
   assert proc.poll() is None,'Native GDB launch exited; retain console';assert time.monotonic()<deadline,'Diagnostic startup watchdog';time.sleep(.05)
  q=r.Qmp(sock,transcript,deadline);q.call('qom-set',{'path':'/machine/soc/electrical','property':'project-json','value':json.dumps(graph)});q.call('cont')
  while True:
   text=uart.read_text(errors='replace') if uart.exists() else ''
   if re.search(r'^LCDCAM END [^\r\n]*\r?\n',text,re.M):break
   assert proc.poll() is None,'GDB/QEMU exited before complete I80 outcome';assert q.call('query-status')['running'],'Actual diagnostic paused unexpectedly';assert time.monotonic()<deadline,'Diagnostic host watchdog, no callback success inferred';time.sleep(.05)
  result['whole_i80_end_observed']=True
  try:q.call('quit')
  except (ConnectionResetError,EOFError):result['quit_peer_closed']=True
  proc.wait(timeout=15);result['debugger_returncode']=proc.returncode;assert proc.returncode==0,'Orderly diagnostic debugger exit failed'
  trace=(evidence/'gdb-console.log').read_text();origin=re.findall(r'^COUNTER0_ORIGIN value=(\d+) base=(-?\d+) enabled=(\d+)$',trace,re.M);assert origin==[('0','0','1')],origin
  loads=re.findall(r'^COUNTER_LOAD unit=(\d+) value=(\d+) prior=(\d+) base=(-?\d+) enabled=(\d+)$',trace,re.M);assert all(int(unit)!=0 or int(value)==0 for unit,value,prior,base,enabled in loads),loads
  result.update(status='PASS_READ_ONLY_COLD_COUNTER_ORIGIN',counter0_origin=origin,actual_counter_loads=loads,callback_rows=re.findall(r'^LCDCAM I80_DONE .*$',text,re.M),original_flash_unchanged=digest(flash)==result['original_flash_sha256']);assert result['original_flash_unchanged']
except Exception as error:result['error']=str(error);(evidence/'failure-traceback.log').write_text(traceback.format_exc())
finally:
 if q is not None:
  try:q.close()
  except OSError:pass
 if proc is not None and proc.poll() is None:proc.terminate();proc.wait(timeout=15)
 (evidence/'qmp-transcript.json').write_text(json.dumps(transcript,indent=2)+'\n');result['evidence_sha256']={path.name:digest(path) for path in evidence.iterdir() if path.is_file()};(evidence/'result.json').write_text(json.dumps(result,indent=2)+'\n')
print(json.dumps({'status':result['status'],'sdk':args.sdk,'evidence':str(evidence),'error':result.get('error')}));assert result['status']=='PASS_READ_ONLY_COLD_COUNTER_ORIGIN'
