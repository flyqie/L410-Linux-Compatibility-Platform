"""External durable supervisor for apt and entry builds across broker restarts."""
import fcntl,json,os,secrets,signal,subprocess,sys,time
from pathlib import Path
BASE=Path('/data/vendor/l410-linux');CODE=Path('/vendor/bin/l410-linux-control');ROOT=BASE/'rootfs-bookworm';STORE=BASE/'run/jobs'
def boot():return Path('/proc/sys/kernel/random/boot_id').read_text().strip()
def identity(pid):
 try:
  fields=Path('/proc/%d/stat'%pid).read_text().rsplit(')',1)[1].split()
  return fields[19] if fields[0] not in ('Z','X') else None
 except (OSError,IndexError):return None
def read(kind):
 try:return json.loads((STORE/(kind+'.json')).read_text())
 except FileNotFoundError:return None
def save(kind,data):
 p=STORE/(kind+'.json');tmp=p.with_suffix('.new')
 with tmp.open('w') as f:json.dump(data,f);f.flush();os.fsync(f.fileno())
 tmp.replace(p)
class Job:
 def __init__(self,kind,data,child=None):self.kind=kind;self.id=data['id'];self.data=data;self.child=child;self.returncode=None
 @property
 def pid(self):return (read(self.kind) or self.data).get('pid',self.child.pid if self.child else 0)
 @property
 def metadata(self):return self.data['metadata']
 def poll(self):
  if self.child:self.child.poll()
  data=read(self.kind)
  if not data or data['id']!=self.id:self.returncode=125;return 125
  self.data=data
  if data['state']=='done':self.returncode=data['exit'];return self.returncode
  if data['boot']==boot():
   if data['state']=='prepared' and time.time()-data['created']<10:return None
   if data.get('start') and identity(data['pid'])==data['start']:return None
  data.update(state='done',exit=125,error='Supervisor interrupted; inspect log and package state before retry',finished=time.time());save(self.kind,data);self.returncode=125;return 125
 def wait(self,timeout=None):
  begin=time.monotonic()
  while self.poll() is None:
   if timeout is not None and time.monotonic()-begin>=timeout:raise subprocess.TimeoutExpired(self.kind,timeout)
   time.sleep(.1)
  return self.returncode
 def terminate(self):
  if self.poll() is None and self.data.get('start') and identity(self.data['pid'])==self.data['start']:os.kill(self.data['pid'],signal.SIGTERM)
 def handled(self):
  data=read(self.kind)
  if data and data['id']==self.id and data['state']=='done':data['handled']=True;save(self.kind,data)
def recover(kind):
 data=read(kind)
 return Job(kind,data) if data else None
def launch(kind,command,env,log,metadata):
 STORE.mkdir(mode=0o700,parents=True,exist_ok=True)
 old=recover(kind)
 if old and old.poll() is None:raise ValueError(kind+' task is already active')
 data={'id':secrets.token_hex(12),'state':'prepared','boot':boot(),'created':time.time(),'metadata':metadata,'command':command,'handled':False}
 save(kind,data)
 loader=[str(ROOT/'lib/ld-linux-aarch64.so.1'),'--library-path',str(ROOT/'usr/lib/aarch64-linux-gnu')+':'+str(ROOT/'lib/aarch64-linux-gnu'),str(ROOT/'usr/bin/python3'),str(CODE/'managed_jobs.py'),kind,data['id']]
 try:
  with Path(log).open('wb') as output:child=subprocess.Popen(loader,env=env,stdin=subprocess.DEVNULL,stdout=output,stderr=subprocess.STDOUT,start_new_session=True)
 except BaseException:
  data.update(state='done',exit=125,finished=time.time());save(kind,data);raise
 return Job(kind,data,child)
def supervise(kind,key):
 lock=(STORE/(kind+'.lock')).open('a');fcntl.flock(lock,fcntl.LOCK_EX|fcntl.LOCK_NB)
 data=read(kind)
 if not data or data['id']!=key:raise ValueError('job replaced before supervisor start')
 data.update(state='running',pid=os.getpid(),start=identity(os.getpid()));save(kind,data)
 child=None
 def terminate(signum,frame):
  if child and child.poll() is None:child.terminate()
 signal.signal(signal.SIGTERM,terminate);signal.signal(signal.SIGINT,terminate)
 code=125
 try:
  child=subprocess.Popen(data['command'],stdin=subprocess.DEVNULL)
  code=child.wait()
 except BaseException as e:print('SUPERVISOR_ERROR',str(e),flush=True)
 finally:
  data.update(state='done',exit=code,finished=time.time());save(kind,data)
 return code
if __name__=='__main__':
 if len(sys.argv)!=3 or sys.argv[1] not in ('apt','build'):raise SystemExit(2)
 raise SystemExit(supervise(sys.argv[1],sys.argv[2]))
