/*
 * 应用窗口能力：一个实例 = 容器里一个 Linux 顶层窗口 = 一个**真实应用主窗口**
 * （系统自带的最小化/关闭按钮、独立任务条目、可被窗口管理器正常激活）。
 *
 * 为什么必须是独立能力实例：宿主能力里那些"子窗口"没有完整的窗口管理语义（没有最小化按钮、
 * 不进任务列表）。独立能力各有自己的 JS VM，所以：
 *   - 输入：页面通过 UI 桥（libl410engine 的 uiConnect/uiSend）发给持有引擎通道的宿主能力；
 *   - 就绪/几何/关闭：写一个 JSON 到应用自己的 files 目录，再 startAbility 通知宿主能力（事件驱动，不轮询）。
 */
/* 这套构建里模块要用 requireNapi 取，ES 的 '@ohos.*' 导入解析不了
   （崩过一次：ReferenceError: Cannot find module '@ohos.app.ability.UIAbility'）。
   照宿主能力里已经在用的写法来。 */
const appWinUiAbility = requireNapi('app.ability.UIAbility');

export default class EngineWindowAbility extends appWinUiAbility {
  onCreate(want) {
    /* 页面和本能力在同一个 VM：页面就绪后要能把表面端点交回来（globalThis 这个对象）。 */
    globalThis.l410WindowAbility = this;
    this.params = want && want.parameters ? want.parameters : {};
    this.hostId = Number(this.params.l410HostWindowId || 0);
    this.clientId = Number(this.params.l410ClientId || 0);
    this.clientPid = Number(this.params.l410ClientPid || 0);
    this.title = String(this.params.l410Title || 'Linux');
    this.kind = String(this.params.l410Kind || 'toplevel');
    this.parentClientId = Number(this.params.l410ParentClientId || 0);
    this.uiEndpoint = String(this.params.l410UiEndpoint || '');
    this.popupWindows = new Map();
    console.info('L410_APPWIN_CREATE', this.hostId, this.clientPid + ':' + this.clientId, this.title);
  }

  onWindowStageCreate(stage) {
    this.stage = stage;
    /* Eliminate the default black client-area frame while ArkUI loads and the
       first Linux buffer is acquired.  The XComponent is revealed only after
       its queue is ready, so this briefly exposes the desktop instead of a
       fabricated black frame. */
    try {
      const earlyWindow = stage.getMainWindowSync();
      if (earlyWindow && earlyWindow.setWindowBackgroundColor) {
        earlyWindow.setWindowBackgroundColor('#00000000');
      }
    } catch (error) { }
    const storage = new LocalStorage({
      linuxWindowId: this.hostId,
      manager: false,
      appWindow: true,
      uiEndpoint: this.uiEndpoint,
      clientId: this.clientId,
      clientPid: this.clientPid
    });
    stage.loadContent('pages/EngineWindow', storage, (error) => {
      if (error) {
        /* 这个回调在页面其实已经加载成功时也会报错（本项目实测如此：页面桥是通的），
           所以**不能因此 return**——否则窗口配置（标题/尺寸/可聚焦/事件）和命令订阅都不会执行，
           表现就是 resize/close 命令收不到、标题没设。 */
        console.warn('L410_APPWIN_LOAD_REPORTED', this.hostId, error.message || String(error));
      }
      console.info('L410_APPWIN_CONTENT', this.hostId);
      this.configureWindow();
    });
  }

