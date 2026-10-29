/*
 * 兼容引擎窗口页。两种实例：
 *   - 宿主页（manager=true）：承载引擎通道模块（libl410engine.so）的 XComponent，
 *     模块注册的导出（start/setListener/send/status）从这里拿到，对应 anco 的
 *     anco_service_broker 那层；页面本身不显示任何 Linux 画面。
 *   - 窗口页（manager=false）：一个 ArkUI 窗口 = 容器里一个 Linux 顶层窗口，
 *     承载该窗口的图形表面（libl410displaymulti.so）+ 透明输入层。
 *
 * 模块通过 XComponent 的 libraryname 加载（本设备上已验证的方式）。
 */
const engineWindowDisplay = requireNapi('display');
/* Linux toolkit windows are custom editors from Harmony's point of view.  Use
 * the system input-method client API directly, exactly as a native custom edit
 * box does; a transparent TextInput is only kept as an API-level fallback. */
let engineWindowInputMethod = null;
try { engineWindowInputMethod = requireNapi('inputMethod'); }
catch (error) { engineWindowInputMethod = null; }

class EngineWindow extends ViewPU {
  constructor(parent, params, storage, id = -1) {
    super(parent, storage, id);
    const shared = LocalStorage.getShared();
    this.windowId = Number(shared.get('linuxWindowId') || 0);
    this.manager = !!shared.get('manager');
    /* 真实应用窗口模式：这个页面是独立窗口能力的主窗口，输入要经 UI 桥送回宿主能力。 */
    this.appWindow = !!shared.get('appWindow');
    this.uiEndpoint = String(shared.get('uiEndpoint') || '');
    this.__visible = new ObservedPropertySimplePU(false, this, 'visible');
    this.__status = new ObservedPropertySimplePU(this.manager ? '兼容引擎宿主' : '等待窗口', this, 'status');
    this.__imeX = new ObservedPropertySimplePU(0, this, 'imeX');
    this.__imeY = new ObservedPropertySimplePU(0, this, 'imeY');
    this.__imeW = new ObservedPropertySimplePU(1, this, 'imeW');
    this.__imeH = new ObservedPropertySimplePU(2, this, 'imeH');
    this.__imeText = new ObservedPropertySimplePU('', this, 'imeText');
    /* 输入法是否正在占用焦点：为真时输入覆盖层不可聚焦，焦点归那个看不见的输入框。 */
    this.__imeActive = new ObservedPropertySimplePU(false, this, 'imeActive');
    this.__imeCustomActive = new ObservedPropertySimplePU(false, this, 'imeCustomActive');
    this.width = 0;
    this.height = 0;
    this.endpoint = '';
    this.bound = false;
    this.imePreviewSubscribed = false;
    this.finalizeConstruction();
  }
  get visible() { return this.__visible.get(); }
  set visible(v) { this.__visible.set(v); }
  get status() { return this.__status.get(); }
  set status(v) { this.__status.set(v); }
  get imeX() { return this.__imeX.get(); }
  set imeX(v) { this.__imeX.set(v); }
  get imeY() { return this.__imeY.get(); }
  set imeY(v) { this.__imeY.set(v); }
  get imeW() { return this.__imeW.get(); }
  set imeW(v) { this.__imeW.set(v); }
  get imeH() { return this.__imeH.get(); }
  set imeH(v) { this.__imeH.set(v); }
  get imeText() { return this.__imeText.get(); }
  get imeActive() { return this.__imeActive.get(); }
  set imeActive(v) { this.__imeActive.set(v); }
  set imeText(v) { this.__imeText.set(v); }
  get imeCustomActive() { return this.__imeCustomActive.get(); }
  set imeCustomActive(v) { this.__imeCustomActive.set(v); }
  aboutToAppear() { }
  aboutToBeDeleted() {
    this.disposed = true;
    clearTimeout(this.readyTimer);
    clearTimeout(this.resizeStatusTimer);
    clearInterval(this.statusTimer);
    if (!this.manager) {
      try { if (globalThis.l410EnginePages) delete globalThis.l410EnginePages[this.windowId]; } catch (error) { }
    }
    if (!this.manager && globalThis.l410EngineAbility) {
      try { globalThis.l410EngineAbility.windowGone(this.windowId, this.endpoint); } catch (error) { }
    }
    this.__visible.aboutToBeDeleted();
    this.__status.aboutToBeDeleted();
    this.__imeX.aboutToBeDeleted();
    this.__imeY.aboutToBeDeleted();
    this.__imeW.aboutToBeDeleted();
    this.__imeH.aboutToBeDeleted();
    this.__imeText.aboutToBeDeleted();
    this.__imeActive.aboutToBeDeleted();
    this.__imeCustomActive.aboutToBeDeleted();
    SubscriberManager.Get().delete(this.id__());
    this.aboutToBeDeletedInternal();
  }
  purgeVariableDependenciesOnElmtId(id) {
    this.__visible.purgeDependencyOnElmtId(id);
    this.__status.purgeDependencyOnElmtId(id);
    this.__imeX.purgeDependencyOnElmtId(id);
    this.__imeY.purgeDependencyOnElmtId(id);
    this.__imeW.purgeDependencyOnElmtId(id);
    this.__imeH.purgeDependencyOnElmtId(id);
    this.__imeText.purgeDependencyOnElmtId(id);
    this.__imeActive.purgeDependencyOnElmtId(id);
    this.__imeCustomActive.purgeDependencyOnElmtId(id);
  }

