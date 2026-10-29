// Pure event translation and ordered delivery for the NativeWindow surface.
function l410NativeKey(event) {
  if(event.type!==0)return undefined;
  const keys={2054:'Return',2055:'BackSpace',2014:'Left',2015:'Right',2012:'Up',2013:'Down',2049:'Tab',2070:'Escape',2043:'comma',2044:'period',2056:'grave',2057:'minus',2058:'equal',2059:'bracketleft',2060:'bracketright',2061:'backslash',2062:'semicolon',2063:'apostrophe',2064:'slash',2068:'Page_Up',2069:'Page_Down',2071:'Delete',2074:'Caps_Lock',2081:'Home',2082:'End',2083:'Insert',2050:'space'};
  let key=keys[event.keyCode];
  if(!key&&event.keyCode>=2017&&event.keyCode<=2042)key=String.fromCharCode(97+event.keyCode-2017);
  if(!key&&event.keyCode>=2000&&event.keyCode<=2009)key=String(event.keyCode-2000);
  if(event.keyCode>=2090&&event.keyCode<=2101)key='F'+(event.keyCode-2089);
  if(!key)return undefined;
  const modifiers=[];
  if(event.getModifierKeyState)for(const pair of [['Ctrl','ctrl'],['Alt','alt'],['Shift','shift']])if(event.getModifierKeyState([pair[0]]))modifiers.push(pair[1]);
  return {kind:'key',key:modifiers.concat([key]).join('+')};
}
function l410NativePoint(event,width,height,clampOutside=false,sourceWidth=1024,sourceHeight=720) {
  if(!Number.isFinite(width)||!Number.isFinite(height)||width<=0||height<=0||!Number.isFinite(event.x)||!Number.isFinite(event.y))return undefined;
  // Match OH_SCALING_MODE_SCALE_FIT_V2, including centered letterboxing.
  if(!Number.isFinite(sourceWidth)||!Number.isFinite(sourceHeight)||sourceWidth<=0||sourceHeight<=0)return undefined;
  const scale=Math.min(width/sourceWidth,height/sourceHeight);
  const x=(event.x-(width-sourceWidth*scale)/2)/scale,y=(event.y-(height-sourceHeight*scale)/2)/scale;
  if(!clampOutside&&(x<0||y<0||x>=sourceWidth||y>=sourceHeight))return undefined;
  return {x:Math.max(0,Math.min(sourceWidth-1,Math.floor(x))),y:Math.max(0,Math.min(sourceHeight-1,Math.floor(y)))};
}
class L410NativeInputQueue {
  constructor(send,onError){this.send=send;this.onError=onError;this.events=[];this.busy=false;this.closed=false;}
  push(event){
    if(this.closed)return;
    const last=this.events[this.events.length-1];
    if(event.kind==='pointer'&&event.phase==='move'&&last&&last.kind==='pointer'&&last.phase==='move')this.events[this.events.length-1]=event;
    else this.events.push(event);
    this.drain();
  }
  async drain(){
    if(this.busy||this.closed)return;this.busy=true;
    try{while(!this.closed&&this.events.length){const event=this.events.shift();try{await this.send(event);}catch(error){if(!this.closed)this.onError(error);}}}
    finally{this.busy=false;}
  }
  close(){this.closed=true;this.events=[];}
}
