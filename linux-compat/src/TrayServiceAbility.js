const ServiceExtensionAbility = requireNapi('app.ability.ServiceExtensionAbility');
const image = requireNapi('multimedia.image');
const hilog = requireNapi('hilog');
const managerBridge = requireNapi('l410uihost', true, '__L410_BUNDLE__/entry') || {};
const statusBarManager = managerBridge.loadStatusBarManager();

/* Invisible native owner for one Linux application's standard SNI item. */
export default class TrayServiceAbility extends ServiceExtensionAbility {
  onCreate() {
    this.destroyed = false;
    this.polling = false;
    this.trayAdded = false;
    this.trayItem = undefined;
    this.trayGeneration = -1;
    this.iconPixelMap = undefined;
    this.emptySince = 0;
    this.stopping = false;
    this.leftListener = event => {
      const type = event && event.data ? event.data.iconClickType : '';
      if (type === 'leftClick') this.trayAction('activate');
    };
    this.menuListener = event => {
      const code = event && event.data ? event.data.menuCode : '';
      const id = Number(code);
      if (Number.isInteger(id) && id >= 0) this.trayAction('menu', id);
    };
    this.timer = setInterval(() => this.pollTray(), 900);
    this.pollTray();
  }

  onRequest() { this.pollTray(); }

  targetBundle() {
    const value = this.context.resourceManager.getStringByNameSync('target_bundle');
    if (!/^org\.l410\.linux\.[a-z0-9_.]+$/.test(value))
      throw new Error('Linux 托盘应用身份无效');
    return value;
  }

  async api(path, body) {
    const fn = managerBridge.managerCall || globalThis.l410ManagerCall;
    if (typeof fn !== 'function') throw new Error('Linux 托盘桥尚未就绪');
    const raw = await fn(path, JSON.stringify(body));
    const answer = JSON.parse(raw && raw.length ? raw : '{}');
    if (!answer.ok) throw new Error(answer.error || 'Linux 托盘请求失败');
    return answer.data;
  }

  async loadIcon() {
    if (this.iconPixelMap) return this.iconPixelMap;
    const name = this.context.resourceManager.getStringByNameSync('tray_icon_file');
    const content = this.context.resourceManager.getRawFileContentSync(name);
    const source = image.createImageSource(content.buffer);
    this.iconPixelMap = await source.createPixelMap({ desiredSize: { width: 24, height: 24 } });
    return this.iconPixelMap;
  }

  menuGroups(menu) {
    const groups = []; let group = [];
    for (const item of menu || []) {
      if (!item || item.visible === false) continue;
      if (item.separator) { if (group.length) groups.push(group); group = []; continue; }
      if (!item.label) continue;
      if (item.children && item.children.length) {
        const children = item.children.filter(child => child && child.visible !== false &&
          !child.separator && child.label).map(child => ({
            subTitle: child.label,
            menuAction: { abilityName: 'invalidAbility', notifyOnly: true, menuCode: String(child.id) }
          }));
        if (children.length) group.push({ title: item.label, subMenu: children });
      } else {
        group.push({ title: item.label,
          menuAction: { abilityName: 'invalidAbility', notifyOnly: true, menuCode: String(item.id) } });
      }
    }
    if (group.length) groups.push(group);
    return groups;
  }

  async installTray(item) {
    const icon = await this.loadIcon();
    const menus = this.menuGroups(item.menu);
    const title = item.title || this.context.resourceManager.getStringByNameSync('app_name');
    if (!this.trayAdded) {
      statusBarManager.on('statusBarIconClick', this.leftListener);
      statusBarManager.on('rightMenuClick', this.menuListener);
      await statusBarManager.addToStatusBar(this.context, {
        icons: { white: icon, black: icon },
        quickOperation: { title: title, height: 300, abilityName: '' },
        statusBarGroupMenu: menus,
        hoverTips: item.tooltip || title
      });
      this.trayAdded = true;
    } else {
      await statusBarManager.updateStatusBarMenu(this.context, menus);
      if (typeof statusBarManager.updateStatusBarHoverTips === 'function')
        await statusBarManager.updateStatusBarHoverTips(this.context, item.tooltip || title || ' ');
    }
    this.trayItem = item;
  }

  async removeTray() {
    if (!this.trayAdded) return;
    try { await statusBarManager.removeFromStatusBar(this.context); } catch (_) {}
    try { statusBarManager.off('statusBarIconClick', this.leftListener); } catch (_) {}
    try { statusBarManager.off('rightMenuClick', this.menuListener); } catch (_) {}
    this.trayAdded = false;
    this.trayItem = undefined;
  }

  async trayAction(action, menuId) {
    const item = this.trayItem;
    if (!item) return;
    try {
      await this.api('/tray/action', {
        bundle: this.targetBundle(), key: item.key, action: action, menuId: menuId
      });
    } catch (error) {
      try { hilog.error(0x0000, 'L410Tray', 'action failed %{public}s',
        String((error && error.message) || error)); } catch (_) {}
    }
  }

  async pollTray() {
    if (this.destroyed || this.polling) return;
    this.polling = true;
    try {
      const state = await this.api('/tray/status', { bundle: this.targetBundle() });
      if (state.active && state.item) {
        this.emptySince = 0;
        if (!this.trayAdded || state.generation !== this.trayGeneration) {
          await this.installTray(state.item);
          this.trayGeneration = state.generation;
        }
      } else if (this.trayAdded) {
        await this.removeTray();
        this.emptySince = Date.now();
      } else if (!this.emptySince) {
        this.emptySince = Date.now();
      } else if (!this.stopping && Date.now() - this.emptySince >= 1800) {
        this.stopping = true;
        try {
          await this.context.stopServiceExtensionAbility({
            bundleName: '__L410_BUNDLE__', moduleName: 'entry',
            abilityName: 'TrayServiceAbility'
          });
        } catch (error) {
          this.stopping = false;
          try { hilog.error(0x0000, 'L410Tray', 'self stop failed %{public}s',
            String((error && error.message) || error)); } catch (_) {}
        }
      }
    } catch (error) {
      try { hilog.error(0x0000, 'L410Tray', 'poll failed %{public}s',
        String((error && error.message) || error)); } catch (_) {}
    } finally {
      this.polling = false;
    }
  }

  async onDestroy() {
    this.destroyed = true;
    if (this.timer) clearInterval(this.timer);
    this.timer = undefined;
    await this.removeTray();
  }
}