  /* 输入法：宿主把系统输入法挂在窗口页里一个看不见的 TextInput 上，
     应用上报的光标位置决定它停在哪 —— 候选栏就跟着应用里的光标走。 */
  setCaret(x, y, w, h) {
    if (this.manager || this.disposed) return;
    try {
      const ui = this.getUIContext();
      this.imeX = Math.max(0, Math.round(ui.px2vp(Number(x) || 0)));
      this.imeY = Math.max(0, Math.round(ui.px2vp(Number(y) || 0)));
      /* 不能是退化尺寸：焦点系统会跳过几乎不可见的组件 */
      this.imeW = Math.max(8, Math.round(ui.px2vp(Number(w) || 8)));
      this.imeH = Math.max(8, Math.round(ui.px2vp(Number(h) || 8)));
    } catch (error) { }
    this.updateImeCursor();
  }

  imeCursorInfo() {
    const ability = globalThis.l410WindowAbility;
    const rect = ability && ability.windowRectFor ? ability.windowRectFor(this.windowId)
      : ability && ability.windowRect ? ability.windowRect : {};
    const baseX = Number(rect.left !== undefined ? rect.left : rect.x) || 0;
    const baseY = Number(rect.top !== undefined ? rect.top : rect.y) || 0;
    const ui = this.getUIContext();
    /* Qt/GTK caret coordinates are relative to the client Surface.  A real
       decorated Harmony window reports its outer rectangle, including the
       title bar.  Use the measured outer/client difference so the IME cursor
       and candidate panel follow the application caret.  Popup windows have
       no decoration and therefore get a zero offset. */
    const clientHeight = Math.round(ui.vp2px(this.height || 0));
    const clientOffsetY = this.appWindow && clientHeight > 0
      ? Math.max(0, Number(rect.height || 0) - clientHeight) : 0;
    return {
      left: Math.max(0, Math.round(baseX + ui.vp2px(this.imeX))),
      top: Math.max(0, Math.round(baseY + clientOffsetY + ui.vp2px(this.imeY))),
      width: Math.max(1, Math.round(ui.vp2px(this.imeW))),
      height: Math.max(1, Math.round(ui.vp2px(this.imeH)))
    };
  }

  sendImeKey(code) {
    if (this.appWindow) {
      this.bridgeSend('KEY window=' + this.windowId + ' action=down code=' + code);
      this.bridgeSend('KEY window=' + this.windowId + ' action=up code=' + code);
    } else if (globalThis.l410EngineAbility) {
      const ability = globalThis.l410EngineAbility;
      ability.sendToWindow(this.windowId, 'INPUT_KEY id=' + ability.clientIdOf(this.windowId) +
        ' action=down code=' + code + ' mods=0');
      ability.sendToWindow(this.windowId, 'INPUT_KEY id=' + ability.clientIdOf(this.windowId) +
        ' action=up code=' + code + ' mods=0');
    }
  }

  setupImeController() {
    if (this.imeController || !engineWindowInputMethod) return this.imeController;
    try {
      const controller = engineWindowInputMethod.getController();
      this.imeController = controller;
      console.info('L410_ENGINE_IME_CONTROLLER', this.windowId, 1);
      return controller;
    } catch (error) {
      console.warn('L410_ENGINE_IME_CONTROLLER_FAILED', this.windowId, error.code || 0, error.message);
      return null;
    }
  }

