const filePicker=requireNapi('file.picker');
const fileFs=requireNapi('file.fs');
const fileUtil=requireNapi('util');
const prompt=requireNapi('promptAction');
const managerBridge=requireNapi('l410uihost',true,'__L410_BUNDLE__/entry')||{};
const MAX_ROWS=128;
const BRAND='#0A59F7';
const TEXT='#182431';
const SUBTEXT='#5D6673';
const BG='#F1F3F5';
const CARD='#FFFFFF';

class ConsolePage extends ViewPU {
 constructor(parent,params,storage,id=-1){super(parent,storage,id);
  this.__page=new ObservedPropertySimplePU('overview',this,'page');
  this.__status=new ObservedPropertySimplePU('正在连接 Linux 兼容服务…',this,'status');
  this.__apps=new ObservedPropertyObjectPU([],this,'apps');
  this.__rows=new ObservedPropertyObjectPU([],this,'rows');
  this.__pkg=new ObservedPropertySimplePU('',this,'pkg');
  this.__busy=new ObservedPropertySimplePU(false,this,'busy');
  this.__memory=new ObservedPropertySimplePU('2048',this,'memory');
  this.__pids=new ObservedPropertySimplePU('512',this,'pids');
  this.__shared=new ObservedPropertySimplePU('readwrite',this,'shared');
  this.__playback=new ObservedPropertySimplePU(true,this,'playback');
  this.__microphone=new ObservedPropertySimplePU(false,this,'microphone');
  this.__camera=new ObservedPropertySimplePU(false,this,'camera');
  this.__gpu=new ObservedPropertySimplePU(true,this,'gpu');
  this.__resources=new ObservedPropertySimplePU('正在读取…',this,'resources');
  this.__sensors=new ObservedPropertySimplePU('正在读取…',this,'sensors');
  this.__log=new ObservedPropertySimplePU('',this,'log');
  this.__engineRunning=new ObservedPropertySimplePU(false,this,'engineRunning');
  this.__aptRunning=new ObservedPropertySimplePU(false,this,'aptRunning');
  this.__workerOnline=new ObservedPropertySimplePU(false,this,'workerOnline');
  this.__enginePhase=new ObservedPropertySimplePU('stopped',this,'enginePhase');
  this.__localDeb=new ObservedPropertySimplePU('',this,'localDeb');
  this.__sharedDirectory=new ObservedPropertySimplePU('',this,'sharedDirectory');
  this.__shellOutput=new ObservedPropertySimplePU('',this,'shellOutput');
  this.__shellActive=new ObservedPropertySimplePU(false,this,'shellActive');
   this.__sshRunning=new ObservedPropertySimplePU(false,this,'sshRunning');
   this.__sshEnabled=new ObservedPropertySimplePU(false,this,'sshEnabled');
   this.__sshPort=new ObservedPropertySimplePU('2222',this,'sshPort');
   this.__sshConnections=new ObservedPropertySimplePU(0,this,'sshConnections');
   this.__sshPid=new ObservedPropertySimplePU(0,this,'sshPid');
   this.__sshPublicKey=new ObservedPropertySimplePU('',this,'sshPublicKey');
   this.__sshClientKeyPresent=new ObservedPropertySimplePU(false,this,'sshClientKeyPresent');
   this.__sshClientPublicKey=new ObservedPropertySimplePU('',this,'sshClientPublicKey');
   this.__sshMessage=new ObservedPropertySimplePU('',this,'sshMessage');
   this.variableKeys=['page','status','apps','rows','pkg','busy','memory','pids','shared','playback','microphone','camera','gpu','resources','sensors','log','engineRunning','aptRunning','workerOnline','enginePhase','localDeb','sharedDirectory','shellOutput','shellActive','sshRunning','sshEnabled','sshPort','sshConnections','sshPid','sshPublicKey','sshClientKeyPresent','sshClientPublicKey','sshMessage'];
   this.signature='';this.disposed=false;this.polling=false;this.sshPortDirty=false;this.finalizeConstruction();
 }
 get page(){return this.__page.get();} set page(v){this.__page.set(v);}
 get status(){return this.__status.get();} set status(v){this.__status.set(v);}
 get apps(){return this.__apps.get();} set apps(v){this.__apps.set(v);}
 get rows(){return this.__rows.get();} set rows(v){this.__rows.set(v);}
 get pkg(){return this.__pkg.get();} set pkg(v){this.__pkg.set(v);}
 get busy(){return this.__busy.get();} set busy(v){this.__busy.set(v);}
 get memory(){return this.__memory.get();} set memory(v){this.__memory.set(v);}
 get pids(){return this.__pids.get();} set pids(v){this.__pids.set(v);}
 get shared(){return this.__shared.get();} set shared(v){this.__shared.set(v);}
 get playback(){return this.__playback.get();} set playback(v){this.__playback.set(v);}
 get microphone(){return this.__microphone.get();} set microphone(v){this.__microphone.set(v);}
 get camera(){return this.__camera.get();} set camera(v){this.__camera.set(v);}
 get gpu(){return this.__gpu.get();} set gpu(v){this.__gpu.set(v);}
 get resources(){return this.__resources.get();} set resources(v){this.__resources.set(v);}
 get sensors(){return this.__sensors.get();} set sensors(v){this.__sensors.set(v);}
 get log(){return this.__log.get();} set log(v){this.__log.set(v);}
 get engineRunning(){return this.__engineRunning.get();} set engineRunning(v){this.__engineRunning.set(v);}
 get aptRunning(){return this.__aptRunning.get();} set aptRunning(v){this.__aptRunning.set(v);}
 get workerOnline(){return this.__workerOnline.get();} set workerOnline(v){this.__workerOnline.set(v);}
 get enginePhase(){return this.__enginePhase.get();} set enginePhase(v){this.__enginePhase.set(v);}
 get localDeb(){return this.__localDeb.get();} set localDeb(v){this.__localDeb.set(v);}
 get sharedDirectory(){return this.__sharedDirectory.get();} set sharedDirectory(v){this.__sharedDirectory.set(v);}
 get shellOutput(){return this.__shellOutput.get();} set shellOutput(v){this.__shellOutput.set(v);}
 get shellActive(){return this.__shellActive.get();} set shellActive(v){this.__shellActive.set(v);}
  get sshRunning(){return this.__sshRunning.get();} set sshRunning(v){this.__sshRunning.set(v);}
  get sshEnabled(){return this.__sshEnabled.get();} set sshEnabled(v){this.__sshEnabled.set(v);}
  get sshPort(){return this.__sshPort.get();} set sshPort(v){this.__sshPort.set(v);}
  get sshConnections(){return this.__sshConnections.get();} set sshConnections(v){this.__sshConnections.set(v);}
  get sshPid(){return this.__sshPid.get();} set sshPid(v){this.__sshPid.set(v);}
  get sshPublicKey(){return this.__sshPublicKey.get();} set sshPublicKey(v){this.__sshPublicKey.set(v);}
  get sshClientKeyPresent(){return this.__sshClientKeyPresent.get();} set sshClientKeyPresent(v){this.__sshClientKeyPresent.set(v);}
  get sshClientPublicKey(){return this.__sshClientPublicKey.get();} set sshClientPublicKey(v){this.__sshClientPublicKey.set(v);}
  get sshMessage(){return this.__sshMessage.get();} set sshMessage(v){this.__sshMessage.set(v);}

