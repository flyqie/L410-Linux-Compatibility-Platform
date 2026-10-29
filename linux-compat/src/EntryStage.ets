import AbilityStage from '@ohos.app.ability.AbilityStage';
import l410uihost from 'libl410uihost.so';
import { getL410EntryModulesFactories } from './StatusBarFactories';

/* Supply the same lazy system-module factory map that Qt OHOS installs during
 * AbilityStage creation.  EntryAbility remains ordinary JS so the already
 * verified Linux launcher lifecycle does not get rewritten. */
export default class EntryStage extends AbilityStage {
  onCreate(): void {
    l410uihost.setupStatusBarFactories(getL410EntryModulesFactories());
  }
}
