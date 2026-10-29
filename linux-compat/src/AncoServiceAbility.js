/*
 * Background AnCo coordinator.
 *
 * ShellAssistant_Feature_Anco uses an invisible ServiceExtensionAbility and
 * registers both WMS and graphic lifecycle listeners. WMS carries the task,
 * activity, window type and parent relationship; graphics carries the
 * SurfaceView association. These are separate in the stock implementation.
 */
import graphics from '@hms:virtService.servicebrokerGraphics';
import wms from '@hms:virtService.servicebrokerWms';
// This direct es2abc build cannot resolve @ohos.window as an ES module on
// SP68. The platform window NAPI is available by its registered module name.
const window = requireNapi('window');
import taskManager from './AncoTaskManager';
import systemManager from './AncoSystemManager';
const AncoServiceBase = requireNapi('app.ability.ServiceExtensionAbility');
let ancoServiceHmosFusion = null;
try { ancoServiceHmosFusion = requireNapi('hmos_fusion_manager_napi'); }
catch (error) {
  console.error('L410_ANCO_ORIGINAL_ROIDT_MODULE_FAILED', error.message || String(error));
}
/* The full ets-loader rewrites `import ... from 'lib*.so'` to this exact
 * app-module lookup. Our direct es2abc build skips that rewrite. */
const taskInputNative = requireNapi('l410ancotaskinput', true,
  'com.l410.linux.engine/entry');
let activeService = null;

function safeValue(value) {
  try { return JSON.stringify(value); }
  catch (error) { return String(value); }
}

function note(kind, values) {
  console.info('L410_ANCO_GRAPHIC_CALLBACK', kind, safeValue(values));
  return 0;
}

function noteWms(kind, values) {
  console.info('L410_ANCO_WMS_CALLBACK', kind, safeValue(values));
  return 0;
}

/* Exact callback names come from SP68 ShellAssistant AncoWindowManager's
 * wmsListener object. Do not infer window class from graphic createWindow:
 * the independent WMS stream identifies activity, type, parent and task. */
