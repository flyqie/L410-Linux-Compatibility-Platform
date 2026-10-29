/*
 * L410 兼容引擎：宿主侧 Ability（对应 anco 的 ShellAssistant + 系统 broker 的宿主角色）。
 *
 * 职责：
 *   1. 主窗口加载宿主页，从它的 XComponent 拿到引擎通道模块的导出 → 绑定 l410-engine-<pid>，
 *      把端点写进 files/engine.json 供启动器读取（推送式监听，不轮询）；
 *   2. 容器侧每要求一个窗口，就在这里建一个真正的鸿蒙子窗口（每窗口一个 XComponent =
 *      宿主合成树里的一个节点）；
 *   3. 窗口就绪/几何变化/输入/关闭都转成窗口级消息发给容器；
 *   4. 通道断开时收掉全部窗口。
 *
 * 消息格式：compat-engine/protocol/l410_window_channel.h 与 l410_input_channel.h。
 */
/* 原生模块直接 import（不用 XComponent、不用页面）：
   卓易通那个壳本身没有窗口，引擎通道也不该靠一个可见窗口里的组件去加载。 */
let engineHostNative = null;
try {
  engineHostNative = requireNapi('l410engine', true,
    'com.l410.linux.engine/entry');
}
catch (error) { engineHostNative = null; }
/* 卓易通原版的 Roidt 保活必须由白名单 HAP 本身发起。服务端会按
   Binder 调用 UID 查询 BundleInfo，再核对 Bundle + AppIdentifier；
   Linux runtime 或 native broker 代发都会被原版 CheckPermission 拒绝。 */
let engineHostHmosFusion = null;
try { engineHostHmosFusion = requireNapi('hmos_fusion_manager_napi'); }
catch (error) {
  console.error('L410_ENGINE_ORIGINAL_ROIDT_MODULE_FAILED', error.message || String(error));
  engineHostHmosFusion = null;
}
const engineHostUiAbility = requireNapi('app.ability.UIAbility');
const engineHostFs = requireNapi('file.fs');
const engineHostDisplay = requireNapi('display');
const engineHostPasteboard = requireNapi('pasteboard');
 const engineHostWindow = requireNapi('window');
const engineHostAbilityAccess = requireNapi('abilityAccessCtrl');

/* 字符 → Linux 按键码（宿主按真实键盘事件下发；空格在协议里用 0x1f 带过去）。 */
const engineHostLetterRows = ['qwertyuiop', 'asdfghjkl', 'zxcvbnm'];
const engineHostLetterCodes = [[16, 17, 18, 19, 20, 21, 22, 23, 24, 25], [30, 31, 32, 33, 34, 35, 36, 37, 38],
  [44, 45, 46, 47, 48, 49, 50]];
const engineHostShifted = { '!': '1', '@': '2', '#': '3', '$': '4', '%': '5', '^': '6', '&': '7', '*': '8', '(': '9', ')': '0',
  '_': '-', '+': '=', ':': ';', '"': "'", '<': ',', '>': '.', '?': '/', '~': '`' };
const engineHostPunctuation = { '.': 52, ',': 51, '/': 53, '-': 12, '=': 13, ';': 39, "'": 40, '[': 26, ']': 27, '\\': 43, '`': 41 };

function engineHostKeyFor(character) {
  const escaped = character === ' ' ? '\u001f' : character;
  if (character === ' ')
    return { code: 57, mods: 0, text: escaped };
  if (character >= '0' && character <= '9')
    return { code: character === '0' ? 11 : Number(character) + 1, mods: 0, text: escaped };
  const shifted = engineHostShifted[character];
  if (shifted !== undefined) {
    const base = engineHostKeyFor(shifted);
    return base ? { code: base.code, mods: 1, text: escaped } : null;
  }
  if (engineHostPunctuation[character] !== undefined)
    return { code: engineHostPunctuation[character], mods: 0, text: escaped };
  const lower = character.toLowerCase();
  for (let row = 0; row < engineHostLetterRows.length; row++) {
    const at = engineHostLetterRows[row].indexOf(lower);
    if (at >= 0)
      return { code: engineHostLetterCodes[row][at], mods: character === lower ? 0 : 1, text: escaped };
  }
  return null;
}

/* HarmonyOS KeyCode is a public, stable key namespace (letters start at
   2017).  The Linux toolkits on the other side consume evdev codes.  Passing
   the former through happened to produce messages, but no usable Qt key. */
const engineHostOhosToEvdev = {
  2012: 103, 2013: 108, 2014: 105, 2015: 106,
  2043: 51, 2044: 52, 2045: 56, 2046: 100, 2047: 42, 2048: 54,
  2049: 15, 2050: 57, 2054: 28, 2055: 14, 2056: 41, 2057: 12,
  2058: 13, 2059: 26, 2060: 27, 2061: 43, 2062: 39, 2063: 40,
  2064: 53, 2068: 104, 2069: 109, 2070: 1, 2071: 111, 2072: 29,
  2073: 97, 2074: 58, 2076: 125, 2077: 126, 2081: 102, 2082: 107,
  2083: 110
};
const engineHostLetterEvdev = [30, 48, 46, 32, 18, 33, 34, 35, 23, 36, 37, 38,
  50, 49, 24, 25, 16, 19, 31, 20, 22, 47, 17, 45, 21, 44];

export default class EngineHostAbility extends engineHostUiAbility {
  onCreate(want) {
    console.info('L410_ENGINE_ONCREATE', 'engine host ability created');
    try {
      const bm0 = requireNapi('bundle.bundleManager');
      const info0 = bm0.getBundleInfoForSelfSync(0x00000040 | 0x00000080);
      console.info('L410_ENGINE_SELF_INFO_EARLY', JSON.stringify({
        name: info0.name,
        appIdentifier: info0.appInfo ? info0.appInfo.appIdentifier : null,
        appId: info0.appInfo ? info0.appInfo.appId : null,
        signature: info0.signatureInfo ? info0.signatureInfo.appIdentifier : null
      }));
    } catch (error) { console.error('L410_ENGINE_SELF_INFO_EARLY_FAILED', error.message || String(error)); }
    globalThis.l410EngineAbility = this;
    this.api = null;
    this.endpoint = '';
    this.windows = new Map();       /* windowId -> { window, endpoint } */
    this.launchedApps = new Set();
    this.nextWindowId = 0;
    /* 容器里每个应用都从 1 开始给自己的窗口编号，宿主给每个窗口发一个全局唯一的宿主编号，
       对容器说话时仍旧用它自己的编号：两个应用同时开也不会互相顶掉窗口。 */
    this.nextHostWindowId = 0;
    this.originalRoidtTimer = 0;
    this.originalRoidtReady = false;
    this.pendingManagerStage = null;
    this.decorHeight = 0;      /* 系统标题栏占的高度（设备像素），量一次记住 */
    this.clientIndex = new Map();      /* "<pid>:<容器窗口号>" -> 宿主窗口号 */
    console.info('L410_ENGINE_ABILITY_CREATED');
    this.originalRoidt(0, 'start');
    /* 原版服务在 START 后设置 300 秒超时；在超时前用同一个原版
       START 事务续租，调用身份始终是当前白名单 HAP。 */
    this.originalRoidtTimer = setInterval(() => this.originalRoidt(0, 'renew'), 240000);
    this.noteWant(want);
  }

  originalRoidt(state, phase) {
    if (!engineHostHmosFusion || typeof engineHostHmosFusion.roidtKeepAlive !== 'function') {
      console.error('L410_ENGINE_ORIGINAL_ROIDT_ABSENT', phase, state);
      return;
    }
    try {
      const request = engineHostHmosFusion.roidtKeepAlive(state);
      if (request && typeof request.then === 'function') {
        request.then((result) => {
          console.info('L410_ENGINE_ORIGINAL_ROIDT', phase, state, result);
          this.finishOriginalRoidtStart(phase, state);
        })
          .catch((error) => console.error('L410_ENGINE_ORIGINAL_ROIDT_FAILED', phase, state,
            error && error.message ? error.message : String(error)));
      } else {
        console.info('L410_ENGINE_ORIGINAL_ROIDT', phase, state, request);
        this.finishOriginalRoidtStart(phase, state);
      }
    } catch (error) {
      console.error('L410_ENGINE_ORIGINAL_ROIDT_FAILED', phase, state,
        error && error.message ? error.message : String(error));
    }
  }

  finishOriginalRoidtStart(phase, state) {
    if (phase !== 'start' || state !== 0 || this.originalRoidtReady) return;
    this.originalRoidtReady = true;
    if (!this.pendingManagerStage) return;
    const stage = this.pendingManagerStage;
    this.pendingManagerStage = null;
    this.suppressManagerWindow(stage);
  }

  onNewWant(want) {
    const newWantParams = (want && want.parameters) || {};
    if (newWantParams.l410AppWindowEvent) {
      try { this.onAppWindowEvent(want); }
      catch (error) { console.error('L410_ENGINE_APPWIN_EVENT_FAILED', error.message || String(error)); }
      return;
    }
    if (newWantParams.l410AppWindowCommand) {
      console.info('L410_ENGINE_APPWIN_COMMAND', newWantParams.l410HostWindowId,
        newWantParams.l410AppWindowCommand);
      return;
    }
    /* 重复点击启动器入口：把已有窗口前置，不重复起进程。 */
    this.noteWant(want);
  }

  onWindowStageCreate(stage) {
    this.stage = stage;
    /* 壳不显示任何界面：原生模块直接 import 成功就不加载页面、不需要任何可见窗口
       （卓易通那个 hap 也只是壳，没有自己的窗口）。拿不到模块时才退回页面方式。 */
    if (engineHostNative) {
      console.info('L410_ENGINE_NATIVE_DIRECT', typeof engineHostNative.start);
      try { this.onEngineApi(engineHostNative); }
      catch (error) { console.error('L410_ENGINE_NATIVE_DIRECT_FAILED', error.message || String(error)); }
      /* The broker is internal infrastructure, not an application window.
         Keep its unavoidable UIAbility shell permanently out of focus and
         minimized.  Runtime window events never start this Ability again. */
      if (this.originalRoidtReady) {
        this.suppressManagerWindow(stage);
      } else {
        this.pendingManagerStage = stage;
        try {
          const hostWindow = stage.getMainWindowSync();
          if (hostWindow.setWindowBackgroundColor) hostWindow.setWindowBackgroundColor('#00000000');
          hostWindow.setWindowFocusable(false).catch(() => { });
          hostWindow.setWindowTouchable(false).catch(() => { });
        } catch (error) { }
        console.info('L410_ENGINE_BROKER_WINDOW_WAITS_FOR_ORIGINAL_ROIDT');
      }
      return;
    }
    console.warn('L410_ENGINE_NATIVE_DIRECT_ABSENT');
    /* 主窗口只放宿主页：它承载引擎通道模块（1×1 的 XComponent），不显示 Linux 画面 */
    const storage = new LocalStorage({ linuxWindowId: 0, manager: true });
    stage.loadContent('pages/EngineWindow', storage)
      .then(() => {
        console.info('L410_ENGINE_MANAGER_PAGE_LOADED');
        /* 壳不能占焦点：实测应用窗口 focused 一直是 0，就是壳自己的窗口握着焦点。
           卓易通那个 hap 干脆没有窗口；这里等效处理——不可聚焦 + 立即最小化，
           让它永远不参与焦点竞争、也不挡任何东西。 */
        try {
          const hostWindow = stage.getMainWindowSync();
          hostWindow.setWindowFocusable(false)
            .then(() => console.info('L410_ENGINE_HOST_NOT_FOCUSABLE'))
            .catch((error) => console.warn('L410_ENGINE_HOST_NOT_FOCUSABLE_FAILED', error.message));
          hostWindow.setWindowTouchable(false).catch(() => { });
          if (hostWindow.minimize) {
            hostWindow.minimize().then(() => console.info('L410_ENGINE_HOST_MINIMIZED_EARLY'));
          }
        } catch (error) { console.warn('L410_ENGINE_HOST_WINDOW_SETUP_FAILED', error.message); }
      })
      .catch(error => console.error('L410_ENGINE_MANAGER_PAGE_FAILED', error.code || 0, error.message));
  }

