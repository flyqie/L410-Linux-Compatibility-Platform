/* Each page hosts the SurfaceViews assigned to one native window. The task
 * page and decorated transient pages subscribe to the same AnCo task map.
 * Pixels are provided by the original SurfaceDelegator path.
 */
import graphics from '@hms:virtService.servicebrokerGraphics';
import taskManager from '../entryability/AncoTaskManager';
import systemManager from '../entryability/AncoSystemManager';
const taskInputNative = requireNapi('l410ancotaskinput', true,
  'com.l410.linux.engine/entry');
let taskInputMethod = null;
try { taskInputMethod = requireNapi('inputMethod'); }
catch (error) { console.warn('L410_ANCO_TASK_IMF_UNAVAILABLE', error.message); }

/* Keep one XComponent instance per graphics window. ForEach retains a child
 * with the same key without rebuilding it, so push subsequent layout changes
 * into that child's observed state instead of expecting ForEach to do so. */
class AncoSurfaceView extends ViewPU {
  constructor(parent, params, storage, id = -1) {
    super(parent, storage, id);
    this.__surface = new ObservedPropertyObjectPU(params.surface, this, 'surface');
    this.controller = params.controller;
    this.bind = params.bind;
    this.destroy = params.destroy;
    this.taskId = params.taskId;
    this.finalizeConstruction();
  }

  get surface() { return this.__surface.get(); }
  set surface(value) { this.__surface.set(value); }

  purgeVariableDependenciesOnElmtId(id) {
    this.__surface.purgeDependencyOnElmtId(id);
  }

  aboutToBeDeleted() {
    this.__surface.aboutToBeDeleted();
    SubscriberManager.Get().delete(this.id__());
    this.aboutToBeDeletedInternal();
  }

  initialRender() {
    this.observeComponentCreation2(() => {
      const item = this.surface;
      const ui = this.getUIContext();
      XComponent.create({
        id: 'l410-anco-task-' + this.taskId + '-' + item.windowId +
          '-' + String(item.xcpSuffix || ''),
        type: 'surface',
        controller: this.controller
      });
      // WMS window rectangles are physical pixels; ArkUI numeric layout
      // dimensions are vp. Use the same conversion for every surface, so a
      // Wayland pointer coordinate lands on the pixel the client rendered.
      XComponent.width(Math.max(1, ui.px2vp(Number(item.width) || 0)));
      XComponent.height(Math.max(1, ui.px2vp(Number(item.height) || 0)));
      XComponent.position({
        x: ui.px2vp(Number(item.x) || 0),
        y: ui.px2vp(Number(item.y) || 0)
      });
      XComponent.visibility(item.visible && item.width > 0 && item.height > 0 ?
        Visibility.Visible : Visibility.Hidden);
      XComponent.onLoad(() => this.bind(item.windowId, this.controller));
      XComponent.onDestroy(() => this.destroy(item.windowId, this.controller));
    }, XComponent);
  }

  rerender() { this.updateDirtyElements(); }
}

class AncoTaskPage extends ViewPU {
  constructor(parent, params, storage, id = -1) {
    super(parent, storage, id);
    const pageStorage = storage || LocalStorage.getShared();
    this.taskId = Number(pageStorage.get('taskId') || 0);
    this.ownerWmsWindowId = Number(pageStorage.get('ownerWmsWindowId') || 0);
    this.registry = pageStorage.get('isSystem') ? systemManager : taskManager;
    this.appWindow = pageStorage.get('appWindow');
    this.__surfaces = new ObservedPropertyObjectPU([], this, 'surfaces');
    this.controllers = new Map();
    this.surfaceViews = new Map();
    this.surfaceIds = new Map();
    this.surfaceGenerations = new Map();
    this.rawSurfaces = new Map();
    this.unsubscribe = null;
    this.rectListener = null;
    this.clientWidth = 0;
    this.clientHeight = 0;
    this.geometrySignature = '';
    this.imeTimer = null;
    this.imePollCount = 0;
    this.imeRevision = -1;
    this.imeState = null;
    this.imeController = null;
    this.imeAttaching = false;
    this.imeAttached = false;
    this.imeShown = false;
    this.imeDisposed = false;
    this.imeSubscribed = false;
    this.imePreviewSubscribed = false;
    this.finalizeConstruction();
  }

  get surfaces() { return this.__surfaces.get(); }
  set surfaces(value) { this.__surfaces.set(value); }

