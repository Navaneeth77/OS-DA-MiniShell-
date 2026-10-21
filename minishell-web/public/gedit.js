// gedit.js - the gedit window opened by `gedit FILE` in the terminal.
// The gedit *process* runs in the shell (it can be stopped with Ctrl-Z,
// moved with fg/bg, killed); this window is its user interface.
'use strict';

(function () {
  const tpl = document.getElementById('gedit-tpl');
  const windows = new Map();   // pid -> window object (null while opening)
  let zTop = 10;
  let cascade = 0;

  const esc = s => s.replace(/[&<>]/g, c => (c === '&' ? '&amp;' : c === '<' ? '&lt;' : '&gt;'));

  async function api(url, options) {
    const res = await fetch(url, options);
    const data = await res.json().catch(() => ({}));
    if (!res.ok) throw new Error(data.error || `HTTP ${res.status}`);
    return data;
  }

  // ------------------------------------------------------ C highlighting
  const KEYWORDS = new Set('auto break case const continue default do else enum extern for goto if inline register restrict return sizeof static struct switch typedef union volatile while'.split(' '));
  const TYPES = new Set('char double float int long short signed unsigned void bool size_t ssize_t FILE pid_t NULL'.split(' '));
  const TOKEN = /(\/\*[\s\S]*?(?:\*\/|$))|(\/\/[^\n]*)|("(?:[^"\\\n]|\\.)*"?|'(?:[^'\\\n]|\\.)*'?)|(^[ \t]*#[ \t]*\w*)|(\b0[xX][0-9a-fA-F]+\b|\b\d+(?:\.\d+)?[uUlLfF]*\b)|([A-Za-z_]\w*)|(<[\w./]+>)/gm;

  function highlightC(src) {
    let out = '';
    let last = 0;
    let inInclude = false;
    for (const m of src.matchAll(TOKEN)) {
      const gap = src.slice(last, m.index);
      if (gap.includes('\n')) inInclude = false;
      out += esc(gap);
      const t = m[0];
      if (m[1] || m[2]) out += `<span class="hl-com">${esc(t)}</span>`;
      else if (m[3]) out += `<span class="hl-str">${esc(t)}</span>`;
      else if (m[4]) { out += `<span class="hl-pre">${esc(t)}</span>`; inInclude = /include/.test(t); }
      else if (m[5]) out += `<span class="hl-num">${t}</span>`;
      else if (m[6]) out += KEYWORDS.has(t) ? `<span class="hl-kw">${t}</span>` : TYPES.has(t) ? `<span class="hl-type">${t}</span>` : t;
      else if (m[7]) out += inInclude ? `<span class="hl-str">${esc(t)}</span>` : esc(t);
      last = m.index + t.length;
    }
    return out + esc(src.slice(last)) + '\n';
  }

  const LANGS = { c: 'C', h: 'C', cpp: 'C++', cc: 'C++', hpp: 'C++', py: 'Python', sh: 'sh', txt: 'Plain Text', md: 'Markdown', java: 'Java', js: 'JavaScript' };

  // ------------------------------------------------------------- windows
  async function open(pid) {
    if (!pid || windows.has(pid)) return;
    windows.set(pid, null);
    let info;
    try {
      const status = await api(`/api/gedit/status?pid=${pid}`);
      if (!status.alive) { windows.delete(pid); return; }   // replayed output of an editor that already closed
      info = await api(`/api/gedit/open?pid=${pid}`);
    } catch {
      windows.delete(pid);
      return;
    }
    windows.set(pid, createWindow(pid, info));
  }

  function createWindow(pid, info) {
    const el = tpl.content.firstElementChild.cloneNode(true);
    const $ = sel => el.querySelector(sel);
    const text = $('.g-text');
    const highlight = $('.g-highlight');
    const gutter = $('.g-gutter');
    const editor = $('.g-editor');
    const ext = (info.name.split('.').pop() || '').toLowerCase();
    const isC = ['c', 'h', 'cpp', 'cc', 'hpp', 'java', 'js'].includes(ext);
    const dir = info.path.slice(0, info.path.length - info.name.length - 1) || '/';

    const w = { pid, el, modified: false, stopped: false, closing: false };

    el.style.left = `${Math.max(16, (window.innerWidth - Math.min(760, window.innerWidth - 32)) / 2 + cascade)}px`;
    el.style.top = `${Math.max(16, 60 + cascade)}px`;
    cascade = (cascade + 30) % 150;

    $('.g-lang').textContent = `${LANGS[ext] || 'Plain Text'} ▾`;
    $('.g-path').textContent = dir;
    $('.g-frozen-name').textContent = `${info.name} - gedit`;
    $('.g-confirm-name').textContent = info.name;
    text.value = info.content;

    function updateTitle() {
      $('.g-name').textContent = (w.modified ? '*' : '') + info.name;
    }

    function refresh() {
      highlight.innerHTML = isC ? highlightC(text.value) : esc(text.value) + '\n';
      const lines = text.value.split('\n').length;
      let nums = '';
      for (let i = 1; i <= lines; i++) nums += i + '\n';
      gutter.textContent = nums;
      text.style.height = 'auto';
      text.style.width = 'auto';
      text.style.height = `${Math.max(text.scrollHeight, editor.clientHeight)}px`;
      text.style.width = `${Math.max(text.scrollWidth, editor.clientWidth - gutter.offsetWidth)}px`;
      updatePosition();
    }

    function updatePosition() {
      const before = text.value.slice(0, text.selectionStart);
      const line = before.split('\n').length;
      const col = before.length - before.lastIndexOf('\n');
      $('.g-pos').textContent = `Ln ${line}, Col ${col}`;
    }

    async function save() {
      if (w.stopped) return false;
      try {
        await api(`/api/gedit/save?pid=${pid}`, { method: 'POST', body: text.value });
        w.modified = false;
        updateTitle();
        return true;
      } catch (err) {
        $('.g-path').textContent = `Could not save: ${err.message}`;
        return false;
      }
    }

    async function close(force) {
      if (w.stopped) return;
      if (w.modified && !force) {
        $('.g-confirm').hidden = false;
        $('.g-save-close').focus();
        return;
      }
      w.closing = true;
      try { await api(`/api/gedit/close?pid=${pid}`, { method: 'POST' }); } catch { /* already gone */ }
      destroy();
    }

    function destroy() {
      clearInterval(w.poll);
      el.remove();
      windows.delete(pid);
      window.focusTerminal?.();
    }

    function activate() {
      for (const other of windows.values()) other?.el.classList.add('inactive');
      el.classList.remove('inactive');
      el.style.zIndex = ++zTop;
    }
    w.deactivate = () => el.classList.add('inactive');

    // ---- events
    text.addEventListener('input', () => {
      if (!w.modified) { w.modified = true; updateTitle(); }
      refresh();
    });
    text.addEventListener('keyup', updatePosition);
    text.addEventListener('click', updatePosition);
    text.addEventListener('keydown', e => {
      const mod = e.ctrlKey || e.metaKey;
      if (mod && e.key.toLowerCase() === 's') { e.preventDefault(); save(); }
      else if (mod && e.key.toLowerCase() === 'w') { e.preventDefault(); close(false); }
      else if (e.key === 'Tab' && !mod) {
        e.preventDefault();
        const { selectionStart: s, selectionEnd: t } = text;
        text.setRangeText('\t', s, t, 'end');
        text.dispatchEvent(new Event('input'));
      }
    });
    $('.save-btn').addEventListener('click', save);
    $('.g-close').addEventListener('click', () => close(false));
    $('.g-max').addEventListener('click', () => { el.classList.toggle('maximized'); refresh(); });
    $('.g-discard').addEventListener('click', () => close(true));
    $('.g-cancel').addEventListener('click', () => { $('.g-confirm').hidden = true; text.focus(); });
    $('.g-save-close').addEventListener('click', async () => { if (await save()) close(true); });
    $('.g-force').addEventListener('click', async () => {
      try { await api(`/api/gedit/kill?pid=${pid}`, { method: 'POST' }); } catch { /* ignore */ }
    });
    el.addEventListener('mousedown', activate);

    // drag by the header bar
    $('.gedit-bar').addEventListener('mousedown', e => {
      if (e.target.closest('button, .wc') || el.classList.contains('maximized')) return;
      const startX = e.clientX, startY = e.clientY;
      const left = el.offsetLeft, top = el.offsetTop;
      const move = ev => {
        el.style.left = `${clamp(left + ev.clientX - startX, -el.offsetWidth + 80, window.innerWidth - 80)}px`;
        el.style.top = `${clamp(top + ev.clientY - startY, 0, window.innerHeight - 46)}px`;
      };
      const up = () => { document.removeEventListener('mousemove', move); document.removeEventListener('mouseup', up); };
      document.addEventListener('mousemove', move);
      document.addEventListener('mouseup', up);
      e.preventDefault();
    });
    $('.gedit-bar').addEventListener('dblclick', e => {
      if (!e.target.closest('button, .wc')) { el.classList.toggle('maximized'); refresh(); }
    });

    // Follow the real gedit process: stopped by Ctrl-Z, or killed.
    w.poll = setInterval(async () => {
      if (w.closing) return;
      let status;
      try { status = await api(`/api/gedit/status?pid=${pid}`); } catch { return; }
      if (!status.alive) { destroy(); return; }
      if (status.stopped !== w.stopped) {
        w.stopped = status.stopped;
        $('.g-frozen').hidden = !w.stopped;
        text.readOnly = w.stopped;
      }
    }, 700);

    document.body.appendChild(el);
    updateTitle();
    activate();
    refresh();
    text.focus();
    text.setSelectionRange(0, 0);
    return w;
  }

  const clamp = (v, lo, hi) => Math.max(lo, Math.min(hi, v));

  window.Gedit = {
    open,
    deactivateAll() { for (const w of windows.values()) w?.deactivate(); },
  };
})();