  /* 宿主页把引擎模块导出送过来后，通道才可用 */
  onEngineApi(api) {
    if (!api || typeof api.start !== 'function') {
      console.error('L410_ENGINE_API_INVALID');
      return;
    }
    if (this.api) return;
    this.api = api;
    const endpoint = api.start();
    const listening = api.setListener(line => this.onEngineLine(line));
    this.endpoint = endpoint;
    /* UI 桥自测：同 VM 连自己的桥再发一行，日志里应出现 L410_ENGINE_UIINPUT。 */
    try {
      if (api.uiEndpoint && api.uiConnect && api.uiSend) {
        const bridge = api.uiEndpoint();
        const connected = bridge ? api.uiConnect(bridge) : false;
        const sent = connected ? api.uiSend('UITEST bridge alive') : false;
        console.info('L410_ENGINE_UI_BRIDGE', bridge || '', connected ? 1 : 0, sent ? 1 : 0);
      }
    } catch (error) { console.error('L410_ENGINE_UI_BRIDGE_FAILED', error.message); }
    /* 问 BMS：系统认为我们是谁（AnCo 白名单要按 appIdentifier 对得上） */
    try {
      const bundleManager = requireNapi('bundle.bundleManager');
      if (bundleManager && bundleManager.getBundleInfoForSelfSync) {
        const flags = 0x00000040 | 0x00000080;   // WITH_APPLICATION | WITH_SIGNATURE_INFO
        const info = bundleManager.getBundleInfoForSelfSync(flags);
        console.info('L410_ENGINE_SELF_INFO', JSON.stringify({
          name: info.name, appId: info.appInfo ? info.appInfo.appId : null,
          appIdentifier: info.appInfo ? info.appInfo.appIdentifier : null,
          signatureAppIdentifier: info.signatureInfo ? info.signatureInfo.appIdentifier : null,
          fingerprint: info.signatureInfo ? info.signatureInfo.fingerprint : null
        }));
      } else {
        console.warn('L410_ENGINE_SELF_INFO_API_ABSENT');
      }
    } catch (error) { console.error('L410_ENGINE_SELF_INFO_FAILED', error.message || String(error)); }

    /* Broker capability is exposed for diagnostics.  The former startup probe
       created and destroyed a throw-away surface on every launch, which could
       produce a visible black flash.  Real surfaces are now created only for
       Linux windows after their Harmony window id is known. */
    try {
      if (api.ancoProbe) console.info('L410_ENGINE_ANCO_PROBE', api.ancoProbe());
      console.info('L410_ENGINE_ANCO_EXPORTS', typeof api.ancoProbe, typeof api.ancoWindowProbe, typeof api.ancoSurfaceProbe);
      console.info('L410_ENGINE_BROKER_EXPORTS', typeof api.brokerWindowCreate,
        typeof api.brokerWindowDestroy, typeof api.brokerWindowStatus);
      if (api.ancoLifecycleStart) {
        console.info('L410_ENGINE_ANCO_LIFECYCLE_REGISTER', api.ancoLifecycleStart());
      }
      /* One-shot, host-only protocol probe.  It is enabled only by a sandbox
         marker, removes that marker before calling the broker, and tears the
         node down after the lifecycle callback has had time to arrive. */
      /* The former marker probe passed a fabricated non-zero SurfaceNode id.
         The broker treats every non-zero id as a real RenderService node and
         dereferences it; that correctly crashed this process during ABI
         discovery.  Never synthesize ids here.  Real ids must originate in
         the guest compositor/SurfaceFlinger path. */
      const queueProbe = this.context.filesDir + '/probe-direct-queue';
      if (engineHostFs.accessSync(queueProbe) && api.ancoDirectQueueProbe) {
        engineHostFs.unlinkSync(queueProbe);
        console.info('L410_ENGINE_DIRECT_QUEUE_PROBE', api.ancoDirectQueueProbe());
      }
    } catch (error) { console.error('L410_ENGINE_ANCO_PROBE_FAILED', error.message || String(error)); }
    console.info('L410_ENGINE_CHANNEL', endpoint, 'listener', listening);
    this.report(endpoint);
    /* 容器入口在应用沙箱里可见吗？（拉起应用失败时第一件要查的事） */
    try { console.info('L410_ENGINE_SANDBOX_PATHS', api.envProbe ? api.envProbe() : 'no-probe'); } catch (error) { }
    if (this.pendingApp) this.launchApp(this.pendingApp);
    /* 键盘到底被投给哪个窗口？主窗口也注册一个过滤器，看谁的回调响。 */
    try {
      const mainId = this.stage.getMainWindowSync().getWindowProperties().id;
      this.mainWindowNumericId = mainId;
      if (mainId && api.keyFilterProbe) {
        console.info('L410_ENGINE_KEY_FILTER_MAIN', mainId, api.keyFilterProbe(mainId));
      }
    } catch (error) { console.error('L410_ENGINE_KEY_FILTER_MAIN_FAILED', error.message); }
    /* 宿主侧窗口级输入：键盘事件由宿主直接取（anco 的做法），不依赖控件焦点。 */
    try {
      if (api.inputMonitor) console.info('L410_ENGINE_INPUT_MONITOR', api.inputMonitor());
    } catch (error) { console.error('L410_ENGINE_INPUT_MONITOR_FAILED', error.message); }
    /* 验收脚本也可能要由宿主自己先把应用拉起来（那时还没有任何窗口），
       所以通道一起来就看一眼有没有脚本，不等第一个窗口。 */
    setTimeout(() => this.maybeSelfTest(0), 4000);
  }

  report(endpoint) {
    try {
      const path = this.context.filesDir + '/engine.json';
      const file = engineHostFs.openSync(path, engineHostFs.OpenMode.READ_WRITE | engineHostFs.OpenMode.CREATE | engineHostFs.OpenMode.TRUNC);
      engineHostFs.writeSync(file.fd, JSON.stringify({ endpoint: endpoint, bundle: '__L410_BUNDLE__' }));
      engineHostFs.closeSync(file);
      console.info('L410_ENGINE_REPORTED', path);
    } catch (error) {
      console.error('L410_ENGINE_REPORT_FAILED', error.code || 0, error.message);
    }
  }

  send(line, pid) {
    if (this.api) {
      try { return pid ? !!this.api.send(line, pid) : !!this.api.send(line); } catch (error) { return false; }
    }
    return false;
  }

  /* 窗口级消息只发给拥有这个窗口的那个应用（其它应用不认这个窗口 id）。
     真实窗口路径里容器进程号来自 WINDOW_CREATE 的 pid 字段，未必等于模块登记的 peer pid；
     定向发不出去就退回广播——应用按窗口 id 认消息，广播不会认错。 */
  /* OHOS 窗口 id → 宿主窗口号（窗口级输入事件是按 OHOS 窗口 id 投递的）。 */
  windowForOsId(osId) {
    const key = Number(osId);
    if (!key || !this.osWindowIndex) return 0;
    return this.osWindowIndex.get(key) || 0;
  }

  sendToWindow(id, line) {
    const item = this.windows.get(id);
    const pid = item && item.pid ? item.pid : 0;
    /* 真实窗口路径里容器进程号来自 WINDOW_CREATE 的 pid 字段，实测和模块登记的 peer pid 对不上
       （日志：peers 1、accepted 0）。窗口消息只对该窗口的拥有者有含义，应用会忽略不是自己窗口 id 的
       消息，所以这条路径直接广播，不再猜 pid。 */
    const broadcast = !!(item && item.real);
    let accepted = this.send(line, broadcast ? 0 : pid);
    if (!accepted && pid && !broadcast) {
      accepted = this.send(line, 0);
      console.info('L410_ENGINE_SEND_BROADCAST', id, 'pid', pid, accepted ? 1 : 0);
    }
    return accepted;
  }

  /* 容器自己认的窗口编号（每个应用都从 1 开始，所以对它们说话要用它们自己的号）。 */
  clientIdOf(id) {
    const item = this.windows.get(id);
    return item ? item.clientId : id;
  }

  suppressManagerWindow(stage) {
    try {
      const hostWindow = stage.getMainWindowSync();
      try { if (hostWindow.setWindowBackgroundColor) hostWindow.setWindowBackgroundColor('#00000000'); }
      catch (error) { }
      try { hostWindow.setWindowFocusable(false).catch(() => { }); } catch (error) { }
      try { hostWindow.setWindowTouchable(false).catch(() => { }); } catch (error) { }
      try {
        if (hostWindow.minimize) hostWindow.minimize()
          .then(() => console.info('L410_ENGINE_BROKER_WINDOW_SUPPRESSED'))
          .catch(error => console.warn('L410_ENGINE_BROKER_WINDOW_SUPPRESS_FAILED', error.message));
      } catch (error) { }
    } catch (error) {
      console.warn('L410_ENGINE_BROKER_WINDOW_SETUP_FAILED', error.message || String(error));
    }
  }

  /* Engine messages carry the toolkit-local id.  Host maps are keyed by our
     globally unique id, otherwise a second application or its popup can
     accidentally operate on the first application's window with the same
     local number. */
  hostIdForClient(pid, clientId) {
    return this.clientIndex.get(String(pid || 0) + ':' + String(clientId)) || 0;
  }

  ohosKeyToEvdev(rawCode) {
    const code = Number(rawCode) || 0;
    /* Internal IME/self-test messages already use evdev. */
    if (code > 0 && code < 1000) return code;
    if (code >= 2000 && code <= 2009) return code === 2000 ? 11 : code - 1999;
    if (code >= 2017 && code <= 2042) return engineHostLetterEvdev[code - 2017];
    return engineHostOhosToEvdev[code] || 0;
  }

