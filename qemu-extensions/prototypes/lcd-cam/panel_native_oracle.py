"""Independent exact panel frames and ordinary SDK event oracle, no QEMU imports."""
import importlib.util
from pathlib import Path
import re
import sys
REFERENCE=Path(__file__).resolve().parent/'reference'
sys.path.insert(0,str(REFERENCE))
try:
    spec=importlib.util.spec_from_file_location('_ordinary_panel_reference',REFERENCE/'capture_reference.py');ref=importlib.util.module_from_spec(spec);spec.loader.exec_module(ref)
finally:sys.path.remove(str(REFERENCE))
def need(c,m):
    if not c:raise RuntimeError(m)
def verify(profile,text,frames,snapshots,evidence):
    records,uart_hash=ref.uart_records(evidence/'uart.log')
    need(not re.search(r'Guru Meditation|assert failed|abort\(\)|Backtrace:',text),'SDK panic in actual UART')
    need(any(t=='END' for t,r in records),'Ordinary firmware did not finish')
    physical=[s['_host_observation']['gdma'] for s in snapshots if 'gdma' in s['_host_observation']]
    need(any(c['direction']=='OUT' and c['peripheral']==5 and not c['park'] for s in physical for c in s['channels']),'Missing actual active LCD peri5 GDMA ownership observation')
    need(snapshots[-1]['lcd_running'] is False,'LCD remains running after official deletion')
    need(all(b['lcd_words']>=a['lcd_words'] and b['lcd_frames']>=a['lcd_frames'] for a,b in zip(snapshots,snapshots[1:])),'Controller progress decreased')
    proof=[];matched=set();periods=set()
    bus,bpp=(8,16) if profile=='i80' else next(((a,b) for name,a,b in (('serial8-rgb332',8,8),('serial8-rgb565',8,16),('serial8-rgb888',8,24),('parallel16-rgb888',16,24)) if profile.endswith(name)),(16,16))
    lane=profile if profile=='i80' else '-'.join(profile.split('-')[:2])
    wanted=range(3) if lane in ('i80','rgb-demand') else range(2) if lane=='rgb-double' else range(1)
    for item in frames:
        meta=item['metadata']
        need(meta['valid'] and meta['visible'] and meta['errors']==0,'Actual malformed/invisible physical frame cannot be filtered')
        candidates=[]
        for f in wanted:
            pattern={'source':'lcd-fixture' if bpp==16 else 'lcd-rgb332-fixture' if bpp==8 else 'lcd-rgb888-fixture','frame':f}
            expected=ref.expected_rgb(pattern,64,48)
            if item['rgb888']==expected:candidates.append((f,pattern))
        need(len(candidates)==1,'Actual complete panel frame differs byte-by-byte from every submitted pattern')
        f,pattern=candidates[0];matched.add(f)
        contract=dict(lane=lane,width=64,height=48,component_id='D',sequence=meta['sequence'],pattern=pattern,metadata=dict(pixels=3072))
        if lane!='i80':
            period=meta['period_min_ns'];need(period==meta['period_max_ns'] and period in (1000,2000),'Unexpected physical RGB PCLK cadence');periods.add(period)
            contract.update(bus_width=bus,bits_per_pixel=bpp,memory_format={8:'RGB332',16:'RGB565-LE',24:'RGB888'}[bpp],pclk_period_ns=period,timing=dict(hsync_pulse_width=2,h_back_porch=4,h_front_porch=4,vsync_pulse_width=2,v_back_porch=2,v_front_porch=2))
        value=ref.panel_qom(evidence/f"panel-{meta['sequence']:06d}-bundle.json",contract)
        proof.append(dict(frame=f,**value))
    need(set(wanted)<=matched,f'Missing exact physical frames {set(wanted)-matched}')
    def rows(tag):return [r for t,r in records if t==tag]
    if lane=='i80':
        need(len(frames)==3 and len(rows('I80_SUBMIT'))==len(rows('I80_DONE'))==3,'I80 must capture all three separately queued frames')
        buffers=rows('I80_BUFFER')
        need(len(buffers)==3 and {int(r['frame']) for r in buffers}=={0,1,2},'Missing three actual separately allocated I80 buffers')
        addresses=[]
        for row in buffers:
            address=int(row['alias'],16);addresses.append(address)
            need(int(row['bytes'])==6144 and int(row['allocated'])>=6144 and row['internal']==row['dma']==row['distinct']=='1' and row['external']=='0','I80 actual buffer is not distinct owned internal6144 DMA storage')
            need(0x3fc80000<=address<address+6144<=0x3fd00000,'I80 buffer outside actual internal RAM')
            observed=[item for state in snapshots for item in state['_host_observation'].get('i80_buffers',[]) if item['frame']==int(row['frame'])]
            need(observed and all(item['address']==address and item['raw_reads'] for item in observed),'Missing stopped read-only I80 source lease')
            expected=ref.expected565({'source':'lcd-fixture','frame':int(row['frame'])},64,48,True)
            need(all(item['data']==expected for item in observed),'Actual I80 source bytes differ byte-by-byte')
            descriptors=[d for state in physical for channel in state['channels'] if channel['direction']=='OUT' and channel['peripheral']==5 for d in channel['descriptors']]
            need(any(address<=d['buffer']<address+6144 and d['owner'] for d in descriptors),'Missing actual GDMA-owned descriptor for submitted I80 buffer')
        need(all(a+6144<=b or b+6144<=a for index,a in enumerate(addresses) for b in addresses[index+1:]),'I80 source spans alias')
        for f in wanted:
            ownership=ref.panel_uart(evidence/'uart.log',dict(lane=lane,width=64,height=48,pattern={'source':'lcd-fixture','frame':f}))
            meta=next(p['metadata'] for p in proof if p['frame']==f)
            need(ownership['eof_callback_us']*1000>=meta['end_ns'],'I80 callback precedes actual completed physical window')
        need(any(s['last_lcd_done_ns']>0 for s in snapshots),'I80 lacks actual controller completion')
    else:
        need(rows('RGB_VSYNC'),'No actual RGB VSYNC callbacks')
        counts=[int(r['count']) for r in rows('RGB_VSYNC')];need(all(b>=a for a,b in zip(counts,counts[1:])), 'VSYNC callback count decreased')
        for f in wanted:
            pattern={'source':'lcd-fixture' if bpp==16 else 'lcd-rgb332-fixture' if bpp==8 else 'lcd-rgb888-fixture','frame':f}
            memory=ref.rgb_memory(dict(width=64,height=48,bus_width=bus,bits_per_pixel=bpp,memory_format={8:'RGB332',16:'RGB565-LE',24:'RGB888'}[bpp],pattern=pattern,pclk_period_ns=1000,timing=dict(hsync_pulse_width=2,h_back_porch=4,h_front_porch=4,vsync_pulse_width=2,v_back_porch=2,v_front_porch=2)))
            payloads=[r for r in rows('RGB_PAYLOAD') if r.get('frame')==str(f)] if lane!='rgb-bounce' else rows('RGB_BOUNCE_PAYLOAD')
            need(payloads and all(r['hash']==ref.fnv1a(memory) and int(r['bytes'])==len(memory) and int(r['bus_width'])==bus and int(r['bits_per_pixel'])==bpp for r in payloads),'Actual source payload/bounce hash/count/format differs')
        if lane=='rgb-double':
            owned=rows('RGB_OWNERSHIP');retained=rows('RGB_RETENTION');need(len(owned)==1 and int(owned[0]['reusable_events'])>0 and owned[0]['old_retained_until_delete']=='1' and owned[0]['overwrite_permitted']=='0','Unsafe or missing old-buffer retention contract')
            need({r['phase'] for r in retained}=={'prepared','before_swap','after_swap_events','before_stop'} and all(r['before_hash']==r['after_hash'] and r['bytes_equal']=='1' and r['mismatches']=='0' for r in retained),'Old framebuffer modified before official deletion')
        if lane=='rgb-bounce':
            need(any(int(r['bounce'])>0 for r in rows('RGB_VSYNC')) and any(r['distinct']=='1' and r['errors']=='0' and int(r['isr_count'])>0 for r in rows('RGB_BOUNCE_STORAGE')),'Actual ISR bounce refill/storage not observed')
        if lane=='rgb-demand':
            need(len(proof)==3 and counts[-1]==3,'Demand mode produced unsolicited/missing physical refreshes')
            need(any(r.get('op')=='restart_demand_expected_invalid_state' and r['err']=='ESP_ERR_INVALID_STATE' for r in rows('API')),'Official demand restart invalid-state contract missing')
        else:need(periods=={1000,2000},'Missing actual VSYNC-latched 1MHz->500kHz physical scanout')
        need(any(r.get('op')=='rgb_delete' and r['err']=='ESP_OK' for r in rows('API')) and len(rows('RGB_STOPPED'))==1,'Official LCD/GDMA deletion stop not observed')
    return dict(status='QUALIFIED_LIMITED',profile=profile,frames=proof,uart_sha256=uart_hash,active_peri5_observed=True,official_deletion_stopped=True,qualification='Exact enumerated ordinary physical frames, timing and callback/storage observations only',unqualified=['Physical instrument/reference/metrology gates','Deliberate starvation and post-delete recreated-panel resume not exercised by this ordinary fixture','Negative wiring/power graphs require separate actual runs'])
