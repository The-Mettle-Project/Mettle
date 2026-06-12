/**
 * Mettle debug adapter: an inline Debug Adapter Protocol implementation
 * bridging VS Code to the compiler's --debug-hooks runtime.
 *
 * Launch flow: compile the program with `--build --debug-hooks` (always -O0;
 * optimized code moves the hooks), host a named pipe, spawn the binary with
 * METTLE_DBG_PIPE pointing at it. The instrumented runtime connects, sends
 * its function/file tables, and stops at entry awaiting configuration.
 *
 * The wire protocol is line-based TSV (see src/runtime/debug.c); the exact
 * session shape is pinned by scripts/test-debug-protocol.js. Variables are
 * read and written through live pointers the instrumentation registered, so
 * Set Value in the Variables pane genuinely changes program state.
 */

const vscode = require('vscode');
const net = require('net');
const path = require('path');
const fs = require('fs');
const os = require('os');
const { execFile, spawn } = require('child_process');

let nextPipeId = 1;

class MettleDebugAdapter {
  /**
   * @param {{ findCompiler: (workspaceRoot: string, filePath: string) => string,
   *           log?: (line: string) => void }} deps
   */
  constructor(deps) {
    this.deps = deps;
    this._onDidSendMessage = new vscode.EventEmitter();
    this.onDidSendMessage = this._onDidSendMessage.event;
    this._seq = 1;

    this.server = null;
    this.conn = null;
    this.child = null;
    this.exePath = null;
    this.lineBuffer = '';
    this.pendingLines = [];
    this.lineWaiters = [];
    this.queryChain = Promise.resolve();
    this.files = new Map();      // file_id -> path
    this.fileIds = new Map();    // normalized path -> file_id
    this.functions = new Map();  // fn_id -> { name, fileId }
    this.ready = false;
    this.configured = false;
    this.stopOnEntry = false;
    this.terminated = false;
    this.lastStop = null;        // { reason, fileId, line, depth, fnId }
    this.pendingBreakpoints = []; // protocol lines queued before the pipe is up

    // variables tree: handle -> { frame, path } ('' = the frame's locals)
    this.varHandles = new Map();
    this.nextVarHandle = 1;
    // logpoints / hit counts, keyed `${fileId}:${line}`
    this.bpMeta = new Map();
    this.lastException = null;   // { code, address }
  }

  allocVarHandle(frame, path) {
    const handle = this.nextVarHandle++;
    this.varHandles.set(handle, { frame, path });
    return handle;
  }

  /** Child path for a member of `parentPath`: dotted for fields, appended
   * for `[i]` elements, bare name at the top level. */
  static childPath(parentPath, name) {
    if (!parentPath) return name;
    return name.startsWith('[') ? parentPath + name : `${parentPath}.${name}`;
  }

  // --- DAP plumbing ---------------------------------------------------------

  sendEvent(event, body) {
    this._onDidSendMessage.fire({ seq: this._seq++, type: 'event', event, body });
  }

  sendResponse(request, body, success = true, message = undefined) {
    this._onDidSendMessage.fire({
      seq: this._seq++, type: 'response', request_seq: request.seq,
      command: request.command, success, message, body,
    });
  }

  log(line) {
    if (this.deps.log) this.deps.log(`[debug] ${line}`);
  }

  output(category, text) {
    this.sendEvent('output', { category, output: text });
  }

  handleMessage(message) {
    if (message.type === 'response') return; // replies to reverse requests (runInTerminal)
    if (message.type !== 'request') return;
    const handler = this[`request_${message.command}`];
    if (typeof handler === 'function') {
      Promise.resolve(handler.call(this, message))
        .catch((err) => this.sendResponse(message, undefined, false, String(err.message || err)));
    } else {
      this.sendResponse(message, {});
    }
  }

  dispose() {
    this.shutdown();
  }

