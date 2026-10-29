const filePicker=requireNapi('file.picker');
const fileFs=requireNapi('file.fs');
const prompt=requireNapi('promptAction');
const managerBridge=requireNapi('l410uihost',true,'__L410_BUNDLE__/entry')||{};
const BRAND='#0A59F7';
const TEXT='#182431';
const SUBTEXT='#5D6673';
const BG='#F1F3F5';

class HapInstallerPage extends ViewPU {
 constructor(parent,params,storage,id=-1){super(parent,storage,id);
  this.__status=new ObservedPropertySimplePU('请选择一个 HAP 安装包',this,'status');
  this.__pkg=new ObservedPropertyObjectPU(undefined,this,'pkg');
  this.__busy=new ObservedPropertySimplePU(false,this,'busy');
  this.__output=new ObservedPropertySimplePU('',this,'output');
  this.__polling=new ObservedPropertySimplePU(false,this,'polling');
  this.variableKeys=['status','pkg','busy','output','polling'];this.finalizeConstruction();
 }
 get status(){return this.__status.get();} set status(v){this.__status.set(v);}
 get pkg(){return this.__pkg.get();} set pkg(v){this.__pkg.set(v);}
 get busy(){return this.__busy.get();} set busy(v){this.__busy.set(v);}
 get output(){return this.__output.get();} set output(v){this.__output.set(v);}
 get polling(){return this.__polling.get();} set polling(v){this.__polling.set(v);}
 async api(path,body){const fn=managerBridge.managerCall||globalThis.l410ManagerCall;if(typeof fn!=='function')throw new Error('安装服务尚未就绪');const raw=await fn(path,body===undefined?undefined:JSON.stringify(body));const answer=JSON.parse(raw&&raw.length?raw:'{}');if(!answer.ok)throw new Error(answer.error||'安装服务请求失败');return answer.data;}
 async choose(){if(this.busy)return;this.busy=true;try{
  const picker=new filePicker.DocumentViewPicker(globalThis.l410HapContext);
  const selected=await picker.select({maxSelectNumber:1,selectMode:0,fileSuffixFilters:['HAP 安装包|.hap']});
  if(!selected||!selected.length){this.status='已取消选择';return;}
  const target=globalThis.l410HapContext.filesDir+'/incoming.hap';
  try{fileFs.unlinkSync(target);}catch(_){}
  this.status='正在复制安装包…';await fileFs.copyFile(selected[0],target);
  const stat=fileFs.statSync(target);if(stat.size>1073741824)throw new Error('HAP 大于 1 GiB');
  const result=await this.api('/hap/inspect',{});this.pkg=result.package;
  this.status='已读取安装包，等待确认';this.output='系统安装时会再次验证签名、API 版本和设备兼容性。';
 }catch(e){this.pkg=undefined;this.status='读取失败';this.output=e.message||String(e);prompt.showToast({message:this.output,duration:4000});}finally{this.busy=false;}}
 async install(){if(this.busy||!this.pkg)return;this.busy=true;try{const state=await this.api('/hap/install',{});this.status='正在由系统安装服务验证并安装…';this.output=state.output||'';}catch(e){this.busy=false;this.status='安装未启动';this.output=e.message||String(e);prompt.showToast({message:this.output,duration:4000});}}
 async poll(){if(!this.busy||this.polling)return;this.polling=true;try{const state=await this.api('/hap/status');if(state.state==='installing'){this.output=state.output||this.output;return;}this.busy=false;this.output=state.output||'';if(state.state==='installed'){this.status='安装完成';prompt.showToast({message:'HAP 安装成功',duration:3000});}else if(state.state==='error'){this.status='安装失败';prompt.showToast({message:state.error||'安装失败',duration:4000});}}catch(e){this.busy=false;this.status='无法读取安装状态';this.output=e.message||String(e);}finally{this.polling=false;}}
 aboutToAppear(){this.timer=setInterval(()=>this.poll(),1000);}
 aboutToDisappear(){clearInterval(this.timer);}
 aboutToBeDeleted(){clearInterval(this.timer);for(const key of this.variableKeys)this['__'+key].aboutToBeDeleted();SubscriberManager.Get().delete(this.id__());this.aboutToBeDeletedInternal();}
 purgeVariableDependenciesOnElmtId(id){for(const key of this.variableKeys)this['__'+key].purgeDependencyOnElmtId(id);}
 text(value,size=16,color=TEXT,visible=true){this.observeComponentCreation2(()=>{const current=typeof value==='function'?value():value;const currentColor=typeof color==='function'?color():color;const currentVisible=typeof visible==='function'?visible():visible;Text.create(current===undefined||current===null?'':String(current));Text.width('100%');Text.fontSize(size);Text.fontColor(currentColor);Text.textAlign(TextAlign.Start);Text.visibility(currentVisible?Visibility.Visible:Visibility.None);},Text);Text.pop();}
 button(label,action,primary=true,enabled=true){this.observeComponentCreation2(()=>{const current=typeof label==='function'?label():label;const currentEnabled=typeof enabled==='function'?enabled():enabled;Button.createWithLabel(current);Button.height(44);Button.enabled(!this.busy&&currentEnabled);Button.fontColor(primary?'#FFFFFF':BRAND);Button.backgroundColor(primary?BRAND:'#EAF1FF');Button.borderRadius(22);Button.onClick(action);},Button);Button.pop();}
 initialRender(){
  this.observeComponentCreation2(()=>{Scroll.create();Scroll.width('100%');Scroll.height('100%');Scroll.backgroundColor(BG);},Scroll);
   this.observeComponentCreation2(()=>{Column.create({space:18});Column.width('100%');Column.padding({left:32,right:32,top:18,bottom:32});},Column);
  this.observeComponentCreation2(()=>{XComponent.create({id:'hapManagerClient',type:'node',libraryname:'l410uihost'},'__L410_BUNDLE__/entry');XComponent.id('hapManagerClient');XComponent.width(1);XComponent.height(1);},XComponent);
  this.text('HAP 安装器',30,TEXT);this.text('选择本机 HAP 文件，查看应用信息后交给鸿蒙系统安装服务。',15,SUBTEXT);
  this.observeComponentCreation2(()=>{Column.create({space:14});Column.width('100%');Column.padding(24);Column.backgroundColor('#FFFFFF');Column.borderRadius(16);},Column);
  this.text(()=>this.status,20,()=>this.status==='安装完成'?'#2E7D32':TEXT);
  this.text(()=>this.pkg?this.pkg.bundle:'尚未选择文件',16,()=>this.pkg?TEXT:SUBTEXT);
  this.text(()=>this.pkg?'版本 '+this.pkg.versionName+'（'+this.pkg.versionCode+'）':'',14,SUBTEXT,()=>!!this.pkg);
  this.text(()=>this.pkg?'API '+this.pkg.minAPIVersion+' – '+this.pkg.targetAPIVersion:'',14,SUBTEXT,()=>!!this.pkg);
  this.text(()=>this.pkg?'设备 '+(this.pkg.deviceTypes||[]).join('、')+'　大小 '+Math.round(this.pkg.bytes/1048576)+' MiB':'',14,SUBTEXT,()=>!!this.pkg);
  this.text(()=>this.pkg?'入口 '+(this.pkg.abilities||[]).join('、'):'',14,SUBTEXT,()=>!!this.pkg);
  this.text(()=>this.pkg&&this.pkg.qtRuntime?(this.pkg.qtRuntime.detected?(this.pkg.qtRuntime.eligible?'Qt 兼容优化：将自动应用':'Qt 兼容优化：'+this.pkg.qtRuntime.reason):'原生运行库：无需 Qt 兼容优化'):'',14,()=>this.pkg&&this.pkg.qtRuntime&&this.pkg.qtRuntime.detected&&!this.pkg.qtRuntime.eligible?'#B45309':SUBTEXT,()=>!!this.pkg);
  this.observeComponentCreation2(()=>{Row.create({space:12});Row.width('100%');Row.margin({top:8});},Row);this.button('选择 HAP',()=>this.choose(),false);this.button('安装',()=>this.install(),true,()=>!!this.pkg);Row.pop();Column.pop();
  this.observeComponentCreation2(()=>{Column.create({space:10});Column.width('100%');Column.padding(20);Column.backgroundColor('#FFFFFF');Column.borderRadius(16);},Column);
   this.text('安装说明',18,TEXT);this.text('安装由鸿蒙 Bundle Manager 完成。兼容的 Qt HAP 会自动使用已验证的 API 20 运行库；未签名或已优化的安装包使用本机开发签名。系统 API 和设备类型仍由系统校验，更新应用时保留其数据。',14,SUBTEXT);
  this.observeComponentCreation2(()=>{Text.create(this.output||'安装输出会显示在这里。');Text.width('100%');Text.fontSize(13);Text.fontFamily('monospace');Text.fontColor(SUBTEXT);Text.copyOption(CopyOptions.Local);Text.padding(12);Text.backgroundColor('#F7F8FA');Text.borderRadius(8);},Text);Text.pop();Column.pop();Column.pop();Scroll.pop();
 }
 rerender(){this.updateDirtyElements();}
}
ViewStackProcessor.StartGetAccessRecordingFor(ViewStackProcessor.AllocateNewElmetIdForNextComponent());
loadDocument(new HapInstallerPage(undefined,{}));
ViewStackProcessor.StopGetAccessRecording();
