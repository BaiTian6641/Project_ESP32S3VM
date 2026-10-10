#!/usr/bin/env python3
"""Source-bound ordinary LCD/CAM collector. Real public QMP only, no injection.

The five external-memory profiles use the existing strict memory provenance and
oracle. Ordinary profiles retain independent panel/sensor bytes, SDK compiler
inputs and precise host stop/resume boundaries, never historical image receipts.
"""
import argparse
import hashlib
import importlib.util
import json
import pathlib
import re
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
import time
import traceback
import zipfile
sys.dont_write_bytecode=True
LANE=pathlib.Path(__file__).resolve().parent
ROOT=LANE.parents[2]
def module(path,name):
    spec=importlib.util.spec_from_file_location(name,path);m=importlib.util.module_from_spec(spec);sys.modules[name]=m;spec.loader.exec_module(m);return m
BASE=module(LANE.parent/'memory/run-lcd-cam-consumers.py','ordinary_capture_base')
require,digest,save,load,append=BASE.require,BASE.digest,BASE.save,BASE.load,BASE.append
PROFILES=('i80',)+tuple(f'rgb-{m}{f}' for m in ('double','bounce','demand') for f in ('','-serial8-rgb332','-serial8-rgb565','-serial8-rgb888','-parallel16-rgb888'))+tuple('camera-'+s for s in ('rgb565','yuv422','jpeg','multi','slow','truncation','overflow','reset','disconnected-sccb','disconnected-pclk'))+tuple(BASE.PROFILES)
MILESTONES=re.compile(r'^LCDCAM (?:I80_BUFFER|I80_SUBMIT|I80_DONE|RGB_VSYNC|RGB_SWAP_REQUEST|RGB_STOPPED|CAM_BUFFER|CAM_FRAME|CAM_RECOVERY|CAM_PHYSICAL_FAULT|CAM_TWO_HELD|CAM_HOLD_BEGIN|CAM_HOLD_END|CAM_RETURN_BEGIN|CAM_REARM|CAM_FAULT|CAM_RESET|CAM_PWDN)\b.*$',re.M)
class Provenance(BASE.Provenance):
    def official_camera(self,args):
        lock=load(self.pin(args.native_fixture/'component-lock.json'))
        require(lock['release']=='v2.1.8' and lock['commit']=='b0556a78e13d42974aae19116f8a1e14c29b0f4d' and lock['source_modified'] is False,'Approved single official camera2.1.8 pin differs')
        archive=pathlib.Path(lock['archive_evidence'])
        if not archive.is_absolute():archive=ROOT/archive
        self.pin(archive,lock['archive_sha256'])
        vendor=args.native_fixture/'components/esp32-camera'
        with zipfile.ZipFile(archive) as source:
            official={name.split('/',1)[1]:source.read(name) for name in source.namelist() if '/' in name and not name.endswith('/')}
        actual={str(p.relative_to(vendor)):p for p in vendor.rglob('*') if p.is_file()}
        require(set(official)==set(actual),'Compiled official camera file inventory differs')
        for relative,data in official.items():
            require(actual[relative].read_bytes()==data,'Compiled official camera source modified: '+relative)
            self.pin(actual[relative],hashlib.sha256(data).hexdigest())
        save(self.evidence/'official-camera-identity.json',dict(commit=lock['commit'],archive_sha256=lock['archive_sha256'],files={relative:hashlib.sha256(data).hexdigest() for relative,data in official.items()}))
    def extra_defaults(self,args,source):
        preparation=load(args.preparation_receipt)
        rows=preparation.get('sdk_config_extra_defaults',[])
        paths=[source/rel for rel in rows]
        sdk=load(args.sdk_metadata)['profile']
        require(not rows or sdk=='idf-6.1' and len(rows)==1,'Extra SCCB defaults permitted only explicitly on pinned SDK6')
        for path in paths:
            self.pin(path)
            require('CONFIG_SCCB_HARDWARE_I2C_DRIVER_LEGACY=y' in path.read_text() and '# CONFIG_SCCB_HARDWARE_I2C_DRIVER_NEW is not set' in path.read_text(),'Recorded override must select official legacy SCCB explicitly')
        return paths
    def defaults(self,args,source):
        return super().defaults(args,source)+self.extra_defaults(args,source)
    def firmware(self,args):
        self.official_camera(args)
        parent_receipt=args.preparation_receipt.parent/'parent-shared-integration.json'
        require(parent_receipt.is_file(),'Prerequisite: renewed parent shared source authority required')
        parent=load(self.pin(parent_receipt));preparation=load(args.preparation_receipt)
        inventory=load(self.pin(args.preparation_receipt.parent/'source-inventory.json',preparation['fixture_source_inventory_sha256']))
        rows={r['path']:r for r in inventory}
        workspace=next(r for r in preparation['sdk_projects'] if r['sdk']==load(args.sdk_metadata)['profile'])
        source_root=pathlib.Path(workspace['source_root']).resolve(strict=True)
        require(args.native_fixture==pathlib.Path(workspace['project_path']).resolve(),'Reviewed SDK workspace differs')
        for relative,expected in parent['source_files_sha256_lf'].items():
            require(relative in rows,'Reviewed shared file absent from actual compiled inventory')
            row=rows[relative];path=self.pin(source_root/relative,row['sha256'])
            require(path.stat().st_size==row['bytes'] and path.stat().st_mode&0o222==0,'Compiled shared source is not byte-exact/read-only')
            require(hashlib.sha256(path.read_bytes().replace(b'\r\n',b'\n')).hexdigest()==expected,'Actual compiled shared source LF content differs from review authority')
        for relative,expected in parent['source_files_sha256_lf'].items():
            path=self.pin(ROOT/relative)
            require(hashlib.sha256(path.read_bytes().replace(b'\r\n',b'\n')).hexdigest()==expected,'Current reviewed shared source differs from compiled snapshot')
        if args.profile.startswith('lcd-'):
            proof=super().firmware(args)
            if self.extra_defaults(args,pathlib.Path(next(r for r in load(args.preparation_receipt)['sdk_projects'] if r['sdk']==load(args.sdk_metadata)['profile'])['source_root'])):
                require(proof['configuration'].get('CONFIG_SCCB_HARDWARE_I2C_DRIVER_LEGACY')=='y' and proof['configuration'].get('CONFIG_SCCB_HARDWARE_I2C_DRIVER_NEW')!='y','SDK6 external SCCB selection differs')
            return proof
        from ruamel.yaml import YAML
        prep=load(args.preparation_receipt);receipts=args.preparation_receipt.parent
        inventory=load(self.pin(receipts/'source-inventory.json',prep['fixture_source_inventory_sha256']))
        package=load(self.pin(receipts/'package-receipt.json',prep['component_package_receipt_sha256']))
        self.pin(receipts/'fixture-receipt.json',prep['fixture_receipt_sha256'])
        sdk_meta=load(args.sdk_metadata);sdk=pathlib.Path(sdk_meta['source']).resolve();sdk_profile=sdk_meta['profile']
        require(sdk_meta['commit']==BASE.SDK_LOCKS[sdk_profile]==self.git(sdk,'rev-parse','HEAD').decode().strip(),'Pinned SDK HEAD differs')
        require(not self.git(sdk,'status','--porcelain','--untracked-files=no'),'Pinned SDK contains tracked modifications')
        self.pin(sdk/'tools/tools.json',sdk_meta['tools_manifest_sha256'])
        for row in sdk_meta['submodule_revisions']:require(row['commit']==row['expected_commit']==self.git(sdk/row['path'],'rev-parse','HEAD').decode().strip(),'SDK submodule differs')
        workspace=next(row for row in prep['sdk_projects'] if row['sdk']==sdk_profile);source=pathlib.Path(workspace['source_root'])
        firmware_profile='camera-rgb565' if args.profile in ('camera-disconnected-sccb','camera-disconnected-pclk') else args.profile
        require(args.native_fixture==pathlib.Path(workspace['project_path']) and args.build_dir==pathlib.Path(workspace['generated_build_root'])/firmware_profile,'Fresh SDK source/build receipt differs')
        for row in inventory:
            path=self.pin(source/row['path'],row['sha256']);require(path.stat().st_size==row['bytes'] and path.stat().st_mode&0o222==0,'Source snapshot is not byte-exact/read-only')
        fs=json.loads(self.command(['findmnt','--json','--target',str(args.build_dir),'--output','TARGET,SOURCE,FSTYPE,OPTIONS']))
        require(fs['filesystems'][0]['fstype']=='ext4','Prerequisite: firmware must be compiled on native ext4')
        desc=load(self.pin(args.build_dir/'project_description.json'));flashes=load(self.pin(args.build_dir/'flasher_args.json'));commands=load(self.pin(args.build_dir/'compile_commands.json'));self.pin(args.build_dir/'CMakeCache.txt')
        require(desc['project_name']=='lcd_cam_native' and desc['target']=='esp32s3' and pathlib.Path(desc['idf_path'])==sdk and pathlib.Path(desc['project_path'])==args.native_fixture and pathlib.Path(desc['build_dir'])==args.build_dir,'Compiler project/SDK paths differ')
        defaults=[args.native_fixture/'sdkconfig.defaults',args.native_fixture/f'sdkconfig.{firmware_profile}.defaults']
        explicit=self.extra_defaults(args,source);defaults.extend(explicit)
        require([pathlib.Path(s) for s in desc['config_defaults'].split(';')]==defaults,'Ordinary profile defaults/order differs')
        cfg_path=self.pin(pathlib.Path(desc['config_file']));cfg=dict(re.findall(r'^(CONFIG_[A-Z0-9_]+)=(.*)$',cfg_path.read_text(),re.M))
        require(cfg_path.is_relative_to(args.build_dir) and cfg.get('CONFIG_IDF_TARGET')=='"esp32s3"' and cfg.get('CONFIG_LCD_CAM_NATIVE_PSRAM')!='y','Ordinary internal profile configuration differs')
        wanted=dict(re.findall(r'^(CONFIG_[A-Z0-9_]+)=(.*)$',defaults[1].read_text(),re.M));require(all(cfg.get(k)==v for k,v in wanted.items()),'Compiled selected profile differs')
        if explicit:require(cfg.get('CONFIG_SCCB_HARDWARE_I2C_DRIVER_LEGACY')=='y' and cfg.get('CONFIG_SCCB_HARDWARE_I2C_DRIVER_NEW')!='y','SDK6 ordinary SCCB selection differs')
        elf=self.pin(args.build_dir/desc['app_elf']);app=self.pin(args.build_dir/desc['app_bin']);data=app.read_bytes();require(struct.unpack_from('<I',data,32)[0]==0xabcd5432 and data[176:208].hex()==digest(elf),'Actual IDF app descriptor ELF binding differs')
        image=args.merged_flash.read_bytes();require(len(image)==4194304,'Merged flash extent differs');merged=[]
        for offset,name in flashes['flash_files'].items():
            path=self.pin(args.build_dir/name);b=path.read_bytes();start=int(offset,0);require(image[start:start+len(b)]==b,'Merged actual built input differs');merged.append(dict(path=str(path),offset=start,sha256=digest(path)))
        jpeg=pathlib.Path(package['component_path']);require(package['source_commit']==BASE.JPEG_COMMIT and package['archive_sha256']==BASE.JPEG_SHA,'JPEG public package differs')
        for row in package['package_files']:self.pin(jpeg.parent/row['path'],row['sha256'])
        lock_path=self.pin(args.native_fixture/'dependencies.lock');lock=YAML(typ='safe').load(lock_path.read_text());rows=[v for k,v in lock['dependencies'].items() if k.split('/')[-1]=='esp_jpeg']
        require(len(rows)==1 and rows[0]['version']=='1.3.1' and rows[0]['source']['type']=='local','Real Component Manager local JPEG version/source differs')
        resolved=pathlib.Path(rows[0]['source']['path'])
        if not resolved.is_absolute():resolved=args.native_fixture/resolved
        require(resolved.resolve(strict=True)==jpeg.resolve(strict=True),'Real Component Manager local JPEG resolution differs')
        camera=load(self.pin(args.native_fixture/'component-lock.json'));require(camera['commit']==BASE.CAMERA_COMMIT and camera['source_modified'] is False,'Official camera source lock differs')
        compiled={}
        for row in commands:
            path=pathlib.Path(row['file']);path=path if path.is_absolute() else pathlib.Path(row['directory'])/path;self.pin(path);require(path.stat().st_mtime_ns<=elf.stat().st_mtime_ns,'Compiler source newer than ELF');compiled[str(path)]=dict(sha256=digest(path),compile_command=row)
        require(str(args.native_fixture/'main/lcd_cam_native.c') in compiled,'Ordinary actual main absent from compile inventory')
        components=desc['build_component_info'];j=[(k,v) for k,v in components.items() if pathlib.Path(v['dir'])==jpeg];c=[v for v in components.values() if pathlib.Path(v['dir'])==args.native_fixture/'components/esp32-camera'];require(len(j)==len(c)==1 and j[0][0] in c[0]['reqs'] and j[0][0] in c[0]['managed_reqs'],'Real camera->JPEG public dependency edge missing')
        for info in components.values():
            for path in info.get('sources',[]):self.pin(path)
        save(self.evidence/'compiled-source-identity.json',compiled);save(self.evidence/'sdk-metadata.json',sdk_meta);save(self.evidence/'dependencies-lock.json',lock)
        return dict(sdk=sdk_meta,configuration=cfg,merged_inputs=merged,source_inventory_sha256=prep['fixture_source_inventory_sha256'],native_filesystem=fs)
