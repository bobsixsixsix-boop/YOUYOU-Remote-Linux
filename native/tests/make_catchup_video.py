"""Build 24-picture reference chains for youyou_decode_catchup_media_test.

python3 native/tests/make_catchup_video.py /tmp/catchup-fixtures
cmake --build <native-build> --target youyou_decode_catchup_media_test
SDL_AUDIODRIVER=dummy UU_DECODER=software <native-build>/youyou_decode_catchup_media_test /tmp/catchup-fixtures/h264.pack h264 candidate
Repeat for hevc/av1. Append empty-tail, send-error, receive-error or damaged to
check the final good picture across a non-image AU or a test-only codec fault.
"""
import argparse
from pathlib import Path
import struct
import subprocess
import tempfile

def generate(codec, directory):
    with tempfile.TemporaryDirectory() as scratch:
        path = Path(scratch)/("chain.ivf" if codec == "av1" else "chain."+codec)
        encoder = {"h264":"libx264", "hevc":"libx265", "av1":"libaom-av1"}[codec]
        options = ["-cpu-used","8","-lag-in-frames","0"] if codec == "av1" else [
            "-preset","ultrafast","-tune","zerolatency","-bf","0"]
        if codec == "hevc":
            options += ["-x265-params","log-level=error:pools=1"]
        subprocess.run(["ffmpeg","-nostdin","-v","error","-f","lavfi","-i",
                        "testsrc2=s=160x96:r=120","-frames:v","24","-pix_fmt","yuv420p",
                        "-c:v",encoder,"-g","120","-threads","1",*options,
                        "-f","ivf" if codec == "av1" else codec,str(path)],check=True)
        data = path.read_bytes()
        frames = []
        if codec == "av1":
            position = 32
            while position < len(data):
                size = struct.unpack_from("<I",data,position)[0]
                position += 12
                frames.append(data[position:position+size])
                position += size
        else:
            sizes = subprocess.check_output(["ffprobe","-v","error","-f",codec,
                "-show_entries","packet=size","-of","csv=p=0",str(path)],text=True)
            position = 0
            for size in map(int,sizes.splitlines()):
                frames.append(data[position:position+size])
                position += size
        if len(frames) != 24 or position != len(data):
            raise ValueError("Unexpected encoded fixture boundaries")
        (directory/(codec+".pack")).write_bytes(
            b"".join(struct.pack("<I",len(frame))+frame for frame in frames))

if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output",type=Path)
    directory = parser.parse_args().output
    directory.mkdir(parents=True,exist_ok=True)
    for codec in ("h264","hevc","av1"):
        generate(codec,directory)
