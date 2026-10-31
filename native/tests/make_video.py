"""Generate 720p Annex-B access units and AV1 RTP fixtures using FFmpeg."""
import argparse
from pathlib import Path
import re
import struct
import subprocess
import tempfile


def leb(n):
    out=bytearray()
    while n>=128:
        out.append(128 | (n&127))
        n>>=7
    return bytes(out)+bytes([n])


def unpack_obus(data):
    pos=0
    while pos<len(data):
        start=pos
        header=data[pos]
        pos+=1+bool(header&4)
        assert header&2
        header_bytes=bytes([header&~2])+data[start+1:pos]
        size=0
        for i in range(8):
            b=data[pos];pos+=1
            size|=(b&127)<<(7*i)
            if b<128:break
        assert size<=len(data)-pos
        if (header>>3)&15!=2:
            yield header_bytes+data[pos:pos+size]
        pos+=size


def av1_rtp(ivf,target):
    assert ivf[:4]==b'DKIF'
    for loss in (False,True):
        path=target/('probe-av1-loss.pack' if loss else 'probe-av1.pack')
        with path.open('wb') as out:
            pos=32;frame=0;seq=65530;pending_rtx=None
            while pos<len(ivf):
                size=struct.unpack_from('<I',ivf,pos)[0];pos+=12
                obus=list(unpack_obus(ivf[pos:pos+size]));pos+=size
                # Recover after loss with an ordinary displayed keyframe: no N
                # flag or repeated sequence header. The receiver must restore it.
                if frame==15:obus=[o for o in obus if o[0]>>3!=1]
                if frame in (4,8):
                    assert len(obus)==1
                    cut=len(obus[0])//2
                    payloads=[b'\x50'+obus[0][:cut],b'\x90'+obus[0][cut:]]
                else:
                    count=frame%4
                    while len(obus)<count:obus.append(b'\x78\x00')
                    if len(obus)!=count:count=0
                    agg=bytes([(count<<4)|(8 if frame==0 else 0)])
                    payloads=[agg+b''.join((leb(len(o)) if not count or i+1!=count else b'')+o for i,o in enumerate(obus))]
                for i,payload in enumerate(payloads):
                    seq=(seq+1)&65535
                    if loss and frame==8 and i==0:continue
                    marker=i+1==len(payloads)
                    rtx=frame==4 and i==1
                    packet=struct.pack('!BBHII',128,(128 if marker else 0)+(99 if rtx else 98),211 if rtx else seq,
                                       frame*3000,456 if rtx else 123)
                    if rtx:packet+=struct.pack('!H',seq)
                    packet+=payload
                    record=struct.pack('<I',len(packet))+packet
                    if rtx:
                        pending_rtx=record
                    else:
                        out.write(record)
                        if pending_rtx:
                            # The following primary sequence number exposes
                            # the gap before its RFC 4588 retransmit arrives.
                            out.write(pending_rtx)
                            pending_rtx=None
                frame+=1


def fallback_rtp(codec, target):
    """Feed the alternate H.26x payload types from an AV1-first SDP offer."""
    pt=96 if codec=="hevc" else 100
    with (target/f"probe-{codec}.pack").open('rb') as src,(target/f"probe-auto-{codec}.pack").open('wb') as out:
        seq=1000;frame=0
        while header:=src.read(4):
            size=struct.unpack('<I',header)[0]
            units=re.split(b'\x00\x00\x00?\x01',src.read(size))[1:]
            units=[n for n in units if n and ((n[0]>>1)&63!=35 if codec=="hevc" else n[0]&31!=9)]
            for i,nal in enumerate(units):
                hdr=struct.pack('!BBHII',128,pt|(128 if i+1==len(units) else 0),seq,frame*3000,123)
                out.write(struct.pack('<I',len(hdr)+len(nal))+hdr+nal)
                seq+=1
            frame+=1


def generate(codec, target):
    with tempfile.TemporaryDirectory() as tmp:
        if codec=="av1":
            ivf=Path(tmp)/"red.ivf"
            subprocess.run(["ffmpeg","-nostdin","-loglevel","error","-f","lavfi","-i",
                            "color=red:s=1280x720:r=30","-frames:v","42","-pix_fmt","yuv420p",
                            "-c:v","libaom-av1","-cpu-used","8","-g","15","-lag-in-frames","0",
                            "-threads","4","-f","ivf",str(ivf)],check=True)
            av1_rtp(ivf.read_bytes(),target)
            return
        bitstream = Path(tmp) / ("red." + codec)
        encode = ["ffmpeg", "-nostdin", "-loglevel", "error", "-f", "lavfi", "-i",
                  "color=c=red:s=1280x720:r=30", "-frames:v", "42", "-pix_fmt", "yuv420p",
                  "-c:v", "libx265" if codec == "hevc" else "libx264", "-bf", "0", "-g", "30",
                  "-preset", "ultrafast", "-tune", "zerolatency"]
        if codec == "hevc":
            encode += ["-x265-params", "log-level=error:pools=1"]
        subprocess.run(encode + ["-f", codec, str(bitstream)], check=True)
        sizes = subprocess.check_output(
            ["ffprobe", "-v", "error", "-f", codec, "-show_entries", "packet=size",
             "-of", "csv=p=0", str(bitstream)], text=True)
        with bitstream.open("rb") as src, (target / f"probe-{codec}.pack").open("wb") as out:
            for line in sizes.splitlines():
                size = int(line)
                packet = src.read(size)
                if len(packet) != size:
                    raise ValueError("Incomplete access unit")
                out.write(struct.pack("<I", size))
                out.write(packet)
            if src.read(1):
                raise ValueError("Unparsed bytes in bitstream")
        fallback_rtp(codec,target)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path)
    target = parser.parse_args().output
    target.mkdir(parents=True, exist_ok=True)
    for name in ("h264", "hevc","av1"):
        generate(name, target)