  routeKey(target, action, rawCode, mods, text) {
    const code = this.ohosKeyToEvdev(rawCode);
    if (!target || !code) return false;
    const normalizedAction = action === 'up' || action === '1' ? 'up' : 'down';
    if (!this.pressedModifiers) this.pressedModifiers = 0;
    const modifierBit = (code === 42 || code === 54) ? 1
      : (code === 29 || code === 97) ? 2
        : (code === 56 || code === 100) ? 4
          : (code === 125 || code === 126) ? 8 : 0;
    if (modifierBit && normalizedAction === 'down') this.pressedModifiers |= modifierBit;
    const effectiveMods = (Number(mods) || 0) | this.pressedModifiers;
    const stamp = Date.now();
    const signature = target + ':' + normalizedAction + ':' + code;
    if (!this.keySeen) this.keySeen = new Map();
    const last = this.keySeen.get(signature) || 0;
    if (stamp - last < 45) return false;
    this.keySeen.set(signature, stamp);
    this.sendToWindow(target, 'INPUT_KEY id=' + this.clientIdOf(target) +
      ' action=' + normalizedAction + ' code=' + code + ' mods=' + effectiveMods +
      (text ? ' text=' + text : ''), target);
    console.info('L410_ENGINE_KEY_ROUTED', target, normalizedAction, code, 'raw', rawCode);
    if (modifierBit && normalizedAction === 'up') this.pressedModifiers &= ~modifierBit;
    return true;
  }

  routePointer(target, action, x, y, button) {
    if (!target) return false;
    const normalizedAction = action === 'down' || action === 'up' ? action : 'move';
    const px = Math.round(Number(x) || 0);
    const py = Math.round(Number(y) || 0);
    const pb = Number(button) || (normalizedAction === 'move' ? 0 : 1);
    const stamp = Date.now();
    /* ArkUI reports one physical click through both onMouse and onTouch.  A
       double down/up breaks Qt grabs and made menus appear stuck or displaced. */
    const signature = target + ':' + normalizedAction + ':' + px + ':' + py + ':' + pb;
    if (!this.pointerSeen) this.pointerSeen = new Map();
    const last = this.pointerSeen.get(signature) || 0;
    const threshold = normalizedAction === 'move' ? 8 : 24;
    if (stamp - last < threshold) return false;
    this.pointerSeen.set(signature, stamp);
    if (normalizedAction === 'down') this.inputWindowId = target;
    this.sendToWindow(target, 'INPUT_POINTER id=' + this.clientIdOf(target) +
      ' action=' + normalizedAction + ' x=' + px + ' y=' + py + ' button=' + pb, target);
    if (normalizedAction === 'down') console.info('L410_ENGINE_POINTER_ROUTED', target, px, py);
    return true;
  }

  /* ---------------------------------------------------------------- 引擎消息 */

  onEngineLine(raw) {
    try {
      /* 一个容器进程一条连接：消息前缀是来源进程号，回消息时按 pid 找对人。 */
      const split = raw.indexOf(' ');
      const pid = Number(raw.slice(0, split));
      const line = split > 0 ? raw.slice(split + 1) : raw;
      if (!Number.isFinite(pid)) {
        console.error('L410_ENGINE_LINE_MALFORMED', raw);
        return;
      }
      if (line.startsWith('KEYEVENT ')) {
        /* 系统按窗口投来的真键盘事件（窗口级过滤器）→ 换算后转给容器应用。 */
        const fields = this.fields(line);
        const target = this.windowForOsId(fields.window) || this.inputWindowId ||
                       this.lastWindowId || this.mainWindowId || 0;
        const item = this.windows.get(target);
        /* These are composition keys while the Harmony custom editor owns the
           IME session.  The committed Unicode string arrives through
           insertText -> IME_COMMIT; forwarding pinyin here inserted Latin
           characters into the Linux editor beside the system composition. */
        if (item && item.imeActive) return;
        /* OH_Input C API: cancel=0, down=1, up=2.  Treating 1 as up left
           applications with release-only streams and broke held modifiers. */
        if (fields.action === '0') return;
        const action = fields.action === '1' ? 'down' : 'up';
        this.routeKey(target, action, fields.code, fields.mods || 0, '');
        return;
      }
      if (line.startsWith('MOUSEEVENT ') || line.startsWith('TOUCHEVENT ')) {
        /* 指针事件带的是显示坐标：按窗口矩形换算成窗口内坐标（anco 的 windowTransform）。 */
        const fields = this.fields(line);
        const target = this.windowForOsId(fields.window) || this.inputWindowId ||
                       this.lastWindowId || this.mainWindowId || 0;
        const item = this.windows.get(target);
        const rect = item && item.rect ? item.rect : null;
        const x = Number(fields.x) - (rect && rect.left ? Number(rect.left) : 0);
        /* Native input coordinates are display-relative and windowRect starts
           at the decorated frame.  Qt expects client coordinates, below the
           Harmony title bar. */
        const y = Number(fields.y) - (rect && rect.top ? Number(rect.top) : 0)
          - (item ? Number(item.clientOffsetY || 0) : 0);
        const raw = Number(fields.action);
        let action = '';
        let button = 1;
        if (line.startsWith('MOUSEEVENT ')) {
          /* Input_MouseEventAction: cancel=0, move=1, button-down=2,
             button-up=3, axis begin/update/end=4/5/6.  The old 0/1/2 mapping
             turned every hover move into a mouse-up, which repeatedly closed
             menus and broke Qt grabs. */
          action = raw === 1 ? 'move' : raw === 2 ? 'down' : raw === 3 ? 'up' : '';
          const nativeButton = Number(fields.button);
          /* Input_MouseEventButton: left=0, middle=1, right=2. */
          button = nativeButton === 2 ? 3 : nativeButton === 1 ? 2 : 1;
        } else {
          /* Input_TouchEventAction: cancel=0, down=1, move=2, up=3. */
          action = raw === 1 ? 'down' : raw === 2 ? 'move' : raw === 3 ? 'up' : '';
          button = 1;
        }
        if (action) this.routePointer(target, action, x, y, button);
        return;
      }
      if (line.startsWith('ANCO_')) {
        console.info('L410_ENGINE_ANCO_LINE', line);
        return;
      }
      if (line.startsWith('UI ')) {
        /* 来自"窗口能力"（独立 JS VM）的输入：那边拿不到这里的引擎通道，经 UI 桥发过来。 */
        const payload = line.slice(3).trim();
        console.info('L410_ENGINE_UIINPUT', payload);
        if (payload.startsWith('APPWIN ')) {
          const fields = this.fields(payload);
          const hostId = Number(fields.window) || 0;
          const record = {
            endpoint: String(fields.endpoint || ''), density: Number(fields.density) || 0,
            osWindowId: Number(fields.os) || 0,
            rect: { left: Number(fields.left) || 0, top: Number(fields.top) || 0,
              width: Number(fields.rectw) || 0, height: Number(fields.recth) || 0 },
            width: Number(fields.w) || 0, height: Number(fields.h) || 0
          };
          this.onAppWindowEvent({ parameters: { l410HostWindowId: hostId,
            l410AppWindowEvent: String(fields.reason || ''), l410Record: record } });
          return;
        }
        if (payload.startsWith('FILTERS ')) {
          const fields = this.fields(payload);
          const target = Number(fields.window) || 0;
          const item = this.windows.get(target);
          if (item) item.nativeInputMask = Number(fields.mask) || 0;
          console.info('L410_ENGINE_NATIVE_INPUT', target, Number(fields.mask) || 0);
          return;
        }
        if (payload.startsWith('POINTER ')) {
          const fields = this.fields(payload);
          const target = Number(fields.window) || this.inputWindowId || this.lastWindowId || 0;
          const item = this.windows.get(target);
          /* windowFilters returned bit 2/4 for mouse/touch.  Once the native
             window route is active, ArkUI callbacks duplicate the same click. */
          if (item && (Number(item.nativeInputMask) & 6)) return;
          this.routePointer(target, fields.action || 'move', fields.x || 0, fields.y || 0,
            fields.button || 0);
          return;
        }
        if (payload.startsWith('WHEEL ')) {
          const fields = this.fields(payload);
          const target = Number(fields.window) || this.inputWindowId || this.lastWindowId || 0;
          if (target) {
            this.sendToWindow(target, 'INPUT_WHEEL id=' + this.clientIdOf(target) +
              ' x=' + (fields.x || 0) + ' y=' + (fields.y || 0) +
              ' delta=' + (fields.delta || 0), target);
          }
          return;
        }
        if (payload.startsWith('TEXT ')) {
          const fields = this.fields(payload);
          const target = Number(fields.window) || this.inputWindowId || this.lastWindowId || 0;
          /* Committed text can contain spaces (for example a Chinese phrase
             followed by Latin words).  fields() tokenizes protocol headers
             on spaces, so preserve the complete text= tail here. */
          const at = payload.indexOf(' text=');
          if (target && at >= 0) this.imeCommit(target, payload.slice(at + 6));
          return;
        }
        if (payload.startsWith('PREEDIT ')) {
          const fields = this.fields(payload);
          const target = Number(fields.window) || this.inputWindowId || this.lastWindowId || 0;
          const at = payload.indexOf(' text=');
          if (target) this.imePreedit(target, at >= 0 ? payload.slice(at + 6) : '');
          return;
        }
        if (payload.startsWith('KEY ')) {
          const fields = this.fields(payload);
          const target = Number(fields.window) || this.inputWindowId || this.lastWindowId || 0;
          const item = this.windows.get(target);
          if (item && (Number(item.nativeInputMask) & 1)) return;
          this.routeKey(target, fields.action || 'down', fields.code || 0,
            fields.mods || 0, fields.text || '');
        }
        return;
      }
      if (line === 'ENGINE_CLIENT_CONNECTED') {
        console.info('L410_ENGINE_CLIENT_CONNECTED', pid);
        this.send('DISPLAY_SIZE w=' + engineHostDisplay.getDefaultDisplaySync().width +
          ' h=' + engineHostDisplay.getDefaultDisplaySync().height +
          ' density=' + engineHostDisplay.getDefaultDisplaySync().densityPixels, pid);
        this.startClipboardWatch();
        return;
      }
      if (line === 'ENGINE_CLIENT_GONE') {
        console.info('L410_ENGINE_CLIENT_GONE', pid);
        this.closeWindowsOf(pid);
        return;
      }
      const fields = this.fields(line);
      if (line.startsWith('WINDOW_CREATE ')) {
        /* 标题可能带空格：取 title= 之后的整段，不按空格切。 */
        const at = line.indexOf('title=');
        this.createWindow(Number(fields.id), Number(fields.w), Number(fields.h),
          at >= 0 ? line.slice(at + 6) : 'Linux', fields.kind || 'toplevel', pid,
          Number(fields.parent || 0), Number(fields.x || 0), Number(fields.y || 0));
        return;
      }
      if (line.startsWith('WINDOW_REMOVE ') || line.startsWith('WINDOW_CLOSE ')) {
        this.windowClosed(this.hostIdForClient(pid, Number(fields.id)));
        return;
      }
      if (line.startsWith('WINDOW_TITLE ')) {
        this.setTitle(Number(fields.id), fields.title || 'Linux');
        return;
      }
      if (line.startsWith('WINDOW_HIDE ')) {
        const hiddenId = this.hostIdForClient(pid, Number(fields.id));
        const hidden = this.windows.get(hiddenId);
        if (hidden && hidden.real && hidden.kind !== 'toplevel') {
          this.appWindowCommand('hide', hiddenId, {});
          const parentId = this.hostIdForClient(pid, hidden.parentClientId || 0);
          if (this.inputWindowId === hiddenId && parentId) this.inputWindowId = parentId;
        } else if (hidden && hidden.window && hidden.kind !== 'toplevel') {
          const operation = hidden.window.hide ? hidden.window.hide() :
            (hidden.window.hideWindow ? hidden.window.hideWindow() : null);
          if (operation && operation.catch) operation.catch(() => { });
          const parentId = this.hostIdForClient(pid, hidden.parentClientId || 0);
          if (this.inputWindowId === hiddenId && parentId) this.inputWindowId = parentId;
        }
        return;
      }
      if (line.startsWith('WINDOW_SHOW ')) {
        const shown = this.windows.get(this.hostIdForClient(pid, Number(fields.id)));
        if (shown && shown.real) {
          /* Qt recreates non-toplevels after hide, so a live real window is
             already visible. */
        } else if (shown && shown.window && shown.window.showWindow)
          shown.window.showWindow().catch(() => { });
        return;
      }
      if (line.startsWith('WINDOW_PARENT ')) {
        const child = this.windows.get(this.hostIdForClient(pid, Number(fields.id)));
        if (child) child.parentClientId = Number(fields.parent || 0);
        return;
      }
      if (line.startsWith('WINDOW_RESIZE ')) {
        /* 应用要求换尺寸（对话框、恢复窗口）：宿主照办，页面随后报回客户区真实尺寸。 */
        const item = this.windows.get(this.hostIdForClient(pid, Number(fields.id)));
        const width = Number(fields.w);
        const height = Number(fields.h);
        if (item && width > 64 && height > 64) {
          if (item.real) {
            this.appWindowCommand('resize', item.id, { width: Math.min(width, 2100), height: Math.min(height, 1400) });
          } else item.window.resize(Math.min(width, 2100), Math.min(height, 1400))
            .then(() => console.info('L410_ENGINE_WINDOW_RESIZED', Number(fields.id), width, height))
            .catch(error => console.error('L410_ENGINE_RESIZE_FAILED', error.message));
        }
        return;
      }
      if (line.startsWith('WINDOW_MOVE ')) {
        const item = this.windows.get(this.hostIdForClient(pid, Number(fields.id)));
        const x = Number(fields.x);
        const y = Number(fields.y);
        if (item && Number.isFinite(x) && Number.isFinite(y)) {
          if (item.real) {
            this.appWindowCommand('move', item.id, { x: Math.max(0, Math.min(x, 2000)), y: Math.max(0, Math.min(y, 1200)) });
          } else item.window.moveWindowTo(Math.max(0, Math.min(x, 2000)), Math.max(0, Math.min(y, 1200)))
            .catch(error => console.error('L410_ENGINE_MOVE_FAILED', error.message));
        }
        return;
      }
      if (line.startsWith('IME_')) {
        this.onImeLine(line, fields, pid);
        return;
      }
      if (line.startsWith('CLIPBOARD_')) {
        this.onClipboardLine(line, fields, pid);
        return;
      }
      console.info('L410_ENGINE_UNKNOWN', line);
    } catch (error) {
      console.error('L410_ENGINE_LINE_FAILED', error.message, line);
    }
  }

