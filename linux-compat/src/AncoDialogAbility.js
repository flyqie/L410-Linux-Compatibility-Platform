/* One WindowStage main window for each Wayland xdg_toplevel.set_parent.
 * The original AnCo WMS/Graphic target still owns its pixels and lifecycle;
 * this UIAbility supplies an independent HarmonyOS desktop window boundary. */
const AncoUiAbility = requireNapi('app.ability.UIAbility');
const taskInputNative = requireNapi('l410ancotaskinput', true,
  'com.l410.linux.engine/entry');
import taskManager from './AncoTaskManager';
let stockFusionInput = null;
let stockProcess = null;
let stockDisplay = null;
try {
  stockFusionInput = requireNapi('shell_fusion_manager', true,
    'com.l410.linux.engine/entry');
  stockProcess = requireNapi('process');
  stockDisplay = requireNapi('display');
} catch (error) {
  console.error('L410_ANCO_DIALOG_STOCK_MODULE_FAILED',
    error.code || 0, error.message || String(error));
}

export default class AncoDialogAbility extends AncoUiAbility {
  onCreate(want) {
    const params = want?.parameters || {};
    this.taskId = Number(params.ancoTaskId || 0);
    this.graphicId = Number(params.ancoGraphicId || 0);
    this.ownerWmsWindowId = Number(params.ancoDialogWmsId || 0);
    this.rootSurface = null;
    this.record = null;
    this.mainClientGeometry = null;
    this.nativeId = 0;
    this.inputBound = false;
    this.ready = false;
    this.retired = false;
    this.visible = false;
    this.desiredGeometry = '';
    this.appliedGeometry = '';
    this.operation = Promise.resolve();
    this.unsubscribe = null;
    this.refreshTimer = null;
    this.closingHandler = null;
    if (this.taskId <= 0 || this.graphicId <= 0 ||
        this.ownerWmsWindowId <= 0)
      console.error('L410_ANCO_DIALOG_INVALID_ID', this.taskId,
        this.graphicId, this.ownerWmsWindowId);
  }

  async onWindowStageCreate(stage) {
    this.stage = stage;
    const win = stage.getMainWindowSync();
    this.window = win;
    try {
      // A fresh WindowStage can reject appearance setters until its scene
      // session is active (1300002 on this device). None may abort the
      // SurfaceView/input binding needed to bring up the dialog itself.
      for (const [name, value] of [
        ['setWindowBackgroundColor', '#00000000'],
        ['setHandwritingFlag', true],
        ['setWindowFocusable', true],
        ['setWindowTouchable', true]
      ]) {
        if (typeof win[name] !== 'function') continue;
        try { await win[name](value); }
        catch (error) {
          console.warn('L410_ANCO_DIALOG_WINDOW_OPTION_FAILED', name,
            error.code || 0, error.message || String(error));
        }
      }
      this.nativeId = Number(win.getWindowProperties().id);
      if (!Number.isInteger(this.nativeId) || this.nativeId <= 0)
        throw new Error('dialog main window ID unavailable');
      await this.bindTaskInput();
      this.closingHandler = () => {
        const result = taskInputNative.notifyTaskClosing(this.taskId,
          this.nativeId);
        console.info('L410_ANCO_DIALOG_CLOSE_REQUEST', this.taskId,
          this.graphicId, this.nativeId, result);
        return Promise.resolve(true);
      };
      win.on('windowWillClose', this.closingHandler);
      this.unsubscribe = taskManager.register(this.taskId, {
        activity: () => {},
        removeActivity: () => this.retire(),
        clientGeometry: geometry => {
          this.mainClientGeometry = geometry;
          this.applyGeometry();
        },
        surface: record => this.receiveSurface(record),
        removeSurface: id => {
          if (Number(id) === this.graphicId) this.retire();
        }
      });
      const storage = new LocalStorage({ appWindow: win,
        taskId: this.taskId, ownerWmsWindowId: this.ownerWmsWindowId });
      stage.loadContent('pages/AncoTaskPage', storage, error => {
        if (error && Number(error.code) !== 0) {
          console.error('L410_ANCO_DIALOG_PAGE_FAILED', this.graphicId,
            error.code || 0, error.message || String(error));
          this.retire();
          return;
        }
        if (this.retired) return;
        this.registerStockInputWindow();
        this.refreshTimer = setTimeout(() => {
          this.refreshTimer = null;
          if (!this.retired) this.registerStockInputWindow();
        }, 1500);
        this.ready = true;
        this.applyGeometry();
        console.info('L410_ANCO_DIALOG_MAIN_WINDOW_READY', this.taskId,
          this.graphicId, this.ownerWmsWindowId, this.nativeId);
      });
    } catch (error) {
      console.error('L410_ANCO_DIALOG_START_FAILED', this.graphicId,
        error.code || 0, error.message || String(error));
      this.retire();
    }
  }

  async bindTaskInput() {
    // Graphic callbacks may reach this Ability before the primary task's
    // WindowStage has bound its native ID. Only that ordering race is retried.
    for (let attempt = 0; attempt < 50 && !this.retired; attempt++) {
      const result = taskInputNative.bindTaskWindow(this.taskId,
        this.nativeId);
      if (result === 0) {
        this.inputBound = true;
        return;
      }
      if (result !== -4) throw new Error('dialog input bind failed: ' + result);
      await new Promise(resolve => setTimeout(resolve, 100));
    }
    throw new Error('primary task input window not ready');
  }

  receiveSurface(record) {
    if (record.waylandRole === 'main' &&
        Number(record.wmsParentWindowId) === 0)
      this.rootSurface = record;
    if (Number(record.windowId) === this.graphicId &&
        record.waylandRole === 'dialog' &&
        Number(record.ownerWmsWindowId) === this.ownerWmsWindowId)
      this.record = record;
    this.applyGeometry();
  }