  /* 给这个真实窗口注册窗口级输入过滤器（键/鼠标/触摸）。
     原生模块的 windowFilters 会把回调里的事件作为 KEYEVENT/MOUSEEVENT/TOUCHEVENT 行投给 JS。 */
  registerInputFilters(win, targetHostId) {
    const routedHostId = Number(targetHostId) || this.hostId;
    try {
      const props = win.getWindowProperties ? win.getWindowProperties() : {};
      const windowId = props && props.id;
      if (!windowId) {
        console.warn('L410_APPWIN_NO_WINDOW_ID', routedHostId);
        return;
      }
      /* 原生模块在这个 VM 里通过 1×1 的 XComponent 加载，页面已经拿到它的导出，
         存在 globalThis.l410EngineApi 上（宿主页也是这么做的）。 */
      const api = globalThis.l410EngineApi;
      if (api && api.windowFilters) {
        const result = api.windowFilters(windowId);
        this.nativeInputMask = Number(result) || 0;
        console.info('L410_APPWIN_FILTERS', routedHostId, 'windowId', windowId, 'result', result);
        const page = globalThis.l410EnginePages && globalThis.l410EnginePages[routedHostId];
        if (page && page.bridgeSend) {
          page.bridgeSend('FILTERS window=' + routedHostId + ' mask=' + this.nativeInputMask);
        }
      } else {
        console.warn('L410_APPWIN_NO_ENGINE_API', this.hostId);
      }
    } catch (error) {
      console.warn('L410_APPWIN_FILTER_ERROR', this.hostId, error.message || String(error));
    }
  }

