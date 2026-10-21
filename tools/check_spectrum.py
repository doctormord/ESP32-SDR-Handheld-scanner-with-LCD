#!/usr/bin/env python3
"""Check every advertised spectrum profile on one board (requires pyserial).

Does not flash firmware. Uses the current serial transport at the given baud,
runs bounded captures, verifies frame CRCs and end reports, and prints JSON.
Example: python tools/check_spectrum.py --port /dev/ttyACM2 --milliseconds 1000
"""
import argparse
import json
import time
import zlib


def exact(port, count):
    data=bytearray()
    while len(data)<count:
        chunk=port.read(count-len(data))
        if not chunk:
            raise TimeoutError(f'Short response: {len(data)}/{count} bytes')
        data.extend(chunk)
    return bytes(data)


def command(port, text):
    port.write((text+'\n').encode('ascii'))
    return port.readline().decode('ascii').strip()


def check(port, milliseconds, stats=False):
    deadline=time.monotonic()+10
    port.timeout=.2
    while time.monotonic()<deadline:
        port.write(b'\nRELEASE\n')
        if port.readline().strip()==b'OK':
            break
    else:
        raise TimeoutError('Could not synchronize the serial lease')
    port.timeout=5
    port.reset_input_buffer()
    result={'identity':command(port,'INFO'),'transport':command(port,'TRANSPORT?'),'runs':[]}
    caps=command(port,'CAPS').split()
    if 'SPECCAPS' not in caps:
        raise RuntimeError('Firmware does not advertise spectrum capabilities')
    if stats and 'SPECSTAT' not in caps:raise RuntimeError('Firmware does not advertise statistics')
    line=command(port,'SPECINFO?')
    if not line.startswith('SPECINFO '):
        raise RuntimeError(line)
    result['capabilities']=json.loads(line[9:])
    for profile in result['capabilities']['profiles']:
        fs,rate,n,stride,units=profile[:5]
        for detector in (0,1):
            header=command(port,f'SPEC {milliseconds} {stride} {units} {detector} {rate} {n}'+(' 1' if stats else '')).split()
            if len(header)!=5 or header[0]!='SPEC' or int(header[1])!=n or int(header[2])!=fs:
                raise RuntimeError(f'Unexpected start: {header}')
            run={'profile':profile,'detector':detector,'frames':0,'ffts':0,'stats':[]}
            previous=-1
            while True:
                magic=exact(port,4)
                if magic==b'SPEC':
                    report=(magic+port.readline()).decode('ascii').strip().split()
                    if len(report)!=13 or report[0]!='SPECEND' or int(report[1])!=0:
                        raise RuntimeError(f'Capture failed for {profile}, detector {detector}: {report}')
                    run['report']=[int(v) for v in report[1:]]
                    break
                if magic==b'SPS1':
                    frame=magic+exact(port,36)
                    if zlib.crc32(frame[:-4])!=int.from_bytes(frame[-4:],'little'):raise RuntimeError('Statistics CRC mismatch')
                    def u16(at):return int.from_bytes(frame[at:at+2],'little')
                    def u32(at):return int.from_bytes(frame[at:at+4],'little')
                    if any(u16(at)>1000 for at in [4,6,8,30]):raise RuntimeError('Invalid statistics percentage')
                    if u32(16)>u32(12):raise RuntimeError('Invalid heap statistics')
                    run['stats'].append({'core0':u16(4),'core1':u16(6),'coverage':u16(8),'mode':u16(10),'ffts_per_s':u32(32)})
                    continue
                if magic!=b'SPC1':
                    raise RuntimeError(f'Lost framing: {magic!r}')
                frame=magic+exact(port,n+28)
                if zlib.crc32(frame[:-4])!=int.from_bytes(frame[-4:],'little'):
                    raise RuntimeError('Frame CRC mismatch')
                if 1<<frame[26]!=n or frame[27]!=2:
                    raise RuntimeError('Unexpected FFT shape or power encoding')
                index=int.from_bytes(frame[8:16],'little')
                ffts=int.from_bytes(frame[20:22],'little')
                if index<=previous or not ffts:
                    raise RuntimeError('Empty FFT frame or non-monotonic sample index')
                previous=index
                run['frames']+=1
                run['ffts']+=ffts
            if not run['frames']:
                raise RuntimeError(f'No spectra for {profile}')
            if stats and milliseconds>=500 and not run['stats']:raise RuntimeError(f'No statistics for {profile}')
            if stats and milliseconds>=500 and not any(s['ffts_per_s'] for s in run['stats']):
                raise RuntimeError(f'No sustained FFT processing for {profile}')
            result['runs'].append(run)
    result['after']=command(port,'INFO')
    if result['after']!=result['identity']:
        raise RuntimeError('Command access failed after capture')
    command(port,'RELEASE')
    return result


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--port',required=True)
    parser.add_argument('--baud',type=int,default=2000000)
    parser.add_argument('--milliseconds',type=int,default=300)
    parser.add_argument('--stats',action='store_true',help='Request and validate SPS1 telemetry')
    args=parser.parse_args()
    if not 100<=args.milliseconds<=60000:
        parser.error('--milliseconds must be between 100 and 60000')
    import serial
    port=serial.Serial(port=None,baudrate=args.baud,timeout=5)
    port.dtr=port.rts=False
    port.port=args.port
    port.open()
    try:
        print(json.dumps(check(port,args.milliseconds,args.stats),indent=2))
    finally:
        port.close()


if __name__=='__main__':
    main()
