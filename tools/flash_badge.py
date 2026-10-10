"""Preserving update for this badge derivative; never flash the merged image.

Run with the ESP-IDF Python environment. Backup files contain personal data:
keep --backup-dir private and outside deliverables/source control.
"""
from pathlib import Path
import argparse
import hashlib
import json
import subprocess
import sys
import struct
from verify_firmware import parse_partition_table, parse_flash_args, verify_firmware_layout
from verify_voice_bundle import verify_voice_bundle

# Erase/write granularity matters even when the supplied image is shorter than
# one sector. Reject any plan touching persistent user data before opening USB.
USER_REGIONS=((0x9000,0x10000),(0x690000,0x750000),(0x7d0000,0x800000))

def validate_write_ranges(ranges):
    for offset,size in ranges:
        if offset<0 or size<=0 or offset+size>0x800000:
            raise ValueError('Invalid flash write range')
        start=offset//0x1000*0x1000
        end=(offset+size+0xfff)//0x1000*0x1000
        if any(start<protected_end and end>protected_start for protected_start,protected_end in USER_REGIONS):
            raise ValueError('Write/erase range overlaps user data; update aborted')

def validate_segmented_build(build, images):
    """Validate in RAM; the preserving package intentionally has no full image."""
    args=(build/'flash_args').read_text(encoding='utf-8')
    offsets=parse_flash_args(args)
    expected={name:int(address,16) for address,name in images.items()}
    if offsets!=expected or '--flash_size 8MB' not in args:
        raise ValueError('Flash arguments and segmented images disagree')
    blobs={name:(build/name).read_bytes() for name in expected}
    validate_write_ranges([(expected[name],len(data)) for name,data in blobs.items()])
    merged=bytearray(b'\xff'*max(expected[name]+len(data) for name,data in blobs.items()))
    for name,data in blobs.items():
        start=expected[name];merged[start:start+len(data)]=data
    verify_firmware_layout(merged,build,0x8000,0x10000)
    partitions,md5=parse_partition_table(merged[0x8000:0x8c00])
    if not md5:raise ValueError('Target partition MD5 missing')
    validate_existing(merged)
    verify_voice_bundle(build,offsets,partitions,merged)

def old_badge_application(data):
    """Recognize the shipped 1.7.1 image and verify its exact byte boundary."""
    app=data[0x10000:0x210000]
    if len(app)<24 or app[0]!=0xe9 or not 1<=app[1]<=16 or app[23]!=1:
        raise ValueError('Unrecognized installed application')
    position=24;checksum=0xef
    for _ in range(app[1]):
        if position+8>len(app):raise ValueError('Truncated installed image')
        _,length=struct.unpack_from('<II',app,position);position+=8
        if length>len(app)-position:raise ValueError('Installed image exceeds the new application boundary')
        for byte in app[position:position+length]:checksum^=byte
        position+=length
    end=(position+16)//16*16
    if end+32>len(app) or app[end-1]!=checksum or hashlib.sha256(app[:end]).digest()!=app[end:end+32]:
        raise ValueError('Installed application checksum/hash mismatch')
    app=app[:end+32]
    if b'空分员工牌' not in app:
        raise ValueError('Residual-tail migration only recognizes the shipped badge 1.7.1')
    return app

def validate_existing(data,reuse_unused_tail=False):
    partitions,_=parse_partition_table(data[0x8000:0x8c00],0x9000)
    labels={p.label:p for p in partitions}
    for name,offset,size in [('nvs',0x9000,0x6000),('phy_init',0xf000,0x1000),('badge_slots',0x690000,0xc0000),('badge_user',0x7d0000,0x30000)]:
        if name not in labels or (labels[name].offset,labels[name].size)!=(offset,size):
            raise ValueError('Unrecognized existing user-data layout; update aborted')
    if 'voice_data' not in labels:
        if any(x!=255 for x in data[0x210000:0x690000]+data[0x750000:0x7d0000]):
            if not reuse_unused_tail:
                raise ValueError('Residual bytes found: inspect backup before using --reuse-unused-tail for badge 1.7.1')
            if set(labels)!={'nvs','phy_init','factory','badge_slots','badge_user'} or (labels['factory'].offset,labels['factory'].size)!=(0x10000,0x680000):
                raise ValueError('Residual region is not a known unused badge application tail')
            old_badge_application(data)
            print('Verified 1.7.1 application hash and boundary; residual unused tail may be reused')
    else:
        # Both shipped layouts keep user data fixed. 1.9.1 reclaims 768 KiB
        # from the old voice region for the application, which is rewritten.
        if set(labels)!={'nvs','phy_init','factory','voice_data','badge_slots','voice_tail','badge_user'}:
            raise ValueError('Unrecognized existing voice layout; update aborted')
        layout=tuple((labels[n].offset,labels[n].size) for n in ['factory','voice_data','voice_tail'])
        if layout not in [((0x10000,0x200000),(0x210000,0x480000),(0x750000,0x80000)),
                          ((0x10000,0x2c0000),(0x2d0000,0x3c0000),(0x750000,0x80000)),
                          ((0x10000,0x2e0000),(0x2f0000,0x3a0000),(0x750000,0x80000)),
                          ((0x10000,0x3c0000),(0x3d0000,0x2c0000),(0x750000,0x80000))]:
            raise ValueError('Unrecognized existing voice layout; update aborted')

