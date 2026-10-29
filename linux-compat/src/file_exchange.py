"""Chunked user-selected file exchange confined to the external shared tree."""
import base64,os,secrets,stat,tempfile,time
from pathlib import Path
BASE=Path('/data/vendor/l410-linux');SHARED=BASE/'shared';CHUNK=65536
TEMP=BASE/'run/file-exchange'
sessions={}
def initialize():
    TEMP.mkdir(mode=0o700,exist_ok=True)
    if TEMP.is_symlink():raise ValueError('invalid transfer staging directory')
    TEMP.chmod(0o700)
    # Called only after the broker's exclusive lock is acquired. These are our
    # incomplete imports from a previous interrupted process, never shared files.
    for path in TEMP.glob('import-*'):
        if stat.S_ISREG(path.lstat().st_mode):path.unlink()
def parts(value):
    if not isinstance(value,str) or len(value)>2048 or '\x00' in value or value.startswith('/'):
        raise ValueError('无效共享路径')
    result=value.split('/') if value else []
    if any(p in ('','.','..') or '\\' in p for p in result):raise ValueError('共享路径不能跨出目录')
    return result
def directory(value):
    fd=os.open(SHARED,os.O_RDONLY|os.O_DIRECTORY|os.O_NOFOLLOW)
    try:
        for name in parts(value):
            other=os.open(name,os.O_RDONLY|os.O_DIRECTORY|os.O_NOFOLLOW,dir_fd=fd);os.close(fd);fd=other
        return fd
    except BaseException:os.close(fd);raise
def cancel(key):
    if key is None:return {'cancelled':True}
    if not isinstance(key,str):raise ValueError('无效传输标识')
    item=sessions.pop(key,None)
    if item:
        os.close(item['fd'])
        if 'dirfd' in item:os.close(item['dirfd'])
        if 'temporary' in item:Path(item['temporary']).unlink(missing_ok=True)
    return {'cancelled':True}
def clean(all=False):
    for key,item in list(sessions.items()):
        if all or time.monotonic()-item['updated']>120:cancel(key)
def listing(path):
    fd=directory(path)
    try:
        result=[]
        for name in sorted(os.listdir(fd)):
            try:info=os.stat(name,dir_fd=fd,follow_symlinks=False)
            except FileNotFoundError:continue
            if stat.S_ISREG(info.st_mode) or stat.S_ISDIR(info.st_mode):
                result.append({'name':name,'directory':stat.S_ISDIR(info.st_mode),'size':info.st_size})
        result.sort(key=lambda i:(not i['directory'],i['name']))
        return {'path':path,'files':result}
    finally:os.close(fd)
def new_session(item):
    key=secrets.token_urlsafe(24);item['updated']=time.monotonic();sessions[key]=item
    return {'id':key,'size':item['size'],'chunkSize':CHUNK}
def session(body,kind):
    key=body.get('id');item=sessions.get(key) if isinstance(key,str) else None
    if not item or item['kind']!=kind:raise ValueError('传输不存在或已超时，请重试')
    if type(body.get('offset')) is not int or body['offset']!=item['offset']:raise ValueError('分块位置不一致')
    item['updated']=time.monotonic();return key,item
def dispatch(route,body,shared_mode):
    clean()
    if route=='cancel':return cancel(body.get('id'))
    if shared_mode=='off':raise ValueError('共享目录已关闭')
    # Read-only refers to the Linux mount. Explicit native import may still add
    # files; Linux applications only obtain the selected read-only mount.
    if route=='list':return listing(body.get('path',''))
    if route in ('import/start','export/start'):
        if len(sessions)>=4:raise ValueError('文件传输任务已满，请稍后再试')
    if route=='import/start':
        name=body.get('name');size=body.get('size');path=body.get('path','')
        if not isinstance(name,str) or parts(name)!=[name] or len(name.encode())>240:raise ValueError('无效文件名')
        if type(size) is not int or size<0 or size>2**53-1:raise ValueError('无效文件大小')
        space=os.statvfs(SHARED);reserved=sum(s['size']-s['offset'] for s in sessions.values() if s['kind']=='import')
        if size+reserved+64*1024*1024>space.f_bavail*space.f_frsize:raise ValueError('外置盘剩余空间不足')
        parent=directory(path)
        try:fd,tmp=tempfile.mkstemp(prefix='import-',dir=TEMP)
        except BaseException:os.close(parent);raise
        return new_session({'kind':'import','size':size,'offset':0,'fd':fd,'temporary':tmp,'dirfd':parent,'name':name,'path':path})
    if route=='import/chunk':
        key,item=session(body,'import')
        data=body.get('data')
        if not isinstance(data,str) or len(data)>((CHUNK+2)//3)*4:raise ValueError('文件分块过大')
        try:raw=base64.b64decode(data,validate=True)
        except ValueError:raise ValueError('文件分块编码无效')
        if not raw or item['offset']+len(raw)>item['size']:raise ValueError('文件分块长度不一致')
        view=memoryview(raw)
        while view:
            n=os.write(item['fd'],view)
            if n<=0:raise OSError('文件写入中断')
            view=view[n:]
        item['offset']+=len(raw);return {'offset':item['offset']}
    if route=='import/finish':
        key,item=session(body,'import')
        if item['offset']!=item['size']:raise ValueError('文件尚未传完')
        os.fsync(item['fd']);os.fchmod(item['fd'],0o644);os.fchown(item['fd'],1000,1000)
        name=item['name'];stem,suffix=os.path.splitext(name)
        for number in range(10000):
            candidate=name if not number else stem+' ('+str(number)+')'+suffix
            try:os.link(item['temporary'],candidate,dst_dir_fd=item['dirfd'],follow_symlinks=False);break
            except FileExistsError:continue
        else:raise ValueError('同名文件过多')
        os.fsync(item['dirfd']);result={'name':candidate,'path':item['path'],'size':item['size']};cancel(key);return result
    if route=='export/start':
        path=body.get('path','');names=parts(path)
        if not names:raise ValueError('请选择文件')
        parent=directory('/'.join(names[:-1]))
        try:fd=os.open(names[-1],os.O_RDONLY|os.O_NOFOLLOW|os.O_NONBLOCK,dir_fd=parent)
        finally:os.close(parent)
        info=os.fstat(fd)
        if not stat.S_ISREG(info.st_mode):os.close(fd);raise ValueError('只支持普通文件')
        return new_session({'kind':'export','size':info.st_size,'offset':0,'fd':fd,'mtime':info.st_mtime_ns})
    if route=='export/chunk':
        key,item=session(body,'export');raw=os.read(item['fd'],min(CHUNK,item['size']-item['offset']))
        item['offset']+=len(raw);done=item['offset']==item['size']
        if not raw and not done:cancel(key);raise ValueError('源文件已缩短，请重试')
        result={'data':base64.b64encode(raw).decode(),'offset':item['offset'],'done':done}
        if done:
            info=os.fstat(item['fd']);changed=info.st_size!=item['size'] or info.st_mtime_ns!=item['mtime'];cancel(key)
            if changed:raise ValueError('源文件在传输中被修改，请重试导出')
        return result
    raise ValueError('不支持的文件操作')