const wmsListener = {
  createActivity(activityId, taskId) {
    noteWms('createActivity', { activityId, taskId });
    if (activeService) {
      const id = Number(taskId);
      const nextActivity = Number(activityId);
      const previous = activeService.activities.get(id);
      if (previous !== undefined && previous !== nextActivity)
        wmsListener.removeActivity(previous, id);
      activeService.activities.set(id, nextActivity);
      if (id > 0) {
        taskManager.activity(taskId, activityId);
      }
      activeService.flushPendingGraphicWindows();
    }
    return 0;
  },
  removeActivity(activityId, taskId) {
    noteWms('removeActivity', { activityId, taskId });
    if (activeService) {
      const id = Number(taskId);
      const aid = Number(activityId);
      if (activeService.activities.get(id) !== aid) {
        console.warn('L410_ANCO_WMS_STALE_ACTIVITY_REMOVE', id, aid,
          activeService.activities.get(id));
        return 0;
      }
      activeService.activities.delete(id);
      if (id > 0) taskManager.removeActivity(id, activityId);
      for (const [windowId, record] of activeService.wmsWindows) {
        if (record.taskId === id && record.activityId === aid) {
          activeService.wmsWindows.delete(windowId);
          activeService.dropPendingForAssociatedWindow(windowId);
        }
      }
      for (const [graphicId, record] of activeService.graphicWindows) {
        if (record.taskId === id && record.kind === 'task') {
          if (record.role === 'dialog')
            activeService.dialogWmsWindows.delete(record.parentId);
          activeService.graphicWindows.delete(graphicId);
        }
      }
    }
    return 0;
  },
  createWindow(windowId, activityId, windowType, parentWindowId, taskId) {
    noteWms('createWindow', { windowId, activityId, windowType, parentWindowId, taskId });
    if (activeService) {
      const id = Number(windowId);
      const aid = Number(activityId);
      const task = Number(taskId);
      const prior = activeService.wmsWindows.get(id);
      if (prior && prior.activityId === aid && prior.taskId === task &&
          prior.windowType === Number(windowType) &&
          prior.parentWindowId === Number(parentWindowId)) {
        // A repeated create is not a new native window; retain its layout.
        if (task === -1 && Number(windowType) !== 10 &&
            !activeService.systemWindows.has(id))
          activeService.createSystemWindow(id);
        activeService.flushPendingGraphicWindows();
        return 0;
      }
      if (prior) {
        if (prior.taskId === -1)
          activeService.retireSystemDescendants(id);
        for (const [graphicId, record] of [...activeService.graphicWindows]) {
          if (record.parentId === id || record.systemBaseId === id)
            activeService.removeGraphicWindow(graphicId);
        }
        if (prior.taskId === -1 && prior.windowType !== 10)
          activeService.removeSystemWindow(id);
        console.info('L410_ANCO_WMS_WINDOW_ID_REPLACED', id,
          prior.activityId, prior.taskId, aid, task);
      }
      activeService.wmsWindows.set(id, {
        activityId: aid, windowType: Number(windowType),
        parentWindowId: Number(parentWindowId), taskId: task,
        visible: false, layout: null
      });
      if (task === -1 && Number(windowType) !== 10)
        activeService.createSystemWindow(id);
      activeService.flushPendingGraphicWindows();
    }
    return 0;
  },
  removeWindow(windowId, activityId, taskId) {
    noteWms('removeWindow', { windowId, activityId, taskId });
    if (activeService) {
      const id = Number(windowId);
      const record = activeService.wmsWindows.get(id);
      if (!record) return 0;
      if (record.activityId !== Number(activityId) ||
          record.taskId !== Number(taskId)) {
        console.warn('L410_ANCO_WMS_STALE_WINDOW_REMOVE', id,
          activityId, taskId, record.activityId, record.taskId);
        return 0;
      }
      activeService.wmsWindows.delete(id);
      activeService.dropPendingForAssociatedWindow(id);
      if (record.taskId === -1)
        activeService.retireSystemDescendants(id);
      // WMS may retire the parent before Graphics sends removeWindow. A
      // SurfaceView must never outlive the WMS window it is associated with.
      for (const [graphicId, record] of [...activeService.graphicWindows]) {
        if (record.parentId === id || record.systemBaseId === id)
          activeService.removeGraphicWindow(graphicId);
      }
      if (record.taskId === -1 && record.windowType !== 10)
        activeService.removeSystemWindow(id);
    }
    return 0;
  },
  showWindow(windowId, activityId, taskId, reason) {
    noteWms('showWindow', [windowId, activityId, taskId, reason]);
    if (activeService) {
      activeService.updateWmsWindow(windowId, activityId, taskId, 'show');
      try {
        console.info('L410_ANCO_TASK_FOCUS_RESEND', Number(taskId),
          activeService.taskInput?.resendFocus?.() ?? -1);
        console.info('L410_ANCO_TASK_GEOMETRY_RESEND', Number(taskId),
          activeService.taskInput?.resendGeometry?.() ?? -1);
      } catch (error) {
        console.warn('L410_ANCO_TASK_FOCUS_RESEND_FAILED', Number(taskId),
          error.message || String(error));
      }
      activeService.reportInputStatus();
    }
    return 0;
  },
  hideWindow(windowId, activityId, taskId, reason) {
    noteWms('hideWindow', [windowId, activityId, taskId, reason]);
    if (activeService)
      activeService.updateWmsWindow(windowId, activityId, taskId, 'hide');
    return 0;
  },
  updateLayoutParams(windowId, activityId, properties, taskId) {
    noteWms('updateLayoutParams', [windowId, activityId, properties, taskId]);
    if (activeService)
      activeService.updateWmsWindow(windowId, activityId, taskId,
        'layout', properties);
    return 0;
  },
  createDimmer(...args) { return noteWms('createDimmer', args); },
  updateDimmer(...args) { return noteWms('updateDimmer', args); },
  removeDimmer(...args) { return noteWms('removeDimmer', args); },
  setPreferredOrientation(...args) { return noteWms('setPreferredOrientation', args); },
  setSystemBarEnable(...args) { return noteWms('setSystemBarEnable', args); },
  setWakeUpScreen(...args) { return noteWms('setWakeUpScreen', args); },
  setBrightness(...args) { return noteWms('setBrightness', args); },
  setSystemBarAppearance(...args) { return noteWms('setSystemBarAppearance', args); },
  setLayoutFullScreen(...args) { return noteWms('setLayoutFullScreen', args); },
  moveActivityToTop(...args) { return noteWms('moveActivityToTop', args); },
  createLetterBox(...args) { return noteWms('createLetterBox', args); },
  showLetterBox(...args) { return noteWms('showLetterBox', args); },
  hideLetterBox(...args) { return noteWms('hideLetterBox', args); },
  removeLetterBox(...args) { return noteWms('removeLetterBox', args); },
  removeBlurWindow(...args) { return noteWms('removeBlurWindow', args); },
  notifySurfaceViewExist(...args) { return noteWms('notifySurfaceViewExist', args); },
  setEnableLandscapeMultiWindow(...args) {
    return noteWms('setEnableLandscapeMultiWindow', args);
  }
};

