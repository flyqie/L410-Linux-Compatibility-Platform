const UIAbility = requireNapi('app.ability.UIAbility');
const promptAction = requireNapi('promptAction');
const managerBridge = requireNapi('l410uihost', true, '__L410_BUNDLE__/entry') || {};

/* A desktop entry is only a one-shot request to the resident Linux runtime.
 * It never owns a Linux window or a status-bar item. Real windows are created
 * by the AnCo WMS/graphics lifecycle; Linux SNI items are owned separately. */
export default class EntryAbility extends UIAbility {
  onCreate() {
    this.running = false;
    this.finished = false;
    this.pendingError = '';
    this.activate();
  }

  onWindowStageCreate(stage) {
    this.stage = stage;
    if (this.pendingError) this.showError(this.pendingError);
    else if (this.finished) {
      /* terminateSelf() from onCreate can arrive before the WindowStage exists.
       * Repeat it at the first valid stage boundary so the one-shot launcher
       * never waits for the framework's empty-content timeout. */
      this.context.terminateSelf().catch(() => {});
    } else this.activate();
  }

  onNewWant() {
    this.finished = false;
    this.pendingError = '';
    this.activate();
  }

  desktopId() {
    const value = this.context.resourceManager.getStringByNameSync('desktop_id');
    if (!/^[A-Za-z0-9_.+-]{1,160}\.desktop$/.test(value))
      throw new Error('Linux desktop 入口身份无效');
    return value;
  }

  async api(path, body) {
    const fn = managerBridge.managerCall || globalThis.l410ManagerCall;
    if (typeof fn !== 'function') throw new Error('Linux 启动桥尚未就绪');
    const raw = await fn(path, JSON.stringify(body));
    const answer = JSON.parse(raw && raw.length ? raw : '{}');
    if (!answer.ok) throw new Error(answer.error || 'Linux 启动请求失败');
    return answer.data;
  }

  async showError(message) {
    this.pendingError = message;
    try {
      LocalStorage.getShared().setOrCreate('l410EntryError', message);
      if (this.stage) await this.stage.loadContent('pages/LinuxEntry');
    } catch (_) {}
    try { promptAction.showToast({ message: message, duration: 4000 }); } catch (_) {}
  }

  async activate() {
    if (this.running || this.finished) return;
    this.running = true;
    try {
      await this.api('/anco/launch', { id: this.desktopId() });
      this.finished = true;
      /* End before content is attached. The Linux window has an independent
       * AnCo task and remains alive after this launcher process exits. */
      try { await this.context.terminateSelf(); } catch (_) {}
    } catch (error) {
      await this.showError(String((error && error.message) || error || 'Linux 应用启动失败'));
    } finally {
      this.running = false;
    }
  }
}