  async configureWindow() {
    try {
      const win = this.stage.getMainWindowSync();
      /* 主窗口就是应用窗口：标题、尺寸、可聚焦，全部按容器应用的要求设一次。
         之后不再有任何"事后纠正"——尺寸只由这个真实窗口说了算。 */
      if (win.setWindowTitle) {
        try { win.setWindowTitle(this.title); }
        catch (error) { console.warn('L410_APPWIN_TITLE_SKIPPED', error.message); }
      }
      const width = Math.min(Number(this.params.l410Width || 900), 2100);
      const height = Math.min(Number(this.params.l410Height || 620), 1400);
      if (this.kind === 'popup' && win.setWindowDecorVisible) {
        try { await win.setWindowDecorVisible(false); }
        catch (error) { console.warn('L410_APPWIN_POPUP_DECOR_FAILED', this.hostId, error.message); }
      }
      /* EngineWindow keeps its XComponent transparent until the first complete
         Linux buffer is flushed.  Match the native window background to that
         contract so startup and a resize generation change never expose the
         system's default black clear color. */
      try {
        if (win.setWindowBackgroundColor) win.setWindowBackgroundColor('#00000000');
      } catch (error) {
        console.warn('L410_APPWIN_BACKGROUND_FAILED', this.hostId, error.message || String(error));
      }
      await win.resize(width, height);
      const initialX = Number(this.params.l410InitialX || 0);
      const initialY = Number(this.params.l410InitialY || 0);
      await win.moveWindowTo(this.kind === 'popup' ? Math.max(0, initialX) : 140,
                             this.kind === 'popup' ? Math.max(0, initialY) : 120);
      await win.setWindowFocusable(true);
      /* Device MMI's InputWindowsManager::IsAncoWindow checks bit 1 in its
         WindowInfo flags. SceneBoard sets that bit from
         WINDOW_FLAG_HANDWRITING, and the window NAPI exposes exactly this
         private-system route. This is the same ANCO input classification used
         by the device bridge; without it MMI treats the broker Surface as an
         ordinary ArkUI window and never calls IAncoConsumer. */
      try {
        if (win.setHandwritingFlag) {
          await win.setHandwritingFlag(true);
          console.info('L410_APPWIN_ANCO_INPUT_FLAG', this.hostId, 1);
        } else {
          console.warn('L410_APPWIN_ANCO_INPUT_FLAG_UNAVAILABLE', this.hostId);
        }
      } catch (error) {
        console.warn('L410_APPWIN_ANCO_INPUT_FLAG_FAILED', this.hostId, error.message || String(error));
      }
      /* 记下自己的 OHOS 窗口 id 与矩形：输入事件按窗口换算坐标要用。 */
      try {
        const props = win.getWindowProperties ? win.getWindowProperties() : {};
        this.osWindowId = props && props.id ? props.id : 0;
        this.windowRect = props && props.windowRect ? props.windowRect : null;
        console.info('L410_APPWIN_OSWINDOW', this.hostId, this.osWindowId,
          JSON.stringify(this.windowRect || {}));
        setTimeout(() => {
          try {
            const page = globalThis.l410EnginePages && globalThis.l410EnginePages[this.hostId];
            if (this.osWindowId && page && page.native && page.native.publish) {
              console.info('L410_APPWIN_SURFACE_PUBLISHED', this.hostId, this.osWindowId,
                page.native.publish(Number(this.osWindowId)));
            }
          } catch (error) {
            console.warn('L410_APPWIN_SURFACE_PUBLISH_FAILED', this.hostId, error.message || String(error));
          }
        }, 200);
      } catch (error) { console.warn('L410_APPWIN_OSWINDOW_FAILED', error.message); }
      /* 关键：窗口必须可触摸/可接收指针事件，否则窗口管理器根本不会把鼠标事件投给它
         （实测覆盖层的鼠标/触摸计数一直是 0，就是漏了这一条）。 */
      try {
        if (win.setWindowTouchable) {
          await win.setWindowTouchable(true);
          console.info('L410_APPWIN_TOUCHABLE', this.hostId);
        }
      } catch (error) { console.warn('L410_APPWIN_TOUCHABLE_FAILED', error.message); }
      /* startAbility can create a visible but unfocused multiton main window on
         this desktop build.  Show it with the API-20 focus contract after the
         focusable/touchable flags are in place.  This is the normal window
         activation path; shiftAppWindowFocus only applies to main/child pairs
         and correctly rejects two independent main windows. */
      try {
        await win.showWindow({ focusOnShow: true });
        console.info('L410_APPWIN_SHOWN_FOCUSED', this.hostId);
      } catch (error) {
        try {
          await win.showWindow();
          console.info('L410_APPWIN_SHOWN', this.hostId);
        } catch (second) {
          console.warn('L410_APPWIN_SHOW_FAILED', this.hostId, second.message || String(second));
        }
      }
      /* 窗口级输入过滤器：系统把键/鼠标/触摸投给这个窗口时，由原生模块直接收（anco 的形态），
         不再依赖 ArkUI 里任何控件。 */
      try { this.registerInputFilters(win); }
      catch (error) { console.warn('L410_APPWIN_FILTER_FAILED', error.message); }
      /* 焦点/可见性/状态全打出来：鸿蒙只把指针与键盘投给**活动窗口**，
         如果我们的窗口从来没被当成活动窗口，点它、敲它都不会有任何事件。 */
      try {
        const report = (tag) => {
          try {
            const p = win.getWindowProperties ? win.getWindowProperties() : {};
            console.info('L410_APPWIN_STATE', tag, this.hostId,
              'id', p.id || 0, 'focused', p.isFocused ? 1 : 0, 'focusable', p.focusable ? 1 : 0,
              'touchable', p.touchable ? 1 : 0, 'type', p.type, 'mode', p.windowMode,
              'rect', JSON.stringify(p.windowRect || {}));
          } catch (error) { }
        };
        report('initial');
        const subscribe = (name, callback) => {
          try { win.on(name, callback); }
          catch (error) { console.warn('L410_APPWIN_EVENT_UNSUPPORTED', name, error.message || String(error)); }
        };
        subscribe('windowEvent', (data) => console.info('L410_APPWIN_EVENT_KIND', this.hostId, data));
        subscribe('windowVisibilityChange', (visible) => {
          console.info('L410_APPWIN_VISIBILITY', this.hostId, visible ? 1 : 0);
          report('visibility');
        });
        subscribe('windowStatusChange', (status) => {
          console.info('L410_APPWIN_STATUS', this.hostId, status);
          report('status');
        });
        subscribe('windowFocus', () => { console.info('L410_APPWIN_ACTIVATED', this.hostId); report('focus'); });
        subscribe('windowUnfocus', () => { console.info('L410_APPWIN_DEACTIVATED', this.hostId); report('unfocus'); });
      } catch (error) { console.warn('L410_APPWIN_STATE_SUBSCRIBE_FAILED', error.message); }
      /* 用户点系统关闭按钮：先问容器应用（它可能弹"是否保存"）。 */
      win.on('windowWillClose', () => {
        console.info('L410_APPWIN_WILL_CLOSE', this.hostId);
        if (this.forceClose) return Promise.resolve(false);
        this.notifyHost('close');
        return Promise.resolve(true);
      });
      win.on('windowRectChange', () => {
        try {
          const props = win.getWindowProperties ? win.getWindowProperties() : {};
          this.windowRect = props && props.windowRect ? props.windowRect : this.windowRect;
          this.osWindowId = props && props.id ? props.id : this.osWindowId;
        } catch (error) { }
        /* windowRect includes the system frame. Keep it for global input
           coordinates, but never publish it as the drawable client size. */
      });
      /* This event describes the decorated main window on this desktop build.
         Stack.onAreaChange in the page reports the XComponent client extent. */
      win.on('windowSizeChange', (size) => {
        console.info('L410_APPWIN_SIZE', this.hostId, size.width, size.height);
      });
      this.notifyHost('configured', { width: width, height: height });
    this.subscribeCommands();
    } catch (error) {
      console.error('L410_APPWIN_CONFIGURE_FAILED', this.hostId, error.message || String(error));
    }
  }