  fields(line) {
    const out = {};
    for (const token of line.split(' ')) {
      const at = token.indexOf('=');
      if (at > 0) out[token.slice(0, at)] = token.slice(at + 1);
    }
    return out;
  }

  sendDisplayInfo() {
    try {
      const output = engineHostDisplay.getDefaultDisplaySync();
      this.send('DISPLAY_SIZE w=' + output.width + ' h=' + output.height + ' density=' + output.densityPixels);
    } catch (error) {
      console.error('L410_ENGINE_DISPLAY_FAILED', error.message);
    }
  }

  /* ------------------------------------------------------------------ 窗口 */

  /* 每一步都设超时：哪一步卡住，日志里就有它的名字。 */
  step(name, promise) {
    return Promise.race([
      promise,
      new Promise((resolve, reject) => setTimeout(() => reject(new Error('timeout:' + name)), 4000))
    ]);
  }

  /* Each Linux top-level is always a real Harmony application window.  This
     used to depend on a marker in filesDir; reinstalling the HAP removed that
     marker and silently fell back to the obsolete host-subwindow experiment.
     A runtime flag must not change the windowing architecture. */
  realWindowsEnabled() {
    if (this.realWindows === undefined) {
      this.realWindows = true;
      console.info('L410_ENGINE_REAL_WINDOWS', 1, 'fixed');
    }
    return true;
  }

  /* Transitional switch for bringing up the real AnCo GraphicBroker path.
     When present, Linux clients render into a ConsumerSurface created by our
     broker module and published directly with SetSurfaceWithWindowID.  No
     ArkUI page, XComponent or pixel copy participates in this path.  Keep the
     established Ability path available until lifecycle/input callbacks have
     passed the same tests, so a failed broker experiment is recoverable by
     deleting one data-file marker. */
  directAncoEnabled() {
    try {
      return !!engineHostFs.accessSync(this.context.filesDir + '/direct-anco');
    } catch (error) {
      return false;
    }
  }

  createDirectAncoWindow(hostId, item, width, height, initialX, initialY) {
    if (!this.api || typeof this.api.brokerWindowCreate !== 'function') return false;
    /* Project-owned ids are kept away from normal WMS ids.  The id is the
       stable key shared by GraphicBroker, MMI's AncoWindowInfo table and the
       Linux toolkit window. */
    const brokerId = 41000 + Number(hostId);
    let reply;
    try {
      reply = JSON.parse(String(this.api.brokerWindowCreate(
        brokerId, Math.max(16, Number(width) || 900), Math.max(16, Number(height) || 620))));
    } catch (error) {
      console.error('L410_ENGINE_DIRECT_ANCO_CREATE_FAILED', hostId,
        error.message || String(error));
      return false;
    }
    if (!reply || Number(reply.result) !== 0 || !reply.endpoint) {
      console.error('L410_ENGINE_DIRECT_ANCO_CREATE_REJECTED', hostId, JSON.stringify(reply || {}));
      return false;
    }
    item.directAnco = true;
    item.brokerWindowId = brokerId;
    item.endpoint = String(reply.endpoint);
    item.ready = true;
    item.width = Math.max(16, Number(width) || 900);
    item.height = Math.max(16, Number(height) || 620);
    item.left = Number(initialX) || 0;
    item.top = Number(initialY) || 0;
    this.lastWindowId = hostId;
    if (!this.mainWindowId && item.kind !== 'popup') this.mainWindowId = hostId;
    const accepted = this.sendToWindow(hostId, 'WINDOW_CREATED id=' + item.clientId +
      ' surface=' + item.endpoint + ' w=' + item.width + ' h=' + item.height +
      ' density=' + (this.lastDensity || 1.35) + ' x=' + item.left + ' y=' + item.top);
    console.info('L410_ENGINE_DIRECT_ANCO_READY', hostId, brokerId, item.endpoint,
      item.width, item.height, accepted ? 1 : 0);
    this.traceBrokerWindow(item);
    return true;
  }

  async startAppWindow(hostId, id, width, height, title, pid, kind, parent, initialX, initialY) {
    const endpoint = this.api && this.api.uiEndpoint ? this.api.uiEndpoint() : '';
    try {
      await this.context.startAbility({
        bundleName: 'com.l410.linux.engine',
        abilityName: 'EngineWindowAbility',
        parameters: {
          l410HostWindowId: hostId,
          l410ClientId: id,
          l410ClientPid: pid || 0,
          l410Title: title || 'Linux',
          l410Width: width || 900,
          l410Height: height || 620,
          l410Kind: kind || 'toplevel',
          l410ParentClientId: parent || 0,
          l410InitialX: Number(initialX) || 0,
          l410InitialY: Number(initialY) || 0,
          l410UiEndpoint: endpoint
        }
      });
      console.info('L410_ENGINE_APPWIN_STARTED', hostId, 'client', id, 'bridge', endpoint);
    } catch (error) {
      console.error('L410_ENGINE_APPWIN_START_FAILED', hostId, error.message || String(error));
      this.clientIndex.delete(String(pid || 0) + ':' + String(id));
      this.windows.delete(hostId);
    }
  }

