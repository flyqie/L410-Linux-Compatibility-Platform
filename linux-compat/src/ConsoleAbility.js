const UIAbility=requireNapi('app.ability.UIAbility');
const windowModule=requireNapi('window');
export default class ConsoleAbility extends UIAbility {
 onCreate(){globalThis.l410ConsoleContext=this.context;}
 normalizeDesktopMaximize(status){
  if(status!==windowModule.WindowStatusType.MAXIMIZE){this.desktopMaximizeConverted=false;return;}
  if(this.desktopMaximizeConverted||!this.mainWindow)return;
  const rect=this.mainWindow.getWindowProperties().windowRect;
  if(!rect||rect.top<=0)return;
  this.desktopMaximizeConverted=true;
  this.mainWindow.maximize(windowModule.MaximizePresentation.EXIT_IMMERSIVE).then(()=>{
   console.info('L410_CONSOLE_MAXIMIZE desktop non-immersive');
  }).catch(error=>{
   this.desktopMaximizeConverted=false;
   console.error('L410_CONSOLE_MAXIMIZE',error&&error.code?error.code:error);
  });
 }
 onWindowStageCreate(stage){
  this.mainWindow=stage.getMainWindowSync();
  this.desktopMaximizeConverted=false;
  this.windowStatusCallback=status=>this.normalizeDesktopMaximize(status);
  this.mainWindow.on('windowStatusDidChange',this.windowStatusCallback);
  stage.loadContent('pages/Linux',error=>{
   if(error&&error.code)console.error('L410_CONSOLE_LOAD',error.code);
   this.normalizeDesktopMaximize(this.mainWindow.getWindowStatus());
  });
 }
 onWindowStageDestroy(){
  if(this.mainWindow&&this.windowStatusCallback){
   try{this.mainWindow.off('windowStatusDidChange',this.windowStatusCallback);}catch(error){}
  }
  this.mainWindow=undefined;
  this.windowStatusCallback=undefined;
 }
}