  shutdown() {
    try { if (this.conn) this.conn.write('detach\n'); } catch (_) { /* gone */ }
    try { if (this.child && this.child.exitCode === null) this.child.kill(); } catch (_) { /* gone */ }
    try { if (this.server) this.server.close(); } catch (_) { /* gone */ }
    if (this.exePath) {
      const exe = this.exePath;
      setTimeout(() => { try { fs.unlinkSync(exe); } catch (_) { /* in use */ } }, 500);
      this.exePath = null;
    }
  }

  // --- runtime pipe ----------------------------------------------------------

  send(line) {
    if (this.conn) this.conn.write(line + '\n');
  }

  nextLine(timeoutMs = 10000) {
    if (this.pendingLines.length > 0) return Promise.resolve(this.pendingLines.shift());
    return new Promise((resolve, reject) => {
      const timer = setTimeout(() => reject(new Error('debug runtime timeout')), timeoutMs);
      this.lineWaiters.push((line) => { clearTimeout(timer); resolve(line); });
    });
  }

  /** Async events (stopped) are dispatched immediately; query replies are
   * buffered for the in-flight collector. */
  onPipeLine(line) {
    const fields = line.split('\t');
    if (fields[0] === 'stopped') {
      this.lastStop = {
        reason: fields[1],
        fileId: parseInt(fields[2], 10),
        line: parseInt(fields[3], 10),
        depth: parseInt(fields[4], 10),
        fnId: parseInt(fields[5], 10),
      };
      // stale variable handles die with the stop that created them
      this.varHandles.clear();
      this.nextVarHandle = 1;

      if (fields[1] === 'entry') {
        // held at entry: surfaced after configurationDone (stopOnEntry) or
        // released by it; not a user-visible stop by itself
        this.entryStopSeen = true;
        if (this.entryWaiter) this.entryWaiter();
        return;
      }
      if (fields[1] === 'exception') {
        this.lastException = { code: fields[6] || '?', address: fields[7] || '?' };
        this.sendEvent('stopped', {
          reason: 'exception',
          description: this.describeException(),
          text: this.describeException(),
          threadId: 1,
          allThreadsStopped: true,
        });
        return;
      }
      if (fields[1] === 'breakpoint' &&
          this.handleBreakpointMeta(this.lastStop.fileId, this.lastStop.line)) {
        return; // logpoint or unmet hit count: auto-continued
      }
      const reasonMap = { breakpoint: 'breakpoint', step: 'step', pause: 'pause' };
      this.sendEvent('stopped', {
        reason: reasonMap[fields[1]] || fields[1],
        threadId: 1,
        allThreadsStopped: true,
      });
      return;
    }
    if (this.lineWaiters.length > 0) this.lineWaiters.shift()(line);
    else this.pendingLines.push(line);
  }

  describeException() {
    const code = this.lastException ? this.lastException.code : '?';
    const names = {
      '0xc0000005': 'Access violation',
      '0xc0000094': 'Integer division by zero',
      '0xc000001d': 'Illegal instruction',
      '0xc000008e': 'Floating-point division by zero',
      '0xc000008c': 'Array bounds exceeded',
    };
    const name = names[String(code).toLowerCase()] || 'Hardware exception';
    return `${name} (${code}) at ${this.lastException ? this.lastException.address : '?'}`;
  }