  /* 窗口能力发来的事件：读它写的记录，再转成给容器应用的消息。 */
  onAppWindowEvent(want) {
    const params = (want && want.parameters) || {};
    const hostId = Number(params.l410HostWindowId || 0);
    const reason = String(params.l410AppWindowEvent || '');
    if (!hostId) return;
    const item = this.windows.get(hostId);
    console.info('L410_ENGINE_APPWIN_EVENT', hostId, reason);
    let record = params.l410Record || {};
    if (!params.l410Record) {
      try {
        const fs = requireNapi('file.fs');
        const path = this.context.filesDir + '/appwin-' + hostId + '.json';
        const content = fs.readTextSync(path);
        record = JSON.parse(content);
      } catch (error) {
        console.warn('L410_ENGINE_APPWIN_RECORD_MISSING', hostId, error.message || String(error));
      }
    }
    if (!item) return;
    /* 窗口级输入事件按 OHOS 窗口 id 找宿主窗口；坐标按窗口矩形换算。 */
    if (record.osWindowId) {
      if (!this.osWindowIndex) this.osWindowIndex = new Map();
      this.osWindowIndex.set(Number(record.osWindowId), hostId);
      item.osWindowId = Number(record.osWindowId);
    }
    if (record.rect) item.rect = record.rect;
    if (record.rect && Number(record.height) > 0) {
      item.clientOffsetY = Math.max(0, Number(record.rect.height || 0) - Number(record.height));
    }
    /* Independent Linux top-levels are independent main windows.  Their own
       Ability shows them with focusOnShow; shiftAppWindowFocus is only for a
       main/child pair and must not be used between these main windows. */
    if (reason === 'ready') {
      const width = Number(record.width || item.requestedWidth || 0);
      const height = Number(record.height || item.requestedHeight || 0);
      item.ready = true;
      const density = (record.density || this.lastDensity || 1.35);
      const endpoint = String(record.endpoint || '');
      const left = Number((record.rect && record.rect.left) || 0);
      const top = Number((record.rect && record.rect.top) || 0);
      if (!endpoint) {
        console.error('L410_ENGINE_SURFACE_ENDPOINT_MISSING', hostId);
        this.sendToWindow(hostId, 'WINDOW_FAILED id=' + item.clientId + ' reason=surface_endpoint');
        return;
      }
      item.endpoint = endpoint;
      this.lastWindowId = hostId;
      if (!this.mainWindowId && item.kind !== 'popup') this.mainWindowId = hostId;
      const accepted = this.sendToWindow(hostId, 'WINDOW_CREATED id=' + item.clientId +
        ' surface=' + endpoint + ' w=' + width + ' h=' + height + ' density=' + density +
        ' x=' + left + ' y=' + top);
      console.info('L410_ENGINE_APPWIN_CREATED_SENT', hostId, 'pid', item.pid || 0,
        'client', item.clientId, 'accepted', accepted ? 1 : 0,
        'peers', this.api && this.api.status ? this.api.status() : 'n/a');
      console.info('L410_ENGINE_APPWIN_READY', hostId, endpoint, width, height);
      /* Broker shell stays permanently suppressed. */
      this.retireManagerWindow();
      return;
    }
    if (reason === 'geometry') {
      /* A real application window lives in another Ability/VM, so item.window
         intentionally does not exist here.  The old call silently threw and
         Qt never learned the maximized client size, leaving it painting the
         obsolete queue until it appeared frozen. */
      const width = Number(record.width || (record.rect && record.rect.width) || 0);
      const height = Number(record.height || (record.rect && record.rect.height) || 0);
      const density = Number(record.density || this.lastDensity || 1);
      const left = Number((record.rect && record.rect.left) || 0);
      const top = Number((record.rect && record.rect.top) || 0);
      if (width > 0 && height > 0 && (item.width !== width || item.height !== height ||
          item.left !== left || item.top !== top)) {
        item.width = width;
        item.height = height;
        item.left = left;
        item.top = top;
        this.sendToWindow(hostId, 'WINDOW_GEOMETRY id=' + item.clientId +
          ' w=' + width + ' h=' + height + ' density=' + density + ' x=' + left + ' y=' + top);
        console.info('L410_ENGINE_APPWIN_GEOMETRY', hostId, width, height, density);
      }
      return;
    }
    if (reason === 'close') {
      /* 用户点了系统关闭按钮：先问容器应用（它可能弹"是否保存"）。 */
      this.sendToWindow(hostId, 'WINDOW_CLOSED id=' + item.clientId);
      console.info('L410_ENGINE_APPWIN_CLOSE_REQUESTED', hostId);
      return;
    }
    if (reason === 'gone') {
      console.info('L410_ENGINE_APPWIN_GONE', hostId);
      this.destroyWindow(hostId, true);
      this.restoreManagerWindow();
    }
  }

  /* 第一个真实应用窗口就绪后，把宿主自己的窗口最小化，让应用窗口露出来；
     等所有应用窗口都关掉再恢复（不然用户看不到自己启动的程序）。 */
  retireManagerWindow() {
    try {
      const open = [];
      this.windows.forEach((item) => { if (item.real && item.ready) open.push(item.id); });
      if (!open.length || this.managerRetired) return;
      this.managerRetired = true;
      const win = this.stage.getMainWindowSync();
      if (win.minimize) {
        win.minimize().then(() => console.info('L410_ENGINE_MANAGER_MINIMIZED', open.length))
          .catch((error) => console.warn('L410_ENGINE_MANAGER_MINIMIZE_FAILED', error.message));
      }
    } catch (error) {
      console.warn('L410_ENGINE_MANAGER_MINIMIZE_ERROR', error.message || String(error));
    }
  }

  restoreManagerWindow() {
    /* Internal broker UI is never restored or exposed to the user. */
    this.managerRetired = true;
  }

  /* 给某个真实窗口发命令（宿主 → 窗口能力）：系统事件，跨 VM，不会新建能力实例。 */
  appWindowCommand(command, hostId, extra) {
    try {
      const events = requireNapi('commonEventManager');
      const item = this.windows.get(hostId);
      const ownerHostId = Number(item && item.ownerHostId) || hostId;
      const parameters = Object.assign({ l410AppWindowCommand: command,
        l410HostWindowId: ownerHostId, l410TargetWindowId: hostId }, extra || {});
      events.publish('l410_appwin_command', { parameters: parameters, code: 0, data: '' },
        (error) => { if (error) console.warn('L410_ENGINE_APPWIN_PUBLISH_FAILED', command, hostId, error.message); });
      console.info('L410_ENGINE_APPWIN_PUBLISH', command, hostId);
    } catch (error) {
      console.error('L410_ENGINE_APPWIN_PUBLISH_ERROR', command, hostId, error.message || String(error));
    }
  }

  async createWindow(id, width, height, title, kind, pid, parent, initialX, initialY) {
    const clientKey = String(pid || 0) + ':' + String(id);
    if (!id || this.clientIndex.has(clientKey)) return;
    const hostId = ++this.nextHostWindowId;
    this.clientIndex.set(clientKey, hostId);
    const popup = kind === 'popup';
    console.info('L410_ENGINE_CREATE_BEGIN', hostId, 'client', id, 'pid', pid || 0,
      width, height, kind || 'toplevel', title);
    if (this.realWindowsEnabled() && popup) {
      /* A transient must be created by its real parent Ability's WindowStage.
         A child of the hidden broker stage is in a different composition tree:
         it can receive buffers while remaining invisible or behind the app. */
      const parentHostId = this.hostIdForClient(pid, parent);
      const owner = this.windows.get(parentHostId);
      if (owner && owner.real) {
        /* Popup windows at every transient depth live in the real top-level
           Ability's WindowStage.  A submenu's direct parent is another popup,
           not a separate Ability, so route through the parent's root owner. */
        const ownerHostId = Number(owner.ownerHostId) || parentHostId;
        this.windows.set(hostId, { endpoint: '', id: hostId, clientId: id,
          kind: 'popup', pid: pid || 0, parentClientId: parent || 0,
          requestedWidth: width, requestedHeight: height, real: true,
          ownerHostId: ownerHostId, parentHostId: parentHostId });
        this.appWindowCommand('create-popup', hostId, {
          popupHostId: hostId, popupClientId: id, popupClientPid: pid || 0,
          popupParentHostId: parentHostId,
          popupWidth: width, popupHeight: height, popupTitle: title,
          popupX: Number(initialX) || 0, popupY: Number(initialY) || 0
        });
        console.info('L410_ENGINE_CREATE_PARENT_SUBWINDOW', hostId, 'owner', ownerHostId);
        return;
      }
      console.warn('L410_ENGINE_POPUP_PARENT_MISSING', hostId, pid || 0, parent || 0);
    }
    if (this.realWindowsEnabled() && !popup) {
      /* Long-lived top levels are real application windows.  A Qt popup is a
         transient surface owned by its parent; starting another multiton
         UIAbility gave every menu a task-level window, stole focus, and took
         long enough that the matching mouse release arrived at the parent.
         Popups continue below as undecorated compositor subwindows. */
      /* 先探一下这条通道此刻是否真的活着（后面要往这个应用发 WINDOW_CREATED）。 */
      const probe = this.send('ENGINE_PROBE', pid || 0);
      console.info('L410_ENGINE_APPWIN_CHANNEL', hostId, 'pid', pid || 0, 'send', probe ? 1 : 0,
        this.api && this.api.status ? this.api.status() : 'n/a');
      /* Qt popup geometry is already screen-relative.  Adding the parent
         Harmony rect a second time displaced menus by the main-window offset. */
      const placementX = Number(initialX) || 0;
      const placementY = Number(initialY) || 0;
      this.windows.set(hostId, { endpoint: '', id: hostId, clientId: id, kind: kind || 'toplevel',
        pid: pid || 0, parentClientId: parent || 0, requestedWidth: width,
        requestedHeight: height, real: true });
      const directItem = this.windows.get(hostId);
      if (this.directAncoEnabled()) {
        if (this.createDirectAncoWindow(hostId, directItem, width, height, placementX, placementY)) return;
        console.warn('L410_ENGINE_DIRECT_ANCO_FALLBACK', hostId);
      }
      await this.startAppWindow(hostId, id, width, height, title, pid, kind, parent, placementX, placementY);
      return;
    }
    let window;
    try {
      /* 子窗口名字必须全局唯一：两个应用都会从 1 开始编号，用宿主编号才不撞名。 */
      window = await this.step('createSubWindowWithOptions', this.stage.createSubWindowWithOptions(
        'l410-window-' + hostId, { title: title, decorEnabled: !popup, isModal: false }));
    } catch (error) {
      console.error('L410_ENGINE_CREATE_FAILED', hostId, 'client', id, error.message);
      this.clientIndex.delete(clientKey);
      return;
    }
    console.info('L410_ENGINE_CREATE_SUBWINDOW', hostId);
    /* 子窗口默认可能是"不可聚焦"的：那样指针进得来、键盘永远不进。
       显式设成可聚焦，键盘才有机会落到我们的输入层。 */
    try { await this.step('setWindowFocusable', window.setWindowFocusable(!popup)); }
    catch (error) { console.warn('L410_ENGINE_FOCUSABLE_FAILED', hostId, error.message); }
    const item = { window: window, endpoint: '', id: hostId, clientId: id,
      kind: kind || 'toplevel', pid: pid || 0, parentClientId: parent || 0,
      requestedWidth: width, requestedHeight: height };
    this.windows.set(hostId, item);

    /* 建窗过程中不动尺寸：实测会刷 acquire failed。装饰差额仍在窗口就绪后纠正一次。 */

    window.on('windowWillClose', () => {
      console.info('L410_ENGINE_WINDOW_WILL_CLOSE', hostId);
      /* 用户在系统标题栏点关闭：先问应用（它可能要先弹"是否保存"），
         应用真的关掉自己的窗口时会回 WINDOW_REMOVE，那时才销毁这个鸿蒙窗口。
         系统 API 约定：resolve(true) = 不关窗，resolve(false) = 继续关。 */
      if (item.forceClose) return Promise.resolve(false);
      this.sendToWindow(hostId, 'WINDOW_CLOSED id=' + item.clientId);
      console.info('L410_ENGINE_CLOSE_REQUESTED', hostId);
      return Promise.resolve(true);
    });
    window.on('windowRectChange', () => this.reportGeometry(hostId));
    /* 带系统装饰的子窗口不接受改标题（"Window set title failed"），失败不阻断建窗 */
    try { await this.step('setWindowTitle', window.setWindowTitle(title)); }
    catch (error) { console.warn('L410_ENGINE_TITLE_SKIPPED', hostId, error.message); }

    try {
      /* 装饰高度已知就一次到位（客户区正好等于应用要的尺寸）；
         还不知道时先按原尺寸建，等窗口就绪后量一次差额——只在第一个窗口发生。 */
      const extraHeight = (!popup && this.decorHeight > 0) ? this.decorHeight : 0;
      if (extraHeight > 0) item.sizeCorrected = true;
      await this.step('resize', window.resize(Math.min(width || 1024, 2100),
        Math.min((height || 700) + extraHeight, 1400)));
      const moveX = popup && Number.isFinite(initialX) ? initialX : 120 + (id % 6) * 48;
      const moveY = popup && Number.isFinite(initialY) ? initialY : 100 + (id % 6) * 48;
      await this.step('moveWindowTo', window.moveWindowTo(Math.max(0, moveX), Math.max(0, moveY)));
      const storage = new LocalStorage({ linuxWindowId: hostId, manager: false });
      await this.step('loadContent', window.loadContent('pages/EngineWindow', storage));
      await this.step('showWindow', window.showWindow());
    } catch (error) {
      console.error('L410_ENGINE_CREATE_FAILED', hostId, error.message);
      return;
    }
    console.info('L410_ENGINE_WINDOW_CREATED', hostId);
  }

