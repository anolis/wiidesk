#!/usr/bin/env python3
"""Real player UI against ALSA null; never uses the host's speakers."""
import os
import pathlib
import signal
import shutil
import subprocess
import time
from audio_test import fixtures
from x11_desktop_smoke import run, launch, close, wait_for
from x11_core_apps_smoke import keys, type_text

home=pathlib.Path(os.environ['WIIDESK_TEST_HOME'])
base=home/'audio-fixtures'
if os.environ.get('WIIDESK_AUDIO_FIXTURE_DIR'):
    shutil.copytree(os.environ['WIIDESK_AUDIO_FIXTURE_DIR'],base)
    wav,mp3=base/'tone.wav',base/'tone.mp3'
else:
    wav,mp3=fixtures(base)
if os.environ.get('WIIDESK_TEST_AUDIO_DIRECT'):
    from x11_desktop_smoke import windows
    app=subprocess.Popen([os.environ['WIIDESK_TEST_BUILD']+'/wiidesk-x11-audio'])
    wait_for(lambda:windows('^WiiDesk Audio$'),'audio opens directly')
    window=windows('^WiiDesk Audio$')[0]
    run('xdotool','windowactivate','--sync',window)
else:
    window=launch(13,'^WiiDesk Audio$')


def status(): return run('xprop','-id',window,'_WIIDESK_AUDIO_STATUS')
def state(): return run('xprop','-id',window,'_WIIDESK_AUDIO_STATE')
def pos(): return int(state().split('position=')[1].split()[0])


type_text(str(base/'playlist.m3u')); keys('Return')
wait_for(lambda:'Loaded 2 track' in status(),'playlist loads')
keys('space'); wait_for(lambda:'Playing' in status(),'WAV playing')
wait_for(lambda:pos()>100,'progress advances')
keys('space'); wait_for(lambda:'Paused' in status(),'pause')
before=pos(); time.sleep(.5); assert pos()==before
keys('minus'); wait_for(lambda:'volume=40' in state(),'volume')
keys('Right'); wait_for(lambda:pos()==3000,'seek paused WAV to end')
keys('space'); wait_for(lambda:'track=1 ' in state() and 'Playing' in status(),'automatic next MP3')
keys('space'); wait_for(lambda:'Paused' in status(),'pause MP3')
keys('ctrl+Left'); wait_for(lambda:'track=0 ' in state(),'previous track')
keys('Escape'); wait_for(lambda:'Stopped' in status(),'stop')
keys('Tab','ctrl+a'); type_text(str(base/'saved.m3u')); keys('F2')
wait_for(lambda:'Playlist saved' in status(),'save playlist')
saved=(base/'saved.m3u').read_text()
assert str(wav) in saved and str(mp3) in saved
keys('F2'); wait_for(lambda:'existing files are not replaced' in status(),'save does not overwrite')
assert (base/'saved.m3u').read_text()==saved
keys('Return'); wait_for(lambda:'Loaded 2 track' in status(),'reload saved playlist')
keys('space'); wait_for(lambda:'Playing' in status(),'play before close')
pid=int(run('xprop','-id',window,'_NET_WM_PID').split('=')[1])
children=run('pgrep','-P',str(pid)).split()
assert children
for child in children: os.kill(int(child),signal.SIGSTOP)
close(window)
wait_for(lambda:all(not pathlib.Path(f'/proc/{child}').exists() for child in children),'close reaps stopped audio worker')
print('PASS: Audio launcher/load, WAV/MP3 playback, progress, pause, seek, volume, auto-next, previous, stop, save/reload/no-overwrite, close cleanup')