  subscribeImeController() {
    const controller = this.imeController;
    if (!controller || this.imeSubscribed) return;
    try {
      const listen = (name, callback) => {
        try { controller.on(name, callback); }
        catch (error) { console.warn('L410_ENGINE_IME_LISTENER_FAILED', this.windowId, name, error.code || 0, error.message); }
      };
      listen('insertText', text => {
        console.info('L410_ENGINE_IME_INSERT', this.windowId, String(text || '').length);
        this.imePreview('');
        this.imeChanged(String(text || ''));
        /* Qt reports the resulting surrounding text through IME_STATE.  Do not
           replace it with an invented empty document between commit and that
           authoritative update. */
      });
      listen('deleteLeft', length => {
        const count = Math.max(1, Math.min(Number(length) || 1, 64));
        for (let i = 0; i < count; i++) this.sendImeKey(14); /* KEY_BACKSPACE */
      });
      listen('deleteRight', length => {
        const count = Math.max(1, Math.min(Number(length) || 1, 64));
        for (let i = 0; i < count; i++) this.sendImeKey(111); /* KEY_DELETE */
      });
      listen('sendFunctionKey', key => this.sendImeKey(28)); /* enter/newline */
      listen('moveCursor', direction => {
        /* Direction enum: up/down/left/right.  Linux evdev cursor keys. */
        const keys = { 1: 103, 2: 108, 3: 105, 4: 106 };
        const value = direction && direction.direction !== undefined ? direction.direction : direction;
        const code = keys[Number(value) || 0];
        if (code) this.sendImeKey(code);
      });
      listen('sendKeyboardStatus', status =>
        console.info('L410_ENGINE_IME_STATUS', this.windowId, status));
      /* Custom editors are queried synchronously while attach() is being
         negotiated.  Register these before attach and always return a valid
         snapshot; omitting them made some Chinese IMEs reject composition and
         fall back to raw Latin key events.  The Linux-side surrounding-text
         mirror can be filled in later without changing this contract. */
      listen('getLeftTextOfCursor', length => {
        const text = String(this.imeSurroundingText || '');
        const cursor = Math.max(0, Math.min(Number(this.imeCursorIndex) || 0, text.length));
        return text.slice(Math.max(0, cursor - Math.max(0, Number(length) || 0)), cursor);
      });
      listen('getRightTextOfCursor', length => {
        const text = String(this.imeSurroundingText || '');
        const cursor = Math.max(0, Math.min(Number(this.imeCursorIndex) || 0, text.length));
        return text.slice(cursor, cursor + Math.max(0, Number(length) || 0));
      });
      listen('getTextIndexAtCursor', () => Math.max(0, Number(this.imeCursorIndex) || 0));
      this.imeSubscribed = true;
      console.info('L410_ENGINE_IME_SUBSCRIBED', this.windowId);
    } catch (error) {
      console.warn('L410_ENGINE_IME_SUBSCRIBE_FAILED', this.windowId, error.code || 0, error.message);
    }
  }

  /* Harmony IME uses preview text for composition (notably pinyin).  The SDK
     requires these two listeners to exist before attach(), otherwise the input
     method cannot negotiate a composing-text client. */
  subscribeImePreview() {
    const controller = this.imeController;
    if (!controller || this.imePreviewSubscribed) return;
    try {
      controller.on('setPreviewText', (value, range) => {
        const text = String(value || '');
        console.info('L410_ENGINE_IME_PREVIEW', this.windowId, text.length,
          range ? JSON.stringify(range) : '');
        this.imePreview(text);
      });
      controller.on('finishTextPreview', () => this.imePreview(''));
      this.imePreviewSubscribed = true;
      console.info('L410_ENGINE_IME_PREVIEW_SUBSCRIBED', this.windowId);
    } catch (error) {
      console.warn('L410_ENGINE_IME_PREVIEW_SUBSCRIBE_FAILED', this.windowId,
        error.code || 0, error.message);
    }
  }

  imePreview(value) {
    if (this.manager || this.disposed) return;
    const flat = String(value === undefined ? '' : value).split('\n').join('\u001f');
    if (this.appWindow) {
      this.bridgeSend('PREEDIT window=' + this.windowId + ' text=' + flat);
    } else if (globalThis.l410EngineAbility) {
      globalThis.l410EngineAbility.imePreedit(this.windowId, flat);
    }
  }

  updateImeCursor() {
    if (!this.imeAttached || !this.imeController || !this.imeController.updateCursor) return;
    try {
      const result = this.imeController.updateCursor(this.imeCursorInfo());
      if (result && result.catch) result.catch(error =>
        console.warn('L410_ENGINE_IME_CURSOR_FAILED', this.windowId, error.code || 0, error.message));
    } catch (error) {
      console.warn('L410_ENGINE_IME_CURSOR_FAILED', this.windowId, error.code || 0, error.message);
    }
  }

  updateImeState(text, cursor, anchor) {
    if (this.manager || this.disposed) return;
    const surrounding = String(text === undefined ? '' : text);
    const end = surrounding.length;
    this.imeSurroundingText = surrounding;
    this.imeCursorIndex = Math.max(0, Math.min(Number(cursor) || 0, end));
    this.imeAnchorIndex = Math.max(0, Math.min(Number(anchor) || 0, end));
    if (!this.imeAttached || !this.imeController || !this.imeController.changeSelection) return;
    try {
      const result = this.imeController.changeSelection(surrounding,
        this.imeCursorIndex, this.imeAnchorIndex);
      if (result && result.catch) result.catch(error =>
        console.warn('L410_ENGINE_IME_SELECTION_FAILED', this.windowId,
          error.code || 0, error.message));
    } catch (error) {
      console.warn('L410_ENGINE_IME_SELECTION_FAILED', this.windowId,
        error.code || 0, error.message);
    }
  }