  aboutToAppear() {
    if (!this.taskId) return;
    if (this.appWindow && typeof this.appWindow.on === 'function') {
      this.rectListener = change => {
        if (!this.ownerWmsWindowId) {
          this.publishClientGeometry();
        } else if ([1, 2, 6].includes(Number(change?.reason))) {
          // Only completed native maximize/recover/drag transactions may
          // reconfigure an xdg child. Initial layout and programmatic
          // resize/move callbacks otherwise echo the guest request forever.
          this.publishClientGeometry(true);
        }
      };
      try { this.appWindow.on('windowRectChange', this.rectListener); }
      catch (error) {
        console.warn('L410_ANCO_TASK_RECT_LISTENER_FAILED', this.taskId,
          error.message || String(error));
        this.rectListener = null;
      }
    }
    this.unsubscribe = this.registry.register(this.taskId, {
      activity: activityId =>
        console.info('L410_ANCO_TASK_ACTIVITY', this.taskId, activityId),
      removeActivity: activityId =>
        console.info('L410_ANCO_TASK_ACTIVITY_REMOVED', this.taskId, activityId),
      surface: record => this.upsertSurface(record),
      removeSurface: windowId => this.removeSurface(windowId)
    });
    if (!this.ownerWmsWindowId)
      this.imeTimer = setInterval(() => this.pollImeState(), 100);
  }

  imeWindowId() {
    try { return Number(this.appWindow.getWindowProperties().id) || 0; }
    catch (error) { return 0; }
  }

  imeCursorInfo() {
    const state = this.imeState || {};
    const rect = this.appWindow?.getWindowProperties()?.windowRect || {};
    let insetY = 0;
    try {
      const clientHeight = this.getUIContext().vp2px(this.clientHeight);
      if (clientHeight > 0)
        insetY = Math.max(0, Number(rect.height || 0) - clientHeight);
    } catch (error) { }
    return {
      left: Math.max(0, Math.round(Number(rect.left ?? rect.x ?? 0) + Number(state.x || 0))),
      top: Math.max(0, Math.round(Number(rect.top ?? rect.y ?? 0) + insetY + Number(state.y || 0))),
      width: Math.max(1, Math.round(Number(state.width || 1))),
      height: Math.max(1, Math.round(Number(state.height || 1)))
    };
  }

  imeLogFailure(label, error) {
    console.warn(label, this.taskId, error?.code || 0,
      error?.message || String(error));
  }

  imeInvoke(name, ...args) {
    const controller = this.imeController;
    if (!controller || typeof controller[name] !== 'function') return;
    try {
      const result = controller[name](...args);
      if (result?.catch) result.catch(error =>
        this.imeLogFailure('L410_ANCO_TASK_IME_' + name + '_FAILED', error));
    } catch (error) {
      this.imeLogFailure('L410_ANCO_TASK_IME_' + name + '_FAILED', error);
    }
  }

  subscribeImePreview() {
    if (this.imePreviewSubscribed) return;
    this.imeController.on('setPreviewText', value => {
      if (this.imeDisposed || !this.imeState?.enabled) return;
      const text = String(value || '');
      console.info('L410_ANCO_TASK_IME_PREVIEW', this.taskId, text.length);
      const result = taskInputNative.publishImePreedit(this.taskId, text);
      if (result !== 0) console.warn('L410_ANCO_TASK_IME_PREVIEW_SEND', result);
    });
    this.imeController.on('finishTextPreview', () => {
      if (!this.imeDisposed && this.imeState?.enabled)
        taskInputNative.publishImePreedit(this.taskId, '');
    });
    this.imePreviewSubscribed = true;
  }

  subscribeImeEditor() {
    if (this.imeSubscribed) return;
    const listen = (event, callback) => {
      try { this.imeController.on(event, callback); }
      catch (error) { this.imeLogFailure('L410_ANCO_TASK_IME_LISTENER_' + event, error); }
    };
    listen('insertText', value => {
      if (this.imeDisposed || !this.imeState?.enabled) return;
      const text = String(value || '');
      console.info('L410_ANCO_TASK_IME_INSERT', this.taskId, text.length);
      if (text) {
        const result = taskInputNative.publishImeText(this.taskId, text);
        if (result !== 0) console.warn('L410_ANCO_TASK_IME_INSERT_SEND', result);
      }
    });
    listen('getLeftTextOfCursor', length => {
      const state = this.imeState || {};
      const text = String(state.surrounding || '');
      const cursor = Math.max(0, Math.min(Number(state.cursor) || 0, text.length));
      return text.slice(Math.max(0, cursor - Math.max(0, Number(length) || 0)), cursor);
    });
    listen('getRightTextOfCursor', length => {
      const state = this.imeState || {};
      const text = String(state.surrounding || '');
      const cursor = Math.max(0, Math.min(Number(state.cursor) || 0, text.length));
      return text.slice(cursor, cursor + Math.max(0, Number(length) || 0));
    });
    listen('getTextIndexAtCursor', () => Number(this.imeState?.cursor || 0));
    listen('sendKeyboardStatus', value =>
      console.info('L410_ANCO_TASK_IME_KEYBOARD', this.taskId, value));
    this.imeSubscribed = true;
  }

