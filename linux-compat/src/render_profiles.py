"""Explicit per-process graphics choices; never replace rootfs/host libraries."""
import json,re
from pathlib import Path
PROFILES={'software':'默认软件显示','gl4es':'OpenGL 2.1 / GL4ES','vulkan-wsi':'Vulkan 1.0 / 实验性 X11'}
FILE=Path('/data/vendor/l410-linux/config/render-profiles.json')

def display_description(profile,native=False):
    """Report configuration, not proof that a driver rendered a frame."""
    renderer={'software':'Xvfb software','gl4es':'GL4ES OpenGL-to-GLES translation',
              'vulkan-wsi':'Vulkan experimental X11 WSI',
              'vulkan-offscreen':'Vulkan offscreen'}.get(profile,'unknown')
    presentation=('native-cpu-copy' if profile=='software' else 'native-gpu-blit') if native else 'http-cpu-frame-transfer'
    return {'renderer':renderer,'presentationConfigured':presentation,
            'rendererEvidence':'configuration-only','hardwareAccelerationVerified':None}
def read():
    result=json.loads(FILE.read_text()) if FILE.exists() else {}
    if not isinstance(result,dict) or any(not re.fullmatch(r'[A-Za-z0-9_.+-]{1,160}\.desktop',key) or not isinstance(value,str) or value not in PROFILES for key,value in result.items()):
        raise ValueError('invalid application render profiles')
    return result
def save(desktop_id,profile):
    if not isinstance(profile,str) or profile not in PROFILES:raise ValueError('不支持的渲染方式')
    values=read()
    if profile=='software':values.pop(desktop_id,None)
    else:values[desktop_id]=profile
    candidate=FILE.with_suffix('.new');candidate.write_text(json.dumps(values));candidate.replace(FILE)
def selected(app_name,launch):
    return {'glxgears':'gl4es','vkcube':'vulkan-wsi','vulkantriangle':'vulkan-offscreen'}.get(app_name,(launch or {}).get('renderProfile','software'))
def apply(env,profile):
    # Callers supply a fixed enum, never arbitrary LD_* paths from .desktop data.
    if profile=='software':env['LIBGL_ALWAYS_SOFTWARE']='1';return
    env.pop('LIBGL_ALWAYS_SOFTWARE',None)
    if profile=='gl4es':
        p='/opt/l410/mali'
        env.update(LIBGL_FB='3',LIBGL_ES='2',LIBGL_DEEPBIND='0',LIBGL_EGL=p+'/libmali.so.0.18.0',LIBGL_GLES=p+'/libmali.so.0.18.0',LD_LIBRARY_PATH=p,LD_PRELOAD=p+'/libdrm-query-compat.so:'+p+'/libwayland-client.so.0:'+p+'/libwayland-server.so.0')
    elif profile in ('vulkan-wsi','vulkan-offscreen'):
        icd='vulkan-l410-wsi.json' if profile=='vulkan-wsi' else 'vulkan-l410.json'
        env.update(LD_LIBRARY_PATH='/opt/l410/hybris',HYBRIS_LINKER_DIR='/opt/l410/hybris',HYBRIS_ANDROID_SDK_VERSION='29',HYBRIS_LD_LIBRARY_PATH='/opt/l410/vulkan-bionic',VK_ICD_FILENAMES='/opt/l410/hybris/'+icd,XDG_RUNTIME_DIR='/tmp')
    else:raise ValueError('unsupported render profile')