  async attachImeController() {
    if (this.imeAttached) return true;
    if (this.imeAttaching) return false;
    const controller = this.setupImeController();
    if (!controller) return false;
    this.imeAttaching = true;
    const ability = globalThis.l410WindowAbility;
    const windowId = Number(ability && ability.osWindowId) || 0;
    /* Bind a complete custom editor before showing the input method. */
    const cursor = this.imeCursorInfo();
    const config = {
      inputAttribute: { textInputType: 1, enterKeyType: 8 }, /* MULTILINE, NEWLINE */
      cursorInfo: cursor,
      selection: { start: Math.max(0, Number(this.imeCursorIndex) || 0),
        end: Math.max(0, Number(this.imeAnchorIndex) || 0) },
      windowId: windowId || undefined,
      newEditBox: true
    };
    try {
      /* attach(true) could display the keyboard before listeners, window id,
         cursor and selection existed.  Physical pinyin keys then bypassed the
         custom editor and only their raw Latin events reached Linux. */
      this.subscribeImePreview();
      await controller.attach(false, config);
      this.imeAttached = true;
      this.imeCustomActive = true;
      /* API 17 permits preview listeners before attach, but the remaining
         custom-editor listeners return 12800009 if registered while detached.
         Install the full editor contract immediately after attach, before the
         keyboard is shown. */
      this.subscribeImeController();
      if (windowId && controller.setCallingWindow) await controller.setCallingWindow(windowId);
      if (controller.updateCursor) await controller.updateCursor(cursor);
      if (controller.changeSelection) await controller.changeSelection(
        String(this.imeSurroundingText || ''),
        Math.max(0, Number(this.imeCursorIndex) || 0),
        Math.max(0, Number(this.imeAnchorIndex) || 0));
      if (controller.showTextInput) await controller.showTextInput();
      console.info('L410_ENGINE_IME_ATTACHED', this.windowId, windowId,
        JSON.stringify(this.imeCursorInfo()));
      this.imeAttaching = false;
      return true;
    } catch (error) {
      console.warn('L410_ENGINE_IME_ATTACH_FAILED', this.windowId, error.code || 0, error.message);
      /* Some releases auto-attach ArkUI text clients.  Let showTextInput use
         that binding before falling back to the transparent control path. */
      try {
        await controller.showTextInput();
        this.imeAttached = true;
        this.imeCustomActive = true;
        this.subscribeImeController();
        console.info('L410_ENGINE_IME_SHOWN_EXISTING', this.windowId, windowId);
        this.imeAttaching = false;
        return true;
      } catch (second) {
        console.warn('L410_ENGINE_IME_SHOW_FAILED', this.windowId, second.code || 0, second.message);
      }
    }
    this.imeAttaching = false;
    return false;
  }

  imeShow() {
    if (this.manager || this.disposed) return;
    this.imeActive = true;
    if (this.imeAttached) {
      this.imeCustomActive = true;
      try {
        const shown = this.imeController && this.imeController.showTextInput
          ? this.imeController.showTextInput() : null;
        if (shown && shown.catch) shown.catch(() => { });
      } catch (error) { }
      return;
    }
    if (this.imeAttaching) return;
    /* A Harmony custom editor and an ArkUI TextInput must never own the same
       IME session at once.  Attach the window-level custom editor first.  The
       transparent TextInput is enabled only if that API really fails. */
    this.attachImeController().then(attached => {
      if (attached || this.disposed) return;
      this.activateImeFallback();
    }).catch(() => this.activateImeFallback());
  }

  activateImeFallback() {
    if (this.manager || this.disposed || this.imeAttached) return;
    this.imeActive = true;
    const id = 'l410-engine-ime-' + this.windowId;
    const attempt = (left) => {
      if (this.disposed || !this.imeActive) return;
      /* 老 API 试过：requestFocus 从来没生效（日志里只有 IME_SHOW 没有 IME_FOCUS）。
         现在走 UIContext 的 focusController，并把结果打出来。 */
      let requested = false;
      try {
        const controller = this.getUIContext().getFocusController();
        if (controller && controller.requestFocus) {
          const result = controller.requestFocus(id);
          requested = true;
          if (result && result.then) {
            result.then(ok => console.info('L410_ENGINE_IME_REQUEST', this.windowId, '->', ok))
                  .catch(error => console.warn('L410_ENGINE_IME_REQUEST_FAILED', this.windowId, error.message));
          } else {
            console.info('L410_ENGINE_IME_REQUEST', this.windowId, '->', result);
          }
        }
      } catch (error) {
        console.warn('L410_ENGINE_IME_REQUEST_ERROR', this.windowId, error.message);
      }
      /* This device's UIContext controller returns void and has occasionally
         accepted the request only through the global focusControl path.  The
         calls are idempotent, so issue both after the reactive focusable flag
         has been applied. */
      try { focusControl.requestFocus(id); } catch (error) { }
      if (left > 0 && !this.imeAttached) setTimeout(() => attempt(left - 1), 150);
    };
    setTimeout(() => attempt(3), 40);
    console.info('L410_ENGINE_IME_FALLBACK_ARMED', this.windowId);
  }

