/* One invisible standard UIAbility per AnCo task, as in ShellAssistant. */
const AncoUiAbility = requireNapi('app.ability.UIAbility');
const taskInputNative = requireNapi('l410ancotaskinput', true,
  'com.l410.linux.engine/entry');
/* ShellAssistant registers each task window with MMI through this stock
   module after the page is built.  The handwriting flag alone only changes
   WindowInfo.flags; it does not provide the task/process association. */
let stockFusionInput = null;
let stockInputProcess = null;
let stockInputDisplay = null;
let stockDeviceInfo = null;
let stockSystemParameter = null;
try {
  stockFusionInput = requireNapi('shell_fusion_manager', true,
    'com.l410.linux.engine/entry');
  stockInputProcess = requireNapi('process');
  stockInputDisplay = requireNapi('display');
  stockDeviceInfo = requireNapi('deviceInfo');
  stockSystemParameter = requireNapi('systemParameterEnhance');
} catch (error) {
  console.error('L410_ANCO_STOCK_WINDOW_MODULE_FAILED',
    error.code || 0, error.message || String(error));
}

export default class AncoTaskAbility extends AncoUiAbility {
  onCreate(want) {
    const params = want && want.parameters ? want.parameters : {};
    this.taskId = Number(params.keyId || params.ancoTaskId || 0);
    this.missionId = Number(params.missionId || 0);
    this.inputBound = false;
    this.stockInputWindowId = 0;
    this.stockInputRefreshTimer = null;
    this.stockInputGeometryTimer = null;
    this.stockInputGeometryHandler = null;
    this.windowClosingHandler = null;
    if (!Number.isInteger(this.taskId) || this.taskId <= 0)
      console.error('L410_ANCO_TASK_INVALID_ID', this.taskId);
    console.info('L410_ANCO_TASK_CREATE', this.taskId, this.missionId);
  }