  /* 宿主发来的命令：走系统事件订阅（不是 startAbility——multiton 每次都会新建实例）。
     等价于用户在系统标题栏上拖窗口边 / 点关闭按钮。 */
  subscribeCommands() {
    try {
      const events = requireNapi('commonEventManager');
      this.subscriber = events.createSubscriberSync({ events: ['l410_appwin_command'] });
      events.subscribe(this.subscriber, (error, data) => {
        if (error) {
          console.warn('L410_APPWIN_COMMAND_RECEIVE_FAILED', this.hostId,
            error.message || String(error));
          return;
        }
        this.onCommand(data);
      });
      console.info('L410_APPWIN_SUBSCRIBED', this.hostId);
    } catch (error) {
      console.error('L410_APPWIN_SUBSCRIBE_FAILED', this.hostId, error.message || String(error));
    }
  }

  onCommand(data) {
    const params = (data && data.parameters) || {};
    const command = String(params.l410AppWindowCommand || '');
    if (!command) return;
    if (Number(params.l410HostWindowId || 0) !== this.hostId) return;
    const targetHostId = Number(params.l410TargetWindowId || 0) || this.hostId;
    console.info('L410_APPWIN_COMMAND', targetHostId, command);
    if (command === 'create-popup') this.createPopup(params);
    else if (command === 'resize' && targetHostId !== this.hostId)
      this.resizePopup(targetHostId, Number(params.width || 0), Number(params.height || 0));
    else if (command === 'resize') this.resizeSelf(Number(params.width || 0), Number(params.height || 0));
    else if (command === 'move' && targetHostId !== this.hostId)
      this.movePopup(targetHostId, Number(params.x || 0), Number(params.y || 0));
    else if (command === 'move') this.moveSelf(Number(params.x || 0), Number(params.y || 0));
    else if ((command === 'hide' || command === 'close') && targetHostId !== this.hostId)
      this.destroyPopup(targetHostId);
    else if (command === 'hide' || command === 'close') this.requestClose();
    else {
      const page = globalThis.l410EnginePages && globalThis.l410EnginePages[targetHostId];
      if (!page) return;
      if (command === 'ime-show') page.imeShow();
      else if (command === 'ime-hide') page.imeHide();
      else if (command === 'ime-reset') page.imeReset();
      else if (command === 'ime-state') page.updateImeState(String(params.text || ''),
        Number(params.cursor || 0), Number(params.anchor || 0));
      else if (command === 'ime-caret') page.setCaret(Number(params.x || 0), Number(params.y || 0),
        Number(params.w || 1), Number(params.h || 2));
    }
  }