  imeHide() {
    if (this.manager || this.disposed) return;
    this.imeActive = false;
    this.imeCustomActive = false;
    try {
      const result = this.imeController && this.imeController.hideTextInput
        ? this.imeController.hideTextInput() : null;
      if (result && result.catch) result.catch(() => { });
    } catch (error) { }
    /* A bound custom editor already owns the native window's IME lifecycle,
       and hardware keys still arrive through the window-level input filter.
       Forcing the ArkUI overlay to focus here re-activates the Linux window,
       which makes Qt request IME again after every delayed hide and creates a
       hide -> overlay focus -> show/reset loop.  Only the TextInput fallback
       needs an ArkUI focus target. */
    if (!this.imeAttached) {
      try { focusControl.requestFocus('l410-engine-input-' + this.windowId); } catch (error) { }
    }
  }

  imeReset() {
    this.imeText = '';
    this.imePreview('');
  }

  /* 系统输入法（含中文候选）打字进来 → 直接交给容器里的应用。 */
  imeChanged(value, preview) {
    if (this.manager || this.disposed) return;
    /* Preview text is IME composition (for example the pinyin currently under
       conversion), not committed application text. */
    if (preview && String(preview.value || preview.text || '').length) return;
    const text = String(value === undefined ? '' : value);
    if (!text) return;
    this.imeText = '';
    const flat = text.split('\n').join('\u001f');
    if (this.appWindow) {
      this.bridgeSend('TEXT window=' + this.windowId + ' text=' + flat);
    } else if (globalThis.l410EngineAbility) {
      globalThis.l410EngineAbility.imeCommit(this.windowId, flat);
    }
  }

  imeSubmit() {
    if (this.manager || this.disposed || !globalThis.l410EngineAbility) return;
    globalThis.l410EngineAbility.imeEnter(this.windowId);
  }

  density() {
    try { return Number(engineWindowDisplay.getDefaultDisplaySync().densityPixels) || 1; }
    catch (error) { return 1; }
  }

  /* 窗口能力页：把原生模块当 UI 桥客户端用，输入行经它送回持有引擎通道的宿主能力。 */
  onBridgeModule(context) {
    this.bridge = context;
    /* 窗口能力页是独立 VM：这里也要把原生模块挂到 globalThis，
       能力侧注册窗口级输入过滤器（windowFilters）要用它。 */
    globalThis.l410EngineApi = context;
    try {
      const ability = globalThis.l410WindowAbility;
      if (ability) ability.bridgePage = this;
    } catch (error) { }
    let connected = false;
    try { connected = !!(this.uiEndpoint && context.uiConnect && context.uiConnect(this.uiEndpoint)); }
    catch (error) { console.error('L410_ENGINE_APPWIN_BRIDGE_FAILED', this.windowId, error.message); }
    console.info('L410_ENGINE_APPWIN_BRIDGE', this.windowId, connected ? 1 : 0);
    /* One bounded handshake proves that this independent ability VM writes to
       the host-facing end of the socket.  It catches descriptor-direction and
       second-window regressions before the user has to touch the mouse. */
    if (connected && context.uiSend) {
      try { context.uiSend('UITEST appwin window=' + this.windowId); } catch (error) { }
      try {
        const ability = globalThis.l410WindowAbility;
        const mask = Number(ability && ability.nativeInputMask) || 0;
        if (mask) context.uiSend('FILTERS window=' + this.windowId + ' mask=' + mask);
      } catch (error) { }
    }
  }

  bridgeSend(line) {
    try {
      if (this.bridge && this.bridge.uiSend) return this.bridge.uiSend(line);
    } catch (error) { }
    return false;
  }

  pointer(event, phase, button) {
    if (this.appWindow) {
      const ui = this.getUIContext();
      const x = Math.round(ui.vp2px(Number(event.x)));
      const y = Math.round(ui.vp2px(Number(event.y)));
      if (!Number.isFinite(x) || !Number.isFinite(y)) return;
      if (phase === 'down' && !this.imeActive) {
        try { focusControl.requestFocus('l410-engine-input-' + this.windowId); } catch (error) { }
      }
      this.bridgeSend('POINTER window=' + this.windowId + ' action=' + phase +
        ' x=' + x + ' y=' + y + ' button=' + (button || 0));
      return;
    }
    this.pointerToHost(event, phase, button);
  }

  wheel(event) {
    if (this.appWindow) {
      const ui = this.getUIContext();
      const x = Math.round(ui.vp2px(Number(event.x)));
      const y = Math.round(ui.vp2px(Number(event.y)));
      const dy = Number(event.getVerticalAxisValue());
      if (!dy) return;
      this.bridgeSend('WHEEL window=' + this.windowId + ' x=' + x + ' y=' + y + ' delta=' + Math.round(dy * 120));
      return;
    }
    this.wheelToHost(event);
  }

  key(event) {
    if (this.appWindow) {
      const action = event.type === KeyType.Down ? 'down' : 'up';
      const text = String(event.keyText === undefined ? '' : event.keyText);
      return this.bridgeSend('KEY window=' + this.windowId + ' action=' + action +
        ' code=' + event.keyCode + (text ? ' text=' + text : ''));
    }
    return this.keyToHost(event);
  }

  /* 宿主页：拿到引擎通道模块的导出 */
  onEngineModule(context) {
    globalThis.l410EngineApi = context;
    if (globalThis.l410EngineAbility) globalThis.l410EngineAbility.onEngineApi(context);
  }

