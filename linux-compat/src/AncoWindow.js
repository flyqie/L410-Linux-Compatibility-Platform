/*
 * Direct AnCo surface page.
 *
 * One instance is one Harmony application window.  The XComponent owns a
 * real RenderService surface; servicebrokerGraphics associates that surface
 * with the guest window id.  No screenshot, framebuffer upload, scaling or
 * host-side re-rendering is involved here.
 */
import graphics from '@hms:virtService.servicebrokerGraphics';

class AncoWindow extends ViewPU {
  constructor(parent, params, storage, id = -1) {
    super(parent, storage, id);
    /* loadContent receives a distinct LocalStorage for each native window,
       as the stock SystemWindow path does.  A process-wide shared storage
       would let one popup bind another popup's Surface id. */
    const pageStorage = storage || LocalStorage.getShared();
    this.windowId = Number(pageStorage.get('ancoWindowId') || 0);
    this.probeSurface = !!pageStorage.get('ancoProbeSurface');
    this.controller = new XComponentController();
    this.surfaceId = '';
    this.bound = false;
    this.engineContext = null;
    this.probeStarted = false;
    this.finalizeConstruction();
  }

  bindSurface() {
    if (this.bound || !this.windowId) return;
    try {
      const surfaceId = String(this.controller.getXComponentSurfaceId() || '');
      if (!/^[1-9][0-9]*$/.test(surfaceId)) {
        console.error('L410_ANCO_XCOMPONENT_INVALID_SURFACE', this.windowId, surfaceId);
        return;
      }
      graphics.setSurfaceByIdNapi(BigInt(surfaceId), this.windowId);
      this.surfaceId = surfaceId;
      this.bound = true;
      console.info('L410_ANCO_XCOMPONENT_BOUND', this.windowId, surfaceId);
      this.maybeRunProbe();
    } catch (error) {
      console.error('L410_ANCO_XCOMPONENT_BIND_FAILED', this.windowId,
        error.code || 0, error.message || String(error));
    }
  }

  maybeRunProbe() {
    if (!this.probeSurface || !this.bound || this.probeStarted ||
        !this.engineContext ||
        typeof this.engineContext.ancoSurfaceProbe !== 'function') return;
    this.probeStarted = true;
    console.info('L410_ANCO_PROBE_ARMED', this.windowId, true, true);
    const result = this.engineContext.ancoSurfaceProbe(this.windowId);
    console.info('L410_ANCO_IN_PROCESS_SURFACE_PROBE', this.windowId, result);
  }

  destroySurface() {
    if (this.surfaceId && graphics &&
        typeof graphics.destroySurfaceByIdNapi === 'function') {
      try {
        /* Original AncoWindowManager.destroySurface uses Window ID.
           The controller Surface ID belongs only to setSurfaceByIdNapi. */
        graphics.destroySurfaceByIdNapi(this.windowId);
        console.info('L410_ANCO_XCOMPONENT_DESTROYED', this.windowId, this.surfaceId);
      } catch (error) {
        console.warn('L410_ANCO_XCOMPONENT_DESTROY_FAILED', this.windowId,
          error.code || 0, error.message || String(error));
      }
    }
    this.bound = false;
    this.surfaceId = '';
  }

  aboutToBeDeleted() {
    this.destroySurface();
    SubscriberManager.Get().delete(this.id__());
    this.aboutToBeDeletedInternal();
  }

  initialRender() {
    this.observeComponentCreation2(() => {
      Stack.create();
      Stack.width('100%');
      Stack.height('100%');
      Stack.backgroundColor('#00000000');
    }, Stack);
    if (this.probeSurface) {
      this.observeComponentCreation2(() => {
        XComponent.create({
          id: 'l410-anco-probe-module-' + this.windowId,
          type: 'surface',
          libraryname: 'l410engine'
        }, '__L410_BUNDLE__/entry');
        XComponent.width(1);
        XComponent.height(1);
        XComponent.opacity(0);
        XComponent.onLoad(context => {
          this.engineContext = context;
          this.maybeRunProbe();
        });
      }, XComponent);
    }
    this.observeComponentCreation2(() => {
      XComponent.create({
        id: 'l410-anco-surface-' + this.windowId,
        type: 'surface',
        controller: this.controller
      });
      XComponent.width('100%');
      XComponent.height('100%');
      XComponent.focusable(false);
      XComponent.onLoad(() => this.bindSurface());
      XComponent.onDestroy(() => {
        this.destroySurface();
        console.info('L410_ANCO_XCOMPONENT_ON_DESTROY', this.windowId);
      });
    }, XComponent);
    Stack.pop();
  }

  rerender() { this.updateDirtyElements(); }
}

ViewStackProcessor.StartGetAccessRecordingFor(ViewStackProcessor.AllocateNewElmetIdForNextComponent());
loadDocument(new AncoWindow(undefined, {}));
ViewStackProcessor.StopGetAccessRecording();