class Collector(BASE.Collector):
    def __init__(self,*args):
        super().__init__(*args);self.electrical_snapshots=[];self.ledc_snapshots=[];self.buffer_seen=set();self.i80_seen=set()
    def memory_bytes(self,address,length):
        require(0<length<=1024*1024,'Actual guest lease exceeds bounded payload')
        require(0x3fc80000<=address<address+length<=0x3fd00000 or 0x3c000000<=address<address+length<=0x3e000000,'Actual guest lease pointer outside RAM')
        raw=bytearray();reads=[]
        for offset in range(0,length,256):
            count=min(256,length-offset);command=f'xp /{count}bx 0x{address+offset:x}';reply=self.qmp.call('human-monitor-command',{'command-line':command});values=[]
            for line in reply.splitlines():
                match=re.fullmatch(r'\s*(?:0x)?([0-9a-fA-F]+):\s*((?:0x[0-9a-fA-F]+\s*)+)\s*',line)
                require(match is not None,'Physical guest read malformed')
                require(int(match[1],16)==address+offset+len(values),'Physical guest read ordering differs')
                values.extend(int(v,16) for v in re.findall(r'0x[0-9a-fA-F]+',match[2]))
            require(len(values)==count and all(v<=255 for v in values),'Physical guest read extent differs')
            raw.extend(values);reads.append(dict(command=command,reply=reply))
        return bytes(raw),reads
    def observe(self,phase,text,physical=False):
        status=super().observe(phase,text,physical);snap=self.controller_snapshots[-1];host=snap['_host_observation']
        electrical=self.qmp.get(BASE.ELECTRICAL,'snapshot-json');self.electrical_snapshots.append(electrical);host['electrical']=electrical
        host['sensors']=status['sensors'];host['running']=not host['paused']
        host['panels']=status['panels']
        diagnostics=self.evidence/'guest-errors.log'
        if diagnostics.exists():
            with diagnostics.open('rb') as stream:raw=stream.read(8*1024*1024+1)
            require(len(raw)<=8*1024*1024,'Actual guest diagnostic prefix exceeds evidence bound')
            host['guest_errors']=raw.decode(errors='replace')
        else:host['guest_errors']=''
        if not self.camera and physical and 'LCDCAM API op=panel_delete ' not in text:
            host['i80_buffers']=[]
            for line in text.splitlines():
                if not line.startswith('LCDCAM I80_BUFFER '):continue
                row=dict(re.findall(r'(\w+)=([^\s]+)',line));frame=int(row['frame'])
                if frame in self.i80_seen or not re.search(r'LCDCAM I80_SUBMIT frame='+str(frame)+r'\b',text):continue
                address,length=int(row['alias'],16),int(row['bytes']);raw,reads=self.memory_bytes(address,length)
                binary=f'guest-i80-{frame}-{address:x}.bin';(self.evidence/binary).write_bytes(raw);save(self.evidence/(binary+'.reads.json'),reads)
                host['i80_buffers'].append(dict(frame=frame,address=address,data=raw,raw_reads=reads));self.i80_seen.add(frame)
        if self.camera and physical:
            ledc=dict(zip(('channel_conf0','channel_hpoint','channel_duty','channel_conf1'),BASE.physical_words(self.qmp,0x60019000,4)))
            ledc.update(timer_conf=BASE.physical_words(self.qmp,0x600190a0,1)[0],timer_value=BASE.physical_words(self.qmp,0x600190a4,1)[0],clock_conf=BASE.physical_words(self.qmp,0x600190d0,1)[0],gpio15_matrix_out=BASE.physical_words(self.qmp,0x60004554+15*4,1)[0])
            self.ledc_snapshots.append(ledc);host['ledc']=ledc
            host['camera_buffers']=[]
            # Physical read-only guest lease observations; never cached-MMIO writes.
            for line in text.splitlines():
                if not re.match(r'LCDCAM (?:CAM_BUFFER|CAM_EXTERNAL_BUFFER|CAM_FRAME|CAM_HOLD_BEGIN)\b',line):continue
                row=dict(re.findall(r'(\w+)=([^\s]+)',line));address=row.get('payload',row.get('buf',row.get('alias',row.get('buffer'))));length=row.get('len',row.get('bytes'));frame=row.get('frame')
                if address is None or length is None or frame is None:continue
                external_buffer=line.startswith('LCDCAM CAM_EXTERNAL_BUFFER ')
                held=re.search(r'LCDCAM CAM_HOLD_BEGIN frame='+re.escape(frame)+r'\b',text)
                emitted=re.search(r'LCDCAM CAM_FRAME frame='+re.escape(frame)+r'\b',text)
                if not (held or external_buffer and emitted):continue
                key=(frame,address,length)
                if key in self.buffer_seen:continue
                # Only read the newest frame still leased; earlier UART rows may have returned.
                later=text[text.rfind(line)+len(line):]
                if re.search(r'LCDCAM (?:CAM_RETURN|CAM_RETURN_BEGIN) frame='+re.escape(frame)+r'\b',later):continue
                addr,n=int(address,16),int(length);raw,reads=self.memory_bytes(addr,n)
                binary=f'guest-camera-{frame}-{addr:x}.bin';(self.evidence/binary).write_bytes(raw);save(self.evidence/(binary+'.reads.json'),reads);host['camera_buffers'].append(dict(frame=int(frame),address=addr,data=bytes(raw),raw_reads=reads));self.buffer_seen.add(key)
        return status
    def panel(self,status):
        before=self.panel_next;super().panel(status)
        if self.panel_next>before:
            row=self.panel_frames[-1];save(self.evidence/f'panel-{self.panel_next-1:06d}-bundle.json',dict(capture=row['capture'],windows=row['windows']))
    def sensor(self,status):
        before=self.sensor_last
        super().sensor(status)
        if self.sensor_last>before:
            self.qmp.set(BASE.SENSORS,'capture-request-json',dict(component_id='D',frame=0,offset=0,count=0))