  async resizeSelf(width, height) {
    try {
      const win = this.stage.getMainWindowSync();
      await win.resize(Math.min(width || 900, 2100), Math.min(height || 620, 1400));
      console.info('L410_APPWIN_RESIZED', this.hostId, width, height);
      /* resize() takes the decorated window extent on this build.  Its resulting
         XComponent area is reported by Stack.onAreaChange; publishing the
         requested outer size here would corrupt the client-size protocol. */
    } catch (error) {
      console.error('L410_APPWIN_RESIZE_FAILED', this.hostId, error.message || String(error));
    }
  }

  async moveSelf(x, y) {
    try {
      await this.stage.getMainWindowSync().moveWindowTo(Math.max(0, x), Math.max(0, y));
      console.info('L410_APPWIN_MOVED', this.hostId, x, y);
    } catch (error) {
      console.warn('L410_APPWIN_MOVE_FAILED', this.hostId, error.message || String(error));
    }
  }

  windowRectFor(hostId) {
    const record = this.popupWindows && this.popupWindows.get(Number(hostId));
    return record ? (record.rect || {}) : (this.windowRect || {});
  }

  osWindowIdFor(hostId) {
    const record = this.popupWindows && this.popupWindows.get(Number(hostId));
    return record ? Number(record.osWindowId || 0) : Number(this.osWindowId || 0);
  }

  pageReady(hostId, endpoint, width, height, density) {
    const id = Number(hostId) || this.hostId;
    const record = this.popupWindows && this.popupWindows.get(id);
    if (record) {
      record.endpoint = endpoint;
      record.surfaceDensity = density;
      this.notifyHostFor(id, 'ready', { width: width, height: height });
      return;
    }
    this.surfaceEndpoint = endpoint;
    this.surfaceWidth = width;
    this.surfaceHeight = height;
    this.surfaceDensity = density;
    this.notifyHost('ready', { width: width, height: height });
  }

  queueGeometryFor(hostId, width, height) {
    const id = Number(hostId) || this.hostId;
    if (id === this.hostId) { this.queueGeometry(width, height); return; }
    const record = this.popupWindows && this.popupWindows.get(id);
    if (!record || !(width > 0 && height > 0)) return;
    record.pendingWidth = width;
    record.pendingHeight = height;
    if (record.geometryTimer) return;
    record.geometryTimer = setTimeout(() => {
      record.geometryTimer = 0;
      const signature = record.pendingWidth + 'x' + record.pendingHeight;
      if (signature === record.lastGeometrySignature) return;
      record.lastGeometrySignature = signature;
      this.notifyHostFor(id, 'geometry', { width: record.pendingWidth, height: record.pendingHeight });
    }, 16);
  }

