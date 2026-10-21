// terminal.js - a small VT100/xterm terminal emulator connected to the
// MiniShell++ pseudo-terminal. Output arrives as Server-Sent Events (raw
// bytes, base64); every key press is sent back as the bytes a real
// terminal would send (Enter = \r, Ctrl-C = \x03, Ctrl-Z = \x1a, ...).
'use strict';

(function () {
  const termEl = document.getElementById('term');
  const screenEl = document.getElementById('term-screen');
  const inputEl = document.getElementById('term-input');
  const titleEl = document.getElementById('term-title');
  const offlineEl = document.getElementById('offline');

  const DEFAULT = Object.freeze({ fg: null, bg: null, b: false, d: false, i: false, u: false, inv: false });
  const clamp = (v, lo, hi) => Math.max(lo, Math.min(hi, v));
  const esc = s => s.replace(/[&<>]/g, c => (c === '&' ? '&amp;' : c === '<' ? '&lt;' : '&gt;'));

  function xterm256(n) {
    if (n < 16) return `var(--c${n})`;
    if (n < 232) {
      const lv = [0, 95, 135, 175, 215, 255];
      n -= 16;
      return `rgb(${lv[Math.floor(n / 36)]},${lv[Math.floor(n / 6) % 6]},${lv[n % 6]})`;
    }
    const g = 8 + 10 * (n - 232);
    return `rgb(${g},${g},${g})`;
  }
  const color = c => (typeof c === 'number' ? xterm256(clamp(c, 0, 255)) : c);

  class Emulator {
    constructor() {
      this.cols = 80;
      this.rows = 24;
      this.dirty = new Set();
      this.scheduled = false;
      this.follow = true;
      this.onTitle = null;
      this.onPrivate = null;
      this.onReply = null;
      this.reset();
    }

    reset() {
      this.lines = [];
      this.rowEls = [];
      screenEl.replaceChildren();
      this.x = 0;
      this.y = 0;
      this.style = DEFAULT;
      this.saved = { x: 0, y: 0, style: DEFAULT };
      this.cursorVisible = true;
      this.state = 0;
      this.seq = '';
      this.decoder = new TextDecoder();
      this.dirty.clear();
      for (let i = 0; i < this.rows; i++) this.pushLine();
    }

    get top() { return this.lines.length - this.rows; }

    pushLine() {
      this.lines.push([]);
      const el = document.createElement('div');
      el.className = 'row';
      screenEl.appendChild(el);
      this.rowEls.push(el);
      this.markDirty(this.lines.length - 1);
    }

    trimScrollback() {
      const max = this.rows + 5000;
      if (this.lines.length <= max) return;
      const drop = this.lines.length - (this.rows + 4000);
      this.lines.splice(0, drop);
      for (const el of this.rowEls.splice(0, drop)) el.remove();
      this.dirty.clear();
      this.markScreenDirty();
    }

    markDirty(i) {
      this.dirty.add(i);
      if (!this.scheduled) {
        this.scheduled = true;
        requestAnimationFrame(() => this.render());
      }
    }

    markScreenDirty() {
      for (let i = Math.max(0, this.top); i < this.lines.length; i++) this.markDirty(i);
    }

    // ------------------------------------------------------------ input
    write(bytes) {
      const text = this.decoder.decode(bytes, { stream: true });
      for (const ch of text) this.feed(ch);
    }

    feed(ch) {
      const code = ch.codePointAt(0);
      switch (this.state) {
        case 0:
          if (code === 0x1b) { this.state = 1; return; }
          if (code < 0x20 || code === 0x7f) { this.control(code); return; }
          this.put(ch);
          return;
        case 1:   // after ESC
          this.state = 0;
          if (ch === '[') { this.state = 2; this.seq = ''; }
          else if (ch === ']') { this.state = 3; this.seq = ''; }
          else if ('()*+#'.includes(ch)) this.state = 5;
          else if (ch === '7') this.saveCursor();
          else if (ch === '8') this.restoreCursor();
          else if (ch === 'D') this.lineFeed();
          else if (ch === 'E') { this.moveTo(0, this.y); this.lineFeed(); }
          else if (ch === 'M') this.reverseIndex();
          else if (ch === 'c') this.reset();
          return;
        case 2:   // CSI
          if (code >= 0x40 && code <= 0x7e) { this.state = 0; this.csi(this.seq, ch); return; }
          if (code === 0x1b) { this.state = 1; return; }
          this.seq += ch;
          if (this.seq.length > 64) this.state = 0;
          return;
        case 3:   // OSC
          if (code === 0x07) { this.state = 0; this.osc(this.seq); return; }
          if (code === 0x1b) { this.state = 4; return; }
          this.seq += ch;
          if (this.seq.length > 8192) this.state = 0;
          return;
        case 4:
          this.state = 0;
          if (ch === '\\') this.osc(this.seq);
          return;
        case 5:
          this.state = 0;
          return;
      }
    }

    control(code) {
      switch (code) {
        case 0x0d: this.moveTo(0, this.y); break;
        case 0x0a: case 0x0b: case 0x0c: this.lineFeed(); break;
        case 0x08: this.moveTo(Math.min(this.x, this.cols) - 1, this.y); break;
        case 0x09: this.moveTo(Math.min(this.cols - 1, (Math.floor(this.x / 8) + 1) * 8), this.y); break;
        default: break;   // BEL and friends
      }
    }

    put(ch) {
      if (this.x >= this.cols) {   // deferred auto-wrap
        this.x = 0;
        this.lineFeed();
      }
      const li = this.top + this.y;
      this.lines[li][this.x] = { ch, st: this.style };
      this.x++;
      this.markDirty(li);
    }

    lineFeed() {
      this.markDirty(this.top + this.y);
      if (this.y < this.rows - 1) this.y++;
      else {
        this.pushLine();
        this.trimScrollback();
      }
      this.markDirty(this.top + this.y);
    }

    reverseIndex() {
      if (this.y > 0) { this.moveTo(this.x, this.y - 1); return; }
      const t = this.top;
      for (let i = t + this.rows - 1; i > t; i--) this.lines[i] = this.lines[i - 1];
      this.lines[t] = [];
      this.markScreenDirty();
    }

    moveTo(x, y) {
      this.markDirty(this.top + this.y);
      this.x = clamp(x, 0, this.cols - 1);
      this.y = clamp(y, 0, this.rows - 1);
      this.markDirty(this.top + this.y);
    }

    saveCursor() { this.saved = { x: this.x, y: this.y, style: this.style }; }
    restoreCursor() { this.moveTo(this.saved.x, this.saved.y); this.style = this.saved.style; }

    csi(seq, f) {
      let priv = '';
      if (seq && '?>=!'.includes(seq[0])) { priv = seq[0]; seq = seq.slice(1); }
      const p = seq === '' ? [] : seq.split(';').map(s => parseInt(s, 10));
      const n = (i, d = 1) => (p[i] === undefined || Number.isNaN(p[i]) || p[i] === 0 ? d : p[i]);
      const t = this.top;
      const line = this.lines[t + this.y];
      const cx = Math.min(this.x, this.cols - 1);

      if (priv === '?') {
        if ((f === 'h' || f === 'l') && p.includes(25)) {
          this.cursorVisible = f === 'h';
          this.markDirty(t + this.y);
        }
        return;
      }
      if (priv) return;

      switch (f) {
        case 'm': this.sgr(p); break;
        case 'A': this.moveTo(this.x, this.y - n(0)); break;
        case 'B': case 'e': this.moveTo(this.x, this.y + n(0)); break;
        case 'C': case 'a': this.moveTo(cx + n(0), this.y); break;
        case 'D': this.moveTo(cx - n(0), this.y); break;
        case 'E': this.moveTo(0, this.y + n(0)); break;
        case 'F': this.moveTo(0, this.y - n(0)); break;
        case 'G': case '`': this.moveTo(n(0) - 1, this.y); break;
        case 'H': case 'f': this.moveTo(n(1) - 1, n(0) - 1); break;
        case 'd': this.moveTo(this.x, n(0) - 1); break;
        case 'J': this.eraseDisplay(p[0] || 0); break;
        case 'K': this.eraseLine(p[0] || 0); break;
        case 'P': line.splice(cx, n(0)); this.markDirty(t + this.y); break;
        case '@': line.splice(cx, 0, ...new Array(n(0))); if (line.length > this.cols) line.length = this.cols; this.markDirty(t + this.y); break;
        case 'X': for (let k = 0; k < n(0); k++) delete line[cx + k]; this.markDirty(t + this.y); break;
        case 'L': {
          const cnt = Math.min(n(0), this.rows - this.y);
          this.lines.splice(t + this.y, 0, ...Array.from({ length: cnt }, () => []));
          this.lines.splice(t + this.rows, cnt);
          this.markScreenDirty();
          break;
        }
        case 'M': {
          const cnt = Math.min(n(0), this.rows - this.y);
          this.lines.splice(t + this.y, cnt);
          this.lines.splice(t + this.rows - cnt, 0, ...Array.from({ length: cnt }, () => []));
          this.markScreenDirty();
          break;
        }
        case 's': this.saveCursor(); break;
        case 'u': this.restoreCursor(); break;
        case 'n':
          if (p[0] === 6) this.onReply?.(`\x1b[${this.y + 1};${cx + 1}R`);
          else if (p[0] === 5) this.onReply?.('\x1b[0n');
          break;
        case 'c': this.onReply?.('\x1b[?1;2c'); break;
        default: break;
      }
    }

    eraseLine(mode) {
      const li = this.top + this.y;
      const line = this.lines[li];
      if (mode === 0) { if (line.length > this.x) line.length = this.x; }
      else if (mode === 1) { for (let k = 0; k <= Math.min(this.x, this.cols - 1); k++) delete line[k]; }
      else this.lines[li] = [];
      this.markDirty(li);
    }

    eraseDisplay(mode) {
      const t = this.top;
      if (mode === 0) {
        this.eraseLine(0);
        for (let i = t + this.y + 1; i < t + this.rows; i++) this.lines[i] = [];
      } else if (mode === 1) {
        for (let i = t; i < t + this.y; i++) this.lines[i] = [];
        this.eraseLine(1);
      } else if (mode === 2) {
        for (let i = t; i < t + this.rows; i++) this.lines[i] = [];
      } else if (mode === 3) {
        this.lines.splice(0, t);
        for (const el of this.rowEls.splice(0, t)) el.remove();
        this.dirty.clear();
      }
      this.markScreenDirty();
    }

    sgr(p) {
      if (p.length === 0) p = [0];
      let s = { ...this.style };
      for (let i = 0; i < p.length; i++) {
        const v = Number.isNaN(p[i]) ? 0 : p[i];
        if (v === 0) s = { ...DEFAULT };
        else if (v === 1) s.b = true;
        else if (v === 2) s.d = true;
        else if (v === 3) s.i = true;
        else if (v === 4) s.u = true;
        else if (v === 7) s.inv = true;
        else if (v === 22) { s.b = false; s.d = false; }
        else if (v === 23) s.i = false;
        else if (v === 24) s.u = false;
        else if (v === 27) s.inv = false;
        else if (v >= 30 && v <= 37) s.fg = v - 30;
        else if (v === 39) s.fg = null;
        else if (v >= 40 && v <= 47) s.bg = v - 40;
        else if (v === 49) s.bg = null;
        else if (v >= 90 && v <= 97) s.fg = v - 90 + 8;
        else if (v >= 100 && v <= 107) s.bg = v - 100 + 8;
        else if (v === 38 || v === 48) {
          const key = v === 38 ? 'fg' : 'bg';
          if (p[i + 1] === 5) { s[key] = p[i + 2] | 0; i += 2; }
          else if (p[i + 1] === 2) { s[key] = `rgb(${p[i + 2] | 0},${p[i + 3] | 0},${p[i + 4] | 0})`; i += 4; }
        }
      }
      const plain = !s.fg && s.fg !== 0 && !s.bg && s.bg !== 0 && !s.b && !s.d && !s.i && !s.u && !s.inv;
      this.style = plain ? DEFAULT : Object.freeze(s);
    }

    osc(body) {
      const semi = body.indexOf(';');
      const code = semi < 0 ? body : body.slice(0, semi);
      const rest = semi < 0 ? '' : body.slice(semi + 1);
      if (code === '0' || code === '2') this.onTitle?.(rest);
      else if (code === '7777') this.onPrivate?.(rest);
    }

    // ----------------------------------------------------------- render
    spanFor(st, text) {
      if (st === DEFAULT) return esc(text);
      let fg = st.fg, bg = st.bg;
      if (st.b && typeof fg === 'number' && fg < 8) fg += 8;   // bold is bright, like GNOME Terminal
      if (st.inv) {
        const f = fg, b = bg;
        fg = b === null ? 'var(--term-bg)' : b;
        bg = f === null ? 'var(--term-fg)' : f;
      }
      const css = [];
      if (fg !== null) css.push(`color:${color(fg)}`);
      if (bg !== null) css.push(`background:${color(bg)}`);
      const cls = [st.b && 'b', st.i && 'i', st.u && 'u', st.d && 'd'].filter(Boolean).join(' ');
      return `<span${cls ? ` class="${cls}"` : ''}${css.length ? ` style="${css.join(';')}"` : ''}>${esc(text)}</span>`;
    }

    renderLine(i) {
      const line = this.lines[i];
      const el = this.rowEls[i];
      if (!line || !el) return;
      const cursorHere = this.cursorVisible && i === this.top + this.y;
      const cx = Math.min(this.x, this.cols - 1);
      const len = cursorHere ? Math.max(line.length, cx + 1) : line.length;
      let html = '', run = '', runStyle = null;
      const flush = () => { if (run) html += this.spanFor(runStyle, run); run = ''; };
      for (let c = 0; c < len; c++) {
        const cell = line[c];
        const ch = cell ? cell.ch : ' ';
        const st = cell ? cell.st : DEFAULT;
        if (cursorHere && c === cx) {
          flush();
          runStyle = null;
          html += `<span class="cursor">${esc(ch)}</span>`;
          continue;
        }
        if (st !== runStyle) { flush(); runStyle = st; }
        run += ch;
      }
      flush();
      el.innerHTML = html;
    }

    render() {
      this.scheduled = false;
      for (const i of this.dirty) this.renderLine(i);
      this.dirty.clear();
      if (this.follow) termEl.scrollTop = termEl.scrollHeight;
    }

    resize(cols, rows) {
      const absY = this.top + this.y;
      this.cols = cols;
      while (this.lines.length < rows) this.pushLine();
      this.rows = rows;
      this.y = clamp(absY - this.top, 0, rows - 1);
      this.x = Math.min(this.x, cols);
      this.markScreenDirty();
    }
  }

  const emu = new Emulator();
  window.MiniTerm = emu;

  // ------------------------------------------------------------- server
  let pending = '';
  let flushing = false;
  async function send(data) {
    pending += data;
    if (flushing) return;
    flushing = true;
    while (pending) {
      const chunk = pending;
      pending = '';
      try {
        await fetch('/api/input', { method: 'POST', body: chunk });
      } catch {
        offlineEl.hidden = false;
      }
    }
    flushing = false;
  }
  emu.onReply = send;

  emu.onTitle = title => {
    titleEl.textContent = title || 'Terminal';
    document.title = title || 'Terminal';
  };
  emu.onPrivate = body => {
    const [kind, pid] = body.split(';');
    if (kind === 'gedit' && window.Gedit) window.Gedit.open(Number(pid));
  };

  function connect() {
    const es = new EventSource('/api/stream');
    es.addEventListener('reset', () => {
      emu.reset();
      offlineEl.hidden = true;
    });
    es.onmessage = e => {
      const bin = atob(e.data);
      const bytes = new Uint8Array(bin.length);
      for (let i = 0; i < bin.length; i++) bytes[i] = bin.charCodeAt(i);
      emu.write(bytes);
    };
    es.onerror = () => {
      if (es.readyState === EventSource.CLOSED) offlineEl.hidden = false;
      setTimeout(() => { if (es.readyState !== EventSource.OPEN) offlineEl.hidden = false; }, 1500);
    };
    es.onopen = () => { offlineEl.hidden = true; };
  }

  // ---------------------------------------------------------------- size
  let lastSize = '';
  function fit() {
    const probe = document.createElement('span');
    probe.className = 'measure';
    probe.textContent = 'M'.repeat(100);
    termEl.appendChild(probe);
    const cw = probe.getBoundingClientRect().width / 100;
    probe.remove();
    const lh = parseFloat(getComputedStyle(screenEl).lineHeight);
    const cs = getComputedStyle(termEl);
    const w = termEl.clientWidth - parseFloat(cs.paddingLeft) - parseFloat(cs.paddingRight);
    const h = termEl.clientHeight - parseFloat(cs.paddingTop) - parseFloat(cs.paddingBottom);
    const cols = clamp(Math.floor(w / cw), 20, 500);
    const rows = clamp(Math.floor(h / lh), 5, 200);
    const key = `${cols}x${rows}`;
    if (key === lastSize) return;
    lastSize = key;
    emu.resize(cols, rows);
    fetch(`/api/resize?cols=${cols}&rows=${rows}`, { method: 'POST' }).catch(() => {});
  }
  let resizeTimer;
  window.addEventListener('resize', () => {
    clearTimeout(resizeTimer);
    resizeTimer = setTimeout(fit, 80);
  });

  // ------------------------------------------------------------ keyboard
  const KEYS = {
    Enter: '\r', Backspace: '\x7f', Tab: '\t', Escape: '\x1b',
    ArrowUp: '\x1b[A', ArrowDown: '\x1b[B', ArrowRight: '\x1b[C', ArrowLeft: '\x1b[D',
    Home: '\x1b[H', End: '\x1b[F', Delete: '\x1b[3~', Insert: '\x1b[2~', PageUp: '\x1b[5~', PageDown: '\x1b[6~',
  };

  inputEl.addEventListener('keydown', e => {
    if (e.isComposing) return;
    const CODES = { 13: 'Enter', 8: 'Backspace', 9: 'Tab', 27: 'Escape', 37: 'ArrowLeft', 38: 'ArrowUp', 39: 'ArrowRight', 40: 'ArrowDown', 46: 'Delete', 36: 'Home', 35: 'End' };
    const k = e.key && e.key !== 'Unidentified' ? e.key : (CODES[e.keyCode] || (e.code || '').replace(/^Key/, '').toLowerCase());
    const selection = window.getSelection().toString();

    if (e.metaKey) {
      if (k.toLowerCase() === 'c' && selection) navigator.clipboard?.writeText(selection);
      return;   // let the browser handle Cmd+V (paste event below), Cmd+R, ...
    }

    let data = null;
    if (e.ctrlKey && !e.altKey) {
      if (e.shiftKey && k.toLowerCase() === 'c') {
        if (selection) navigator.clipboard?.writeText(selection);
        e.preventDefault();
        return;
      }
      if (e.shiftKey && k.toLowerCase() === 'v') return;
      if (k.length === 1) {
        const c = k.toLowerCase().charCodeAt(0);
        if (c >= 97 && c <= 122) data = String.fromCharCode(c - 96);   // Ctrl-C = 0x03, Ctrl-Z = 0x1a
        else if (k === '[') data = '\x1b';
        else if (k === '\\') data = '\x1c';
        else if (k === ']') data = '\x1d';
        else if (k === ' ' || k === '@') data = '\x00';
      }
    }
    if (data === null && KEYS[k] !== undefined) data = KEYS[k];
    // Printable characters arrive through the input event below.

    if (data !== null) {
      e.preventDefault();
      emu.follow = true;
      restartBlink();
      send(data);
    }
  });

  inputEl.addEventListener('input', e => {
    if (e.isComposing) return;
    const text = inputEl.value;
    inputEl.value = '';
    if (text) {
      emu.follow = true;
      restartBlink();
      send(text.replace(/\r?\n/g, '\r'));
    }
  });
  inputEl.addEventListener('compositionend', () => {
    const text = inputEl.value;
    inputEl.value = '';
    if (text) send(text);
  });
  inputEl.addEventListener('paste', e => {
    e.preventDefault();
    const text = e.clipboardData.getData('text');
    if (text) {
      emu.follow = true;
      send(text.replace(/\r?\n/g, '\r'));
    }
  });

  // --------------------------------------------------------------- focus
  function focusTerminal() {
    inputEl.focus({ preventScroll: true });
  }
  window.focusTerminal = focusTerminal;
  inputEl.addEventListener('focus', () => {
    termEl.classList.add('focused');
    window.Gedit?.deactivateAll();
  });
  inputEl.addEventListener('blur', () => termEl.classList.remove('focused'));
  termEl.addEventListener('mouseup', () => {
    if (!window.getSelection().toString()) focusTerminal();
  });
  termEl.addEventListener('scroll', () => {
    emu.follow = termEl.scrollHeight - termEl.scrollTop - termEl.clientHeight < 40;
  });

  let blinkTimer;
  function restartBlink() {
    termEl.classList.remove('blink-off');
    clearInterval(blinkTimer);
    blinkTimer = setInterval(() => termEl.classList.toggle('blink-off'), 600);
  }

  // ---------------------------------------------------------------- start
  (document.fonts?.ready || Promise.resolve()).then(() => {
    fit();
    connect();
    focusTerminal();
    restartBlink();
  });
})();