  async onWindowStageCreate(stage) {
    this.stage = stage;
    let win;
    try { win = stage.getMainWindowSync(); }
    catch (error) {
      console.error('L410_ANCO_TASK_MAIN_WINDOW_FAILED', this.taskId,
        error.code || 0, error.message || String(error));
      return;
    }
    /* SP68 MMI's IsAncoWindow tests WindowInfo.flags bit 1. The stock window
       NAPI routes setHandwritingFlag through SceneBoard to that exact bit; the
       existing EngineWindowAbility already uses it. Apply it to this per-task
       native window before MMI binds the task, otherwise the stream path sees
       an ordinary ArkUI window even when its native ID has focus. */
    try {
      if (typeof win.setHandwritingFlag === 'function') {
        await win.setHandwritingFlag(true);
        console.info('L410_ANCO_TASK_INPUT_FLAG', this.taskId, 1);
      } else {
        console.warn('L410_ANCO_TASK_INPUT_FLAG_UNAVAILABLE', this.taskId);
      }
    } catch (error) {
      console.warn('L410_ANCO_TASK_INPUT_FLAG_FAILED', this.taskId,
        error.code || 0, error.message || String(error));
    }
    try {
      const nativeId = Number(win.getWindowProperties().id);
      console.info('L410_ANCO_TASK_NATIVE_WINDOW', this.taskId, nativeId);
      if (Number.isInteger(nativeId) && nativeId > 0 &&
          taskInputNative && typeof taskInputNative.bindTask === 'function') {
        const result = taskInputNative.bindTask(this.taskId, nativeId);
        this.inputBound = result === 0;
        console.info('L410_ANCO_TASK_INPUT_BOUND', this.taskId, nativeId,
          result);
      }
      if (Number.isInteger(nativeId) && nativeId > 0 &&
          typeof win.on === 'function') {
        const closing = () => {
          let result = -1;
          try {
            result = taskInputNative.notifyTaskClosing(this.taskId);
          } catch (error) {
            console.error('L410_ANCO_TASK_CLOSE_REQUEST_FAILED', this.taskId,
              nativeId, error.code || 0, error.message || String(error));
          }
          console.info('L410_ANCO_TASK_CLOSE_REQUEST', this.taskId, nativeId,
            result);
          /* Keep the native window alive while xdg_toplevel.close lets the
             Linux client decide whether to exit or show its save dialog. */
          return Promise.resolve(true);
        };
        try {
          win.on('windowWillClose', closing);
          this.windowClosingHandler = closing;
          console.info('L410_ANCO_TASK_CLOSE_LISTENER_READY', this.taskId,
            nativeId);
        } catch (error) {
          console.error('L410_ANCO_TASK_CLOSE_LISTENER_FAILED', this.taskId,
            nativeId, error.code || 0, error.message || String(error));
        }
      }
    } catch (error) {
      console.warn('L410_ANCO_TASK_NATIVE_WINDOW_FAILED', this.taskId,
        error.code || 0, error.message || String(error));
    }
    const storage = new LocalStorage({ appWindow: win, taskId: this.taskId,
      missionId: this.missionId });
    stage.loadContent('pages/AncoTaskPage', storage, error => {
      if (error && Number(error.code) !== 0)
        console.error('L410_ANCO_TASK_PAGE_FAILED', this.taskId,
          error.code || 0, error.message || String(error),
          JSON.stringify(error), Object.getOwnPropertyNames(error).join(','));
      else {
        console.info('L410_ANCO_TASK_PAGE_READY', this.taskId, this.missionId,
          error ? 'callback-code-0' : 'callback-empty');
        /* ShellAssistant AncoAbility performs these exact Window operations
           after AncoMainPage has loaded.  They put the task window into the
           stock container-modal decoration path; without them SceneBoard can
           draw a caption while its move/resize/maximize contract is absent. */
        try {
          win.setWindowLayoutFullScreen(true);
          const captionbar = Number(stockSystemParameter.getSync(
            'hmos_fusion.container.pc.freemode.captionbar', '0')) === 1;
          const deviceType = String(stockDeviceInfo.deviceType || '');
          if (captionbar && (deviceType === '2in1' || deviceType === 'tablet')) {
            win.setWindowDecorHeight(win.getUIContext().px2vp(28) + 32);
            win.setDecorButtonStyle({ buttonIconSize: 24 });
          }
          console.info('L410_ANCO_TASK_ORIGINAL_DECOR_READY', this.taskId,
            captionbar ? 1 : 0, deviceType);
        } catch (error) {
          console.error('L410_ANCO_TASK_ORIGINAL_DECOR_FAILED', this.taskId,
            error.code || 0, error.message || String(error));
        }
        this.registerStockInputWindow(win, 1);
        /* The stock module derives its MMI hot area from the native window's
           current windowRect/drawableRect.  Re-publish through that same
           module when SceneBoard changes the client area (for example when a
           client-decorated X11 window takes ownership of its title bar). */
        if (typeof win.on === 'function') {
          const geometryChanged = () => {
            if (this.stockInputGeometryTimer)
              clearTimeout(this.stockInputGeometryTimer);
            this.stockInputGeometryTimer = setTimeout(() => {
              this.stockInputGeometryTimer = null;
              if (this.stage && this.stockInputWindowId)
                this.registerStockInputWindow(win, 3);
            }, 50);
          };
          try {
            win.on('windowRectChange', geometryChanged);
            win.on('windowSizeChange', geometryChanged);
            this.stockInputGeometryHandler = geometryChanged;
            console.info('L410_ANCO_STOCK_WINDOW_GEOMETRY_LISTENER_READY',
              this.taskId);
          } catch (error) {
            try { win.off('windowRectChange', geometryChanged); }
            catch (offError) { }
            try { win.off('windowSizeChange', geometryChanged); }
            catch (offError) { }
            console.error('L410_ANCO_STOCK_WINDOW_GEOMETRY_LISTENER_FAILED',
              this.taskId, error.code || 0, error.message || String(error));
          }
        }
        /* ShellAssistant also re-sends on page show. The first page-build
           call can precede completion of the new MMI UDS session. */
        this.stockInputRefreshTimer = setTimeout(() => {
          this.stockInputRefreshTimer = null;
          if (this.stage && this.stockInputWindowId)
            this.registerStockInputWindow(win, 3);
        }, 1500);
      }
    });
  }

