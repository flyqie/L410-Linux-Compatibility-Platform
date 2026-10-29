/* Per-task activity and SurfaceView coordination, matching the original
 * AncoXcpManager taskMap/appWinCallbackMap split. */
class TaskManager {
  constructor() {
    this.tasks = new Map();
    this.callbacks = new Map();
  }

  task(taskId) {
    const id = Number(taskId);
    if (!Number.isInteger(id) || id <= 0) throw new Error('invalid AnCo task ID');
    if (!this.tasks.has(id)) this.tasks.set(id, {
      activityId: 0, surfaces: new Map(), clientGeometry: null });
    return this.tasks.get(id);
  }

  register(taskId, callback) {
    const id = Number(taskId);
    const task = this.task(id);
    let subscribers = this.callbacks.get(id);
    if (!subscribers) {
      subscribers = new Set();
      this.callbacks.set(id, subscribers);
    }
    subscribers.add(callback);
    if (task.activityId) callback.activity(task.activityId);
    if (task.clientGeometry && typeof callback.clientGeometry === 'function')
      callback.clientGeometry(task.clientGeometry);
    for (const surface of task.surfaces.values()) callback.surface(surface);
    return () => {
      const current = this.callbacks.get(id);
      if (current !== subscribers) return;
      current.delete(callback);
      if (!current.size) this.callbacks.delete(id);
    };
  }

  notify(id, method, ...args) {
    for (const callback of [...(this.callbacks.get(id) || [])]) {
      if (typeof callback[method] === 'function') callback[method](...args);
    }
  }

  activity(taskId, activityId) {
    const id = Number(taskId);
    const task = this.task(id);
    task.activityId = Number(activityId);
    this.notify(id, 'activity', task.activityId);
  }

  publishClientGeometry(taskId, geometry) {
    const id = Number(taskId);
    const task = this.task(id);
    task.clientGeometry = Object.assign({}, geometry);
    this.notify(id, 'clientGeometry', task.clientGeometry);
  }

  removeActivity(taskId, activityId) {
    const id = Number(taskId);
    const task = this.tasks.get(id);
    if (!task || task.activityId !== Number(activityId)) return;
    this.retire(id, task);
  }

  retire(id, task) {
    const subscribers = [...(this.callbacks.get(id) || [])];
    this.tasks.delete(id);
    this.callbacks.delete(id);
    for (const callback of subscribers) {
      // WMS can remove the activity before Graphics emits removeWindow.
      // Retire every SurfaceView while its task page is still registered.
      for (const windowId of task.surfaces.keys()) {
        try { callback.removeSurface(windowId); }
        catch (error) {
          console.error('L410_ANCO_TASK_SURFACE_RETIRE_FAILED', id, windowId,
            error.message || String(error));
        }
      }
      if (task.activityId) {
        try { callback.removeActivity(task.activityId); }
        catch (error) {
          console.error('L410_ANCO_TASK_ACTIVITY_RETIRE_FAILED', id,
            error.message || String(error));
        }
      }
    }
  }

  reset() {
    for (const [id, task] of [...this.tasks]) this.retire(id, task);
  }

  surface(taskId, surface) {
    const id = Number(taskId);
    const task = this.task(id);
    const windowId = Number(surface.windowId);
    if (!Number.isInteger(windowId) || windowId <= 0) throw new Error('invalid surface ID');
    const previous = task.surfaces.get(windowId);
    const record = Object.assign({ x: 0, y: 0, width: 0, height: 0,
      visible: false }, previous || {}, surface, { windowId });
    task.surfaces.set(windowId, record);
    this.notify(id, 'surface', record);
  }

  command(taskId, windowId, command, values = {}) {
    const id = Number(taskId);
    const task = this.tasks.get(id);
    const key = Number(windowId);
    if (!task || !task.surfaces.has(key)) return;
    if (command === 'remove') {
      task.surfaces.delete(key);
      this.notify(id, 'removeSurface', key);
      return;
    }
    if (command === 'zIndex') {
      const ordered = [...task.surfaces.values()]
        .sort((a, b) => Number(a.zIndex || 0) - Number(b.zIndex || 0))
        .filter(item => item.windowId !== key);
      const adjacent = Number(values.adjacentWindowId || 0);
      const index = ordered.findIndex(item => item.windowId === adjacent);
      const below = !!values.isBelow;
      const insertAt = index < 0 ? (below ? 0 : ordered.length) :
        index + (below ? 0 : 1);
      ordered.splice(insertAt, 0, task.surfaces.get(key));
      ordered.forEach((item, position) => {
        const update = Object.assign({}, item, { zIndex: position });
        task.surfaces.set(update.windowId, update);
        this.notify(id, 'surface', update);
      });
      return;
    }
    const prior = task.surfaces.get(key);
    const update = Object.assign({}, prior);
    if (command === 'show') update.visible = true;
    else if (command === 'hide') update.visible = false;
    else if (command === 'layout') {
      for (const name of ['x', 'y', 'width', 'height']) {
        if (values[name] !== undefined && Number.isFinite(Number(values[name])))
          update[name] = Number(values[name]);
      }
    } else if (command === 'background') update.color = values.color;
    else return;
    task.surfaces.set(key, update);
    this.notify(id, 'surface', update);
  }
}

const ancoTaskManager = new TaskManager();
export default ancoTaskManager;
