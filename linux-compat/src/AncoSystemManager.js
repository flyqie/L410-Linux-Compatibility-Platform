/* A system WMS window owns an ArkUI page. Its graphics children are kept
 * separate from task IDs, which can have the same numeric value. */
class SystemManager {
  constructor() { this.windows = new Map(); }

  window(windowId) {
    const id = Number(windowId);
    if (!Number.isInteger(id) || id <= 0) throw new Error('invalid system window ID');
    if (!this.windows.has(id)) this.windows.set(id, {
      surfaces: new Map(), callback: null
    });
    return this.windows.get(id);
  }

  register(windowId, callback) {
    const entry = this.window(windowId);
    entry.callback = callback;
    for (const surface of entry.surfaces.values()) callback.surface(surface);
    return () => { if (entry.callback === callback) entry.callback = null; };
  }

  surface(windowId, surface) {
    const entry = this.window(windowId);
    const id = Number(surface.windowId);
    const record = Object.assign({ x: 0, y: 0, width: 0, height: 0,
      visible: false }, entry.surfaces.get(id) || {}, surface, { windowId: id });
    entry.surfaces.set(id, record);
    if (entry.callback) entry.callback.surface(record);
  }

  command(windowId, graphicId, command, values = {}) {
    const entry = this.windows.get(Number(windowId));
    const id = Number(graphicId);
    if (!entry || !entry.surfaces.has(id)) return;
    if (command === 'remove') {
      entry.surfaces.delete(id);
      if (entry.callback) entry.callback.removeSurface(id);
      return;
    }
    const record = Object.assign({}, entry.surfaces.get(id));
    if (command === 'show') record.visible = true;
    else if (command === 'hide') record.visible = false;
    else if (command === 'layout') {
      for (const key of ['x', 'y', 'width', 'height'])
        if (values[key] !== undefined && Number.isFinite(Number(values[key])))
          record[key] = Number(values[key]);
    } else if (command === 'background') record.color = values.color;
    else return;
    entry.surfaces.set(id, record);
    if (entry.callback) entry.callback.surface(record);
  }

  remove(windowId) {
    const id = Number(windowId);
    const entry = this.windows.get(id);
    if (!entry) return;
    this.windows.delete(id);
    if (entry.callback) for (const graphicId of entry.surfaces.keys())
      entry.callback.removeSurface(graphicId);
    entry.callback = null;
  }

  reset() { for (const id of [...this.windows.keys()]) this.remove(id); }
}

const ancoSystemManager = new SystemManager();
export default ancoSystemManager;
