/* AutoRemesher GUI front end: parameter panel, job control, SSE log, preview. */
(function () {
  'use strict';

  var TOKEN = (function () {
    var m = location.pathname.match(/\/s\/([0-9a-zA-Z]+)\//);
    if (m) return m[1];
    var q = new URLSearchParams(location.search).get('token');
    return q || '';
  })();

  function api(path, params) {
    var url = path + (path.indexOf('?') >= 0 ? '&' : '?') + 'token=' + encodeURIComponent(TOKEN);
    if (params) {
      Object.keys(params).forEach(function (k) {
        if (params[k] !== undefined && params[k] !== null) {
          url += '&' + encodeURIComponent(k) + '=' + encodeURIComponent(params[k]);
        }
      });
    }
    return url;
  }

  function $(id) { return document.getElementById(id); }

  var el = {
    log: $('log'), logCount: $('logcount'), logPane: $('logpane'),
    bar: $('bar'), pct: $('pct'), status: $('status'), elapsed: $('elapsed'),
    meta: document.querySelector('.meta'),
    run: $('run'), cancel: $('cancel'), download: $('download'),
    drop: $('drop'), file: $('file'), dropInfo: $('drop-info'),
    overlay: $('overlay'), stats: $('stats'), canvas: $('gl'),
    cliChip: $('cli-chip'), cliDot: $('cli-dot'), cliText: $('cli-text'),
    verChip: $('ver-chip'), mode: $('mode'),
    logToggle: $('logtoggle'), logClear: $('logclear')
  };

  var state = {
    input: null,
    jobId: null,
    running: false,
    startedAt: 0,
    timer: null,
    es: null,
    result: null,
    logLines: 0
  };

  var params = {};

  var viewer = null;
  if (window.Viewer) {
    try { viewer = new window.Viewer(el.canvas); } catch (e) { viewer = null; }
  }
  if (!viewer || !viewer.ready) {
    el.overlay.textContent = 'WebGL is unavailable in this browser — the log and export still work.';
  }

  // --------------------------------------------------------------- formatting
  function fmtInt(n) {
    if (n === undefined || n === null || isNaN(n)) return '—';
    return Number(n).toLocaleString('en-US');
  }
  function fmtBytes(n) {
    if (!n && n !== 0) return '—';
    var u = ['B', 'KB', 'MB', 'GB'];
    var i = 0;
    var v = n;
    while (v >= 1024 && i < u.length - 1) { v /= 1024; i++; }
    return (i === 0 ? v : v.toFixed(1)) + ' ' + u[i];
  }
  function nowLabel() {
    var d = new Date();
    function p(x, w) { return String(x).padStart(w, '0'); }
    return p(d.getHours(), 2) + ':' + p(d.getMinutes(), 2) + ':' + p(d.getSeconds(), 2);
  }

  // --------------------------------------------------------------------- log
  function addLog(level, message) {
    if (message === undefined || message === null) return;
    var line = document.createElement('div');
    line.className = 'line ' + (level || 'info');
    var t = document.createElement('span');
    t.className = 't';
    t.textContent = nowLabel();
    var m = document.createElement('span');
    m.className = 'm';
    m.textContent = String(message);
    line.appendChild(t);
    line.appendChild(m);
    var atBottom = el.log.scrollTop + el.log.clientHeight >= el.log.scrollHeight - 24;
    el.log.appendChild(line);
    state.logLines++;
    while (el.log.childElementCount > 2000) {
      el.log.removeChild(el.log.firstChild);
    }
    el.logCount.textContent = state.logLines + ' lines';
    if (atBottom) el.log.scrollTop = el.log.scrollHeight;
  }

  el.logClear.addEventListener('click', function () {
    el.log.innerHTML = '';
    state.logLines = 0;
    el.logCount.textContent = '';
  });

  el.logToggle.addEventListener('click', function () {
    var collapsed = el.logPane.classList.toggle('collapsed');
    el.logToggle.setAttribute('aria-expanded', String(!collapsed));
    el.logToggle.firstChild.nodeValue = collapsed ? 'Log ▲' : 'Log ▼';
    if (viewer) viewer.requestRender();
  });

  // ------------------------------------------------------------------- params
  function initParams() {
    var rows = document.querySelectorAll('.row[data-param]');
    Array.prototype.forEach.call(rows, function (row) {
      var name = row.getAttribute('data-param');
      var type = row.getAttribute('data-type') || 'range';
      var def = row.getAttribute('data-default') || '0';
      var num = row.querySelector('.num');
      var rng = row.querySelector('.rng');
      var sel = row.querySelector('.sel');
      var chk = row.querySelector('input[type="checkbox"]');

      if (type === 'bool') {
        params[name] = def === 'true';
        chk.checked = params[name];
        chk.addEventListener('change', function () { params[name] = chk.checked; });
        return;
      }
      if (type === 'select') {
        params[name] = def;
        sel.value = def;
        sel.addEventListener('change', function () { params[name] = sel.value; });
        return;
      }
      if (type === 'int') {
        params[name] = Number(def);
        num.value = String(params[name]);
        num.addEventListener('input', function () {
          var v = Number(num.value);
          if (!isFinite(v)) return;
          params[name] = Math.round(v);
        });
        num.addEventListener('blur', function () {
          var lo = Number(row.getAttribute('data-min') || 0);
          var hi = Number(row.getAttribute('data-max') || 0);
          var v = Math.round(Number(num.value) || 0);
          if (v < lo) v = lo;
          if (v > hi) v = hi;
          params[name] = v;
          num.value = String(v);
        });
        return;
      }

      var min = Number(row.getAttribute('data-min'));
      var max = Number(row.getAttribute('data-max'));
      var step = Number(row.getAttribute('data-step') || '1');
      var logScale = type === 'log';
      var toSlider = function (v) {
        if (!logScale) return v;
        v = Math.min(max, Math.max(min, v));
        return Math.round(1000 * Math.log(v / min) / Math.log(max / min));
      };
      var fromSlider = function (t) {
        if (!logScale) return Number(t);
        return Math.round(min * Math.pow(max / min, t / 1000));
      };

      params[name] = Number(def);
      rng.min = logScale ? 0 : min;
      rng.max = logScale ? 1000 : max;
      rng.step = logScale ? 1 : step;
      rng.value = String(toSlider(params[name]));
      num.value = String(params[name]);

      rng.addEventListener('input', function () {
        var v = fromSlider(Number(rng.value));
        params[name] = v;
        num.value = String(v);
      });
      num.addEventListener('input', function () {
        var v = Number(num.value);
        if (!isFinite(v)) return;
        params[name] = v;
        rng.value = String(toSlider(v));
      });
      num.addEventListener('blur', function () {
        var v = Number(num.value);
        if (!isFinite(v)) v = Number(def);
        var lo = Math.min(min, max);
        if (v < lo) v = lo;
        if (v > max) v = max;
        var snapped = logScale ? Math.round(v) : Math.round(v / step) * step;
        if (logScale) snapped = Math.round(snapped);
        else snapped = Number(snapped.toFixed(4));
        params[name] = snapped;
        num.value = String(snapped);
        rng.value = String(toSlider(snapped));
      });
    });
  }

  function collectParams() {
    var out = { inputId: state.input ? state.input.id : '' };
    Object.keys(params).forEach(function (k) { out[k] = params[k]; });
    return out;
  }

  // ------------------------------------------------------------------ status
  function setStatus(text, kind) {
    el.status.textContent = text;
    el.meta.classList.remove('error', 'done');
    if (kind === 'error') el.meta.classList.add('error');
    if (kind === 'done') el.meta.classList.add('done');
  }
  function setProgress(v) {
    var pct = Math.max(0, Math.min(1, v || 0)) * 100;
    el.bar.style.width = pct.toFixed(1) + '%';
    el.pct.textContent = pct.toFixed(0) + '%';
  }
  function startTimer() {
    stopTimer();
    state.startedAt = Date.now();
    el.elapsed.textContent = '0.0s';
    state.timer = setInterval(function () {
      el.elapsed.textContent = ((Date.now() - state.startedAt) / 1000).toFixed(1) + 's';
    }, 100);
  }
  function stopTimer() {
    if (state.timer) { clearInterval(state.timer); state.timer = null; }
    if (state.startedAt) {
      el.elapsed.textContent = ((Date.now() - state.startedAt) / 1000).toFixed(1) + 's';
    }
  }

  function busy(on) {
    state.running = on;
    el.run.disabled = on || !state.input;
    el.cancel.disabled = !on;
    el.download.disabled = !state.result;
  }

  // ------------------------------------------------------------------ upload
  function handleFiles(files) {
    if (!files || !files.length) return;
    var f = files[0];
    if (!/\.obj$/i.test(f.name)) {
      addLog('warn', 'expected a .obj file, got ' + f.name);
    }
    var fd = new FormData();
    fd.append('file', f, f.name);
    setStatus('uploading ' + f.name, '');
    el.dropInfo.textContent = 'uploading…';
    fetch(api('/api/upload'), {
      method: 'POST',
      headers: { 'X-AR-Token': TOKEN },
      body: fd
    }).then(function (r) {
      return r.json().then(function (j) { return { ok: r.ok, body: j }; });
    }).then(function (res) {
      if (!res.ok) throw new Error(res.body && res.body.error ? res.body.error : 'upload failed');
      var b = res.body;
      state.input = b;
      el.drop.classList.add('hasfile');
      el.dropInfo.textContent = b.name + ' · ' + fmtBytes(b.size) + ' · ' + fmtInt(b.vertices) + ' v · ' +
        fmtInt(b.triangles) + ' tri';
      addLog('info', 'loaded ' + b.name + ' (' + fmtInt(b.vertices) + ' vertices, ' + fmtInt(b.triangles) +
        ' triangles)');
      setStatus('ready', '');
      el.run.disabled = state.running;
    }).catch(function (err) {
      el.dropInfo.textContent = 'upload failed';
      el.drop.classList.remove('hasfile');
      state.input = null;
      el.run.disabled = true;
      addLog('error', err.message);
      setStatus('upload failed', 'error');
    });
  }

  el.drop.addEventListener('click', function () { el.file.click(); });
  el.drop.addEventListener('keydown', function (e) {
    if (e.key === 'Enter' || e.key === ' ') { e.preventDefault(); el.file.click(); }
  });
  el.file.addEventListener('change', function () {
    handleFiles(el.file.files);
    el.file.value = '';
  });
  ['dragenter', 'dragover'].forEach(function (ev) {
    el.drop.addEventListener(ev, function (e) {
      e.preventDefault();
      el.drop.classList.add('hot');
    });
  });
  ['dragleave', 'dragend'].forEach(function (ev) {
    el.drop.addEventListener(ev, function () { el.drop.classList.remove('hot'); });
  });
  el.drop.addEventListener('drop', function (e) {
    e.preventDefault();
    el.drop.classList.remove('hot');
    handleFiles(e.dataTransfer && e.dataTransfer.files);
  });
  ['dragover', 'drop'].forEach(function (ev) {
    window.addEventListener(ev, function (e) { e.preventDefault(); });
  });

  // --------------------------------------------------------------------- run
  function runJob() {
    if (state.running) return;
    if (!state.input) { addLog('warn', 'choose an .obj first'); return; }
    var body = collectParams();
    busy(true);
    el.cancel.disabled = false;
    state.result = null;
    el.download.disabled = true;
    el.stats.hidden = true;
    setProgress(0);
    setStatus('starting', '');
    addLog('start', 'run: target ' + fmtInt(body.targetQuads) + ' quads, adaptivity ' + body.adaptivity);

    fetch(api('/api/run'), {
      method: 'POST',
      headers: { 'Content-Type': 'application/json', 'X-AR-Token': TOKEN },
      body: JSON.stringify(body)
    }).then(function (r) {
      return r.json().then(function (j) { return { ok: r.ok, status: r.status, body: j }; });
    }).then(function (res) {
      if (!res.ok) {
        var msg = res.body && res.body.error ? res.body.error : ('run failed (' + res.status + ')');
        throw new Error(msg);
      }
      state.jobId = res.body.jobId;
      addLog('info', 'job ' + res.body.jobId);
      startTimer();
      attach(res.body.jobId);
    }).catch(function (err) {
      addLog('error', err.message);
      setStatus(err.message, 'error');
      busy(false);
      stopTimer();
    });
  }

  function attach(jobId) {
    detach();
    var url = api('/api/events', { job: jobId });
    var es = new EventSource(url);
    state.es = es;
    es.onmessage = function (ev) {
      var e;
      try { e = JSON.parse(ev.data); } catch (err) { return; }
      handleEvent(e);
    };
    es.onerror = function () {
      if (state.running) setStatus('event stream interrupted', 'error');
    };
  }

  function detach() {
    if (state.es) {
      try { state.es.close(); } catch (e) { }
      state.es = null;
    }
  }

  function handleEvent(e) {
    switch (e.type) {
      case 'start':
        addLog('start', 'input ' + fmtInt(e.vertices) + ' v / ' + fmtInt(e.triangles) +
          ' tri → target ' + fmtInt(e.targetQuads) + ' quads');
        break;
      case 'progress':
        setProgress(e.value);
        if (e.status) setStatus(e.status, '');
        break;
      case 'log':
        addLog(e.level || 'info', e.message);
        break;
      case 'report':
        (e.lines || []).forEach(function (line) { addLog('report', line); });
        break;
      case 'result':
        finishOk(e);
        break;
      case 'error':
        addLog('error', e.message || 'unknown error');
        setStatus(e.message || 'failed', 'error');
        busy(false);
        stopTimer();
        detach();
        break;
      case 'end':
        detach();
        if (state.running) {
          busy(false);
          stopTimer();
          if (e.state === 'cancelled') {
            setStatus('cancelled', '');
            addLog('warn', 'job cancelled');
          } else if (e.state === 'failed') {
            setStatus('failed', 'error');
          } else if (!state.result) {
            setStatus('finished without a result', 'error');
          }
        }
        break;
      default:
        break;
    }
  }

  function finishOk(e) {
    state.result = e;
    busy(false);
    stopTimer();
    detach();
    setProgress(1);
    setStatus('done in ' + (e.seconds || 0).toFixed(2) + 's', 'done');
    addLog('info', 'result: ' + fmtInt(e.vertices) + ' v, ' + fmtInt(e.quads) + ' quads, ' +
      fmtInt(e.nonQuads) + ' non-quads, ' + (e.seconds || 0).toFixed(2) + 's');
    if (e.output) addLog('info', 'output ' + e.output);
    renderStats(e);
    el.download.disabled = false;
    loadModel();
  }

  function renderStats(r) {
    el.stats.innerHTML = '';
    var items = [
      ['vertices', fmtInt(r.vertices)],
      ['quads', fmtInt(r.quads)],
      ['non-quads', fmtInt(r.nonQuads)],
      ['seconds', (r.seconds || 0).toFixed(2)],
      ['triangles', fmtInt(r.triangles)],
      ['output', r.output || '—']
    ];
    items.forEach(function (pair) {
      var d = document.createElement('div');
      d.className = 'stat';
      d.innerHTML = pair[0] + ' <b></b>';
      d.querySelector('b').textContent = pair[1];
      if (pair[0] === 'output') d.title = pair[1];
      el.stats.appendChild(d);
    });
    el.stats.hidden = false;
  }

  function loadModel() {
    if (!state.jobId) return;
    fetch(api('/api/model', { job: state.jobId }), { headers: { 'X-AR-Token': TOKEN } })
      .then(function (r) {
        if (!r.ok) throw new Error('model not ready');
        return r.text();
      })
      .then(function (text) {
        if (!viewer || !viewer.ready) {
          el.overlay.textContent = 'WebGL unavailable — result saved to the workspace, use Save result.';
          return;
        }
        var mesh = viewer.loadText(text);
        el.overlay.classList.add('hidden');
        addLog('info', 'preview: ' + fmtInt(mesh.vertices) + ' v, ' + fmtInt(mesh.faces) + ' faces (' +
          fmtInt(mesh.quads) + ' quads)');
      })
      .catch(function (err) { addLog('warn', 'preview unavailable: ' + err.message); });
  }

  function cancelJob() {
    if (!state.running || !state.jobId) return;
    el.cancel.disabled = true;
    setStatus('cancelling', '');
    fetch(api('/api/cancel'), {
      method: 'POST',
      headers: { 'Content-Type': 'application/json', 'X-AR-Token': TOKEN },
      body: JSON.stringify({ jobId: state.jobId })
    }).then(function (r) { return r.json(); })
      .then(function (j) { addLog('warn', j.cancelled ? 'cancel requested' : 'job already finished'); })
      .catch(function (err) { addLog('error', err.message); });
  }

  function downloadResult() {
    if (!state.jobId) return;
    fetch(api('/api/model', { job: state.jobId }), { headers: { 'X-AR-Token': TOKEN } })
      .then(function (r) {
        if (!r.ok) throw new Error('nothing to save');
        return r.blob();
      })
      .then(function (blob) {
        var a = document.createElement('a');
        var name = state.input ? state.input.name.replace(/\.obj$/i, '') : 'model';
        a.href = URL.createObjectURL(blob);
        a.download = name + '_remeshed.obj';
        document.body.appendChild(a);
        a.click();
        a.remove();
        setTimeout(function () { URL.revokeObjectURL(a.href); }, 5000);
        addLog('info', 'saved ' + a.download);
      })
      .catch(function (err) { addLog('error', err.message); });
  }

  el.run.addEventListener('click', runJob);
  el.cancel.addEventListener('click', cancelJob);
  el.download.addEventListener('click', downloadResult);

  // ---------------------------------------------------------------- viewport
  var MODES = ['shaded', 'both', 'wire'];
  function setMode(mode) {
    if (viewer && viewer.ready) viewer.setMode(mode);
    Array.prototype.forEach.call(el.mode.querySelectorAll('button'), function (b) {
      b.classList.toggle('on', b.getAttribute('data-mode') === mode);
    });
  }
  el.mode.addEventListener('click', function (e) {
    var b = e.target.closest('button[data-mode]');
    if (b) setMode(b.getAttribute('data-mode'));
  });

  // --------------------------------------------------------------- keybinds
  document.addEventListener('keydown', function (e) {
    var typing = /^(INPUT|TEXTAREA|SELECT)$/.test(e.target.tagName);
    if (e.key === 'Enter' && (e.ctrlKey || e.metaKey)) {
      e.preventDefault();
      runJob();
      return;
    }
    if (e.key === 'Escape') {
      if (state.running) { e.preventDefault(); cancelJob(); }
      return;
    }
    if (typing || e.ctrlKey || e.metaKey || e.altKey) return;
    var k = e.key.toLowerCase();
    if (k === 'w') {
      var cur = MODES.indexOf(viewer && viewer.mode ? viewer.mode : 'shaded');
      setMode(MODES[(cur + 1) % MODES.length]);
    } else if (k === 'o') {
      el.file.click();
    } else if (k === 'd') {
      downloadResult();
    } else if (k === 'l') {
      el.logToggle.click();
    } else if (k === 'f') {
      if (viewer && viewer.ready) { viewer.reset(); addLog('info', 'view reset'); }
    }
  });

  // ------------------------------------------------------------------- boot
  function boot() {
    initParams();
    busy(false);
    fetch(api('/api/info'), { headers: { 'X-AR-Token': TOKEN } })
      .then(function (r) {
        if (!r.ok) throw new Error('info failed (' + r.status + ')');
        return r.json();
      })
      .then(function (info) {
        el.verChip.textContent = 'v' + info.version;
        if (info.cli && info.cli.found) {
          el.cliChip.classList.add('ok');
          el.cliText.textContent = 'CLI ' + (info.cli.version || 'found');
          el.cliChip.title = info.cli.path;
          addLog('info', 'CLI: ' + info.cli.path + ' (' + (info.cli.version || 'unknown') + ', via ' +
            (info.cli.source || '?') + ')');
        } else {
          el.cliChip.classList.add('bad');
          el.cliText.textContent = 'CLI not found';
          el.cliChip.title = 'pass --cli PATH when starting autoremesher-gui';
          addLog('error', 'autoremesher CLI not found — start the GUI with --cli PATH');
        }
        addLog('info', 'session ' + info.platform + ', ' + info.cpus + ' cpus');
      })
      .catch(function (err) {
        el.cliChip.classList.add('bad');
        el.cliText.textContent = 'backend unreachable';
        addLog('error', err.message);
      });
    addLog('info', 'keys: Ctrl+Enter run · Esc cancel · W wireframe · O open · D download · L log · F fit');
  }

  boot();
})();