const graphicListener = {
  createWindow(windowId, associatedWindowId, windowType, xcpSuffix) {
    note('createWindow', { windowId, associatedWindowId, windowType, xcpSuffix });
    if (activeService) activeService.createWindow(
      windowId, windowType, associatedWindowId, xcpSuffix);
    return 0;
  },
  removeWindow(windowId) {
    note('removeWindow', { windowId });
    if (activeService) activeService.removeGraphicWindow(windowId);
    return 0;
  },
  showWindow(windowId) {
    note('showWindow', { windowId });
    if (activeService) activeService.sendWindowCommand(windowId, 'show', {});
    return 0;
  },
  hideWindow(windowId) {
    note('hideWindow', { windowId });
    if (activeService) activeService.sendWindowCommand(windowId, 'hide', {});
    return 0;
  },
  updateLayoutParams(windowId, reserved, rect) {
    note('updateLayoutParams', { windowId, reserved, rect });
    if (activeService) activeService.sendWindowCommand(windowId, 'layout', rect || {});
    return 0;
  },
  updateWindowZIndex(windowId, reservedA, reservedB, adjacentWindowId, isBelow) {
    note('updateWindowZIndex',
      { windowId, reservedA, reservedB, adjacentWindowId, isBelow });
    if (activeService) activeService.sendWindowCommand(windowId, 'zIndex',
      { adjacentWindowId, isBelow });
    return 0;
  },
  updateWindowBackgroundColor(windowId, reservedA, reservedB, color) {
    note('updateWindowBackgroundColor', { windowId, reservedA, reservedB, color });
    if (activeService) activeService.sendWindowCommand(windowId, 'background', { color });
    return 0;
  }
};

export default class AncoServiceAbility extends AncoServiceBase {
  onCreate(want) {
    console.info('L410_ANCO_SERVICE_CREATE', safeValue(want));
    this.registered = false;
    this.wmsRegistered = false;
    this.activities = new Map();
    this.wmsWindows = new Map();
    this.graphicWindows = new Map();
    this.dialogWmsWindows = new Set();
    this.pendingGraphicWindows = new Map();
    this.systemWindows = new Map();
    this.taskInput = null;
    this.roidtOnly = !!(want && want.parameters &&
      (want.parameters.l410RoidtOnly === true ||
       String(want.parameters.l410RoidtOnly) === 'true'));
    this.originalRoidtTimer = 0;
    activeService = this;
    if (this.roidtOnly) {
      this.originalRoidt(0, 'start');
      this.originalRoidtTimer = setInterval(
        () => this.originalRoidt(0, 'renew'), 60000);
      console.info('L410_ANCO_ORIGINAL_ROIDT_SERVICE_READY');
    }
    try {
      this.taskInput = taskInputNative;
      console.info('L410_ANCO_TASK_MMI_REGISTER', this.taskInput.start());
    } catch (error) {
      console.error('L410_ANCO_TASK_MMI_REGISTER_FAILED',
        error.code || 0, error.message || String(error));
    }
    if (wms && typeof wms.registerWindowLifecycleCallback === 'function') {
      try {
        const result = wms.registerWindowLifecycleCallback(wmsListener);
        this.wmsRegistered = result === true;
        console.info('L410_ANCO_SERVICE_WMS_REGISTERED', safeValue(result));
        if (!this.wmsRegistered) console.error('L410_ANCO_SERVICE_WMS_REJECTED');
      } catch (error) {
        console.error('L410_ANCO_SERVICE_WMS_REGISTER_FAILED',
          error.code || 0, error.message || String(error));
      }
    } else {
      console.error('L410_ANCO_SERVICE_WMS_API_ABSENT');
    }
    if (!graphics || typeof graphics.registerGraphicLifecycleCallback !== 'function') {
      console.error('L410_ANCO_SERVICE_GRAPHICS_API_ABSENT',
        graphics ? Object.keys(graphics).join(',') : 'module unavailable');
      return;
    }
    try {
      const result = graphics.registerGraphicLifecycleCallback(graphicListener);
      this.registered = true;
      console.info('L410_ANCO_SERVICE_GRAPHICS_REGISTERED', safeValue(result));
    } catch (error) {
      console.error('L410_ANCO_SERVICE_GRAPHICS_REGISTER_FAILED',
        error.code || 0, error.message || String(error));
    }
  }

