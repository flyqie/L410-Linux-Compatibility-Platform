/* Keep this file structurally aligned with Qt OHOS'
 * OhosExportModules.ts.  HarmonyOS resolves this vendor kit only through the
 * ArkTS lazy-import frontend; requireNapi/napi_load_module are not equivalent. */
import lazy { statusBarManager as __kit__StatusBarExtensionKit__statusBarManager } from '@kit.StatusBarExtensionKit';

export function getL410EntryModulesFactories(): object {
  return {
    '@kit.StatusBarExtensionKit.statusBarManager': () => __kit__StatusBarExtensionKit__statusBarManager,
  };
}
