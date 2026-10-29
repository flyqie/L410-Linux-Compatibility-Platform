class LinuxEntryPage extends ViewPU {
  constructor(parent, params, storage, id = -1) {
    super(parent, storage, id);
    this.finalizeConstruction();
  }

  initialRender() {
    this.observeComponentCreation2(() => {
      Column.create({ space: 18 });
      Column.width('100%');
      Column.height('100%');
      Column.padding(32);
      Column.justifyContent(FlexAlign.Center);
      Column.backgroundColor('#F3F5F8');
    }, Column);
    this.observeComponentCreation2(() => {
      Text.create('Linux 应用启动失败');
      Text.fontSize(22);
      Text.textAlign(TextAlign.Center);
    }, Text);
    Text.pop();
    this.observeComponentCreation2(() => {
      Text.create(LocalStorage.getShared().get('l410EntryError') || '请在 Linux 管理平台查看日志');
      Text.fontSize(14);
      Text.fontColor('#B3261E');
      Text.textAlign(TextAlign.Center);
    }, Text);
    Text.pop();
    Column.pop();
  }

  rerender() { this.updateDirtyElements(); }
}

ViewStackProcessor.StartGetAccessRecordingFor(ViewStackElement.AllocateNewElmtIdForNextComponent());
loadDocument(new LinuxEntryPage(undefined, {}));
ViewStackProcessor.StopGetAccessRecording();
