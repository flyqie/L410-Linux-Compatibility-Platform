const UIAbility=requireNapi('app.ability.UIAbility');
export default class HapInstallerAbility extends UIAbility {
 onCreate(){globalThis.l410HapContext=this.context;}
 onWindowStageCreate(stage){stage.loadContent('pages/HapInstallerPage',error=>{if(error&&error.code)console.error('L410_HAP_INSTALLER_LOAD',error.code);});}
}