  /* 窗口页：把该窗口的表面端点报给宿主 broker */
  async connect(context) {
    try {
      this.native = context;
      let state = {};
      for (let attempt = 0; attempt < 120 && !this.disposed; attempt++) {
        state = JSON.parse(context.status());
        if (state.stage === 2 && this.width > 0 && this.height > 0) break;
        await new Promise(resolve => setTimeout(resolve, 50));
      }
      if (this.disposed) return;
      if (state.stage !== 2 || !(this.width > 0 && this.height > 0)) {
        console.error('L410_ENGINE_WINDOW_FAILED', this.windowId, state.stage || 0, state.error || 0);
        return;
      }
      const ui = this.getUIContext();
      const width = Math.round(ui.vp2px(this.width));
      const height = Math.round(ui.vp2px(this.height));
      if (width < 16 || height < 16 || width > 3840 || height > 2160) {
        console.error('L410_ENGINE_WINDOW_BAD_AREA', this.windowId, width, height);
        return;
      }
      console.info('L410_ENGINE_WINDOW_QUEUE', this.windowId, JSON.stringify(state));
      /* The former unconditional 2-second status dump woke every app window
         forever and flooded hilog.  Keep the one queue snapshot above; live
         counters are queried explicitly by diagnostics when needed. */
      this.endpoint = state.endpoint;
      /* Publish the XComponent's real RenderService-backed Surface using the
         actual Harmony window id.  The Linux client still connects to this
         surface's own buffer queue endpoint; no intermediate framebuffer is
         created. */
      if (this.appWindow) {
        const ability = globalThis.l410WindowAbility;
        const osWindowId = ability && ability.osWindowIdFor
          ? ability.osWindowIdFor(this.windowId) : ability && ability.osWindowId;
        if (osWindowId && context.publish) {
          const result = context.publish(Number(osWindowId));
          console.info('L410_ENGINE_XCOMPONENT_PUBLISHED', this.windowId,
            osWindowId, result);
        }
      }
      this.bound = true;
      this.status = '';
      if (!globalThis.l410EnginePages) globalThis.l410EnginePages = {};
      globalThis.l410EnginePages[this.windowId] = this;
      if (this.appWindow) {
        const ability = globalThis.l410WindowAbility;
        if (ability) {
          if (ability.pageReady) {
            ability.pageReady(this.windowId, this.endpoint, width, height, this.density());
          } else {
            ability.surfaceEndpoint = this.endpoint;
            ability.surfaceWidth = width;
            ability.surfaceHeight = height;
            ability.surfaceDensity = this.density();
            ability.notifyHost('ready', { width: width, height: height });
          }
        } else {
          console.error('L410_ENGINE_APPWIN_NO_ABILITY', this.windowId);
        }
      } else if (globalThis.l410EngineAbility) {
        globalThis.l410EngineAbility.windowReady(this.windowId, this.endpoint, width, height, this.density());
      }
      /* Queue readiness precedes the first Linux buffer.  Keep this Surface
         transparent until one complete frame has actually been flushed. */
      let firstFrame = false;
      for (let attempt = 0; attempt < 80 && !this.disposed; attempt++) {
        try {
          const live = JSON.parse(context.status());
          if (Number(live.frames || 0) > 0) { firstFrame = true; break; }
        } catch (error) { }
        await new Promise(resolve => setTimeout(resolve, 25));
      }
      if (!this.disposed) {
        this.visible = true;
        console.info('L410_ENGINE_FIRST_FRAME_REVEAL', this.windowId, firstFrame ? 1 : 0);
      }
      console.info('L410_ENGINE_WINDOW_READY', this.windowId, this.endpoint, width, height);
    } catch (error) {
      console.error('L410_ENGINE_WINDOW_CONNECT_FAILED', this.windowId, error.message);
    }
  }

  pointerToHost(event, phase, button) {
    if (this.manager || !globalThis.l410EngineAbility) return;
    const ui = this.getUIContext();
    const x = Math.round(ui.vp2px(Number(event.x)));
    const y = Math.round(ui.vp2px(Number(event.y)));
    if (!Number.isFinite(x) || !Number.isFinite(y)) return;
    if (phase === 'down') {
      if (!this.imeActive) { try { focusControl.requestFocus('l410-engine-input-' + this.windowId); } catch (error) { } }
      this.pressed = true;
    }
    if (phase === 'up') this.pressed = false;
    globalThis.l410EngineAbility.pointer(this.windowId, phase, x, y, button);
  }

  wheelToHost(event) {
    if (this.manager || !globalThis.l410EngineAbility) return;
    const ui = this.getUIContext();
    const x = Math.round(ui.vp2px(Number(event.x)));
    const y = Math.round(ui.vp2px(Number(event.y)));
    const dx = Number(event.getHorizontalAxisValue());
    const dy = Number(event.getVerticalAxisValue());
    if (!dx && !dy) return;
    globalThis.l410EngineAbility.wheel(this.windowId, x, y, Math.round(dy * 120), Math.abs(dx) > Math.abs(dy) ? 1 : 0);
  }