  async attachIme() {
    if (this.imeDisposed || this.imeAttaching || this.imeAttached ||
        !this.imeState?.enabled || !taskInputMethod) return;
    const windowId = this.imeWindowId();
    if (!windowId) return;
    this.imeAttaching = true;
    try {
      this.imeController = taskInputMethod.getController();
      this.subscribeImePreview();
      const state = this.imeState;
      await this.imeController.attach(false, {
        inputAttribute: { textInputType: 1, enterKeyType: 8 },
        cursorInfo: this.imeCursorInfo(),
        selection: { start: Number(state.cursor) || 0,
          end: Number(state.anchor) || 0 },
        windowId, newEditBox: true
      });
      this.imeAttached = true;
      this.subscribeImeEditor();
      if (this.imeDisposed || !this.imeState?.enabled) {
        this.imeInvoke('hideTextInput');
        return;
      }
      if (this.imeController.setCallingWindow)
        await this.imeController.setCallingWindow(windowId);
      this.imeInvoke('updateCursor', this.imeCursorInfo());
      this.imeInvoke('changeSelection', String(state.surrounding || ''),
        Number(state.cursor) || 0, Number(state.anchor) || 0);
      if (this.imeController.showTextInput)
        await this.imeController.showTextInput();
      this.imeShown = true;
      const ready = taskInputNative.setImeReady(this.taskId, 1);
      if (ready !== 0) console.warn('L410_ANCO_TASK_IME_ROUTE_FAILED', this.taskId, ready);
      console.info('L410_ANCO_TASK_IME_ATTACHED', this.taskId, windowId);
    } catch (error) {
      taskInputNative.setImeReady(this.taskId, 0);
      this.imeLogFailure('L410_ANCO_TASK_IME_ATTACH_FAILED', error);
      this.imeAttached = false;
    } finally {
      this.imeAttaching = false;
    }
  }

  pollImeState() {
    if (this.imeDisposed || !this.taskId || !taskInputNative?.imeState) return;
    let state;
    try { state = taskInputNative.imeState(this.taskId); }
    catch (error) { this.imeLogFailure('L410_ANCO_TASK_IME_POLL_FAILED', error); return; }
    if (++this.imePollCount <= 50 || this.imePollCount % 50 === 0)
      console.info('L410_ANCO_TASK_IME_POLL', this.taskId, this.imePollCount,
        state?.packets, state?.accepted, state?.rejected, state?.senderUid);
    if (!state || !Number.isInteger(state.revision) ||
        state.revision === this.imeRevision) return;
    this.imeRevision = state.revision;
    this.imeState = state;
    console.info('L410_ANCO_TASK_IME_STATE', this.taskId, state.revision,
      state.enabled, String(state.surrounding || '').length);
    if (!state.enabled) {
      taskInputNative.setImeReady(this.taskId, 0);
      if (this.imeAttached && this.imeShown) this.imeInvoke('hideTextInput');
      this.imeShown = false;
      return;
    }
    if (!this.imeAttached) {
      this.attachIme();
      return;
    }
    this.imeInvoke('updateCursor', this.imeCursorInfo());
    this.imeInvoke('changeSelection', String(state.surrounding || ''),
      Number(state.cursor) || 0, Number(state.anchor) || 0);
    if (!this.imeShown) {
      this.imeShown = true;
      try {
        Promise.resolve(this.imeController.showTextInput()).then(() => {
          if (!this.imeDisposed && this.imeState?.enabled)
            taskInputNative.setImeReady(this.taskId, 1);
        }).catch(error =>
          this.imeLogFailure('L410_ANCO_TASK_IME_SHOW_FAILED', error));
      } catch (error) {
        this.imeLogFailure('L410_ANCO_TASK_IME_SHOW_FAILED', error);
      }
    }
  }