  originalRoidt(state, phase) {
    if (!ancoServiceHmosFusion ||
        typeof ancoServiceHmosFusion.roidtKeepAlive !== 'function') {
      console.error('L410_ANCO_ORIGINAL_ROIDT_ABSENT', phase, state);
      return;
    }
    try {
      const request = ancoServiceHmosFusion.roidtKeepAlive(state);
      if (request && typeof request.then === 'function') {
        request.then(result => console.info(
          'L410_ANCO_ORIGINAL_ROIDT', phase, state, result))
          .catch(error => console.error('L410_ANCO_ORIGINAL_ROIDT_FAILED',
            phase, state, error && error.message ? error.message : String(error)));
      } else {
        console.info('L410_ANCO_ORIGINAL_ROIDT', phase, state, request);
      }
    } catch (error) {
      console.error('L410_ANCO_ORIGINAL_ROIDT_FAILED', phase, state,
        error && error.message ? error.message : String(error));
    }
  }

  reportInputStatus() {
    if (!this.taskInput) return;
    /* The short-lived original-surface probe presents its first frame before
       testing a key. Read once after show and once after the stock uinput tool
       finishes its fixed startup delay, so the later state is observable. */
    for (const delay of [1000, 4000]) {
      setTimeout(() => {
        if (activeService === this)
          console.info('L410_ANCO_TASK_MMI_STATUS',
            safeValue(this.taskInput.status()));
      }, delay);
    }
  }

  resolveSystemBase(windowId) {
    let id = Number(windowId);
    const seen = new Set();
    while (id > 0 && !seen.has(id)) {
      seen.add(id);
      const record = this.wmsWindows.get(id);
      if (!record || record.taskId !== -1) return 0;
      if (record.windowType !== 10) return id;
      id = record.parentWindowId;
    }
    return 0;
  }

  retireSystemDescendants(windowId) {
    const descendants = new Set([Number(windowId)]);
    let changed;
    do {
      changed = false;
      for (const [id, record] of this.wmsWindows) {
        if (record.taskId === -1 && !descendants.has(id) &&
            descendants.has(record.parentWindowId)) {
          descendants.add(id);
          changed = true;
        }
      }
    } while (changed);
    for (const id of descendants) {
      if (id !== Number(windowId)) this.wmsWindows.delete(id);
      this.dropPendingForAssociatedWindow(id);
    }
    for (const [graphicId, record] of [...this.graphicWindows])
      if (descendants.has(record.parentId)) this.removeGraphicWindow(graphicId);
  }

  createSystemWindow(windowId) {
    const id = Number(windowId);
    const wmsRecord = this.wmsWindows.get(id);
    if (!wmsRecord || wmsRecord.taskId !== -1 ||
        wmsRecord.windowType === 10 || this.systemWindows.has(id)) return;
    const state = { wmsRecord, window: null };
    this.systemWindows.set(id, state);
    systemManager.window(id);
    /* The stock AncoWindowManager creates one window for a non-SurfaceView
     * system WMS record. Graphics callbacks attach XComponents inside it. */
    try {
      window.createWindow({ name: 'shell_sys_win_' + id,
        windowType: 8, ctx: this.context }, async (error, ohWindow) => {
        if (error && error.code || !ohWindow) {
          console.error('L410_ANCO_SYSTEM_CREATE_FAILED', id,
            error?.code || 0, error?.message || String(error));
          if (this.systemWindows.get(id) === state) {
            this.systemWindows.delete(id);
            systemManager.remove(id);
          }
          return;
        }
        if (this.systemWindows.get(id) !== state ||
            this.wmsWindows.get(id) !== wmsRecord) {
          try { await ohWindow.destroyWindow(); } catch (_) {}
          return;
        }
        state.window = ohWindow;
        try {
          await ohWindow.resize(1, 1);
          await ohWindow.setWindowFocusable(false);
          await ohWindow.setWindowTouchable(false);
          const storage = new LocalStorage({ taskId: id, isSystem: true,
            appWindow: ohWindow });
          await ohWindow.loadContent('pages/AncoTaskPage', storage);
          if (this.systemWindows.get(id) !== state) return;
          try { await ohWindow.setWindowBackgroundColor('#00000000'); }
          catch (error) { console.warn('L410_ANCO_SYSTEM_BACKGROUND_FAILED', id,
            error?.code || 0); }
          this.applySystemWindow(id);
          this.flushPendingGraphicWindows();
          console.info('L410_ANCO_SYSTEM_READY', id);
        } catch (error) {
          console.error('L410_ANCO_SYSTEM_INIT_FAILED', id,
            error?.code || 0, error?.message || String(error));
          this.removeSystemWindow(id);
        }
      });
    } catch (error) {
      this.systemWindows.delete(id);
      systemManager.remove(id);
      console.error('L410_ANCO_SYSTEM_CREATE_FAILED', id,
        error?.code || 0, error?.message || String(error));
    }
  }