  keyToHost(event) {
    if (this.manager || !globalThis.l410EngineAbility) return false;
    return globalThis.l410EngineAbility.key(this.windowId, event);
  }

  initialRender() {
    this.observeComponentCreation2(() => {
      Stack.create();
      Stack.width('100%');
      Stack.height('100%');
      Stack.backgroundColor('#00000000');
      Stack.onAreaChange((_, area) => {
        this.width = Number(area.width);
        this.height = Number(area.height);
        /* 客户区尺寸就是这块表面的尺寸：窗口被拖大拖小时把它如实报给容器。 */
        if (!this.manager && this.bound && this.appWindow) {
           const ability = globalThis.l410WindowAbility;
           if (ability) {
             try {
               const ui = this.getUIContext();
               const w = Math.round(ui.vp2px(this.width));
               const h = Math.round(ui.vp2px(this.height));
               if (ability.queueGeometryFor) ability.queueGeometryFor(this.windowId, w, h);
               else ability.queueGeometry(w, h);
             } catch (error) { }
          }
        } else if (!this.manager && this.bound && globalThis.l410EngineAbility) {
          try {
            const ui = this.getUIContext();
            globalThis.l410EngineAbility.windowGeometry(this.windowId,
              Math.round(ui.vp2px(this.width)), Math.round(ui.vp2px(this.height)), this.density());
          } catch (error) { }
        }
        /* One quiet-period snapshot makes resize failures diagnosable without
           polling or waking every window continuously. */
        if (!this.manager && this.bound && this.native && this.native.status) {
          clearTimeout(this.resizeStatusTimer);
          this.resizeStatusTimer = setTimeout(() => {
            if (this.disposed || !this.native) return;
            try {
              console.info('L410_ENGINE_RESIZE_STATUS', this.windowId, this.native.status());
            } catch (error) { }
          }, 300);
        }
      });
    }, Stack);
    if (this.appWindow) {
      /* 窗口能力页：用同一个原生模块当 UI 桥客户端（1×1，不可见） */
      this.observeComponentCreation2(() => {
        XComponent.create({ id: 'l410-appwin-bridge-' + this.windowId, type: 'surface',
                            libraryname: 'l410engine' }, '__L410_BUNDLE__/entry');
        XComponent.width(1);
        XComponent.height(1);
        XComponent.opacity(0);
        XComponent.onLoad(context => this.onBridgeModule(context));
      }, XComponent);
    }
    if (this.manager) {
      /* 宿主页：只用来加载引擎通道模块（1×1，不可见） */
      this.observeComponentCreation2(() => {
        XComponent.create({ id: 'l410-engine-channel', type: 'surface', libraryname: 'l410engine' }, '__L410_BUNDLE__/entry');
        XComponent.width(1);
        XComponent.height(1);
        XComponent.opacity(0);
        XComponent.onLoad(context => this.onEngineModule(context));
      }, XComponent);
      /* The broker page exists only because this platform build resolves the
         native module through XComponent.  It has no user-facing controls. */
      if (false) {
      /* 键盘事件先落在哪就记在哪：子窗口可能拿不到"激活"，所以要看看是不是落到宿主自己这层。 */
      this.observeComponentCreation2(() => {
        Button.createWithLabel('', { type: ButtonType.Normal, stateEffect: false });
        Button.width('100%');
        Button.height('100%');
        Button.backgroundColor('#00000000');
        Button.borderRadius(0);
        Button.focusable(true);
        Button.id('l410-engine-input-manager');
        Button.onKeyEvent(event => {
          console.info('L410_ENGINE_MANAGER_KEY', event.type, event.keyCode,
            String(event.keyText === undefined ? '' : event.keyText));
          if (globalThis.l410EngineAbility) return globalThis.l410EngineAbility.keyToTarget(event);
          return false;
        });
        Button.onFocus(() => console.info('L410_ENGINE_MANAGER_FOCUS'));
      }, Button);
      Button.pop();
      this.observeComponentCreation2(() => {
        Text.create(this.status);
        Text.fontSize(14);
        Text.fontColor('#8fa3b0');
        Text.textAlign(TextAlign.Center);
        Text.width('100%');
        Text.height(24);
      }, Text);
      Text.pop();
      /* 引擎自己的界面：点一下就把容器里的应用拉起来（应用起来后自成窗口）。 */
      this.observeComponentCreation2(() => {
        Column.create();
        Column.width('100%');
        Column.padding({ top: 24 });
      }, Column);
      for (const entry of [{ name: 'mousepad', label: '文本编辑器 mousepad（GTK）' }]) {
        this.observeComponentCreation2(() => {
          Button.createWithLabel(entry.label, { type: ButtonType.Normal });
          Button.width(320);
          Button.height(44);
          Button.margin({ bottom: 12 });
          Button.onClick(() => {
            if (globalThis.l410EngineAbility) {
              const ok = globalThis.l410EngineAbility.launchApp(entry.name);
              this.status = ok ? ('已启动 ' + entry.name) : ('启动失败：' + entry.name);
            }
          });
        }, Button);
        Button.pop();
      }
      Column.pop();
      }
    } else {
      this.observeComponentCreation2(() => {
        XComponent.create({ id: 'l410-engine-surface-' + this.windowId, type: 'surface', libraryname: 'l410displaymulti' }, '__L410_BUNDLE__/entry');
        XComponent.width('100%');
        XComponent.height('100%');
        XComponent.opacity(this.visible ? 1 : 0);
        XComponent.onLoad(context => this.connect(context));
      }, XComponent);
      this.observeComponentCreation2(() => {
        Button.createWithLabel('', { type: ButtonType.Normal, stateEffect: false });
        Button.width('100%');
        Button.height('100%');
        Button.backgroundColor('#00000000');
        Button.borderRadius(0);
        Button.focusable(!this.imeActive);
        Button.defaultFocus(!this.imeActive);
        Button.id('l410-engine-input-' + this.windowId);
        Button.onMouse(event => {
          const button = event.button === MouseButton.Left ? 1 : event.button === MouseButton.Right ? 3 : event.button === MouseButton.Middle ? 2 : 0;
          if (event.action === MouseAction.Press) {
            /* 按键要进得来，先得把焦点拿到我们这层输入覆盖面上（再转发指针）。 */
            if (!this.imeActive) { try { focusControl.requestFocus('l410-engine-input-' + this.windowId); } catch (error) { } }
          }
          /* GTK menus, resize cursors and tooltips depend on hover motion.  A
             Harmony mouse-move event normally has button=None, so filtering
             zero here made the pointer appear to freeze unless a button was
             held down. */
          if (event.action === MouseAction.Move) this.pointer(event, 'move', this.pressed ? button : 0);
          else if (event.action === MouseAction.Press && button) this.pointer(event, 'down', button);
          else if (event.action === MouseAction.Release && button) this.pointer(event, 'up', button);
        });
        Button.onTouch(event => {
          if (event.type === TouchType.Down) {
            if (!this.imeActive) { try { focusControl.requestFocus('l410-engine-input-' + this.windowId); } catch (error) { } }
          }
          const point = { x: Number(event.touches[0].x), y: Number(event.touches[0].y) };
          if (event.type === TouchType.Down) this.pointer(point, 'down', 1);
          else if (event.type === TouchType.Up) this.pointer(point, 'up', 1);
          else this.pointer(point, 'move', 1);
        });
        Button.onAxisEvent(event => this.wheel(event));
        Button.onKeyEvent(event => this.key(event));
        Button.onFocus(() => {
          console.info('L410_ENGINE_PAGE_FOCUS', this.windowId);
          if (globalThis.l410EngineAbility) globalThis.l410EngineAbility.focus(this.windowId, true);
        });
        Button.onBlur(() => {
          console.info('L410_ENGINE_PAGE_BLUR', this.windowId);
          if (globalThis.l410EngineAbility) globalThis.l410EngineAbility.focus(this.windowId, false);
        });
      }, Button);
      Button.pop();
      /* 系统输入法挂在这个看不见的输入框上：应用里的光标在哪，它就挪到哪。 */
      this.observeComponentCreation2(() => {
        TextInput.create({ text: this.imeText, placeholder: '' });
        TextInput.id('l410-engine-ime-' + this.windowId);
        TextInput.focusable(this.imeActive && !this.imeCustomActive);
        TextInput.defaultFocus(this.imeActive && !this.imeCustomActive);
        TextInput.width(this.imeW);
        TextInput.height(this.imeH);
        /* Keep the input client fully visible to the focus/IME service while
           making its paint transparent.  Near-zero component opacity caused
           this Harmony build to skip it as a focus target. */
        TextInput.opacity(1);
        TextInput.fontColor('#00000000');
        TextInput.caretColor('#00000000');
        if (TextInput.enableKeyboardOnFocus) TextInput.enableKeyboardOnFocus(true);
        TextInput.backgroundColor('#00000000');
        TextInput.borderRadius(0);
        TextInput.padding(0);
        TextInput.position({ x: this.imeX, y: this.imeY });
        TextInput.onChange((value, preview) => this.imeChanged(value, preview));
        TextInput.onSubmit(() => this.imeSubmit());
        TextInput.onKeyEvent(event => this.key(event));
        TextInput.onFocus(() => {
          console.info('L410_ENGINE_IME_FOCUS', this.windowId,
            this.imeX, this.imeY, this.imeW, this.imeH);
          if (this.imeActive && !this.imeAttached) setTimeout(() => this.attachImeController(), 0);
        });
        TextInput.onBlur(() => console.info('L410_ENGINE_IME_BLUR', this.windowId));
      }, TextInput);
      TextInput.pop();
    }
    Stack.pop();
  }
  rerender() { this.updateDirtyElements(); }
}
ViewStackProcessor.StartGetAccessRecordingFor(ViewStackProcessor.AllocateNewElmetIdForNextComponent());
loadDocument(new EngineWindow(undefined, {}));
ViewStackProcessor.StopGetAccessRecording();