  destroyWindow(id, alreadyGone) {
    const item = this.windows.get(id);
    if (!item) return;
    this.destroyBrokerSurface(item);
    if (item.directAnco) {
      this.windows.delete(id);
      this.clientIndex.delete(String(item.pid || 0) + ':' + String(item.clientId));
      this.send('WINDOW_CLOSED id=' + item.clientId, item.pid || 0);
      console.info('L410_ENGINE_DIRECT_ANCO_REMOVED', id);
      return;
    }
    if (item.real && !alreadyGone) {
      this.windows.delete(id);
      this.clientIndex.delete(String(item.pid || 0) + ':' + String(item.clientId));
      this.appWindowCommand('close', id, {});
      return;
    }
    this.windows.delete(id);
    this.clientIndex.delete(String(item.pid || 0) + ':' + String(item.clientId));
    item.forceClose = true;
    try { item.window.destroyWindow(); } catch (error) { console.error('L410_ENGINE_DESTROY_FAILED', id, error.message); }
    this.send('WINDOW_CLOSED id=' + item.clientId, item.pid || 0);
    console.info('L410_ENGINE_WINDOW_REMOVED', id);
  }

  /* 应用自己关掉了窗口（Qt/GTK 把窗口销毁了）→ 鸿蒙这边的窗口也收掉。 */
  windowClosed(id) {
    const item = this.windows.get(id);
    if (item) item.forceClose = true;
    this.destroyWindow(id);
  }

  /* 只是收掉宿主这边的窗口，不通知应用（应用已经把它藏起来或销毁了）。 */
  removeWindow(id) {
    const item = this.windows.get(id);
    if (!item) return;
    this.windows.delete(id);
    this.clientIndex.delete(String(item.pid || 0) + ':' + String(item.clientId));
    this.destroyBrokerSurface(item);
    item.forceClose = true;
    try { item.window.destroyWindow(); } catch (error) { }
    console.info('L410_ENGINE_WINDOW_REMOVED', id);
  }

  /* 用户点系统关闭按钮时走的那条路（应用有权拒绝，比如先问要不要保存）。 */
  requestClose(id) {
    const item = this.windows.get(id);
    if (!item) return;
    this.sendToWindow(id, 'WINDOW_CLOSED id=' + item.clientId);
    console.info('L410_ENGINE_CLOSE_REQUESTED', id);
  }

  closeAllWindows() {
    for (const id of Array.from(this.windows.keys())) this.destroyWindow(id);
  }

  /* 某个容器进程退出：只收它的窗口，别的应用不受影响。 */
  closeWindowsOf(pid) {
    for (const [id, item] of Array.from(this.windows.entries())) {
      if (item.pid === pid) this.destroyWindow(id);
    }
  }

  async setTitle(id, title) {
    const item = this.windows.get(id);
    if (!item) return;
    if (item.real) return;
    try { await item.window.setWindowTitle(title); } catch (error) { }
  }

  reportGeometry(id) {
    const item = this.windows.get(id);
    if (!item || !item.endpoint) return;
    try {
      const rect = item.window.getWindowProperties().windowRect;
      const density = engineHostDisplay.getDefaultDisplaySync().densityPixels;
      this.sendToWindow(id, 'WINDOW_GEOMETRY id=' + item.clientId + ' w=' + rect.width + ' h=' + rect.height +
        ' density=' + density + ' x=' + rect.left + ' y=' + rect.top);
    } catch (error) { }
  }

  /* 窗口页报来的是客户区（表面）的真实尺寸，设备像素。 */
  windowGeometry(id, width, height, density) {
    const item = this.windows.get(id);
    if (!item || !item.endpoint || !(width > 0) || !(height > 0)) return;
    if (item.width === width && item.height === height) return;
    item.width = width;
    item.height = height;
    this.sendToWindow(id, 'WINDOW_GEOMETRY id=' + item.clientId + ' w=' + width + ' h=' + height + ' density=' + (density || 1));
    console.info('L410_ENGINE_WINDOW_GEOMETRY', id, width, height);
  }

  /* 页面回调 */

  windowReady(id, endpoint, width, height, density) {
    const item = this.windows.get(id);
    if (item) item.endpoint = endpoint;
    this.lastWindowId = id;
    if (!this.mainWindowId && (!item || item.kind !== 'popup')) this.mainWindowId = id;
    /* 键盘要进得来，得让系统把"应用内焦点"挪到这个子窗口上（指针已经能进，键盘之前不能）。 */
    try {
      const subWindowId = item && item.window.getWindowProperties
        ? item.window.getWindowProperties().windowId : 0;
      const mainWindowId = this.stage && this.stage.getMainWindowSync
        ? this.stage.getMainWindowSync().getWindowProperties().windowId : 0;
      console.info('L410_ENGINE_WINDOW_IDS', id, 'sub', subWindowId, 'main', mainWindowId);
      try {
        const subProps = item && item.window.getWindowProperties ? item.window.getWindowProperties() : {};
        const mainProps = this.stage && this.stage.getMainWindowSync
          ? this.stage.getMainWindowSync().getWindowProperties() : {};
        console.info('L410_ENGINE_PROPS_SUB', JSON.stringify(subProps));
        console.info('L410_ENGINE_PROPS_MAIN', JSON.stringify(mainProps));
      } catch (error) { console.warn('L410_ENGINE_PROPS_FAILED', error.message); }
      /* 真正的窗口 id 在属性里叫 id（不是 windowId）：用它注册窗口级按键过滤器。
         这是 anco 那种宿主侧窗口级输入：不依赖 ArkUI 控件焦点。 */
      const props = item && item.window.getWindowProperties ? item.window.getWindowProperties() : {};
      if (props && props.id) {
        try {
          const page = globalThis.l410EnginePages && globalThis.l410EnginePages[id];
          const published = page && page.native && page.native.publish
            ? page.native.publish(Number(props.id)) : -1;
          console.info('L410_ENGINE_XCOMPONENT_PUBLISHED', id, props.id, published);
        } catch (error) {
          console.error('L410_ENGINE_XCOMPONENT_PUBLISH_FAILED', id, error.message || String(error));
        }
      }
      if ((!item || item.kind !== 'popup') && props && props.id && this.api && this.api.keyFilterProbe) {
        const code = this.api.keyFilterProbe(props.id);
        console.info('L410_ENGINE_KEY_FILTER', id, 'windowId', props.id, 'result', code);
      }
      /* 光注册过滤器还不够：键盘得先被投给这个窗口。把应用内焦点从主窗口挪到它。 */
      try {
        const mainId = this.stage.getMainWindowSync().getWindowProperties().id;
        if ((!item || item.kind !== 'popup') && mainId && props.id && engineHostWindow.shiftAppWindowFocus) {
          engineHostWindow.shiftAppWindowFocus(mainId, props.id)
            .then(() => console.info('L410_ENGINE_FOCUS_SHIFTED', id, mainId, '->', props.id))
            .catch(error => console.warn('L410_ENGINE_FOCUS_SHIFT_FAILED', id, error.message));
        }
      } catch (error) { console.warn('L410_ENGINE_FOCUS_SHIFT_ERROR', error.message); }
    } catch (error) {
      console.warn('L410_ENGINE_FOCUS_SHIFT_ERROR', id, error.message);
    }
    /* 尺寸只有一个权威：窗口自己。建完窗口之后再动尺寸一定会让应用那一帧的缓冲与画布对不上
       （表现就是黑屏/黑边/丢帧），所以这里不再做任何"事后纠正"：
       应用拿到多大的客户区就用多大，标题栏占掉的那点高度由应用自己重排。 */
    if (item && item.kind !== 'popup' && !item.sizeCorrected) {
      item.sizeCorrected = true;
      const missingHeight = (item.requestedHeight || 0) - (height || 0);
      if (missingHeight > 8 && this.decorHeight === 0) this.decorHeight = missingHeight;
    }
    let left = 0;
    let top = 0;
    try {
      const rect = item && item.window && item.window.getWindowProperties().windowRect;
      if (rect) { left = Number(rect.left || 0); top = Number(rect.top || 0); }
    } catch (error) { }
    this.sendToWindow(id, 'WINDOW_CREATED id=' + (item ? item.clientId : id) + ' surface=' + endpoint +
      ' w=' + width + ' h=' + height + ' density=' + density + ' x=' + left + ' y=' + top);
    console.info('L410_ENGINE_WINDOW_READY', id, endpoint, width, height);
    this.maybeSelfTest(id);
  }

  destroyBrokerSurface(item) {
    if (!item || !item.brokerWindowId || !this.api || !this.api.brokerWindowDestroy) return;
    const windowId = Number(item.brokerWindowId);
    item.brokerWindowId = 0;
    try {
      console.info('L410_ENGINE_BROKER_DESTROY', windowId, this.api.brokerWindowDestroy(windowId));
    } catch (error) {
      console.error('L410_ENGINE_BROKER_DESTROY_FAILED', windowId, error.message || String(error));
    }
  }

  traceBrokerWindow(item) {
    if (!item || !item.brokerWindowId || !this.api || !this.api.brokerWindowStatus) return;
    let samples = 0;
    const sample = () => {
      samples++;
      try {
        console.info('L410_ENGINE_BROKER_STATUS', item.id,
          this.api.brokerWindowStatus(Number(item.brokerWindowId)));
      } catch (error) {
        console.error('L410_ENGINE_BROKER_STATUS_FAILED', item.id, error.message || String(error));
      }
      if (samples < 8 && item.brokerWindowId) setTimeout(sample, 1000);
    };
    sample();
  }