 async api(path,body){
  const fn=managerBridge.managerCall||globalThis.l410ManagerCall;
  if(typeof fn!=='function')throw new Error('管理服务尚未就绪');
  const raw=await fn(path,body===undefined?undefined:JSON.stringify(body));
  const answer=JSON.parse(raw&&raw.length?raw:'{}');
  if(!answer.ok)throw new Error(answer.error||'管理服务请求失败');
  return answer.data;
 }
 apply(state){
  const apps=state.apps||[];
  const signature=JSON.stringify(apps.map(a=>[a.id,a.name,a.entryEnabled,a.entryReady,a.entryBundle,a.launchReady,a.ancoBundle,a.ancoAbility,a.buildState,a.buildError,a.catalogReason]));
  if(signature!==this.signature){
   this.signature=signature;this.apps=apps;
   this.rows=apps.slice(0,MAX_ROWS).map(a=>({id:a.id,name:a.name,reason:a.reason||'',supported:!!a.supported,
    launchReady:!!a.launchReady,ancoBundle:a.ancoBundle||'',ancoAbility:a.ancoAbility||'',
    enabled:!!a.entryEnabled,ready:!!a.entryReady,entryBundle:a.entryBundle||'',buildState:a.buildState||'',buildError:a.buildError||'',catalogReason:a.catalogReason||''}));
  }
  this.engineRunning=!!state.engineRunning;this.enginePhase=state.enginePhase||'stopped';this.aptRunning=!!state.aptRunning;this.workerOnline=!!state.workerOnline;this.sharedDirectory=state.sharedDirectory||'';
  const phaseText={running:'Linux 兼容服务正在运行',starting:'Linux 兼容服务正在启动',start:'Linux 兼容服务正在启动',stop:'Linux 兼容服务正在停止',restart:'Linux 兼容服务正在重启',stopped:'Linux 兼容服务已停止',error:'Linux 兼容服务异常'};
  this.status=phaseText[this.enginePhase]||(this.engineRunning?'Linux 兼容服务正在运行':'Linux 兼容服务已停止');
  if(state.engineOperation&&state.engineOperation.state==='error')this.status+=' · '+state.engineOperation.message;
  if(this.aptRunning)this.status+=' · apt 正在处理';
  if(state.entryBuildRunning)this.status+=' · 正在生成应用入口';
  if(state.settings){const s=state.settings;this.memory=String(s.memoryMiB);this.pids=String(s.pidsLimit);this.shared=s.sharedMode;this.playback=s.playback;this.microphone=s.microphone;this.camera=s.camera;this.gpu=s.gpu;}
  if(state.resources)this.resources='内存 '+Math.round(state.resources.memoryBytes/1048576)+' / '+state.settings.memoryMiB+' MiB　进程 '+state.resources.pids+' / '+state.resources.pidsLimit+'　CPU '+(state.resources.cpuPercent||0)+'%';
  if(state.sensors)this.sensors=state.sensors.map(s=>s.name+' '+s.celsius.toFixed(1)+'°C').join('　')||'没有可读温度传感器';
   if(state.ssh){const s=state.ssh;this.sshRunning=!!s.running;this.sshEnabled=!!s.enabled;this.sshConnections=Number(s.connections)||0;this.sshPid=Number(s.pid)||0;this.sshPublicKey=s.publicKey||'';this.sshClientKeyPresent=!!s.clientKeyPresent;this.sshClientPublicKey=s.clientPublicKey||'';if(!this.sshPortDirty)this.sshPort=String(s.configuredPort||s.actualPort||2222);}
 }
 async refresh(){if(this.polling||this.disposed)return;this.polling=true;try{this.apply(await this.api('/console/status'));}catch(e){console.error('L410_MANAGER_REFRESH_FAILED',e&&e.stack?e.stack:(e.message||String(e)));if(!this.disposed)this.status='连接失败：'+(e.message||String(e));}finally{this.polling=false;}}
 async control(path,body){if(this.busy)return;this.busy=true;try{const result=await this.api(path,body);this.log=result.output!==undefined?result.output:(result.message||'操作已提交。');if(result.message||result.output)prompt.showToast({message:(result.message||result.output).slice(0,120),duration:3000});await this.refresh();}catch(e){this.log=e.message||String(e);prompt.showToast({message:this.log,duration:3000});}finally{this.busy=false;}}
 async launch(item){
  if(!item||!item.id||!item.supported){prompt.showToast({message:(item&&item.reason)||'该 Linux 应用当前不可启动',duration:3500});return;}
  try{const result=await this.api('/anco/launch',{id:item.id});prompt.showToast({message:result.preparing?'正在准备兼容环境并启动':'启动请求已交给 Linux 应用',duration:2500});}
  catch(e){prompt.showToast({message:'启动失败：'+(e.message||String(e)),duration:4000});}
 }
 async launchNative(bundle,ability){
  const context=globalThis.l410ConsoleContext;
  try{await context.startAbility({bundleName:bundle,abilityName:ability});}
  catch(e){prompt.showToast({message:'打开失败：'+(e.message||String(e)),duration:4000});}
 }
 async purge(){if(!this.pkg)return;try{const result=await prompt.showDialog({title:'卸载 Linux 软件包',message:'将从 Debian 用户态卸载 “'+this.pkg+'”。对应鸿蒙菜单入口会在卸载成功后清理。',buttons:[{text:'取消',color:SUBTEXT},{text:'卸载',color:'#C62828'}]});if(result.index===1)this.control('/apt',{action:'purge',package:this.pkg});}catch(_){} }
 async pickDeb(){
  if(this.busy)return;this.busy=true;let sourceFile,targetFile;
  try{
   const context=globalThis.l410ConsoleContext;
   const picker=new filePicker.DocumentViewPicker(context);
   const selected=await picker.select({maxSelectNumber:1,selectMode:0,fileSuffixFilters:['Debian 软件包|.deb']});
   if(!selected||!selected.length)return;
   const source=selected[0];
   if(!source.toLowerCase().includes('.deb'))throw new Error('请选择 .deb 软件包');
   const target=context.filesDir+'/local-install.deb';
   try{fileFs.unlinkSync(target);}catch(_){}
   /* DocumentViewPicker returns an authorized document URI, not a normal
    * pathname.  Open the URI while the grant is live and copy by descriptor
    * into this application's existing broker-visible files directory. */
   sourceFile=fileFs.openSync(source,fileFs.OpenMode.READ_ONLY);
   targetFile=fileFs.openSync(target,fileFs.OpenMode.READ_WRITE|fileFs.OpenMode.CREATE|fileFs.OpenMode.TRUNC);
   await fileFs.copyFile(sourceFile.fd,targetFile.fd);
   fileFs.closeSync(targetFile);targetFile=undefined;
   fileFs.closeSync(sourceFile);sourceFile=undefined;
   const stat=fileFs.statSync(target);
   if(stat.size<128||stat.size>1073741824)throw new Error('deb 文件大小无效');
   this.localDeb=source.split('/').pop();
   const result=await this.api('/apt',{action:'install-local'});
   this.log='本地 deb 已交给 apt 安装，任务 PID '+result.pid;this.page='logs';
   prompt.showToast({message:'已开始安装 '+this.localDeb,duration:3000});
  }catch(e){this.log=e.message||String(e);prompt.showToast({message:this.log,duration:4000});}
  finally{
   if(targetFile){try{fileFs.closeSync(targetFile);}catch(_){}}
   if(sourceFile){try{fileFs.closeSync(sourceFile);}catch(_){}}
   this.busy=false;await this.refresh();
  }
 }
 async openShell(){if(this.busy)return;this.busy=true;try{const state=await this.api('/shell/open',{});this.page='shell';this.shellActive=true;this.shellOutput=state.message||'Linux 终端窗口已打开';prompt.showToast({message:this.shellOutput,duration:3000});}catch(e){this.shellActive=false;this.shellOutput=e.message||String(e);prompt.showToast({message:this.shellOutput,duration:4000});}finally{this.busy=false;}}
  async sshEnable(enabled){if(this.busy)return;if(!enabled){try{const answer=await prompt.showDialog({title:'停用设备 SSH',message:'停用后所有远程 SSH 入口都会关闭。可以在本机此页面重新启用。',buttons:[{text:'取消',color:SUBTEXT},{text:'停用',color:'#C62828'}]});if(answer.index!==1)return;}catch(_){return;}}await this.sshAction('/ssh/enabled',{enabled});}
  async sshChangePort(){const port=parseInt(this.sshPort,10);if(!Number.isInteger(port)||port<1024||port>65535||String(port)!==this.sshPort.trim()){prompt.showToast({message:'端口必须是 1024–65535 的整数',duration:3000});return;}try{const answer=await prompt.showDialog({title:'修改 SSH 端口',message:'管理器会先在新端口验证 Dropbear，再切换现役服务。切换成功后请使用端口 '+port+' 重新连接。',buttons:[{text:'取消',color:SUBTEXT},{text:'验证并切换',color:BRAND}]});if(answer.index!==1)return;}catch(_){return;}await this.sshAction('/ssh/port',{port});}
  async sshRegenerate(){if(this.busy)return;try{const answer=await prompt.showDialog({title:'重新生成 SSH 主机密钥',message:'这会替换设备用于证明自身身份的 ECDSA 主机密钥，不会修改你的客户端登录私钥。之后客户端需要移除旧 known_hosts 记录并接受新密钥。',buttons:[{text:'取消',color:SUBTEXT},{text:'重新生成',color:'#C62828'}]});if(answer.index!==1)return;}catch(_){return;}await this.sshAction('/ssh/regenerate-host-key',{});}
  async sshGenerateClient(){
   if(this.busy)return;
   if(this.sshClientKeyPresent){try{const answer=await prompt.showDialog({title:'重新生成客户端登录密钥',message:'重新生成后，之前导出的私钥将不能再登录。',buttons:[{text:'取消',color:SUBTEXT},{text:'重新生成',color:'#C62828'}]});if(answer.index!==1)return;}catch(_){return;}}
   await this.sshAction('/ssh/client-key/generate',{});
  }
  async sshExportClient(){
   if(this.busy)return;this.busy=true;let file;
   try{
    const result=await this.api('/ssh/client-key/export');
    const picker=new filePicker.DocumentViewPicker(globalThis.l410ConsoleContext);
    const selected=await picker.save({newFileNames:[result.filename||'l410_ssh_ed25519']});
    if(!selected||!selected.length)return;
    const bytes=new fileUtil.Base64Helper().decodeSync(result.privateKeyBase64||'');
    file=fileFs.openSync(selected[0],fileFs.OpenMode.READ_WRITE|fileFs.OpenMode.CREATE|fileFs.OpenMode.TRUNC);
    let written=0;
    while(written<bytes.byteLength){const buffer=bytes.buffer.slice(bytes.byteOffset+written,bytes.byteOffset+bytes.byteLength);const count=fileFs.writeSync(file.fd,buffer);if(count<=0)throw new Error('私钥文件写入中断');written+=count;}
    fileFs.closeSync(file);file=undefined;this.sshMessage='客户端私钥已导出，可随时再次导出';prompt.showToast({message:this.sshMessage,duration:4000});
   }catch(e){this.sshMessage=e.message||String(e);prompt.showToast({message:this.sshMessage,duration:5000});}
   finally{if(file){try{fileFs.closeSync(file);}catch(_){}}this.busy=false;}
  }
  async sshAction(path,body){if(this.busy)return;this.busy=true;try{const state=await this.api(path,body);this.sshMessage=state.message||'操作完成';this.sshPortDirty=false;this.apply({ssh:state});prompt.showToast({message:this.sshMessage,duration:4000});}catch(e){this.sshMessage=e.message||String(e);prompt.showToast({message:this.sshMessage,duration:5000});}finally{this.busy=false;await this.refresh();}}
 async showLog(path){try{const result=await this.api(path);this.log=(result.output||'（暂无输出）').slice(-12000);this.page='logs';}catch(e){this.log=e.message||String(e);} }
 async saveSettings(){const memory=parseInt(this.memory,10),pids=parseInt(this.pids,10);if(!(memory>=512&&memory<=4096)||!(pids>=64&&pids<=1024)){prompt.showToast({message:'内存需 512–4096 MiB，进程数需 64–1024',duration:3000});return;}this.control('/settings',{memoryMiB:memory,pidsLimit:pids,sharedMode:this.shared,playback:this.playback,microphone:this.microphone,camera:this.camera,gpu:this.gpu});}
 aboutToAppear(){this.refresh();this.timer=setInterval(()=>this.refresh(),3000);}
 aboutToDisappear(){clearInterval(this.timer);}
 aboutToBeDeleted(){this.disposed=true;clearInterval(this.timer);for(const key of this.variableKeys)this['__'+key].aboutToBeDeleted();SubscriberManager.Get().delete(this.id__());this.aboutToBeDeletedInternal();}
 purgeVariableDependenciesOnElmtId(id){for(const key of this.variableKeys)this['__'+key].purgeDependencyOnElmtId(id);}

