"""User sessions for the protocol display; no capture backend or test lifetime."""
import json, os, re, subprocess, threading, time, urllib.request
from pathlib import Path
import desktop_entries

BASE=Path('/data/vendor/l410-linux'); CODE=Path('/vendor/bin/l410-linux-control'); ROOT=BASE/'rootfs-bookworm'
TOKEN=(BASE/'config/token').read_text().strip()
ENTER=['/data/local/tmp/boot-tools-island/lib/ld-linux-aarch64.so.1','--library-path','/data/local/tmp/boot-tools-island/lib',str(CODE/'enter'),str(ROOT)]
DESKTOP_PREFIX='desktop:'
# Built-in diagnostics are addressed by short name; installed applications use their desktop id.
APPS={'mousepad':('Mousepad 文本编辑器','usr/bin/mousepad'),'es2gears-wayland':('OpenGL ES 齿轮','usr/bin/es2gears_wayland')}
lock=threading.RLock(); process=None; current=None; failure=''; ready=False
LOG=BASE/'log/protocol-user.log'


def api(path,body=None):
 req=urllib.request.Request('http://127.0.0.1:18511'+path,data=None if body is None else json.dumps(body).encode(),headers={'X-L410-Protocol':TOKEN,'Content-Type':'application/json'})
 return json.load(urllib.request.urlopen(req,timeout=2))


def installed():
 """Installed desktop applications that the protocol display can launch."""
 result=[]
 for entry in desktop_entries.scan(ROOT):
  if not entry['supported']:continue
  result.append({'id':DESKTOP_PREFIX+entry['id'],'name':entry['name'],'icon':entry['icon'],
                 'installed':True,'source':'desktop','reason':''})
 return result


def catalogue():
 result=[{'id':name,'name':label,'icon':'','installed':(ROOT/relative).exists(),'source':'builtin','reason':''}
         for name,(label,relative) in APPS.items()]
 known={entry['id'] for entry in result}
 result+=sorted((entry for entry in installed() if entry['id'] not in known),key=lambda entry:entry['name'])
 return result


def app_info(app_id):
 """Return (description, executable description) or raise for an unusable application."""
 if app_id in APPS:
  label,relative=APPS[app_id]
  if not (ROOT/relative).exists():raise ValueError('应用尚未安装')
  return {'id':app_id,'name':label},None
 if isinstance(app_id,str) and app_id.startswith(DESKTOP_PREFIX):
  desktop_id=app_id[len(DESKTOP_PREFIX):]
  entry=next((item for item in desktop_entries.scan(ROOT) if item['id']==desktop_id),None)
  if not entry:raise ValueError('桌面项不存在或已被移除')
  if not entry['supported']:raise ValueError(entry['reason'] or '此应用暂不支持协议显示')
  return {'id':app_id,'name':entry['name']},{'id':entry['id'],'argv':entry['argv'],'cwd':entry['cwd']}
 raise ValueError('未知的 Linux 应用')


def running():
 return process is not None and process.poll() is None


def status():
 with lock:
  return {'running':running(),'ready':ready and running(),'app':current if running() else None,'error':failure,
          'apps':catalogue(),'home':'/home/linux','shared':'/home/linux/Shared','pid':process.pid if running() else None}


def display_alive():
 for p in Path('/proc').iterdir():
  if not p.name.isdigit():continue
  try:
   if (p/'cmdline').read_bytes().split(b'\0')[0]==b'com.l410.linux.displaylab':return True
  except OSError:pass
 return False


def watch(child):
 global ready,failure
 started=time.monotonic();missing=None
 while child.poll() is None:
  with lock:
   if child is not process or child.poll() is not None:return
   try:
    state=api('/windows')
    if any(w.get('first_buffer_frames',0)>0 for w in state['windows']):ready=True
   except (OSError,ValueError):pass
   if ready and not display_alive():
    missing=missing or time.monotonic()
    if time.monotonic()-missing>3:
     failure='显示窗口已退出，Linux 会话已清理';stop();return
   else:missing=None
   if not ready and time.monotonic()-started>25:
    failure='启动未完成，请查看会话日志';stop();return
  time.sleep(.5)
 with lock:
  if child is process:
   ready=False
   if child.returncode:failure='Linux 会话退出，代码 '+str(child.returncode)


def start(app,env):
 global process,current,failure,ready
 with lock:
  info,executable=app_info(app)
  if running():
   if current!=app:raise ValueError('当前版本一次运行一个应用，请先关闭当前应用')
   subprocess.run(['aa','start','-b','com.l410.linux.displaylab','-a','LinuxAbility'],check=True,timeout=8,capture_output=True)
   return status()
  # A diagnostic session is not owned by this manager; never take it over.
  try:api('/windows')
  except OSError:pass
  else:raise ValueError('显示端口已被其他会话使用，请先关闭该会话')
  modes=[]
  for block in re.split(r'(?m)^crtc\[',Path('/sys/kernel/debug/dri/0/state').read_text())[1:]:
   if re.search(r'(?m)^\s*active=1$',block):
    match=re.search(r'mode: \d+:"(\d+)x(\d+)" (\d+)',block)
    if match:modes.append(tuple(map(int,match.groups())))
  if len(modes)!=1:raise ValueError('无法确定当前显示模式')
  subprocess.run(['aa','force-stop','com.l410.linux.displaylab'],timeout=8,capture_output=True)
  w,h,hz=modes[0]
  command=ENTER+['--gpu','/usr/bin/env','L410_WAYLAND_GPU=1','LD_LIBRARY_PATH=/opt/l410/protocol/mali-abi:/usr/lib/aarch64-linux-gnu:/opt/l410/mali','LD_PRELOAD=/opt/l410/protocol/libwayland-drm-device.so','/usr/bin/python3','/opt/l410/protocol/wayland_session.py']
  with LOG.open('w') as output:
   process=subprocess.Popen(command,env=env,stdin=subprocess.PIPE,stdout=output,stderr=subprocess.STDOUT)
  current=app;failure='';ready=False
  try:
   request={'key':TOKEN,'mode':f'{w}x{h}@{hz*1000}','app':app,'diagnostic':False,'persistent_home':True,'output_scale':None}
   if executable:request['app_command']=executable
   process.stdin.write(json.dumps(request).encode()+b'\n');process.stdin.close()
   subprocess.run(['aa','start','-b','com.l410.linux.displaylab','-a','LinuxAbility'],check=True,timeout=8,capture_output=True)
  except BaseException:stop();raise
  threading.Thread(target=watch,args=(process,),daemon=True).start()
  return status()


def stop():
 global ready
 with lock:
  if running():
   try:api('/stop',{})
   except OSError:pass
   try:process.wait(timeout=8)
   except subprocess.TimeoutExpired:
    process.terminate()
    try:process.wait(timeout=3)
    except subprocess.TimeoutExpired:
     # enter's direct child is the private PID namespace init, never host init.
     for pid in Path(f'/proc/{process.pid}/task/{process.pid}/children').read_text().split():
      if os.readlink(f'/proc/{pid}/ns/pid')!=os.readlink('/proc/1/ns/pid'):os.kill(int(pid),9)
     process.wait(timeout=3)
   subprocess.run(['aa','force-stop','com.l410.linux.displaylab'],timeout=8,capture_output=True)
  ready=False
  return status()