  registerStockInputWindow(win, action = 3) {
    try {
      const props = win.getWindowProperties();
      const nativeId = Number(props.id);
      const displayId = Number(stockInputDisplay?.getDefaultDisplaySync().id);
      const pid = Number(stockInputProcess?.pid);
      const uid = Number(stockInputProcess?.uid);
      if (!Number.isInteger(nativeId) || nativeId <= 0 ||
          !Number.isInteger(displayId) || !Number.isInteger(pid) || pid <= 0 ||
          !Number.isInteger(uid) || uid < 0 ||
          !stockFusionInput || typeof stockFusionInput.sendWindowInfo !== 'function')
        throw new Error('stock input window identity unavailable');
      if (action === 1) {
        const rect = props.windowRect || {};
        const drawable = props.drawableRect || {};
        /* ShellAssistant passes windowRect.left/top/width and
           drawableRect.top only for action 1.  That action also registers
           the native title/hot-area listener and therefore must run once. */
        const hotArea = {
          left: Number(rect.left) || 0,
          top: Number(rect.top) || 0,
          width: Number(rect.width) || 0,
          height: Number(drawable.top) || 0
        };
        stockFusionInput.sendWindowInfo(nativeId, 1, displayId, hotArea,
          pid, uid, this.taskId);
        this.stockInputWindowId = nativeId;
        console.info('L410_ANCO_STOCK_WINDOW_INFO_ADD', this.taskId,
          nativeId, displayId, pid, uid, JSON.stringify(hotArea));
      } else {
        /* SP68 AncoAbility uses action 3 with exactly these two arguments
           when the existing task window must be re-sent to MMI. */
        if (!this.stockInputWindowId) return;
        stockFusionInput.sendWindowInfo(nativeId, 3);
        console.info('L410_ANCO_STOCK_WINDOW_INFO_RESEND', this.taskId,
          nativeId);
      }
    } catch (error) {
      console.error('L410_ANCO_STOCK_WINDOW_INFO_FAILED', this.taskId,
        error.code || 0, error.message || String(error));
    }
  }

  onForeground() { console.info('L410_ANCO_TASK_FOREGROUND', this.taskId); }
  onBackground() { console.info('L410_ANCO_TASK_BACKGROUND', this.taskId); }
  releaseInput() {
    if (this.stockInputRefreshTimer) {
      clearTimeout(this.stockInputRefreshTimer);
      this.stockInputRefreshTimer = null;
    }
    if (this.stockInputGeometryTimer) {
      clearTimeout(this.stockInputGeometryTimer);
      this.stockInputGeometryTimer = null;
    }
    if (this.stockInputGeometryHandler && this.stage) {
      try { this.stage.getMainWindowSync().off('windowRectChange',
        this.stockInputGeometryHandler); }
      catch (error) { }
      try { this.stage.getMainWindowSync().off('windowSizeChange',
        this.stockInputGeometryHandler); }
      catch (error) { }
      this.stockInputGeometryHandler = null;
    }
    if (this.windowClosingHandler && this.stage) {
      try { this.stage.getMainWindowSync().off('windowWillClose',
        this.windowClosingHandler); }
      catch (error) { }
      this.windowClosingHandler = null;
    }
    if (this.stockInputWindowId) {
      const nativeId = this.stockInputWindowId;
      this.stockInputWindowId = 0;
      try {
        stockFusionInput.sendWindowInfo(nativeId, 2);
        console.info('L410_ANCO_STOCK_WINDOW_INFO_REMOVE', this.taskId, nativeId);
      } catch (error) {
        console.warn('L410_ANCO_STOCK_WINDOW_INFO_REMOVE_FAILED', this.taskId,
          nativeId, error.code || 0, error.message || String(error));
      }
    }
    if (!this.inputBound) return;
    this.inputBound = false;
    try {
      console.info('L410_ANCO_TASK_INPUT_UNBOUND', this.taskId,
        taskInputNative.unbindTask(this.taskId));
    } catch (error) {
      console.warn('L410_ANCO_TASK_INPUT_UNBIND_FAILED', this.taskId,
        error.code || 0, error.message || String(error));
    }
  }

  onWindowStageDestroy() {
    this.releaseInput();
    console.info('L410_ANCO_TASK_STAGE_DESTROY', this.taskId);
  }

  onDestroy() {
    this.releaseInput();
    console.info('L410_ANCO_TASK_DESTROY', this.taskId, this.missionId);
  }
}