  applyGeometry() {
    if (!this.ready || this.retired || !this.window || !this.record) return;
    const row = this.record;
    const origin = this.mainClientGeometry;
    const root = this.rootSurface;
    const width = Math.round(Number(row.width) || 0);
    const height = Math.round(Number(row.height) || 0);
    const visible = !!row.visible && !!origin && !!root &&
      width >= 16 && height >= 16;
    const x = visible ? Math.round(Number(origin.left) +
      Number(row.x || 0) - Number(root.x || 0)) : 0;
    const y = visible ? Math.round(Number(origin.top) +
      Number(row.y || 0) - Number(root.y || 0)) : 0;
    const signature = visible ? [x, y, width, height].join(':') : 'hidden';
    if (signature === this.desiredGeometry) return;
    this.desiredGeometry = signature;
    this.operation = this.operation.then(async () => {
      if (this.retired || this.desiredGeometry !== signature ||
          this.appliedGeometry === signature) return;
      if (visible) {
        // WindowStage gives us a decorated native main window. Its outer
        // rectangle is larger than the drawable client area consumed by the
        // Wayland surface. Account for that frame in both size and origin.
        const props = this.window.getWindowProperties();
        const frame = props.drawableRect || {};
        const outer = props.windowRect || {};
        const insetX = Math.max(0, Math.round(Number(frame.left) || 0));
        const insetY = Math.max(0, Math.round(Number(frame.top) || 0));
        const extraW = Math.max(0, Math.round((Number(outer.width) || 0) -
          (Number(frame.width) || Number(outer.width) || 0)));
        const extraH = Math.max(0, Math.round((Number(outer.height) || 0) -
          (Number(frame.height) || Number(outer.height) || 0)));
        await this.window.resize(width + extraW, height + extraH);
        await this.window.moveWindowTo(x - insetX, y - insetY);
        if (!this.visible) await this.window.showWindow({ focusOnShow: true });
        // The dialog page does not echo its initial ArkUI area into Wayland:
        // that area can briefly have the parent window's dimensions. Register
        // the native ID using this dialog's actual WMS client geometry here,
        // before a physical click can arrive at the separate native window.
        const routed = taskInputNative.publishChildGeometry(
          this.taskId, this.nativeId, this.graphicId,
          x, y, width, height, insetY);
        if (routed !== 0)
          throw new Error('dialog native geometry route failed: ' + routed);
        console.info('L410_ANCO_DIALOG_NATIVE_ROUTE_READY',
          this.graphicId, this.nativeId, x, y, width, height);
        console.info('L410_ANCO_DIALOG_FRAME_GEOMETRY', this.graphicId,
          x, y, width, height, insetX, insetY, extraW, extraH);
        this.registerStockInputWindow();
      } else if (this.visible) await this.window.hide();
      this.visible = visible;
      this.appliedGeometry = signature;
    }).catch(error => {
      if (this.desiredGeometry === signature) this.desiredGeometry = '';
      console.error('L410_ANCO_DIALOG_LAYOUT_FAILED', this.graphicId,
        error.code || 0, error.message || String(error));
    });
  }

  registerStockInputWindow() {
    try {
      if (!stockFusionInput || !stockProcess || !stockDisplay ||
          typeof stockFusionInput.sendWindowInfo !== 'function')
        throw new Error('stock input module unavailable');
      const props = this.window.getWindowProperties();
      const rect = props.windowRect || {};
      const drawable = props.drawableRect || {};
      const displayId = Number(stockDisplay.getDefaultDisplaySync().id);
      const pid = Number(stockProcess.pid);
      const uid = Number(stockProcess.uid);
      if (!Number.isInteger(displayId) || !Number.isInteger(pid) ||
          pid <= 0 || !Number.isInteger(uid) || uid < 0)
        throw new Error('stock input window identity unavailable');
      const hotArea = { left: Number(rect.left) || 0,
        top: Number(rect.top) || 0, width: Number(rect.width) || 0,
        height: Number(drawable.top) || 0 };
      stockFusionInput.sendWindowInfo(this.nativeId, 1, displayId,
        hotArea, pid, uid, this.taskId);
      console.info('L410_ANCO_DIALOG_STOCK_INPUT_READY', this.taskId,
        this.nativeId);
    } catch (error) {
      console.error('L410_ANCO_DIALOG_STOCK_INPUT_FAILED', this.graphicId,
        error.code || 0, error.message || String(error));
    }
  }

  retire() {
    if (this.retired) return;
    this.retired = true;
    try { this.context.terminateSelf(); }
    catch (error) { console.error('L410_ANCO_DIALOG_RETIRE_FAILED',
      this.graphicId, error.message || String(error)); }
  }

  onDestroy() {
    this.retired = true;
    if (this.unsubscribe) this.unsubscribe();
    if (this.refreshTimer) clearTimeout(this.refreshTimer);
    if (this.closingHandler && this.window) {
      try { this.window.off('windowWillClose', this.closingHandler); }
      catch (error) { }
    }
    if (this.nativeId) {
      try { stockFusionInput?.sendWindowInfo(this.nativeId, 2); }
      catch (error) { console.warn('L410_ANCO_DIALOG_STOCK_REMOVE_FAILED',
        this.graphicId, error.message || String(error)); }
      if (this.inputBound)
        taskInputNative.unbindTaskWindow(this.taskId, this.nativeId);
    }
    console.info('L410_ANCO_DIALOG_DESTROY', this.taskId, this.graphicId);
  }
}