  /* 自检：宿主侧标记文件存在时，走与真实输入完全相同的通路打一段字，
     用来验证「鸿蒙输入 → 引擎通道 → 容器工具包 → 应用」整条链。
     如果还有 files/input-script.txt（一行一步），就按脚本跑：
       wait <ms> | type <文本> | key <名字> | pointer down|up <x> <y> |
       wheel <x> <y> <delta> | resize <设备像素 w> <h> | title <文本> | close */
  maybeSelfTest(id) {
    let marked = false;
    const dir = this.filesDir();
    const path = dir + '/input-selftest';
    try { marked = !!engineHostFs.accessSync(path); } catch (error) { marked = false; }
    let script = '';
    try {
      if (engineHostFs.accessSync(dir + '/input-script.txt')) script = String(engineHostFs.readTextSync(dir + '/input-script.txt'));
    } catch (error) { script = ''; }
    console.info('L410_ENGINE_SELFTEST_MARKER', path, marked ? 1 : 0, script ? script.length : 0);
    if ((!marked && !script) || this.selfTested) return;
    this.selfTested = true;
    if (script) {
      this.runInputScript(script);
      return;
    }
    const wait = milliseconds => new Promise(resolve => setTimeout(resolve, milliseconds));
    const table = { e: 18, n: 49, g: 34, i: 23, o: 24, 1: 2, 2: 3, space: 57 };
    const press = async (code, mods, text) => {
      this.send('INPUT_KEY id=' + id + ' action=down code=' + code + ' mods=' + (mods || 0) + (text ? ' text=' + text : ''));
      this.send('INPUT_KEY id=' + id + ' action=up code=' + code + ' mods=' + (mods || 0));
      await wait(140);
    };
    const type = async text => {
      for (const character of text) {
        const code = character === ' ' ? table.space : table[character];
        if (code === undefined) continue;
        await press(code, 0, character === ' ' ? '\u001f' : character);
      }
    };
    (async () => {
      await wait(1200);
      console.info('L410_ENGINE_SELFTEST_START', id);
      await type('engine 1');
      await wait(400);
      /* 复制到鸿蒙剪贴板：应用自己发 CLIPBOARD_SET，宿主写系统剪贴板。 */
      await press(30, 2);            /* ctrl+a */
      await press(46, 2, 'c');       /* ctrl+c */
      await wait(1400);
      try {
        /* 先读回系统剪贴板：确认应用复制的内容真的进了鸿蒙剪贴板。 */
        await this.ensurePasteboardPermission();
        const board = engineHostPasteboard.getSystemPasteboard();
        const readback = await board.getData();
        const copied = String(readback.getPrimaryText());
        console.info('L410_ENGINE_SELFTEST_CLIPBOARD', copied.length, copied);
        /* 再往系统剪贴板写一段已知文本，让应用粘回来（反方向验证）。 */
        await board.setData(engineHostPasteboard.createPlainTextData('PASTED-FROM-OHOS'));
        this.publishClipboard('PASTED-FROM-OHOS');
        console.info('L410_ENGINE_SELFTEST_PASTEBOARD_SET');
      } catch (error) {
        console.error('L410_ENGINE_SELFTEST_PASTEBOARD_FAILED', error.message);
      }
      await wait(300);
      /* 再粘回来：应用会向宿主取剪贴板文本，正文应变成 PASTED-FROM-OHOS。 */
      await press(30, 2);            /* ctrl+a */
      await press(47, 2, 'v');       /* ctrl+v */
      await wait(600);
      console.info('L410_ENGINE_SELFTEST_DONE', id);
    })();
  }

  filesDir() {
    try { return this.context.filesDir; } catch (error) { return ''; }
  }

  /* ------------------------------------------------------------ 拉起 Linux 应用 */

  /* 宿主负责把容器里的应用拉起来（启动器入口、页面按钮都走这里）。
     同一个应用再次点击时，把已有窗口提到前面，不重复起进程。 */
  launchApp(name) {
    if (!this.api || !name) return false;
    if (this.launchedApps.has(name) && this.windows.size > 0) {
      for (const item of this.windows.values()) {
        try { item.window.showWindow(); } catch (error) { }
        break;
      }
      console.info('L410_ENGINE_APP_FOREGROUND', name);
      return true;
    }
    try {
      /* 容器生命周期归 root 系统服务（应用沙箱看不到容器，只能请它代劳）。 */
      let reply = '';
      try {
        if (this.api.serviceSend) reply = String(this.api.serviceSend('APP_START name=' + name + ' endpoint=' + (this.endpoint || '')));
      } catch (error) { reply = ''; }
      console.info('L410_ENGINE_APP_SERVICE', name, reply);
      if (reply.startsWith('OK')) {
        this.launchedApps.add(name);
        return true;
      }
      const pid = this.api.launch(name, this.endpoint || '');
      if (pid > 0) {
        this.launchedApps.add(name);
        console.info('L410_ENGINE_APP_LAUNCHED', name, pid);
        return true;
      }
      console.error('L410_ENGINE_APP_LAUNCH_REFUSED', name);
      return false;
    } catch (error) {
      console.error('L410_ENGINE_APP_LAUNCH_FAILED', name, error.message);
      return false;
    }
  }

  /* 启动器入口用 want 参数直接指定应用：onCreate / onNewWant 都会到这里。 */
  noteWant(want) {
    try {
      const app = want && want.parameters ? want.parameters.l410App : '';
      if (app) {
        this.pendingApp = String(app);
        console.info('L410_ENGINE_WANT_APP', this.pendingApp);
        if (this.api) this.launchApp(this.pendingApp);
      }
    } catch (error) { }
  }

  /* 按脚本注入输入/窗口动作：一行一步，走的是真实输入与窗口管理那几条通路。
     wait <ms> | type <文本> | key <名字> | pointer down|up <x> <y> |
     wheel <x> <y> <delta> | resize <设备像素 w> <h> | title <文本> | close | waitwindow <n> */
  async runInputScript(script) {
    const wait = milliseconds => new Promise(resolve => setTimeout(resolve, milliseconds));
    const keys = {
      escape: [1, 0], tab: [15, 0], enter: [28, 0], return: [28, 0], space: [57, 0],
      backspace: [14, 0], delete: [111, 0], up: [103, 0], down: [108, 0], left: [105, 0], right: [106, 0],
      home: [102, 0], end: [107, 0],
      'ctrl+a': [30, 2], 'ctrl+c': [46, 2], 'ctrl+v': [47, 2], 'ctrl+s': [31, 2],
      'ctrl+x': [45, 2], 'ctrl+z': [44, 2], 'ctrl+n': [49, 2], 'ctrl+o': [24, 2],
      'ctrl+w': [17, 2], 'ctrl+q': [16, 2], 'ctrl+f': [33, 2],
    };
    for (const raw of String(script).split('\n')) {
      const line = raw.trim();
      if (!line || line.startsWith('#')) continue;
      const parts = line.split(/\s+/);
      const verb = parts[0];
      /* 键盘和窗口动作默认打到应用的主窗口（第一个建出来的那个）；
         弹出窗口只在 pointer 那一步显式指定时才用。 */
      const target = this.scriptTarget || this.mainWindowId || this.lastWindowId || 1;
      console.info('L410_ENGINE_SCRIPT_STEP', line);
      if (verb === 'target') {
        /* 多个应用同时开时，用 target 指定后面的输入打给谁：main / last / <窗口号>。 */
        const value = parts[1];
        this.scriptTarget = value === 'last' ? (this.lastWindowId || 0)
          : value === 'main' ? (this.mainWindowId || 0)
            : (Number(value) || 0);
        console.info('L410_ENGINE_SCRIPT_TARGET', this.scriptTarget);
      } else if (verb === 'app') {
        /* 走宿主自己的应用拉起路径（和启动器按钮同一条）。 */
        console.info('L410_ENGINE_SCRIPT_APP', parts[1]);
        this.launchApp(parts[1]);
      } else if (verb === 'wait') {
        await wait(Number(parts[1]) || 200);
      } else if (verb === 'type') {
        for (const character of line.slice(5)) {
          const entry = engineHostKeyFor(character);
          if (!entry) continue;
          this.sendToWindow(target, 'INPUT_KEY id=' + this.clientIdOf(target) + ' action=down code=' + entry.code + ' mods=' + entry.mods + ' text=' + entry.text);
          this.sendToWindow(target, 'INPUT_KEY id=' + this.clientIdOf(target) + ' action=up code=' + entry.code + ' mods=' + entry.mods);
          await wait(120);
        }
      } else if (verb === 'key') {
        const entry = keys[parts[1]];
        if (!entry) continue;
        const text = entry[0] === 57 ? '\u001f' : '';
        this.sendToWindow(target, 'INPUT_KEY id=' + this.clientIdOf(target) + ' action=down code=' + entry[0] + ' mods=' + entry[1] + (text ? ' text=' + text : ''));
        this.sendToWindow(target, 'INPUT_KEY id=' + this.clientIdOf(target) + ' action=up code=' + entry[0] + ' mods=' + entry[1]);
        await wait(160);
      } else if (verb === 'pointer') {
        this.pointer(target, parts[1], Number(parts[2]) || 0, Number(parts[3]) || 0, 1);
      } else if (verb === 'wheel') {
        this.wheel(target, Number(parts[1]) || 0, Number(parts[2]) || 0, Number(parts[3]) || 120, 0);
      } else if (verb === 'resize') {
        const item = this.windows.get(target);
        if (item) {
          const width = Number(parts[1]) || 800;
          const height = Number(parts[2]) || 600;
          if (item.real) {
            /* 真实窗口没有 item.window 可调：让窗口能力改它自己的主窗口（等价于用户拖窗口边）。
               不能用 startAbility——multiton 能力每次都会新建实例（屏幕上会堆出白窗口），
               改成系统事件通知那个已经在跑的实例。 */
            this.appWindowCommand('resize', target, { width: width, height: height });
            console.info('L410_ENGINE_SCRIPT_RESIZED', target, width, height, 'real');
          } else {
            try {
              await item.window.resize(width, height);
              console.info('L410_ENGINE_SCRIPT_RESIZED', target, width, height);
            } catch (error) { console.error('L410_ENGINE_SCRIPT_RESIZE_FAILED', error.message); }
          }
        }
      } else if (verb === 'title') {
        this.setTitle(target, line.slice(6));
      } else if (verb === 'close') {
        /* 等同于用户点系统标题栏的关闭按钮：应用有权拒绝（先弹"是否保存"）。 */
        console.info('L410_ENGINE_SCRIPT_CLOSE', target);
        this.requestClose(target);
      } else if (verb === 'destroy') {
        console.info('L410_ENGINE_SCRIPT_DESTROY', target);
        this.destroyWindow(target);
      } else if (verb === 'ime') {
        /* 走的是系统输入法上屏的同一个入口：窗口页的隐藏输入框 → 容器里的应用。 */
        const page = this.page(target);
        if (page) page.imeChanged(line.slice(4));
      } else if (verb === 'imeshow') {
        const page = this.page(target);
        if (page) page.imeShow();
      } else if (verb === 'waitwindow') {
        for (let i = 0; i < 60 && this.windows.size < (Number(parts[1]) || 2); i++) await wait(250);
        console.info('L410_ENGINE_SCRIPT_WINDOWS', this.windows.size);
      }
    }
    console.info('L410_ENGINE_SCRIPT_DONE', this.windows.size);
  }

  /* ------------------------------------------------------- 输入法 / 剪贴板 */

  page(id) {
    try {
      const pages = globalThis.l410EnginePages || {};
      return pages[id] || null;
    } catch (error) { return null; }
  }

