#!/usr/bin/env python3
"""Decode and real ALSA null/file-sink tests; no sound reaches host speakers."""
import math
import os
import pathlib
import queue
import signal
import struct
import subprocess
import sys
import tempfile
import threading
import time
import wave


def fixtures(base):
    base.mkdir(exist_ok=True)
    wav = base / 'tone.wav'
    with wave.open(str(wav), 'wb') as w:
        w.setparams((2, 2, 32000, 0, 'NONE', 'not compressed'))
        w.writeframes(b''.join(struct.pack('<hh', int(4000*math.sin(i*2*math.pi*440/32000)),
                                           int(2000*math.sin(i*2*math.pi*220/32000)))
                               for i in range(32000*3)))
    mp3 = base / 'tone.mp3'
    subprocess.run(['ffmpeg', '-v', 'error', '-y', '-i', str(wav), '-codec:a', 'libmp3lame', '-b:a', '96k', str(mp3)], check=True)
    (base / 'playlist.m3u').write_text('#EXTM3U\ntone.wav\ntone.mp3\n')
    return wav, mp3


class Worker:
    def __init__(self, worker, path, device='null', env=None):
        self.p = subprocess.Popen([worker, str(path), device], stdin=subprocess.PIPE,
                                  stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, env=env)
        self.q = queue.Queue()
        self.lines = []
        def reader():
            for line in self.p.stdout:
                self.q.put(line.strip())
        self.thread = threading.Thread(target=reader, daemon=True); self.thread.start()

    def wait(self, prefix, timeout=8):
        until = time.monotonic()+timeout
        while time.monotonic()<until:
            try: line = self.q.get(timeout=.1)
            except queue.Empty: continue
            self.lines.append(line)
            if line.startswith(prefix): return line
            assert not line.startswith('ERROR'), line
        raise AssertionError((prefix, self.lines, self.p.poll()))

    def send(self, command):
        self.p.stdin.write(command+'\n'); self.p.stdin.flush()

    def close(self):
        if self.p.poll() is None:
            self.send('Q')
        self.p.wait(timeout=3)
        self.p.stdin.close(); self.thread.join(timeout=1)
        self.p.stdout.close(); self.p.stderr.close()


def main():
    build = pathlib.Path(sys.argv[1]).resolve()
    probe, worker = str(build/'audio-probe'), str(build/'wiidesk-audio-worker')
    with tempfile.TemporaryDirectory(prefix='wiidesk-audio-test-') as temporary:
        base = pathlib.Path(temporary); wav, mp3 = fixtures(base)
        def inspect(mode, path, *extra, ok=True):
            p = subprocess.run([probe, mode, str(path), *map(str, extra)], capture_output=True, text=True, timeout=8)
            assert (p.returncode==0)==ok, p.stdout+p.stderr
            return p.stdout
        assert 'RESULT 96000 ' in inspect('decode', wav)
        assert 'RESULT 64000 ' in inspect('decode', wav, 32000)
        assert 'RESULT 96000 ' in inspect('decode', mp3)
        assert 'RESULT 64000 ' in inspect('decode', mp3, 32000)
        small = base/'samples.wav'
        with wave.open(str(small),'wb') as w:
            w.setparams((1,2,8000,0,'NONE','none')); w.writeframes(struct.pack('<hhhh',-32768,-1,0,32767))
        assert 'FIRST -32768 -1 0 32767' in inspect('decode',small)
        with wave.open(str(small),'wb') as w:
            w.setparams((1,1,8000,0,'NONE','none')); w.writeframes(bytes([0,127,128,255]))
        assert 'FIRST -32768 -256 0 32512' in inspect('decode',small)
        bad=base/'bad.wav'; bad.write_bytes(wav.read_bytes()[:100])
        inspect('decode',bad,ok=False)
        fifo=base/'fifo.wav'; os.mkfifo(fifo); inspect('decode',fifo,ok=False)
        symlink=base/'link.wav'; symlink.symlink_to(wav); inspect('decode',symlink,ok=False)
        huge=base/'huge.wav'
        with huge.open('wb') as f: f.truncate(1024**3+1)
        inspect('decode',huge,ok=False)
        assert 'OK 2' in inspect('list',base/'playlist.m3u')
        badlist=base/'bad.m3u'; badlist.write_text('https://example.invalid/tone.mp3\n')
        inspect('list',badlist,ok=False)
        badlist.write_text(('tone.wav\n')*129); inspect('list',badlist,ok=False)
        badlist.write_text('#'*(65536+1)); inspect('list',badlist,ok=False)
        badlist.write_bytes(b'tone.wav\x00hidden\n'); inspect('list',badlist,ok=False)
        badlist.write_bytes(b'\xef\xbb\xbftone.wav\r\ntone.mp3\r\n')
        assert 'OK 2' in inspect('list',badlist)
        for path in (wav,mp3):
            w=Worker(worker,path)
            try:
                w.wait('STATE playing'); time.sleep(.35); w.send('P 1'); w.wait('STATE paused')
                a=w.wait('POS '); time.sleep(.4); b=w.wait('POS '); assert a==b,(a,b)
                w.send('S 1500'); assert 'POS 1500'==w.wait('POS 1500')
                w.send('V 0'); w.send('P 0'); w.wait('STATE playing'); w.wait('STATE ended')
                assert w.p.wait(timeout=3)==0
            finally: w.close()
        # Check the real PCM stream reaches ALSA's file plugin with software volume.
        pcm=base/'output.raw'; config=base/'alsa.conf'
        config.write_text(f'pcm.sink {{ type file slave.pcm {{ type null }} file "{pcm}" format "raw" }}\n')
        w=Worker(worker,small,'sink',dict(os.environ,ALSA_CONFIG_PATH=str(config)))
        try:
            w.wait('STATE ended'); assert w.p.wait(timeout=3)==0
            assert struct.unpack('=hhhh',pcm.read_bytes())==(-16384,-128,0,16256)
        finally: w.close()
        p=subprocess.run([worker,str(wav),'wiidesk_nonexistent_device'],input='Q\n',capture_output=True,text=True,timeout=5)
        assert p.returncode!=0 and 'ERROR Audio device:' in p.stdout
    print('PASS: WAV/MP3 decode/seek, endian conversion, malformed/FIFO/symlink/size bounds, playlists, pause/resume/volume, ALSA output and device errors')


if __name__=='__main__': main()
