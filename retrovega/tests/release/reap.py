#!/usr/bin/env python3
# reap.py SECONDS cmd...: run cmd in its own session as a subreaper; kill its group after the timeout; reap everything
import ctypes,os,signal,subprocess,sys,time
ctypes.CDLL(None).prctl(36,1,0,0,0)
t=float(sys.argv[1]);p=subprocess.Popen(sys.argv[2:],start_new_session=True)
end=time.time()+t;rc=None
while time.time()<end:
    try:
        pid,st=os.waitpid(-1,os.WNOHANG)
    except ChildProcessError:
        break
    if pid==p.pid: rc=os.waitstatus_to_exitcode(st);break
    if pid==0: time.sleep(.1)
try: os.killpg(p.pid,signal.SIGKILL)
except ProcessLookupError: pass
try:
    while True:
        pid,_=os.waitpid(-1,os.WNOHANG)
        if pid==0: time.sleep(.2)
except ChildProcessError: pass
sys.exit(124 if rc is None else rc)