  /** Logpoints and hit counts, applied adapter-side. Returns true when the
   * stop was consumed (execution auto-continues). */
  handleBreakpointMeta(fileId, bpLine) {
    const meta = this.bpMeta.get(`${fileId}:${bpLine}`);
    if (!meta) return false;
    if (meta.hitCondition) {
      meta.hits = (meta.hits || 0) + 1;
      const target = parseInt(meta.hitCondition.replace(/[^0-9]/g, ''), 10) || 1;
      if (meta.hits < target) {
        this.send('go');
        return true;
      }
      meta.hits = 0;
    }
    if (meta.logMessage) {
      // interpolate {expression} parts via the runtime, then keep running
      const template = meta.logMessage;
      const parts = template.split(/(\{[^}]+\})/);
      (async () => {
        let rendered = '';
        for (const part of parts) {
          if (part.startsWith('{') && part.endsWith('}')) {
            const reply = await this.queryOne(`eval\t0\t${part.slice(1, -1).trim()}`);
            const f = reply.split('\t');
            rendered += f[0] === 'evalr' && f[1] === '1' ? f.slice(4).join('\t') : part;
          } else {
            rendered += part;
          }
        }
        this.output('console', rendered + '\n');
        this.send('go');
      })().catch(() => this.send('go'));
      return true;
    }
    return false;
  }

  /** Serialize a query: send `command`, collect lines until `terminator`. */
  query(command, terminator) {
    const run = async () => {
      this.send(command);
      const lines = [];
      for (;;) {
        const line = await this.nextLine();
        if (line === terminator || line.startsWith(terminator + '\t')) return lines;
        lines.push(line);
      }
    };
    const result = this.queryChain.then(run, run);
    this.queryChain = result.then(() => undefined, () => undefined);
    return result;
  }

  /** Single-line replies (evalr/setr). */
  async queryOne(command) {
    const runner = async () => {
      this.send(command);
      return this.nextLine();
    };
    const result = this.queryChain.then(runner, runner);
    this.queryChain = result.then(() => undefined, () => undefined);
    return result;
  }

  // --- requests ------------------------------------------------------------------

  request_initialize(request) {
    this.clientLinesStartAt1 = request.arguments.linesStartAt1 !== false;
    this.sendResponse(request, {
      supportsConfigurationDoneRequest: true,
      supportsSetVariable: true,
      supportsEvaluateForHovers: true,
      supportsTerminateRequest: true,
      supportsConditionalBreakpoints: true,
      supportsHitConditionalBreakpoints: true,
      supportsLogPoints: true,
      supportsExceptionInfoRequest: true,
      supportsSingleThreadExecutionRequests: false,
    });
  }

  async request_launch(request) {
    const args = request.arguments || {};
    const program = args.program;
    if (!program || !fs.existsSync(program)) {
      this.sendResponse(request, undefined, false, `Program not found: ${program}`);
      return;
    }
    this.stopOnEntry = !!args.stopOnEntry;

    const workspaceRoot =
      vscode.workspace.getWorkspaceFolder(vscode.Uri.file(program))?.uri?.fsPath ||
      path.dirname(program);
    const compiler = this.deps.findCompiler(workspaceRoot, program);
    this.exePath = path.join(os.tmpdir(), `mettle-debug-${process.pid}-${nextPipeId}.exe`);

    // compile (debug build: never --release, hooks require -O0)
    this.output('console', `Compiling ${path.basename(program)} (--debug-hooks)...\n`);
    const compileArgs = [
      program, '-o', this.exePath, '--build', '--debug-hooks',
      '-I', path.dirname(program), '-I', workspaceRoot,
      ...(Array.isArray(args.compilerArgs) ? args.compilerArgs : []),
    ];
    // Hand-written kernel objects next to the source (extern ... = "symbol"):
    // link any sibling .o/.obj that defines a declared extern symbol, so
    // programs like the LLM engine debug without a launch.json.
    try {
      const { externLinkObjectsFor } = require('./language');
      for (const obj of externLinkObjectsFor(program)) {
        compileArgs.push('--link-arg', obj);
        this.output('console', `Linking extern object ${path.basename(obj)}\n`);
      }
    } catch (err) {
      this.log(`extern object scan failed: ${err.message}`);
    }
    const compiled = await new Promise((resolve) => {
      execFile(compiler, compileArgs, { cwd: workspaceRoot, maxBuffer: 4 * 1024 * 1024 },
        (err, stdout, stderr) => resolve({ err, output: (stderr || '') + (stdout || '') }));
    });
    if (compiled.err) {
      this.output('stderr', compiled.output);
      this.sendResponse(request, undefined, false, 'Compilation failed; see the debug console.');
      this.sendEvent('terminated', {});
      return;
    }

    // pipe server + process
    const pipeName = `\\\\.\\pipe\\mettle-debug-${process.pid}-${nextPipeId++}`;
    await new Promise((resolve, reject) => {
      this.server = net.createServer((socket) => {
        this.conn = socket;
        socket.setEncoding('utf8');
        socket.on('data', (chunk) => {
          this.lineBuffer += chunk;
          let idx;
          while ((idx = this.lineBuffer.indexOf('\n')) >= 0) {
            this.onPipeLine(this.lineBuffer.slice(0, idx).replace(/\r$/, ''));
            this.lineBuffer = this.lineBuffer.slice(idx + 1);
          }
        });
        socket.on('error', () => { /* runtime exited */ });
        socket.on('close', () => {
          // terminal-mode programs have no child handle: pipe EOF is the
          // end-of-session signal
          if (this.usesTerminal && !this.terminated) {
            this.terminated = true;
            this.sendEvent('terminated', {});
            this.shutdown();
          }
        });
      });
      this.server.on('error', reject);
      this.server.listen(pipeName, resolve);
    });

    const programArgs = Array.isArray(args.args) ? args.args : [];
    const cwd = args.cwd || path.dirname(program);
    if (args.console === 'integratedTerminal') {
      // Interactive programs (stdin prompts) run in the integrated terminal;
      // the debug pipe is independent of stdio. Termination is detected via
      // pipe EOF instead of a child handle.
      this.usesTerminal = true;
      this._onDidSendMessage.fire({
        seq: this._seq++, type: 'request', command: 'runInTerminal',
        arguments: {
          kind: 'integrated',
          title: `Debug ${path.basename(program)}`,
          cwd,
          env: { METTLE_DBG_PIPE: pipeName },
          args: [this.exePath, ...programArgs],
        },
      });
    } else {
      // stdin stays open (an interactive program blocks instead of seeing
      // EOF); use "console": "integratedTerminal" to actually type into it
      this.child = spawn(this.exePath, programArgs, {
        cwd,
        env: { ...process.env, METTLE_DBG_PIPE: pipeName },
        stdio: ['pipe', 'pipe', 'pipe'],
      });
      this.child.stdout.on('data', (d) => this.output('stdout', d.toString()));
      this.child.stderr.on('data', (d) => this.output('stderr', d.toString()));
      this.child.on('exit', (code) => {
        if (this.terminated) return;
        this.terminated = true;
        this.sendEvent('exited', { exitCode: code === null ? -1 : code });
        this.sendEvent('terminated', {});
        this.shutdown();
      });
    }

    // handshake: hello + tables + entry stop
    try {
      await this.nextLine(); // hello
      for (;;) {
        const line = await this.nextLine();
        if (line === 'tablesdone') break;
        const fields = line.split('\t');
        if (fields[0] === 'file') {
          const id = parseInt(fields[1], 10);
          this.files.set(id, fields[2]);
          this.fileIds.set(path.normalize(fields[2]).toLowerCase(), id);
        } else if (fields[0] === 'fn') {
          this.functions.set(parseInt(fields[1], 10), {
            fileId: parseInt(fields[2], 10),
            name: fields[3],
          });
        }
      }
      if (!this.entryStopSeen) {
        await new Promise((resolve) => { this.entryWaiter = resolve; });
      }
    } catch (err) {
      this.sendResponse(request, undefined, false, `Debug handshake failed: ${err.message}`);
      this.shutdown();
      return;
    }

    this.ready = true;
    for (const queued of this.pendingBreakpoints) this.send(queued);
    this.pendingBreakpoints = [];
    this.sendResponse(request, {});
    this.sendEvent('initialized', {});
  }

  request_setBreakpoints(request) {
    const args = request.arguments;
    const sourcePath = args.source && args.source.path ? args.source.path : '';
    const requested = args.breakpoints || [];
    const fileId = this.fileIds.get(path.normalize(sourcePath).toLowerCase());

    if (fileId === undefined) {
      // unknown to the runtime (not part of this program): report unverified
      this.sendResponse(request, {
        breakpoints: requested.map((bp) => ({
          verified: false, line: bp.line,
          message: this.ready ? 'This file is not part of the debugged program.' : 'Pending program start.',
        })),
      });
      return;
    }

    // setbp replaces the file's whole set (plain lines); conditional ones are
    // re-added individually. Logpoints and hit counts are adapter-side.
    for (const key of [...this.bpMeta.keys()]) {
      if (key.startsWith(`${fileId}:`)) this.bpMeta.delete(key);
    }
    const plain = [];
    const commands = [];
    for (const bp of requested) {
      if (bp.condition && bp.condition.trim()) {
        commands.push(`bpadd\t${fileId}\t${bp.line}\t${bp.condition.trim()}`);
      } else {
        plain.push(bp.line);
      }
      if (bp.logMessage || bp.hitCondition) {
        this.bpMeta.set(`${fileId}:${bp.line}`, {
          logMessage: bp.logMessage, hitCondition: bp.hitCondition, hits: 0,
        });
      }
    }
    commands.unshift(`setbp\t${fileId}\t${plain.join(',')}`);
    for (const command of commands) {
      if (this.ready) this.send(command);
      else this.pendingBreakpoints.push(command);
    }
    this.sendResponse(request, {
      breakpoints: requested.map((bp) => ({ verified: true, line: bp.line })),
    });
  }

  request_configurationDone(request) {
    this.configured = true;
    this.sendResponse(request, {});
    if (this.stopOnEntry) {
      this.sendEvent('stopped', { reason: 'entry', threadId: 1, allThreadsStopped: true });
    } else {
      this.send('go');
    }
  }

  request_threads(request) {
    this.sendResponse(request, { threads: [{ id: 1, name: 'main' }] });
  }

  async request_stackTrace(request) {
    const lines = await this.query('stack', 'framesdone');
    const frames = [];
    for (const line of lines) {
      const fields = line.split('\t');
      if (fields[0] !== 'frame') continue;
      const index = parseInt(fields[1], 10);
      const fnId = parseInt(fields[2], 10);
      const fnLine = parseInt(fields[3], 10);
      const fn = this.functions.get(fnId) || { name: `fn#${fnId}`, fileId: -1 };
      const filePath = this.files.get(fn.fileId);
      frames.push({
        id: index + 1, // 0 is reserved
        name: fn.name,
        line: fnLine || 1,
        column: 1,
        source: filePath
          ? { name: path.basename(filePath), path: filePath }
          : undefined,
      });
    }
    this.sendResponse(request, { stackFrames: frames, totalFrames: frames.length });
  }

  request_scopes(request) {
    const frameIndex = request.arguments.frameId - 1;
    this.sendResponse(request, {
      scopes: [{
        name: 'Locals',
        variablesReference: this.allocVarHandle(frameIndex, ''),
        expensive: false,
      }],
    });
  }

  // var line shape: var \t name \t type \t is_param \t kids \t value
  async request_variables(request) {
    const target = this.varHandles.get(request.arguments.variablesReference);
    if (!target) {
      this.sendResponse(request, { variables: [] });
      return;
    }
    const command = target.path === ''
      ? `vars\t${target.frame}`
      : `expand\t${target.frame}\t${target.path}`;
    const lines = await this.query(command, 'varsdone');
    const variables = [];
    for (const line of lines) {
      const fields = line.split('\t');
      if (fields[0] !== 'var') continue;
      const name = fields[1];
      const expandable = fields[4] === '1';
      variables.push({
        name,
        type: fields[2],
        value: fields.slice(5).join('\t'),
        variablesReference: expandable
          ? this.allocVarHandle(target.frame,
              MettleDebugAdapter.childPath(target.path, name))
          : 0,
      });
    }
    this.sendResponse(request, { variables });
  }

  async request_setVariable(request) {
    const target = this.varHandles.get(request.arguments.variablesReference);
    const { name, value } = request.arguments;
    if (!target) {
      this.sendResponse(request, undefined, false, 'Stale variable scope.');
      return;
    }
    const fullPath = MettleDebugAdapter.childPath(target.path, name);
    const reply = await this.queryOne(`set\t${target.frame}\t${fullPath}\t${value}`);
    const fields = reply.split('\t');
    if (fields[0] === 'setr' && fields[1] === '1') {
      this.sendResponse(request, { value: fields.slice(2).join('\t') });
    } else {
      this.sendResponse(request, undefined, false,
        `Cannot write \`${fullPath}\` (unsupported type or not in this frame).`);
    }
  }

  // eval reply shape: evalr \t ok \t type \t kids \t value
  async request_evaluate(request) {
    const args = request.arguments;
    const frameIndex = (args.frameId || 1) - 1;
    const expression = String(args.expression || '').trim();
    if (!/^[A-Za-z_][A-Za-z0-9_]*(\s*(\.|->)\s*[A-Za-z_][A-Za-z0-9_]*|\[\d+\])*$/.test(expression)) {
      this.sendResponse(request, undefined, false,
        'Evaluate accepts variable paths like `box.min.x` or `grid[2]`.');
      return;
    }
    const compact = expression.replace(/\s+/g, '');
    const reply = await this.queryOne(`eval\t${frameIndex}\t${compact}`);
    const fields = reply.split('\t');
    if (fields[0] === 'evalr' && fields[1] === '1') {
      this.sendResponse(request, {
        result: fields.slice(4).join('\t'),
        type: fields[2],
        variablesReference: fields[3] === '1'
          ? this.allocVarHandle(frameIndex, compact)
          : 0,
      });
    } else {
      this.sendResponse(request, undefined, false, `\`${expression}\` is not a variable in this frame.`);
    }
  }

  request_exceptionInfo(request) {
    this.sendResponse(request, {
      exceptionId: this.lastException ? String(this.lastException.code) : 'unknown',
      description: this.describeException(),
      breakMode: 'unhandled',
      details: {
        message: this.describeException(),
        typeName: 'hardware exception',
      },
    });
  }

  request_continue(request) {
    this.send('go');
    this.sendResponse(request, { allThreadsContinued: true });
  }

  request_next(request) {
    this.send('next');
    this.sendResponse(request, {});
  }

  request_stepIn(request) {
    this.send('stepin');
    this.sendResponse(request, {});
  }

  request_stepOut(request) {
    this.send('stepout');
    this.sendResponse(request, {});
  }

  request_pause(request) {
    this.send('pause');
    this.sendResponse(request, {});
  }

  request_terminate(request) {
    this.terminated = true;
    this.shutdown();
    this.sendResponse(request, {});
    this.sendEvent('terminated', {});
  }

  request_disconnect(request) {
    this.terminated = true;
    this.shutdown();
    this.sendResponse(request, {});
  }
}