  applySystemWindow(windowId) {
    const id = Number(windowId);
    const state = this.systemWindows.get(id);
    const record = this.wmsWindows.get(id);
    if (!state?.window || !record || state.wmsRecord !== record) return;
    const ohWindow = state.window;
    const run = async () => {
      if (record.layout) {
        await ohWindow.moveWindowTo(record.layout.x, record.layout.y);
        await ohWindow.resize(record.layout.width, record.layout.height);
      }
      if (record.focusable !== undefined)
        await ohWindow.setWindowFocusable(record.focusable);
      if (record.touchable !== undefined)
        await ohWindow.setWindowTouchable(record.touchable);
      if (record.visible) await ohWindow.showWindow();
      else await ohWindow.hide();
    };
    state.operation = (state.operation || Promise.resolve()).then(() => {
      if (this.systemWindows.get(id) === state) return run();
    }).catch(error => console.warn('L410_ANCO_SYSTEM_STATE_FAILED', id,
      error?.code || 0, error?.message || String(error)));
  }

  removeSystemWindow(windowId) {
    const id = Number(windowId);
    const state = this.systemWindows.get(id);
    if (!state) return;
    this.systemWindows.delete(id);
    systemManager.remove(id);
    if (state.window) {
      const ohWindow = state.window;
      (state.operation || Promise.resolve()).then(() => ohWindow.destroyWindow())
        .catch(error => console.warn('L410_ANCO_SYSTEM_DESTROY_FAILED', id,
          error?.code || 0, error?.message || String(error)));
    }
  }

  updateWmsWindow(windowId, activityId, taskId, command, properties) {
    const id = Number(windowId);
    const record = this.wmsWindows.get(id);
    if (!record || record.activityId !== Number(activityId) ||
        record.taskId !== Number(taskId)) {
      console.warn('L410_ANCO_WMS_WINDOW_STATE_MISMATCH', id,
        activityId, taskId, command);
      return;
    }
    if (command === 'show') record.visible = true;
    else if (command === 'hide') record.visible = false;
    else if (command === 'layout') {
      const props = properties?.windowProperties;
      const rect = props?.windowRect;
      if (rect) {
        const layout = {
          x: Number(rect.left), y: Number(rect.top),
          width: Number(rect.width), height: Number(rect.height)
        };
        if (Object.values(layout).some(value => !Number.isFinite(value)) ||
            layout.width <= 0 || layout.height <= 0 ||
            layout.width > 16384 || layout.height > 16384) return;
        record.layout = layout;
      }
      if (record.taskId === -1 && record.windowType !== 10) {
        if (typeof props?.focusable === 'boolean')
          record.focusable = props.focusable;
        if (typeof props?.touchable === 'boolean')
          record.touchable = props.touchable;
      }
      if (!rect && record.focusable === undefined &&
          record.touchable === undefined) return;
    } else return;
    if (record.taskId === -1 && record.windowType !== 10)
      this.applySystemWindow(id);
    if (record.taskId === -1 && record.windowType !== 10 &&
        command === 'layout' && record.layout) {
      for (const [graphicId, graphic] of this.graphicWindows) {
        if (graphic.kind !== 'system' || graphic.systemBaseId !== id ||
            graphic.parentId === id) continue;
        const child = this.wmsWindows.get(graphic.parentId);
        if (child?.layout) systemManager.command(id, graphicId, 'layout', {
          x: child.layout.x - record.layout.x,
          y: child.layout.y - record.layout.y,
          width: child.layout.width, height: child.layout.height
        });
      }
    }
    for (const [graphicId, graphic] of this.graphicWindows) {
      if (graphic.parentId !== id ||
          graphic.taskId !== record.taskId) continue;
      const target = graphic.kind === 'system' ? systemManager : taskManager;
      let layout = graphic.kind === 'task' ?
        this.taskGlobalLayout(id) : record.layout;
      if (command === 'layout' && graphic.kind === 'system' && record.layout) {
        const base = this.wmsWindows.get(graphic.systemBaseId);
        layout = Object.assign({}, record.layout, {
          x: record.layout.x - (base?.layout?.x || 0),
          y: record.layout.y - (base?.layout?.y || 0)
        });
      }
      if (command === 'show') console.info('L410_WMS_TASK_APPLY_BEGIN', graphicId, id);
      if (command !== 'layout' || layout)
        target.command(graphic.kind === 'system' ? graphic.systemBaseId :
          record.taskId, graphicId, command, command === 'layout' ? layout : {});
      if (command === 'show') console.info('L410_WMS_TASK_APPLY_END', graphicId, id);
      console.info('L410_ANCO_WMS_SURFACE_STATE_APPLIED', graphicId,
        id, command, safeValue(record.layout), record.visible);
    }
    if (command === 'layout' && record.taskId > 0) {
      for (const [graphicId, graphic] of this.graphicWindows) {
        if (graphic.kind !== 'task' || graphic.parentId === id ||
            graphic.taskId !== record.taskId ||
            !this.taskDescendsFrom(graphic.parentId, id)) continue;
        const layout = this.taskGlobalLayout(graphic.parentId);
        if (layout) taskManager.command(record.taskId, graphicId,
          'layout', layout);
      }
    }
  }