def verify_negative(fault,text,frames,snapshots,graph):
    need(fault in ('disconnect-clock','wrong-dc','unpowered'),'Unknown genuine panel negative')
    panel=next(c for c in graph['components'] if c['id']=='D')
    nets=graph['nets']
    role='wr' if panel['type']=='st7789-i80' else 'pclk'
    if fault=='unpowered':need(all('D.vdd' not in n['endpoints'] for n in nets),'Power negative still connected to VDD')
    elif fault=='disconnect-clock':
        need(all(not ('D.'+role in n['endpoints'] and any(e.startswith('U1.') for e in n['endpoints'])) for n in nets),'Clock negative still joined to MCU')
    else:
        need(panel['type']=='st7789-i80' and any('D.dc' in n['endpoints'] and 'U1.io1' in n['endpoints'] for n in nets),'Wrong-DC negative lacks real WR/DC miswire')
    need(not any(f['metadata']['valid'] and f['metadata']['visible'] for f in frames),'Negative unexpectedly produced valid visible complete frame')
    states=[s['_host_observation']['panels']['panels'][0] for s in snapshots]
    need(states,'Missing real panel negative status')
    strict=any(s['_host_observation']['phase']=='strict-electrical-pause' and not s['_host_observation']['running'] and 'UNKNOWN' in s['_host_observation'].get('guest_errors','') for s in snapshots)
    progressed=any(s['lcd_words']>snapshots[0]['lcd_words'] for s in snapshots)
    errors=states[-1]['error_total']>states[0]['error_total']
    if fault=='unpowered':need(any(not s['powered'] or not s['power_known'] for s in states),'Disconnected real VDD did not remove power')
    else:need(strict or progressed and (errors or fault=='disconnect-clock'),'Negative lacks actual surfaced electrical effect')
    need(strict or 'LCDCAM END ' in text or 'LCDCAM ERROR ' in text,'Host watchdog is not negative qualification')
    return dict(status='PASS',scope='genuine-panel-negative-only',fault=fault,strict_unknown=strict,controller_progress=progressed,panel_error_delta=states[-1]['error_total']-states[0]['error_total'],valid_visible_frames=0,hardware_qualified=False)