  publishClientGeometry(nativeChildChange = false) {
    if (this.ownerWmsWindowId && !nativeChildChange) return;
    if (!this.taskId || !this.appWindow ||
        this.clientWidth <= 0 || this.clientHeight <= 0 ||
        !taskInputNative || typeof taskInputNative.publishGeometry !== 'function')
      return;
    try {
      const ui = this.getUIContext();
      let width = Math.round(ui.vp2px(this.clientWidth));
      let height = Math.round(ui.vp2px(this.clientHeight));
      const rect = this.appWindow.getWindowProperties().windowRect;
      if (!rect || width < 16 || height < 16) return;
      // The SurfaceView layout is in physical WMS pixels. ArkUI area changes
      // traverse vp and can round one physical pixel down. Reconcile that
      // quantization before requesting a real Wayland/native resize.
      const primary = this.surfaces.find(item => item.visible &&
        Number(item.width) >= 16 && Number(item.height) >= 16 &&
        (!this.ownerWmsWindowId ||
          Number(item.associatedWindowId) === this.ownerWmsWindowId));
      if (!primary) return;
      if (Math.abs(Number(primary.width) - width) <= 1)
        width = Number(primary.width);
      if (Math.abs(Number(primary.height) - height) <= 1)
        height = Number(primary.height);
      // The native window rectangle includes its decorated frame; the Stack
      // is the drawable client. Keep its origin and size in physical pixels.
      const left = Math.round(Number(rect.left ?? rect.x) || 0);
      const insetY = Math.max(0, Math.round((Number(rect.height) || 0) - height));
      const top = Math.round((Number(rect.top ?? rect.y) || 0) + insetY);
      const signature = [left, top, width, height, insetY].join(':');
      if (signature === this.geometrySignature) return;
      this.geometrySignature = signature;
      const result = this.ownerWmsWindowId ?
        taskInputNative.publishChildGeometry(this.taskId, this.imeWindowId(),
          Number(primary.windowId), left, top, width, height, insetY) :
        taskInputNative.publishGeometry(
          this.taskId, left, top, width, height, insetY);
      if (result !== 0)
        console.warn('L410_ANCO_TASK_GEOMETRY_SEND_FAILED', this.taskId,
          result, left, top, width, height);
      else if (!this.ownerWmsWindowId)
        taskManager.publishClientGeometry(this.taskId,
          { left, top, width, height });
    } catch (error) {
      console.warn('L410_ANCO_TASK_GEOMETRY_FAILED', this.taskId,
        error.message || String(error));
    }
  }

  upsertSurface(record) {
    if (Number(record.ownerWmsWindowId || 0) !== this.ownerWmsWindowId)
      return;
    if (this.ownerWmsWindowId) {
      this.rawSurfaces.set(Number(record.windowId), Object.assign({}, record));
      const owner = [...this.rawSurfaces.values()].find(item =>
        Number(item.associatedWindowId) === this.ownerWmsWindowId);
      if (!owner) return;
      for (const raw of this.rawSurfaces.values())
        this.applySurface(Object.assign({}, raw, {
          x: Number(raw.x || 0) - Number(owner.x || 0),
          y: Number(raw.y || 0) - Number(owner.y || 0)
        }));
      return;
    }
    this.applySurface(record);
  }

  applySurface(record) {
    const id = Number(record.windowId);
    if (record.visible)
      console.info('L410_TASK_UPSERT_BEGIN', this.taskId, id,
        Number(record.width), Number(record.height));
    if (!this.controllers.has(id)) {
      this.controllers.set(id, new XComponentController());
      this.surfaceGenerations.set(id, (this.surfaceGenerations.get(id) || 0) + 1);
    }
    const item = Object.assign({}, record, {
      viewKey: String(id) + ':' + this.surfaceGenerations.get(id)
    });
    const child = this.surfaceViews.get(id);
    if (child) child.surface = item;
    if (record.visible) console.info('L410_TASK_UPSERT_CHILD', this.taskId, id);
    const next = this.surfaces.filter(item => item.windowId !== id);
    next.push(item);
    next.sort((a, b) => Number(a.zIndex || 0) - Number(b.zIndex || 0));
    this.surfaces = next;
    if (record.visible) console.info('L410_TASK_UPSERT_LIST', this.taskId, id);
    this.publishClientGeometry();
    if (record.visible) console.info('L410_TASK_UPSERT_END', this.taskId, id);
  }

  removeSurface(windowId) {
    const id = Number(windowId);
    this.rawSurfaces.delete(id);
    const controller = this.controllers.get(id);
    // Detach before removing the ArkUI child. Its onDestroy may arrive after
    // another surface with this graphics ID has already been bound.
    if (controller) this.destroySurface(id, controller);
    this.surfaces = this.surfaces.filter(item => item.windowId !== id);
  }

