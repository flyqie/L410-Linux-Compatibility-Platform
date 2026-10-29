"""Read desktop application metadata and expand Exec without a host shell."""
import configparser,re,shlex
from pathlib import Path
from rootfs_paths import rootfs_regular_file
def scan(root):
    root=Path(root);result=[]
    for path in sorted((root/'usr/share/applications').glob('*.desktop')):
        if path.is_symlink() or not re.fullmatch(r'[A-Za-z0-9_.+-]{1,160}',path.name):continue
        try:
            cp=configparser.ConfigParser(interpolation=None,strict=False);cp.read(path,encoding='utf8');entry=cp['Desktop Entry']
            if entry.get('Type')!='Application' or entry.getboolean('Hidden',False) or entry.getboolean('NoDisplay',False):continue
            item={'id':path.name,'name':entry.get('Name[zh_CN]',entry.get('Name',path.stem)),'icon':entry.get('Icon',''),'exec':entry.get('Exec',''),'supported':False,'reason':''}
            try:
                terminal=entry.getboolean('Terminal',False)
                argv=[]
                for word in shlex.split(item['exec']):
                    if word in ('%f','%F','%u','%U'):continue
                    if word=='%i':
                        if item['icon']:argv+=['--icon',item['icon']]
                        continue
                    word=word.replace('%%','\x00').replace('%c',item['name']).replace('%k','/usr/share/applications/'+path.name)
                    if '%' in word:raise ValueError('不支持的 Exec 字段代码')
                    argv.append(word.replace('\x00','%'))
                if not argv:raise ValueError('缺少 Exec 命令')
                if argv[0].startswith('/'):
                    found=rootfs_regular_file(root,argv[0]) is not None
                elif '/' in argv[0]:found=False
                else:found=any(rootfs_regular_file(root,'/'+p+'/'+argv[0]) is not None for p in ('usr/bin','bin','usr/local/bin'))
                if not found:raise ValueError('可执行文件不存在')
                cwd=entry.get('Path','/home/linux')
                if not cwd.startswith('/') or '..' in Path(cwd).parts or not (root/cwd.lstrip('/')).is_dir():raise ValueError('工作目录无效')
                if terminal:
                    if not (root/'usr/bin/xterm').is_file():raise ValueError('需要安装 xterm 终端')
                    argv=['/usr/bin/xterm','-e']+argv
                is_terminal=Path(argv[0]).name in ('xterm','uxterm')
                if is_terminal:argv[1:1]=['-fa','DejaVu Sans Mono','-fs','12','-xrm','XTerm*selectToClipboard:true']
                item.update(argv=argv,cwd=cwd,supported=True,terminal=is_terminal)
            except ValueError as error:item['reason']=str(error)
            result.append(item)
        except (OSError,ValueError,configparser.Error,KeyError):continue
    return result