  /* 容器里的应用要求弹出/收起输入法：系统输入法挂在窗口页那个看不见的输入框上。 */
  onImeLine(line, fields, pid) {
    const clientId = Number(fields.id);
    const id = this.hostIdForClient(pid, clientId) || clientId;
    const item = this.windows.get(id);
    const target = this.page(id);
    if (line.startsWith('IME_SHOW')) {
      if (item) item.imeActive = true;
      if (item && item.real) this.appWindowCommand('ime-show', id, {});
      else if (target) target.imeShow();
      console.info('L410_ENGINE_IME_SHOW', id);
    } else if (line.startsWith('IME_HIDE')) {
      if (item) item.imeActive = false;
      if (item && item.real) this.appWindowCommand('ime-hide', id, {});
      else if (target) target.imeHide();
    } else if (line.startsWith('IME_RESET')) {
      if (item && item.real) this.appWindowCommand('ime-reset', id, {});
      else if (target) target.imeReset();
    } else if (line.startsWith('IME_STATE')) {
      let text = '';
      try {
        const hex = String(fields.texthex || '');
        if (hex && /^[0-9a-fA-F]+$/.test(hex) && !(hex.length % 2)) {
          const bytes = [];
          for (let at = 0; at < hex.length; at += 2) bytes.push(parseInt(hex.slice(at, at + 2), 16));
          text = decodeURIComponent(bytes.map(value =>
            '%' + value.toString(16).padStart(2, '0')).join(''));
        }
      } catch (error) { text = ''; }
      const state = { text: text, cursor: Number(fields.cursor || 0), anchor: Number(fields.anchor || 0) };
      if (item && item.real) this.appWindowCommand('ime-state', id, state);
      else if (target) target.updateImeState(state.text, state.cursor, state.anchor);
    } else if (line.startsWith('IME_CURSOR')) {
      /* 应用里的光标矩形（设备像素）→ 输入框位置：候选栏跟着光标走。 */
      const caret = { x: Number(fields.x) || 0, y: Number(fields.y) || 0,
        w: Number(fields.w) || 1, h: Number(fields.h) || 2 };
      const signature = caret.x + ':' + caret.y + ':' + caret.w + ':' + caret.h;
      if (item && item.imeCaretSignature === signature) return;
      if (item) item.imeCaretSignature = signature;
      if (item && item.real) this.appWindowCommand('ime-caret', id, caret);
      else if (target) target.setCaret(caret.x, caret.y, caret.w, caret.h);
    }
  }

  /* 输入法打出来的字（含中文候选上屏）→ 容器里的应用。 */
  imeCommit(id, text) {
    const flat = String(text === undefined ? '' : text).split('\n').join('\u001f');
    if (!flat) return;
    this.sendToWindow(id, 'IME_COMMIT id=' + this.clientIdOf(id) + ' text=' + flat);
  }

  imePreedit(id, text) {
    const flat = String(text === undefined ? '' : text).split('\n').join('\u001f');
    this.sendToWindow(id, 'IME_PREEDIT id=' + this.clientIdOf(id) + ' text=' + flat);
  }

  /* 输入法面板上的回车 → 应用里的回车键。 */
  imeEnter(id) {
    this.sendToWindow(id, 'INPUT_KEY id=' + this.clientIdOf(id) + ' action=down code=28 mods=0');
    this.sendToWindow(id, 'INPUT_KEY id=' + this.clientIdOf(id) + ' action=up code=28 mods=0');
  }

  /* 容器 → 鸿蒙剪贴板 / 鸿蒙剪贴板 → 容器。
     宿主始终留一份鸿蒙剪贴板的当前文本：容器里的工具包是同步取剪贴板的
     （Qt 的 mimeData() 必须当场拿到数据），等一次系统调用往返就会超时。 */
  async startClipboardWatch() {
    try {
      await this.ensurePasteboardPermission();
      const board = engineHostPasteboard.getSystemPasteboard();
      this.pasteboard = board;
      try {
        const data = await board.getData();
        this.publishClipboard(String(data.getPrimaryText() || ''));
      } catch (error) { }
      try {
        board.on('update', () => {
          board.getData().then(data => {
            this.publishClipboard(String(data.getPrimaryText() || ''));
          }).catch(() => { });
        });
      } catch (error) { }
      console.info('L410_ENGINE_CLIPBOARD_WATCH', (this.clipboardText || '').length);
    } catch (error) {
      console.error('L410_ENGINE_CLIPBOARD_WATCH_FAILED', error.message);
    }
  }

  /* 宿主把鸿蒙剪贴板推给容器：工具包的粘贴是同步取数据的，不能等系统调用往返。 */
  publishClipboard(text) {
    this.clipboardText = String(text === undefined ? '' : text);
    this.send('CLIPBOARD_CHANGED id=0 text=' + this.clipboardText.split('\n').join('\u001f'));
  }

  async onClipboardLine(line, fields, pid) {
    const id = Number(fields.id);
    if (line.startsWith('CLIPBOARD_SET')) {
      const at = line.indexOf('text=');
      const text = (at >= 0 ? line.slice(at + 5) : '').split('\u001f').join('\n');
      this.clipboardText = text;
      /* 工具包会连着发几次（选择所有权那套），系统一次只处理一个写请求：
         忙的时候记住最新的，写完再写一次，不然会报 "Another setData is being processed"。 */
      this.pendingClipboard = text;
      if (this.pasteboardBusy) return;
      this.pasteboardBusy = true;
      try {
        const board = this.pasteboard || engineHostPasteboard.getSystemPasteboard();
        while (this.pendingClipboard !== null) {
          const writing = this.pendingClipboard;
          this.pendingClipboard = null;
          await board.setData(engineHostPasteboard.createPlainTextData(writing));
          console.info('L410_ENGINE_CLIPBOARD_SET', id, writing.length);
        }
      } catch (error) {
        this.pendingClipboard = null;
        console.error('L410_ENGINE_CLIPBOARD_SET_FAILED', error.message);
      } finally {
        this.pasteboardBusy = false;
      }
      return;
    }
    if (line.startsWith('CLIPBOARD_GET')) {
      /* Qt/GTK wait synchronously for this reply.  Empty clipboard is still a
         valid answer: previously returning nothing made every empty paste
         block Qt's GUI thread for its full 900 ms timeout.  The local window
         id can overlap between applications, so reply to the requesting
         engine connection rather than broadcasting by host window id. */
      const text = this.clipboardText || '';
      const accepted = this.send('CLIPBOARD_TEXT id=' + id + ' text=' + text.split('\n').join('\u001f'), pid);
      console.info('L410_ENGINE_CLIPBOARD_TEXT', id, text.length, accepted ? 1 : 0);
    }
  }

  async ensurePasteboardPermission() {
    if (this.pasteboardAllowed) return;
    const manager = engineHostAbilityAccess.createAtManager();
    const result = await manager.requestPermissionsFromUser(this.context, ['ohos.permission.READ_PASTEBOARD']);
    this.pasteboardAllowed = result && result.authResults && result.authResults[0] === 0;
    console.info('L410_ENGINE_PASTEBOARD_PERMISSION', this.pasteboardAllowed ? 1 : 0);
  }

  windowGone(id, endpoint) {
    console.info('L410_ENGINE_WINDOW_GONE', id, endpoint || '');
    const item = this.windows.get(id);
    if (item) {
      this.clientIndex.delete(String(item.pid || 0) + ':' + String(item.clientId));
      this.send('WINDOW_CLOSED id=' + item.clientId, item.pid || 0);
    }
    this.windows.delete(id);
  }

  pointer(id, phase, x, y, button) {
    this.inputWindowId = id;
    this.sendToWindow(id, 'INPUT_POINTER id=' + this.clientIdOf(id) + ' action=' + phase +
      ' x=' + x + ' y=' + y + ' button=' + button);
  }

  wheel(id, x, y, delta, horizontal) {
    this.sendToWindow(id, 'INPUT_WHEEL id=' + this.clientIdOf(id) + ' x=' + x + ' y=' + y +
      ' delta=' + delta + ' horizontal=' + horizontal);
  }

  /* 哪个窗口最后被点过，键盘就送给它（子窗口未必拿得到“激活”，宿主这层收到就转发）。 */
  keyToTarget(event) {
    let target = this.inputWindowId || this.lastWindowId || this.mainWindowId || 0;
    const item = this.windows.get(target);
    if (item && item.kind === 'popup' && item.parentClientId)
      target = this.hostIdForClient(item.pid, item.parentClientId) || target;
    if (!target) return false;
    return this.key(target, event);
  }

  key(id, event) {
    const table = { 2054: 28, 2055: 14, 2014: 105, 2015: 106, 2012: 103, 2013: 108, 2049: 15, 2070: 1, 2050: 57, 2071: 111, 2081: 102, 2082: 107, 2068: 104, 2069: 109 };
    let code = table[event.keyCode];
    if (code === undefined && event.keyCode >= 2017 && event.keyCode <= 2042) code = [30, 48, 46, 32, 18, 33, 34, 35, 23, 36, 37, 38, 50, 49, 24, 25, 16, 19, 31, 20, 22, 47, 17, 45, 21, 44][event.keyCode - 2017];
    if (code === undefined && event.keyCode >= 2000 && event.keyCode <= 2009) code = event.keyCode === 2000 ? 11 : event.keyCode - 1999;
    if (code === undefined) return false;
    let mask = 0;
    try {
      if (event.getModifierKeyState(['shift'])) mask |= 1;
      if (event.getModifierKeyState(['ctrl'])) mask |= 2;
      if (event.getModifierKeyState(['alt'])) mask |= 4;
      if (event.getModifierKeyState(['meta'])) mask |= 8;
    } catch (error) { }
    const down = event.type === 0;
    let text = '';
    if (down) {
      try {
        const typed = String(event.keyText === undefined ? '' : event.keyText);
        /* 空格会截断协议行，用 0x1f 带过去，容器侧再换回来。 */
        if (typed.length >= 1 && typed.length <= 4) text = ' text=' + typed.split(' ').join('\u001f');
      } catch (error) { }
    }
    this.sendToWindow(id, 'INPUT_KEY id=' + this.clientIdOf(id) + ' action=' + (down ? 'down' : 'up') +
      ' code=' + code + ' mods=' + mask + text);
    return true;
  }

  focus(id, active) {
    this.sendToWindow(id, 'WINDOW_FOCUS id=' + this.clientIdOf(id) + ' active=' + (active ? 1 : 0));
  }

  onForeground() { }
  onBackground() { }
  onWindowStageDestroy() {
    console.info('L410_ENGINE_STAGE_DESTROYED');
    this.closeAllWindows();
    if (this.api) { try { this.api.stop(); } catch (error) { } }
  }
  onDestroy() {
    console.info('L410_ENGINE_ABILITY_DESTROYED');
    if (this.originalRoidtTimer) {
      clearInterval(this.originalRoidtTimer);
      this.originalRoidtTimer = 0;
    }
    this.originalRoidt(1, 'end');
    if (this.api) { try { this.api.stop(); } catch (error) { } }
  }
}