  async createPopup(params) {
    const id = Number(params.popupHostId || params.l410TargetWindowId || 0);
    if (!id || this.popupWindows.has(id)) return;
    try {
      const win = await this.stage.createSubWindowWithOptions('l410-popup-' + id,
        { title: String(params.popupTitle || ''), decorEnabled: false, isModal: false });
      const requestedX = Math.max(0, Number(params.popupX || 0));
      const requestedY = Math.max(0, Number(params.popupY || 0));
      const requestedWidth = Math.min(Number(params.popupWidth || 320), 2100);
      const requestedHeight = Math.min(Number(params.popupHeight || 240), 1400);
      const record = { id: id, window: win, clientId: Number(params.popupClientId || 0),
        clientPid: Number(params.popupClientPid || 0),
        rect: { left: requestedX, top: requestedY, width: requestedWidth, height: requestedHeight },
        osWindowId: 0, endpoint: '' };
      this.popupWindows.set(id, record);
      try { if (win.setWindowDecorVisible) await win.setWindowDecorVisible(false); } catch (error) { }
      try { if (win.setWindowBackgroundColor) win.setWindowBackgroundColor('#00000000'); } catch (error) { }
      await win.setWindowFocusable(false);
      if (win.setWindowTouchable) await win.setWindowTouchable(true);
      await win.resize(requestedWidth, requestedHeight);
      await win.moveWindowTo(requestedX, requestedY);
      const storage = new LocalStorage({ linuxWindowId: id, manager: false, appWindow: true,
        uiEndpoint: this.uiEndpoint, clientId: record.clientId, clientPid: record.clientPid });
      await win.loadContent('pages/EngineWindow', storage);
      const update = () => {
        try {
          const props = win.getWindowProperties ? win.getWindowProperties() : {};
          const next = props.windowRect;
          if (next && Number(next.width) > 0 && Number(next.height) > 0) record.rect = next;
          record.osWindowId = Number(props.id || record.osWindowId || 0);
        } catch (error) { }
      };
      update();
      try { win.on('windowRectChange', update); } catch (error) { }
      await win.showWindow();
      update();
      setTimeout(() => {
        if (!this.popupWindows.has(id)) return;
        update();
        const page = globalThis.l410EnginePages && globalThis.l410EnginePages[id];
        if (page && page.width > 0 && page.height > 0) {
          try {
            const ui = page.getUIContext();
            this.notifyHostFor(id, 'geometry', {
              width: Math.round(ui.vp2px(page.width)), height: Math.round(ui.vp2px(page.height))
            });
          } catch (error) { }
        }
      }, 80);
      this.registerInputFilters(win, id);
      console.info('L410_APPWIN_POPUP_CREATED', id, 'owner', this.hostId,
        record.osWindowId, JSON.stringify(record.rect));
    } catch (error) {
      this.popupWindows.delete(id);
      console.error('L410_APPWIN_POPUP_CREATE_FAILED', id, error.message || String(error));
      this.notifyHostFor(id, 'gone', {});
    }
  }

  async resizePopup(id, width, height) {
    const record = this.popupWindows.get(Number(id));
    if (record) try { await record.window.resize(width, height); } catch (error) { }
  }

  async movePopup(id, x, y) {
    const record = this.popupWindows.get(Number(id));
    if (record) try { await record.window.moveWindowTo(Math.max(0, x), Math.max(0, y)); } catch (error) { }
  }

  destroyPopup(id) {
    const record = this.popupWindows && this.popupWindows.get(Number(id));
    if (!record) return;
    this.popupWindows.delete(Number(id));
    if (record.geometryTimer) clearTimeout(record.geometryTimer);
    try { record.window.destroyWindow(); } catch (error) { }
    this.notifyHostFor(Number(id), 'gone', {}, record);
    console.info('L410_APPWIN_POPUP_DESTROYED', Number(id));
  }

  notifyHostFor(hostId, reason, extra, detachedRecord) {
    const id = Number(hostId) || this.hostId;
    if (id === this.hostId) { this.notifyHost(reason, extra); return; }
    const record = detachedRecord || (this.popupWindows && this.popupWindows.get(id));
    if (!record) return;
    const rect = record.rect || {};
    const page = globalThis.l410EnginePages && globalThis.l410EnginePages[id];
    const endpoint = record.endpoint || (page && page.endpoint) || '';
    const line = 'APPWIN window=' + id + ' reason=' + reason +
      ' endpoint=' + endpoint + ' density=' + (record.surfaceDensity || 0) +
      ' os=' + (record.osWindowId || 0) +
      ' left=' + (Number(rect.left !== undefined ? rect.left : rect.x) || 0) +
      ' top=' + (Number(rect.top !== undefined ? rect.top : rect.y) || 0) +
      ' rectw=' + (Number(rect.width) || 0) + ' recth=' + (Number(rect.height) || 0) +
      ' w=' + Number(extra && extra.width || 0) + ' h=' + Number(extra && extra.height || 0);
    const sent = !!(page && page.bridgeSend && page.bridgeSend(line));
    console.info('L410_APPWIN_NOTIFY_SOCKET', id, reason, sent ? 1 : 0);
  }

