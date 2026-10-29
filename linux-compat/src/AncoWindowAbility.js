/* A multiton UIAbility: one guest top-level/popup window per instance. */
const AncoUiAbility = requireNapi('app.ability.UIAbility');

export default class AncoWindowAbility extends AncoUiAbility {
  onCreate(want) {
    this.params = want && want.parameters ? want.parameters : {};
    this.windowId = Number(this.params.ancoWindowId || 0);
    this.title = String(this.params.ancoName || 'Linux');
    this.taskId = Number(this.params.ancoTaskId || 0);
    this.displayId = Number(this.params.ancoDisplayId || 0);
    this.probeSurface = !!this.params.ancoProbeSurface;
    console.info('L410_ANCO_WINDOW_ABILITY_CREATE', this.windowId, this.taskId,
      this.title, this.probeSurface);
  }

  onWindowStageCreate(stage) {
    this.stage = stage;
    const storage = new LocalStorage({
      ancoWindowId: this.windowId,
      ancoProbeSurface: this.probeSurface
    });
    try {
      const win = stage.getMainWindowSync();
      if (win.setWindowBackgroundColor) win.setWindowBackgroundColor('#00000000');
    } catch (error) { }
    stage.loadContent('pages/AncoWindow', storage, (error) => {
      if (error) console.warn('L410_ANCO_WINDOW_LOAD_REPORTED', this.windowId,
        error.message || String(error));
      this.configureWindow();
    });
  }

  async configureWindow() {
    try {
      const win = this.stage.getMainWindowSync();
      if (win.setWindowTitle) {
        try { win.setWindowTitle(this.title); } catch (error) { }
      }
      if (win.setWindowFocusable) await win.setWindowFocusable(true);
      if (win.setWindowTouchable) await win.setWindowTouchable(true);
      /* This is the device's stock AnCo input classification bit. */
      if (win.setHandwritingFlag) await win.setHandwritingFlag(true);
      await win.showWindow({ focusOnShow: true });
      const props = win.getWindowProperties ? win.getWindowProperties() : {};
      this.osWindowId = Number(props.id || 0);
      console.info('L410_ANCO_WINDOW_READY', this.windowId, this.osWindowId,
        JSON.stringify(props.windowRect || {}));
      this.subscribeCommands(win);
    } catch (error) {
      console.error('L410_ANCO_WINDOW_CONFIGURE_FAILED', this.windowId,
        error.code || 0, error.message || String(error));
    }
  }

  subscribeCommands(win) {
    try {
      const events = requireNapi('commonEventManager');
      this.events = events;
      this.subscriber = events.createSubscriberSync({ events: ['l410_anco_window_command'] });
      events.subscribe(this.subscriber, (error, data) => {
        if (error) return;
        const p = data && data.parameters ? data.parameters : {};
        if (Number(p.ancoWindowId || 0) !== this.windowId) return;
        this.applyCommand(win, String(p.ancoCommand || ''), p);
      });
      console.info('L410_ANCO_WINDOW_SUBSCRIBED', this.windowId);
    } catch (error) {
      console.warn('L410_ANCO_WINDOW_SUBSCRIBE_FAILED', this.windowId,
        error.code || 0, error.message || String(error));
    }
  }

  async applyCommand(win, command, property) {
    try {
      if (command === 'show') await win.showWindow({ focusOnShow: false });
      else if (command === 'hide') await win.hide();
      else if (command === 'remove') await this.context.terminateSelf();
      else if (command === 'layout') {
        const width = Number(property.width || property.w || 0);
        const height = Number(property.height || property.h || 0);
        const x = Number(property.x !== undefined ? property.x : property.left);
        const y = Number(property.y !== undefined ? property.y : property.top);
        if (width > 0 && height > 0) await win.resize(width, height);
        if (Number.isFinite(x) && Number.isFinite(y)) await win.moveWindowTo(x, y);
      } else if (command === 'background' && win.setWindowBackgroundColor) {
        win.setWindowBackgroundColor(String(property.color || '#00000000'));
      }
      console.info('L410_ANCO_WINDOW_COMMAND_APPLIED', this.windowId, command);
    } catch (error) {
      console.warn('L410_ANCO_WINDOW_COMMAND_FAILED', this.windowId, command,
        error.code || 0, error.message || String(error));
    }
  }

  onDestroy() {
    try {
      if (this.events && this.subscriber) this.events.unsubscribe(this.subscriber);
    } catch (error) { }
    console.info('L410_ANCO_WINDOW_ABILITY_DESTROY', this.windowId);
  }
}