/**
 * @param {vscode.ExtensionContext} context
 * @param {{ findCompiler: Function, log?: Function }} deps
 */
function registerDebugAdapter(context, deps) {
  context.subscriptions.push(
    vscode.debug.registerDebugAdapterDescriptorFactory('mettle', {
      createDebugAdapterDescriptor() {
        return new vscode.DebugAdapterInlineImplementation(new MettleDebugAdapter(deps));
      },
    }),
    vscode.debug.registerDebugConfigurationProvider('mettle', {
      resolveDebugConfiguration(folder, config) {
        // F5 with no launch.json: debug the active Mettle file
        if (!config.type && !config.request && !config.name) {
          const editor = vscode.window.activeTextEditor;
          if (editor && editor.document.languageId === 'mettle') {
            config.type = 'mettle';
            config.name = 'Debug Mettle file';
            config.request = 'launch';
            config.program = editor.document.uri.fsPath;
          }
        }
        if (config.program === '${file}' || !config.program) {
          const editor = vscode.window.activeTextEditor;
          if (editor && editor.document.languageId === 'mettle') {
            config.program = editor.document.uri.fsPath;
          }
        }
        if (!config.program) {
          vscode.window.showInformationMessage('Open a Mettle file to debug it.');
          return undefined;
        }
        return config;
      },
    })
  );
}

module.exports = { registerDebugAdapter, MettleDebugAdapter };