  bindSurface(windowId, controller) {
    const id = Number(windowId);
    if (!controller || this.controllers.get(id) !== controller ||
        this.surfaceIds.has(controller)) return;
    try {
      const surfaceId = String(controller.getXComponentSurfaceId() || '');
      if (!/^[1-9][0-9]*$/.test(surfaceId)) throw new Error('invalid surface ID');
      graphics.setSurfaceByIdNapi(BigInt(surfaceId), id);
      this.surfaceIds.set(controller, surfaceId);
      console.info('L410_ANCO_TASK_SURFACE_BOUND', this.taskId, id, surfaceId);
    } catch (error) {
      console.error('L410_ANCO_TASK_SURFACE_BIND_FAILED', this.taskId, id,
        error.code || 0, error.message || String(error));
    }
  }

  destroySurface(windowId, controller) {
    const id = Number(windowId);
    const surfaceId = this.surfaceIds.get(controller);
    if (surfaceId && graphics &&
        typeof graphics.destroySurfaceByIdNapi === 'function') {
      /* Original AncoWindowManager.destroySurface passes the graphics window
       * ID here.  The decimal XComponent surface ID is only for the bind. */
      try { graphics.destroySurfaceByIdNapi(id); }
      catch (error) {
        console.warn('L410_ANCO_TASK_SURFACE_DESTROY_FAILED', id,
          error.code || 0, error.message || String(error));
      }
    }
    this.surfaceIds.delete(controller);
    if (this.controllers.get(id) === controller) this.controllers.delete(id);
    if (this.surfaceViews.get(id)?.controller === controller)
      this.surfaceViews.delete(id);
  }

  aboutToBeDeleted() {
    console.info('L410_ANCO_TASK_PAGE_DELETED', this.taskId, this.imePollCount);
    this.imeDisposed = true;
    if (!this.ownerWmsWindowId && taskInputNative?.setImeReady)
      taskInputNative.setImeReady(this.taskId, 0);
    if (this.imeTimer) clearInterval(this.imeTimer);
    if (this.imeAttached) this.imeInvoke('hideTextInput');
    if (this.rectListener && this.appWindow) {
      try { this.appWindow.off('windowRectChange', this.rectListener); }
      catch (error) { }
      this.rectListener = null;
    }
    if (this.unsubscribe) this.unsubscribe();
    for (const child of this.surfaceViews.values())
      this.destroySurface(child.surface.windowId, child.controller);
    this.surfaceViews.clear();
    this.surfaceGenerations.clear();
    this.__surfaces.aboutToBeDeleted();
    SubscriberManager.Get().delete(this.id__());
    this.aboutToBeDeletedInternal();
  }

  initialRender() {
    this.observeComponentCreation2(() => {
      Stack.create();
      Stack.width('100%');
      Stack.height('100%');
      Stack.backgroundColor('#00000000');
      Stack.onAreaChange((_, area) => {
        this.clientWidth = Number(area.width) || 0;
        this.clientHeight = Number(area.height) || 0;
        this.publishClientGeometry();
      });
    }, Stack);
    this.observeComponentCreation2((elmtId) => {
      ForEach.create();
      const createSurface = item => {
        this.observeComponentCreation2((childId, isInitialRender) => {
          if (isInitialRender) {
            const child = new AncoSurfaceView(this, {
              surface: item,
              controller: this.controllers.get(item.windowId),
              taskId: this.taskId,
              bind: (windowId, controller) => this.bindSurface(windowId, controller),
              destroy: (windowId, controller) => this.destroySurface(windowId, controller)
            }, undefined, childId);
            this.surfaceViews.set(item.windowId, child);
            ViewPU.create(child);
          } else {
            const child = this.surfaceViews.get(item.windowId);
            if (child) child.surface = Object.assign({}, item);
          }
        }, { name: 'AncoSurfaceView' });
      };
      this.forEachUpdateFunction(elmtId, this.surfaces, createSurface,
        item => item.viewKey, false, false);
    }, ForEach);
    ForEach.pop();
    Stack.pop();
  }

  rerender() { this.updateDirtyElements(); }
}

ViewStackProcessor.StartGetAccessRecordingFor(
  ViewStackProcessor.AllocateNewElmetIdForNextComponent());
loadDocument(new AncoTaskPage(undefined, {}));
ViewStackProcessor.StopGetAccessRecording();