  /* Only callers that measured the ArkUI client area may publish geometry.
     Coalesce bursts to one configure per display interval.  The former 80 ms
     quiet-period debounce intentionally stopped all app rendering while the
     pointer was moving and exposed a black resize Surface. */
  queueGeometry(width, height) {
    if (!(Number(width) > 0 && Number(height) > 0)) return;
    this.pendingGeometryWidth = Math.round(Number(width));
    this.pendingGeometryHeight = Math.round(Number(height));
    /* Coalesce to one update per interval while a drag remains active. */
    if (this.geometryTimer) return;
    this.geometryTimer = setTimeout(() => {
      this.geometryTimer = 0;
      const w = this.pendingGeometryWidth || 0;
      const h = this.pendingGeometryHeight || 0;
      if (!(w > 0 && h > 0)) return;
      const signature = w + 'x' + h;
      if (signature === this.lastGeometrySignature) return;
      this.lastGeometrySignature = signature;
      this.notifyHost('geometry', { width: w, height: h });
    }, 16);
  }

  /* Window events use the already-connected broker socket.  Calling
     startAbility for every move/resize foregrounded the otherwise hidden
     broker window, stole focus and fed window focus back into the IME. */
  notifyHost(reason, extra) {
    try {
      const record = {
        hostWindowId: this.hostId,
        clientId: this.clientId,
        clientPid: this.clientPid,
        reason: reason,
        /* 容器应用要靠这个名字去连它那块表面；页面就绪后会放在 this.surfaceEndpoint 上。 */
        endpoint: this.surfaceEndpoint || '',
        density: this.surfaceDensity || 0,
        /* 宿主需要这个：窗口级输入事件带的是显示坐标和 OHOS 窗口 id，
           要按窗口换算成窗口内坐标（anco 的 windowTransform）。 */
        osWindowId: this.osWindowId || 0,
        rect: this.windowRect || null,
        width: (extra && extra.width) || 0,
        height: (extra && extra.height) || 0,
        stamp: Date.now()
      };
      const rect = record.rect || {};
      const page = (globalThis.l410EnginePages && globalThis.l410EnginePages[this.hostId]) || this.bridgePage;
      const line = 'APPWIN window=' + this.hostId + ' reason=' + reason +
        ' endpoint=' + record.endpoint + ' density=' + record.density +
        ' os=' + record.osWindowId +
        ' left=' + (Number(rect.left !== undefined ? rect.left : rect.x) || 0) +
        ' top=' + (Number(rect.top !== undefined ? rect.top : rect.y) || 0) +
        ' rectw=' + (Number(rect.width) || 0) + ' recth=' + (Number(rect.height) || 0) +
        ' w=' + record.width + ' h=' + record.height;
      const sent = !!(page && page.bridgeSend && page.bridgeSend(line));
      console.info('L410_APPWIN_NOTIFY_SOCKET', this.hostId, reason, sent ? 1 : 0);
      if (!sent && reason !== 'configured') {
        console.error('L410_APPWIN_NOTIFY_SOCKET_FAILED', this.hostId, reason);
      }
    } catch (error) {
      console.error('L410_APPWIN_NOTIFY_ERROR', this.hostId, reason, error.message || String(error));
    }
  }

  /* 宿主确认可以关了：这一次放行系统关闭。 */
  requestClose() {
    this.forceClose = true;
    try { this.stage.getMainWindowSync().destroyWindow(); }
    catch (error) { console.warn('L410_APPWIN_DESTROY_FAILED', this.hostId, error.message); }
  }

  onForeground() {
    console.info('L410_APPWIN_FOREGROUND', this.hostId);
  }

  onBackground() {
    console.info('L410_APPWIN_BACKGROUND', this.hostId);
  }

  onWindowStageDestroy() {
    console.info('L410_APPWIN_STAGE_DESTROY', this.hostId);
    this.notifyHost('gone');
  }

  onDestroy() {
    console.info('L410_APPWIN_DESTROY', this.hostId);
  }
}