  taskWindowOwner(windowId) {
    let id = Number(windowId);
    const visited = new Set();
    while (id && !visited.has(id)) {
      visited.add(id);
      const record = this.wmsWindows.get(id);
      if (!record) break;
      // SP68 translates Android LayoutParams.type before the JS callback.
      // The GraphicBroker suffix carries the Wayland role without that loss.
      if (this.dialogWmsWindows.has(id)) return id;
      id = record.parentWindowId;
    }
    return 0;
  }

  taskGlobalLayout(windowId) {
    const own = this.wmsWindows.get(Number(windowId));
    if (!own?.layout) return null;
    let x = Number(own.layout.x) || 0;
    let y = Number(own.layout.y) || 0;
    let parentId = own.parentWindowId;
    const seen = new Set([Number(windowId)]);
    while (parentId && !seen.has(parentId)) {
      seen.add(parentId);
      const parent = this.wmsWindows.get(parentId);
      if (!parent || parent.taskId !== own.taskId) break;
      if (parent.layout) {
        x += Number(parent.layout.x) || 0;
        y += Number(parent.layout.y) || 0;
      }
      parentId = parent.parentWindowId;
    }
    return Object.assign({}, own.layout, { x, y });
  }

  taskDescendsFrom(windowId, ancestorId) {
    let id = Number(windowId);
    const seen = new Set();
    while (id && !seen.has(id)) {
      if (id === Number(ancestorId)) return true;
      seen.add(id);
      id = this.wmsWindows.get(id)?.parentWindowId || 0;
    }
    return false;
  }