 text(value,size=16,color=TEXT,visible=true){this.observeComponentCreation2(()=>{const current=typeof value==='function'?value():value;const currentColor=typeof color==='function'?color():color;const currentVisible=typeof visible==='function'?visible():visible;Text.create(current===undefined||current===null?'':String(current));Text.fontSize(size);Text.fontColor(currentColor);Text.width('100%');Text.textAlign(TextAlign.Start);Text.visibility(currentVisible?Visibility.Visible:Visibility.None);},Text);Text.pop();}
 sectionTitle(title,subtitle){this.text(title,28,TEXT);this.text(subtitle,14,SUBTEXT);}
 nav(label,key){this.observeComponentCreation2(()=>{Button.createWithLabel(label);Button.width('100%');Button.height(48);Button.fontSize(16);Button.fontColor(this.page===key?'#FFFFFF':TEXT);Button.backgroundColor(this.page===key?BRAND:'#00000000');Button.borderRadius(12);Button.onClick(()=>{this.page=key;});},Button);Button.pop();}
 primary(label,action,visible=true,enabled=true){this.observeComponentCreation2(()=>{const current=typeof label==='function'?label():label;const currentVisible=typeof visible==='function'?visible():visible;const currentEnabled=typeof enabled==='function'?enabled():enabled;Button.createWithLabel(current);Button.height(40);Button.enabled(!this.busy&&currentEnabled);Button.visibility(currentVisible?Visibility.Visible:Visibility.None);Button.fontColor('#FFFFFF');Button.backgroundColor(BRAND);Button.borderRadius(20);Button.onClick(action);},Button);Button.pop();}
 secondary(label,action,visible=true,enabled=true){this.observeComponentCreation2(()=>{const current=typeof label==='function'?label():label;const currentVisible=typeof visible==='function'?visible():visible;const currentEnabled=typeof enabled==='function'?enabled():enabled;Button.createWithLabel(current);Button.height(40);Button.enabled(!this.busy&&currentEnabled);Button.visibility(currentVisible?Visibility.Visible:Visibility.None);Button.fontColor(BRAND);Button.backgroundColor('#EAF1FF');Button.borderRadius(20);Button.onClick(action);},Button);Button.pop();}
 field(value,hint,setter){this.observeComponentCreation2(()=>{const current=typeof value==='function'?value():value;TextInput.create({text:current,placeholder:hint});TextInput.height(44);TextInput.width('100%');TextInput.backgroundColor('#F7F8FA');TextInput.borderRadius(12);TextInput.onChange(v=>setter(v));},TextInput);TextInput.pop();}
 cardStart(grow=false){this.observeComponentCreation2(()=>{Column.create({space:10});if(grow)Column.layoutWeight(1);else Column.width('100%');Column.padding(20);Column.backgroundColor(CARD);Column.borderRadius(16);},Column);}
 appRow(index){
  const item=()=>this.rows[index]||{};
  this.observeComponentCreation2(()=>{Column.create({space:8});Column.width('100%');Column.padding(16);Column.backgroundColor(CARD);Column.borderRadius(16);Column.visibility(item().id?Visibility.Visible:Visibility.None);},Column);
  this.text(()=>item().name||'',18,TEXT);
  this.text(()=>item().id||'',13,SUBTEXT);
  this.text(()=>item().supported?('可直接打开'+(item().enabled?' · 已显示在鸿蒙应用菜单':' · 未加入鸿蒙应用菜单')):(item().reason||'当前不可启动'),13,()=>item().supported?'#2E7D32':'#B3261E');
  this.text(()=>item().buildState?('入口任务：'+item().buildState+(item().buildError?' · '+item().buildError:'')):'',12,()=>item().buildState==='error'?'#B3261E':SUBTEXT,()=>!!item().buildState);
  this.observeComponentCreation2(()=>{Row.create({space:10});Row.width('100%');},Row);
  this.primary('打开',()=>this.launch(item()),true,()=>!!item().supported);
  this.secondary('移出鸿蒙菜单',()=>this.control('/entry',{id:item().id,enabled:false}),()=>!!item().enabled);
  this.secondary(()=>item().buildState==='queued'||item().buildState==='building'?'正在生成入口…':'加入鸿蒙菜单',()=>this.control('/entry',{id:item().id,enabled:true}),()=>!item().enabled,()=>!!item().supported&&item().buildState!=='queued'&&item().buildState!=='building');
  Row.pop();Column.pop();
 }
 overview(){
  this.observeComponentCreation2(()=>{Column.create({space:16});Column.width('100%');Column.visibility(this.page==='overview'?Visibility.Visible:Visibility.None);},Column);
  this.sectionTitle('概览','Linux 应用通过原生 AnCo 窗口、输入和任务机制运行。');
  this.cardStart();this.text(()=>this.status,18,()=>this.engineRunning?'#2E7D32':'#B26A00');this.text(()=>this.resources,14,SUBTEXT);this.text(()=>this.sensors,14,SUBTEXT);Column.pop();
  this.observeComponentCreation2(()=>{Row.create({space:16});Row.width('100%');},Row);
  this.cardStart(true);this.text('已发现应用',14,SUBTEXT);this.text(()=>String(this.apps.length),30,TEXT);Column.pop();
  this.cardStart(true);this.text('入口构建器',14,SUBTEXT);this.text(()=>this.workerOnline?'在线':'离线',22,()=>this.workerOnline?'#2E7D32':'#B26A00');Column.pop();Row.pop();
  this.cardStart();this.text('快捷操作',20,TEXT);this.observeComponentCreation2(()=>{Row.create({space:12});Row.width('100%');},Row);
  this.primary('进入 Linux Shell',()=>this.openShell());this.secondary('安装 Linux 软件',()=>{this.page='packages';});this.secondary('安装 HAP',()=>this.launchNative('com.l410.linux.hapinstaller','HapInstallerAbility'));Row.pop();Column.pop();
  this.cardStart();this.text('兼容服务',20,TEXT);this.text('控制窗口对窗口的 AnCo 兼容环境；停止会关闭其中的 Linux 应用。',14,SUBTEXT);this.observeComponentCreation2(()=>{Row.create({space:12});Row.width('100%');},Row);this.primary('启动',()=>this.control('/console/start',{}),true,()=>!this.engineRunning&&this.enginePhase!=='start'&&this.enginePhase!=='restart');this.secondary('停止',()=>this.control('/console/stop',{}),true,()=>this.engineRunning&&this.enginePhase!=='stop');this.secondary('重启',()=>this.control('/console/restart',{}),true,()=>this.enginePhase!=='start'&&this.enginePhase!=='stop'&&this.enginePhase!=='restart');Row.pop();Column.pop();Column.pop();
 }
 applications(){
  this.observeComponentCreation2(()=>{Column.create({space:12});Column.width('100%');Column.visibility(this.page==='apps'?Visibility.Visible:Visibility.None);},Column);
  this.sectionTitle('Linux 应用','从 Debian 的 .desktop 文件识别应用。“打开”直接启动 Linux 程序；加入菜单只创建鸿蒙桌面入口和图标，移出菜单不会卸载软件。');
  for(let i=0;i<MAX_ROWS;i++)this.appRow(i);
  Column.pop();
 }
 packages(){
  this.observeComponentCreation2(()=>{Column.create({space:16});Column.width('100%');Column.visibility(this.page==='packages'?Visibility.Visible:Visibility.None);},Column);
  this.sectionTitle('软件包','使用 Debian apt 安装、更新和卸载 ARM64 软件包。');
  this.cardStart();this.text('软件包名称',16,TEXT);this.field(()=>this.pkg,'例如 galculator',v=>{this.pkg=v.trim();});
  this.observeComponentCreation2(()=>{Row.create({space:12});Row.width('100%');},Row);this.primary('安装',()=>this.control('/apt',{action:'install',package:this.pkg}));this.secondary('卸载 Linux 软件包',()=>this.purge());Row.pop();Column.pop();
  this.cardStart();this.text('本地 Debian 软件包',18,TEXT);this.text(()=>this.localDeb?('已选择：'+this.localDeb):'从文件管理器选择 ARM64 .deb，并交给 apt 解析依赖和安装。',14,SUBTEXT);this.primary('选择并安装本地 deb',()=>this.pickDeb(),true,()=>!this.aptRunning);Column.pop();
  this.cardStart();this.text(()=>this.aptRunning?'apt 正在运行':'apt 当前空闲',16,()=>this.aptRunning?'#B26A00':'#2E7D32');this.observeComponentCreation2(()=>{Row.create({space:12});Row.width('100%');},Row);this.primary('更新软件源',()=>this.control('/apt',{action:'update'}));this.secondary('查看 apt 输出',()=>this.showLog('/apt/log'));Row.pop();Column.pop();Column.pop();
 }
 settingsPage(){
  this.observeComponentCreation2(()=>{Column.create({space:16});Column.width('100%');Column.visibility(this.page==='settings'?Visibility.Visible:Visibility.None);},Column);
  this.sectionTitle('共享与资源','清楚区分当前图形兼容环境与管理器启动的维护会话；未接通的能力不会显示成可用开关。');
  this.cardStart();this.text('维护会话资源限制',20,TEXT);this.text('以下上限只用于管理器启动的 apt、Shell 等维护会话，不会追溯修改已运行的 Linux 图形应用。',14,SUBTEXT);this.text('内存上限（MiB）',15,TEXT);this.field(()=>this.memory,'512–4096',v=>{this.memory=v;});this.text('进程数量上限',15,TEXT);this.field(()=>this.pids,'64–1024',v=>{this.pids=v;});Column.pop();
  this.cardStart();this.text('共享目录',20,TEXT);this.text('文件管理 → Huawei Share → Linux 兼容共享',14,TEXT);this.text(()=>('鸿蒙侧：'+(this.sharedDirectory||'正在读取…')),14,SUBTEXT);this.text('Linux 侧：/home/linux/Shared',14,SUBTEXT);this.text('选择维护会话是否能够修改共享目录内容。',14,SUBTEXT);this.observeComponentCreation2(()=>{Select.create([{value:'允许读写'},{value:'仅允许读取'},{value:'不挂载共享目录'}]);Select.width(280);Select.selected(['readwrite','readonly','off'].indexOf(this.shared));Select.value({'readwrite':'允许读写','readonly':'仅允许读取','off':'不挂载共享目录'}[this.shared]);Select.onSelect(index=>{this.shared=['readwrite','readonly','off'][index];});},Select);Select.pop();Column.pop();
  this.cardStart();this.text('图形与硬件状态',20,TEXT);this.text('显示与输入：原生 AnCo 窗口链路',15,'#2E7D32');this.text('图形渲染：当前为 CPU 上传路径；硬件 OpenGL ES / Vulkan 尚未完成独立运行验证。',14,SUBTEXT);this.text('音频、麦克风和摄像头：维护会话配置不代表图形应用已获得设备能力，因此这里不再提供误导性的开关。',14,SUBTEXT);Column.pop();
  this.primary('保存配置',()=>this.saveSettings());Column.pop();
 }
 logs(){
  this.observeComponentCreation2(()=>{Column.create({space:16});Column.width('100%');Column.visibility(this.page==='logs'?Visibility.Visible:Visibility.None);},Column);
  this.sectionTitle('日志','可分别查看兼容服务、apt 和入口构建输出；日志区域可独立滚动和复制。');
  this.observeComponentCreation2(()=>{Row.create({space:12});Row.width('100%');},Row);this.primary('兼容服务日志',()=>this.showLog('/console/log'));this.secondary('apt 输出',()=>this.showLog('/apt/log'));this.secondary('入口构建状态',async()=>{try{const result=await this.api('/entry/build-status');this.log=result.output||'（暂无入口任务）';}catch(e){this.log=e.message||String(e);}});Row.pop();
  this.observeComponentCreation2(()=>{Scroll.create();Scroll.width('100%');Scroll.height(520);Scroll.backgroundColor(CARD);Scroll.borderRadius(16);},Scroll);
  this.observeComponentCreation2(()=>{Text.create(this.log||'（请选择一种日志）');Text.fontSize(13);Text.fontFamily('monospace');Text.fontColor(TEXT);Text.copyOption(CopyOptions.Local);Text.width('100%');Text.padding(20);},Text);Text.pop();Scroll.pop();Column.pop();
 }
 shellPage(){
  this.observeComponentCreation2(()=>{Column.create({space:16});Column.width('100%');Column.visibility(this.page==='shell'?Visibility.Visible:Visibility.None);},Column);
  this.sectionTitle('Linux Shell','使用鸿蒙原生 HiShell 终端，通过标准 PTY 进入 Debian 用户态。');
  this.cardStart();this.text('原生终端窗口',20,TEXT);this.text('支持实时字符输入、命令历史、方向键、Tab 补全、Ctrl 组合键、ANSI 颜色以及 vim、top 等交互程序。',14,SUBTEXT);this.text('终端中的进程运行于 Linux 兼容环境；关闭终端窗口会结束对应 Shell，会话之间彼此独立。',14,SUBTEXT);this.primary('打开 Linux 终端',()=>this.openShell());Column.pop();
  this.cardStart();this.text('会话状态',18,TEXT);this.text(()=>this.shellOutput||'尚未打开 Linux 终端',14,()=>this.shellActive?'#2E7D32':SUBTEXT);Column.pop();Column.pop();
 }
  sshPage(){
   this.observeComponentCreation2(()=>{Column.create({space:16});Column.width('100%');Column.visibility(this.page==='ssh'?Visibility.Visible:Visibility.None);},Column);
   this.sectionTitle('SSH 管理','管理设备内置的 root Dropbear 服务。认证始终使用客户端公钥，密码登录保持关闭。');
   this.observeComponentCreation2(()=>{Row.create({space:16});Row.width('100%');},Row);
   this.cardStart(true);this.text('服务状态',14,SUBTEXT);this.text(()=>this.sshRunning?'正在运行':'已停止',24,()=>this.sshRunning?'#2E7D32':'#B3261E');this.text(()=>this.sshRunning?('PID '+this.sshPid+'　活动连接 '+this.sshConnections):(this.sshClientKeyPresent?'可在本机管理器启用':'请先生成登录密钥'),13,SUBTEXT);Column.pop();
   this.cardStart(true);this.text('当前端口',14,SUBTEXT);this.text(()=>this.sshRunning?String(this.sshPort):'—',30,TEXT);this.text('IPv4 与 IPv6 · root 公钥认证',13,SUBTEXT);Column.pop();Row.pop();
   this.cardStart();this.text('启用与停用',20,TEXT);this.text('停用只关闭 SSH 服务，不停止 Linux 兼容环境。生成客户端登录密钥后才可启用。',14,SUBTEXT);this.observeComponentCreation2(()=>{Row.create({space:12});Row.width('100%');},Row);this.primary('启用 SSH',()=>this.sshEnable(true),true,()=>!this.sshRunning&&this.sshClientKeyPresent);this.secondary('停用 SSH',()=>this.sshEnable(false),true,()=>this.sshRunning);Row.pop();Column.pop();
   this.cardStart();this.text('客户端登录密钥',20,TEXT);this.text(()=>this.sshClientKeyPresent?'已生成 ECDSA P-256 登录密钥。公钥已自动写入 authorized_keys，私钥可随时重复导出。':'尚未生成登录密钥。生成前 SSH 保持关闭，但不会影响 Linux 兼容服务和管理器。',14,()=>this.sshClientKeyPresent?'#2E7D32':SUBTEXT);this.text(()=>this.sshClientPublicKey||'',12,'#334155',()=>!!this.sshClientPublicKey);this.observeComponentCreation2(()=>{Row.create({space:12});Row.width('100%');},Row);this.primary(()=>this.sshClientKeyPresent?'重新生成登录密钥':'生成登录密钥',()=>this.sshGenerateClient());this.secondary('导出私钥',()=>this.sshExportClient(),true,()=>this.sshClientKeyPresent);Row.pop();Column.pop();
   this.cardStart();this.text('监听端口',20,TEXT);this.text('范围 1024–65535。切换前会启动候选监听并确认真实 SSH banner；验证失败不会关闭旧端口。',14,SUBTEXT);this.field(()=>this.sshPort,'例如 2222',v=>{this.sshPort=v.trim();this.sshPortDirty=true;});this.primary('验证并应用端口',()=>this.sshChangePort());Column.pop();
   this.cardStart();this.text('ECDSA 主机密钥',20,TEXT);this.text('主机密钥用于客户端确认设备身份，与 authorized_keys 中允许登录的客户端公钥相互独立。私钥不会显示或导出。',14,SUBTEXT);this.text(()=>this.sshPublicKey||'尚未读取到主机公钥',12,()=>this.sshPublicKey?'#334155':'#B3261E');this.secondary('重新生成主机公钥与私钥',()=>this.sshRegenerate());Column.pop();
   this.cardStart();this.text('最近操作',18,TEXT);this.text(()=>this.sshMessage||'尚未执行 SSH 管理操作',14,SUBTEXT);this.secondary('查看 SSH 日志',()=>this.showLog('/ssh/log'));Column.pop();Column.pop();
  }
 initialRender(){
  this.observeComponentCreation2(()=>{Row.create();Row.width('100%');Row.height('100%');Row.backgroundColor(BG);},Row);
  this.observeComponentCreation2(()=>{Column.create({space:8});Column.width(224);Column.height('100%');Column.padding({left:16,right:16,top:18,bottom:20});Column.backgroundColor(CARD);},Column);
  this.text('Linux 兼容平台',22,TEXT);this.text('L410',13,SUBTEXT);
  this.observeComponentCreation2(()=>{Divider.create();Divider.margin({top:12,bottom:12});Divider.color('#E5E8EC');},Divider);
   this.nav('概览','overview');this.nav('Linux 应用','apps');this.nav('软件包','packages');this.nav('Linux Shell','shell');this.nav('SSH 管理','ssh');this.nav('共享与资源','settings');this.nav('日志','logs');Column.pop();
  this.observeComponentCreation2(()=>{Scroll.create();Scroll.layoutWeight(1);Scroll.height('100%');},Scroll);
  this.observeComponentCreation2(()=>{Column.create({space:20});Column.width('100%');Column.justifyContent(FlexAlign.Start);Column.padding({left:32,right:32,top:18,bottom:32});},Column);
  this.observeComponentCreation2(()=>{XComponent.create({id:'managerClient',type:'node',libraryname:'l410uihost'},'__L410_BUNDLE__/entry');XComponent.id('managerClient');XComponent.width(1);XComponent.height(1);},XComponent);
   this.overview();this.applications();this.packages();this.shellPage();this.sshPage();this.settingsPage();this.logs();Column.pop();Scroll.pop();Row.pop();
 }
 rerender(){this.updateDirtyElements();}
}
ViewStackProcessor.StartGetAccessRecordingFor(ViewStackProcessor.AllocateNewElmetIdForNextComponent());
loadDocument(new ConsolePage(undefined,{}));
ViewStackProcessor.StopGetAccessRecording();