def serializable(value):
    if isinstance(value,bytes):return dict(bytes=len(value),sha256=hashlib.sha256(value).hexdigest(),hex=value.hex())
    if isinstance(value,list):return [serializable(v) for v in value]
    if isinstance(value,dict):return {k:serializable(v) for k,v in value.items()}
    return value
def main():
    parser=argparse.ArgumentParser(description=__doc__)
    names=('qemu','qemu-source','runtime-manifest','prepared-source-record','native-fixture','build-dir','sdk-metadata','merged-flash','preparation-receipt','evidence')
    for name in names:parser.add_argument('--'+name,type=pathlib.Path,required=True)
    parser.add_argument('--memory-qualification-receipt',type=pathlib.Path);parser.add_argument('--profile',choices=PROFILES,required=True);parser.add_argument('--watchdog-seconds',type=float,default=300);parser.add_argument('--graph',type=pathlib.Path);parser.add_argument('--camera-oracle',type=pathlib.Path,default=LANE/'camera_native_oracle.py')
    parser.add_argument('--panel-negative',choices=('disconnect-clock','wrong-dc','unpowered'))
    args=parser.parse_args();require(1<=args.watchdog_seconds<=1800,'Watchdog outside diagnostic bound');args.evidence.mkdir(parents=True,exist_ok=True)
    live_root=pathlib.Path.home()/'.cache/esp32s3vm/lcd-cam-ordinary-live';live_root.mkdir(parents=True,exist_ok=True)
    evidence=pathlib.Path(tempfile.mkdtemp(prefix=args.profile+'-',dir=live_root));archive=args.evidence.resolve()/evidence.name
    result=dict(schema_version=1,status='FAIL',profile=args.profile,evidence=str(evidence),archive=str(archive),commands=[],pauses=[],panel_frames=[],sensor_captures=[],hardware_qualified=False,injected_payloads=False);provenance=Provenance(evidence);proc=qmp=collector=None;original=None;started=time.monotonic();uart=evidence/'uart.log';scratch=evidence/'writable-flash.bin'
    try:
        for name in names[:-1]:setattr(args,name.replace('-','_'),getattr(args,name.replace('-','_')).resolve(strict=True))
        for name in ('qemu','runtime_manifest','prepared_source_record','sdk_metadata','merged_flash','preparation_receipt'):provenance.pin(getattr(args,name))
        provenance.pin(pathlib.Path(__file__));provenance.pin(LANE.parent/'memory/run-lcd-cam-consumers.py');original=digest(args.merged_flash);result['runtime_provenance']=provenance.runtime(args);result['firmware_provenance']=provenance.firmware(args)
        external=args.profile.startswith('lcd-');camera='camera-' in args.profile
        if external:
            require(args.memory_qualification_receipt is not None,'Prerequisite: current same-binary memory qualification receipt required');args.memory_qualification_receipt=args.memory_qualification_receipt.resolve(strict=True);result['memory_foundation']=provenance.memory_foundation(args)
        if args.graph:graph=load(provenance.pin(args.graph))
        else:graph=module(provenance.pin(args.native_fixture/'graph_vectors.py'),'ordinary_graph').project(args.profile.removeprefix('lcd-'))
        save(evidence/'project.json',graph);shutil.copyfile(args.merged_flash,scratch);scratch.chmod(0o600);require(digest(scratch)==original,'Fresh writable image differs')
        with socket.socket() as reserve:reserve.bind(('127.0.0.1',0));port=reserve.getsockname()[1]
        command=[str(args.qemu),'-machine','esp32s3','-nographic','-S','-monitor','none','-accel','tcg,thread=single','-icount','shift=0,align=off,sleep=off','-m','8M','-serial',f'file:{uart}','-qmp',f'tcp:127.0.0.1:{port},server=on,wait=off','-drive',f'file={scratch},if=mtd,format=raw,cache=writethrough','-d','guest_errors','-D',str(evidence/'guest-errors.log')];result['commands'].append(command);save(evidence/'commands.json',result['commands']);deadline=time.monotonic()+args.watchdog_seconds
        result['native_clock']=dict(mode='icount',shift=0,align=False,sleep=False,tcg_thread='single',qualification='Established UART/RMT deterministic functional virtual time; not CPU cycle accuracy or physical metrology')
        with (evidence/'stdout.log').open('wb') as out,(evidence/'stderr.log').open('wb') as err:
            proc=subprocess.Popen(command,stdout=out,stderr=err)
            while True:
                require(proc.poll() is None,'QEMU exited before QMP');require(time.monotonic()<deadline,'Diagnostic watchdog connecting QMP')
                try:sock=socket.create_connection(('127.0.0.1',port),timeout=1);break
                except ConnectionRefusedError:time.sleep(.01)
            qmp=BASE.Qmp(sock,evidence/'qmp.jsonl',deadline);require(not qmp.call('query-status')['running'],'CPU escaped initial -S');BASE.discover(qmp,evidence);qmp.set(BASE.ELECTRICAL,'project-json',graph);require(qmp.get(BASE.ELECTRICAL,'project-json')==graph,'Actual public Apply differs');collector=Collector(qmp,evidence,result,camera);collector.observe('applied-before-cpu','',True);qmp.call('cont');marks=0
            while True:
                require(proc.poll() is None,'QEMU exited before firmware completion');require(time.monotonic()<deadline,'Diagnostic host watchdog; ordinary guest remains unqualified');text=uart.read_text(errors='replace') if uart.exists() else '';status=collector.status();append(evidence/'running-status.jsonl',dict(host_monotonic_ns=time.monotonic_ns(),**status));running=qmp.call('query-status')['running'];current=len(MILESTONES.findall(text));end=BASE.END in text
                changed=any(s['capture_frame']>collector.sensor_last and s['capture_byte_count'] for s in status['sensors']) if camera else any(p['captures']>collector.panel_next for p in status['panels']['panels']);counter='cam_bytes' if camera else 'lcd_words';active=not collector.active_seen and status['controller'][counter]>collector.controller_snapshots[-1][counter]
                if changed or current!=marks or end or active or not running or re.search(r'^LCDCAM (?:ERROR|UNQUALIFIED)\b',text,re.M):
                    pause,text=collector.freeze('firmware-end' if end else 'strict-electrical-pause' if not running else 'actual-frame-or-milestone',uart);marks=len(MILESTONES.findall(text))
                    negative=args.profile in ('camera-disconnected-sccb','camera-disconnected-pclk') or args.panel_negative is not None
                    if end or negative and (not running or re.search(r'^LCDCAM (?:ERROR|UNQUALIFIED)\b',text,re.M)):
                        if external:
                            oracle=module(provenance.pin(LANE.parent/'memory/lcd_cam_consumer_oracle.py'),'external_oracle');proof=oracle.verify(args.profile,text,collector.panel_frames,collector.sensor_captures,collector.controller_snapshots)
                            if camera:
                                camera_rules=module(provenance.pin(args.camera_oracle),'camera_ordinary_oracle')
                                proof=camera_rules.verify_external(args.profile,text,collector.sensor_captures,collector.controller_snapshots,proof)
                        elif camera:oracle=module(provenance.pin(args.camera_oracle),'camera_ordinary_oracle');proof=oracle.verify(args.profile,text,collector.sensor_captures,collector.controller_snapshots,collector.electrical_snapshots,collector.ledc_snapshots)
                        else:
                            oracle=module(provenance.pin(LANE/'panel_native_oracle.py'),'panel_ordinary_oracle')
                            proof=oracle.verify_negative(args.panel_negative,text,collector.panel_frames,collector.controller_snapshots,graph) if args.panel_negative else oracle.verify(args.profile,text,collector.panel_frames,collector.controller_snapshots,evidence)
                        save(evidence/'oracle-proof.json',proof);result['oracle']=proof;result['status']=proof['status'];break
                    require(running,'Actual strict electrical pause; no successful ordinary guest claim');require(not re.search(r'^LCDCAM (?:ERROR|UNQUALIFIED)\b',text,re.M),'Actual firmware reported error/unqualified');collector.resume(pause)
                time.sleep(.001)
            qmp.call('quit');proc.wait(timeout=10);require(proc.returncode==0,'Orderly QEMU termination failed')
    except Exception as exc:
        result['status']='PREREQUISITE' if 'Prerequisite:' in str(exc) or isinstance(exc,(FileNotFoundError,ModuleNotFoundError)) else 'FAIL';result['error']=f'{type(exc).__name__}: {exc}';(evidence/'failure-traceback.log').write_text(traceback.format_exc())
    finally:
        if collector:save(evidence/'controller-snapshots.json',serializable(collector.controller_snapshots));save(evidence/'electrical-snapshots.json',collector.electrical_snapshots);save(evidence/'ledc-snapshots.json',collector.ledc_snapshots)
        if proc and proc.poll() is None:
            if qmp:
                qmp.deadline=time.monotonic()+5
                try:qmp.call('quit');proc.wait(timeout=5)
                except Exception as exc:result['cleanup_error']=str(exc)
            if proc.poll() is None:proc.kill();proc.wait()
        if qmp:qmp.close()
        result['input_hashes']=provenance.hashes;result['changed_inputs']=[p for p,h in provenance.hashes.items() if not pathlib.Path(p).is_file() or digest(pathlib.Path(p))!=h]
        if result['changed_inputs']:result['status']='FAIL';result['error']='Pinned input changed during collection'
        if original:result['original_flash_unchanged']=digest(args.merged_flash)==original
        result['host_elapsed_seconds']=time.monotonic()-started;result['qemu_returncode_after_cleanup']=proc.returncode if proc else None;result['evidence_sha256']={str(p.relative_to(evidence)):digest(p) for p in evidence.rglob('*') if p.is_file()};save(evidence/'result.json',result)
        shutil.copytree(evidence,archive)
    print(json.dumps(dict(status=result['status'],profile=args.profile,evidence=str(evidence),archive=str(archive),error=result.get('error'))));return 0 if result['status']=='PASS' else 2 if result['status'] in ('PREREQUISITE','QUALIFIED_LIMITED') else 1
if __name__=='__main__':raise SystemExit(main())