  createWindow(windowId, windowType, associatedWindowId, xcpSuffix, probeSurface = false) {
    const id = Number(windowId || 0);
    if (!id) {
      console.error('L410_ANCO_CREATE_REJECTED_INVALID_ID', String(windowId));
      return;
    }
    const parentId = Number(associatedWindowId || 0);
    if (this.graphicWindows.has(id)) {
      console.info('L410_ANCO_CREATE_ALREADY_TRACKED', id,
        safeValue(this.graphicWindows.get(id)));
      return;
    }
    if (!probeSurface) this.pendingGraphicWindows.set(id, {
      windowType, associatedWindowId, xcpSuffix
    });
    const wmsWindow = this.wmsWindows.get(parentId);
    if (!wmsWindow && !probeSurface) {
      /* Stock AncoWindowManager.createWindow returns when getWindowBean for
         associatedWindowId is absent.  A graphics callback alone is not a
         request to manufacture a new application window. */
      console.warn('L410_ANCO_CREATE_WAIT_WMS_WINDOW', id, parentId);
      return;
    }
    if (!wmsWindow) console.info('L410_ANCO_EXPLICIT_SURFACE_PROBE', id);
    const taskId = wmsWindow ? wmsWindow.taskId : 0;
    console.info('L410_ANCO_WINDOW_ASSOCIATION', id, parentId,
      Number(windowType || 0), String(xcpSuffix || ''), safeValue(wmsWindow || null),
      this.activities.has(taskId));
    if (wmsWindow && taskId === -1) {
      const baseId = this.resolveSystemBase(parentId);
      const base = this.wmsWindows.get(baseId);
      if (!base || base.taskId !== -1 || base.windowType === 10 ||
          !this.systemWindows.has(baseId)) {
        console.info('L410_ANCO_CREATE_WAIT_SYSTEM_BASE', id, parentId,
          baseId, Number(windowType || 0));
        return;
      }
      const record = { parentId, systemBaseId: baseId,
        windowType: Number(windowType || 0), taskId, kind: 'system' };
      this.pendingGraphicWindows.delete(id);
      this.graphicWindows.set(id, record);
      const layout = wmsWindow.layout || base.layout;
      systemManager.surface(baseId, {
        windowId: id, associatedWindowId: parentId,
        windowType: record.windowType, xcpSuffix: String(xcpSuffix || ''),
        visible: !!wmsWindow.visible,
        ...(layout ? { x: layout.x - (base.layout?.x || 0),
          y: layout.y - (base.layout?.y || 0),
          width: layout.width, height: layout.height } : {})
      });
      console.info('L410_ANCO_SYSTEM_SURFACE_QUEUED', baseId, id, parentId);
      return;
    }
    if (wmsWindow && this.activities.get(taskId) !== wmsWindow.activityId) {
      /* The stock app-window branch waits for its activity callback before
         creating the task's SurfaceView node. */
      console.info('L410_ANCO_CREATE_WAIT_ACTIVITY', id, taskId,
        wmsWindow.activityId);
      return;
    }
    if (!probeSurface) {
      const suffix = String(xcpSuffix || '');
      const role = suffix.includes('l410-sp68-wayland-dialog') ? 'dialog' :
        suffix.includes('l410-sp68-wayland-popup') ? 'popup' : 'main';
      const record = { parentId, windowType: Number(windowType || 0), taskId,
        kind: 'task', role };
      this.pendingGraphicWindows.delete(id);
      this.graphicWindows.set(id, record);
      if (role === 'dialog') this.dialogWmsWindows.add(parentId);
      taskManager.surface(taskId, {
        windowId: id, activityId: wmsWindow.activityId,
        associatedWindowId: parentId, windowType: Number(windowType || 0),
        wmsWindowType: wmsWindow.windowType, waylandRole: role,
        wmsParentWindowId: wmsWindow.parentWindowId,
        ownerWmsWindowId: this.taskWindowOwner(parentId),
        xcpSuffix: String(xcpSuffix || ''),
        visible: !!wmsWindow.visible,
        ...(this.taskGlobalLayout(parentId) || {})
      });
      console.info('L410_ANCO_TASK_SURFACE_QUEUED', taskId, id, parentId);
      if (role === 'dialog') {
        try {
          const dispatched = this.context.startAbility({
            bundleName: 'com.l410.linux.engine',
            abilityName: 'AncoDialogAbility',
            parameters: { ancoTaskId: taskId, ancoGraphicId: id,
              ancoDialogWmsId: parentId }
          });
          if (dispatched?.catch) dispatched.catch(error =>
            console.error('L410_ANCO_DIALOG_DISPATCH_FAILED', id,
              error.code || 0, error.message || String(error)));
        } catch (error) {
          console.error('L410_ANCO_DIALOG_DISPATCH_FAILED', id,
            error.code || 0, error.message || String(error));
        }
      }
      return;
    }
    const want = {
      bundleName: 'com.l410.linux.engine',
      abilityName: 'AncoWindowAbility',
      parameters: {
        ancoDisplayId: 0,
        ancoWindowId: id,
        ancoTaskId: taskId,
        ancoName: 'Linux',
        ancoWindowType: Number(windowType || 0),
        ancoAssociatedWindowId: parentId,
        ancoXcpSuffix: String(xcpSuffix || ''),
        ancoProbeSurface: !!probeSurface
      }
    };
    const record = { parentId, windowType: Number(windowType || 0), taskId,
      kind: 'task' };
    this.pendingGraphicWindows.delete(id);
    this.graphicWindows.set(id, record);
    try {
      const result = this.context.startAbility(want);
      if (result && result.then) {
        result.then(() => console.info('L410_ANCO_CREATE_DISPATCHED', id))
          .catch(error => {
            if (this.graphicWindows.get(id) === record) {
              this.graphicWindows.delete(id);
              if (!probeSurface) this.pendingGraphicWindows.set(id,
                { windowType, associatedWindowId, xcpSuffix });
            }
            console.error('L410_ANCO_CREATE_DISPATCH_FAILED', id,
              error.code || 0, error.message || String(error));
          });
      }
    } catch (error) {
      if (this.graphicWindows.get(id) === record) {
        this.graphicWindows.delete(id);
        if (!probeSurface) this.pendingGraphicWindows.set(id,
          { windowType, associatedWindowId, xcpSuffix });
      }
      console.error('L410_ANCO_CREATE_DISPATCH_FAILED', id,
        error.code || 0, error.message || String(error));
    }
  }

  removeGraphicWindow(windowId) {
    const id = Number(windowId || 0);
    this.pendingGraphicWindows.delete(id);
    const record = this.graphicWindows.get(id);
    this.graphicWindows.delete(id);
    if (record?.role === 'dialog') this.dialogWmsWindows.delete(record.parentId);
    if (record) {
      if (record.kind === 'task') taskManager.command(record.taskId, id, 'remove');
      else if (record.kind === 'system')
        systemManager.command(record.systemBaseId, id, 'remove');
      else this.sendWindowCommand(id, 'remove', {});
    }
  }