def read_region(base,offset,size,destination):
    """Bound each USB transfer; esptool checks every chunk's device MD5.

    The Windows CDC bridge can drop a frame during a single multi-MB read.
    No write operation starts unless the complete backup has been assembled.
    """
    chunks=[]
    for position in range(0,size,0x100000):
        length=min(0x100000,size-position)
        part=destination.with_name(destination.name+f'.part{position//0x100000:02d}')
        for attempt in range(3):
            result=subprocess.run(base+['read_flash','--no-progress',hex(offset+position),hex(length),str(part)])
            if result.returncode==0 and part.stat().st_size==length:break
            if attempt==2:raise RuntimeError('USB read failed after three attempts; no unchecked data accepted')
            print('Retrying current read-only block',flush=True)
        chunks.append(part.read_bytes())
        print(f'Checked backup block {position//0x100000+1}/{(size+0xfffff)//0x100000}',flush=True)
    destination.write_bytes(b''.join(chunks))

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--port',required=True)
    parser.add_argument('--backup-dir',required=True,type=Path)
    parser.add_argument('--build',default=Path(__file__).resolve().parent.parent/'build',type=Path)
    parser.add_argument('--reuse-unused-tail',action='store_true',help='Allow inspected residual bytes outside the verified 1.7.1 application; still requires full backup')
    args=parser.parse_args()
    build=args.build.resolve();dest=args.backup_dir.resolve()
    expected={'0x0':'bootloader/bootloader.bin','0x8000':'partition_table/partition-table.bin',
              '0x10000':'FoloToy-AI-Passport.bin','0x3d0000':'voice_data.bin','0x750000':'voice_tail.bin'}
    flash=json.loads((build/'flasher_args.json').read_text())
    if flash['flash_files']!=expected:raise ValueError('Unexpected flash images; update aborted')
    validate_segmented_build(build,expected)
    dest.mkdir(parents=True,exist_ok=True)
    before=dest/'flash-before.bin'
    if before.exists():raise ValueError('Backup already exists; choose a new private directory')
    # Keep the application stopped throughout backup, migration and preservation checks.
    base=[sys.executable,'-m','esptool','--chip','esp32c3','--port',args.port,'--baud','460800','--after','no_reset']
    read_region(base,0,0x800000,before)
    data=before.read_bytes()
    if len(data)!=0x800000:raise ValueError('Incomplete backup')
    (dest/'backup-sha256.txt').write_text(hashlib.sha256(data).hexdigest()+'  flash-before.bin\n')
    validate_existing(data,args.reuse_unused_tail)
    images=[]
    for offset,name in sorted(expected.items(),key=lambda item:int(item[0],16)):
        if offset=='0x8000':continue
        images += [offset,str(build/name)]
    subprocess.run(base+['write_flash','--flash_mode','dio','--flash_freq','80m','--flash_size','8MB',*images],check=True)
    subprocess.run(base+['verify_flash',*images],check=True)
    # Commit the new layout only after all relocated resources have been verified.
    table=['0x8000',str(build/'partition_table/partition-table.bin')]
    subprocess.run(base+['write_flash',*table],check=True)
    protected=[]
    for name,offset,size in [('nvs-phy',0x9000,0x7000),('badge-slots',0x690000,0xc0000),('badge-user',0x7d0000,0x30000)]:
        path=dest/(name+'-before.bin');path.write_bytes(data[offset:offset+size])
        protected += [hex(offset),str(path)]
    subprocess.run(base+['verify_flash',*table,*protected],check=True)
    # A final verified read resets into the application; failures above stay in the loader.
    subprocess.run(base[:-2]+['--after','hard_reset','verify_flash',*table],check=True)
    print('Segmented installation, NVS/PHY and all profile banks preserved byte-for-byte: PASS')

if __name__=='__main__':
    main()