  dropPendingForAssociatedWindow(windowId) {
    const associatedId = Number(windowId);
    for (const [graphicId, pending] of this.pendingGraphicWindows) {
      if (Number(pending.associatedWindowId) === associatedId)
        this.pendingGraphicWindows.delete(graphicId);
    }
  }

  flushPendingGraphicWindows() {
    for (const [graphicId, pending] of [...this.pendingGraphicWindows]) {
      /* createWindow keeps unresolved entries pending. Removing the entry
         before dispatch prevents a repeated WMS notification from opening a
         second UIAbility for the same SurfaceView. */
      this.createWindow(graphicId, pending.windowType,
        pending.associatedWindowId, pending.xcpSuffix);
    }
  }

  sendWindowCommand(windowId, command, property) {
    const id = Number(windowId || 0);
    const record = this.graphicWindows.get(id);
    if (record && (record.kind === 'task' || record.kind === 'system')) {
      const payload = Object.assign({}, property || {});
      if (command === 'layout') {
        if (payload.x === undefined && payload.left !== undefined)
          payload.x = payload.left;
        if (payload.y === undefined && payload.top !== undefined)
          payload.y = payload.top;
        if (payload.width === undefined && payload.right !== undefined &&
            payload.left !== undefined)
          payload.width = Number(payload.right) - Number(payload.left);
        if (payload.height === undefined && payload.bottom !== undefined &&
            payload.top !== undefined)
          payload.height = Number(payload.bottom) - Number(payload.top);
      }
      (record.kind === 'system' ? systemManager : taskManager).command(
        record.kind === 'system' ? record.systemBaseId : record.taskId,
        id, command, payload);
      return;
    }
    try {
      const events = requireNapi('commonEventManager');
      const parameters = Object.assign({}, property || {}, {
        ancoWindowId: Number(windowId || 0), ancoCommand: String(command)
      });
      events.publish('l410_anco_window_command', { parameters }, error => {
        if (error) console.warn('L410_ANCO_COMMAND_PUBLISH_FAILED', windowId, command,
          error.code || 0, error.message || String(error));
      });
    } catch (error) {
      console.warn('L410_ANCO_COMMAND_PUBLISH_FAILED', windowId, command,
        error.code || 0, error.message || String(error));
    }
  }

  onRequest(want, startId) {
    console.info('L410_ANCO_SERVICE_REQUEST', startId, safeValue(want));
    /* Root-only deployment diagnostics can request a real SurfaceView without
       exporting the UI ability.  Production windows still originate from the
       broker lifecycle callback above. */
    const p = want && want.parameters ? want.parameters : {};
    const probeId = Number(p.ancoProbeWindowId || 0);
    if (probeId > 0) {
      this.createWindow(probeId, 0, 0, '', true);
    }
  }

  onConnect(want) {
    console.info('L410_ANCO_SERVICE_CONNECT', safeValue(want));
    return null;
  }

  onDisconnect(want) {
    console.info('L410_ANCO_SERVICE_DISCONNECT', safeValue(want));
  }

  onDestroy() {
    if (this.originalRoidtTimer) {
      clearInterval(this.originalRoidtTimer);
      this.originalRoidtTimer = 0;
    }
    if (this.roidtOnly) this.originalRoidt(1, 'end');
    if (this.taskInput) {
      try { console.info('L410_ANCO_TASK_MMI_STOP', this.taskInput.stop()); }
      catch (error) {
        console.warn('L410_ANCO_TASK_MMI_STOP_FAILED',
          error.code || 0, error.message || String(error));
      }
      this.taskInput = null;
    }
    if (activeService === this) taskManager.reset();
    for (const id of [...this.systemWindows.keys()]) this.removeSystemWindow(id);
    systemManager.reset();
    if (this.wmsRegistered && wms &&
        typeof wms.unregisterWindowLifecycleCallback === 'function') {
      try { wms.unregisterWindowLifecycleCallback(); }
      catch (error) {
        console.error('L410_ANCO_SERVICE_WMS_UNREGISTER_FAILED',
          error.code || 0, error.message || String(error));
      }
    }
    if (this.registered && graphics &&
        typeof graphics.unregisterGraphicLifecycleCallback === 'function') {
      try { graphics.unregisterGraphicLifecycleCallback(); }
      catch (error) {
        console.error('L410_ANCO_SERVICE_GRAPHICS_UNREGISTER_FAILED',
          error.code || 0, error.message || String(error));
      }
    }
    this.registered = false;
    this.wmsRegistered = false;
    this.activities.clear();
    this.wmsWindows.clear();
    this.graphicWindows.clear();
    this.pendingGraphicWindows.clear();
    if (activeService === this) activeService = null;
    console.info('L410_ANCO_SERVICE_DESTROY');
  }
}
