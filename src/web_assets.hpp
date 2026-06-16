#pragma once

namespace kiosk {

// The kiosk SPA, served at "/". It loads every dashboard as an absolutely-stacked
// <iframe> and switches which one is shown. Switching never reloads Grafana:
//  - "visibility" mode: inactive frames stay laid out (correct chart sizing) but unpainted.
//  - "opacity" mode: inactive frames stay painted (zero-cost flip, higher GPU load).
// Background frames keep refreshing because Chromium is launched with the
// no-throttle flags, so a freshly-shown dashboard is already up to date.
inline constexpr const char *kIndexHtml = R"HTML(<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1, maximum-scale=1">
<title>kiosk-manager</title>
<style>
  html, body { margin: 0; height: 100%; width: 100%; background: #000; overflow: hidden; }
  #root { position: fixed; inset: 0; }
  .frame { position: absolute; inset: 0; width: 100%; height: 100%; border: 0; background: #000; }

  /* visibility mode (default) */
  body.vis .frame { visibility: hidden; pointer-events: none; }
  body.vis .frame.active { visibility: visible; pointer-events: auto; }

  /* opacity mode */
  body.opa .frame { opacity: 0; pointer-events: none; z-index: 0; transition: none; }
  body.opa .frame.active { opacity: 1; pointer-events: auto; z-index: 1; }

  #msg { position: fixed; inset: 0; display: flex; align-items: center; justify-content: center;
         color: #888; font: 16px system-ui, sans-serif; }
</style>
</head>
<body class="vis">
<div id="msg">loading dashboards…</div>
<div id="root"></div>
<script>
(function () {
  var root = document.getElementById('root');
  var msg = document.getElementById('msg');
  var frames = [];      // iframe elements, index-aligned with dashboards
  var urls = [];        // dashboard URLs
  var current = 0;
  var preloadMode = 'all';   // 'all' or 'window'
  var preloadN = 1;          // radius for window mode

  function safeUrl(u) {
    // Only allow http(s); never let a config typo turn into a javascript:/data: src.
    try { var p = new URL(u, location.href); return (p.protocol === 'http:' || p.protocol === 'https:') ? u : 'about:blank'; }
    catch (e) { return 'about:blank'; }
  }

  function ensureLoaded(i) {
    if (i < 0 || i >= frames.length) return;
    var f = frames[i];
    if (!f.src) f.src = safeUrl(urls[i]);
  }

  function applyWindow(center) {
    if (preloadMode !== 'window') return;
    for (var i = 0; i < frames.length; i++) {
      if (Math.abs(i - center) <= preloadN) ensureLoaded(i);
    }
  }

  function setActive(i) {
    if (i < 0 || i >= frames.length) return;
    ensureLoaded(i);
    applyWindow(i);
    for (var k = 0; k < frames.length; k++) {
      frames[k].classList.toggle('active', k === i);
    }
    current = i;
  }

  function build(cfg) {
    document.body.className = (cfg.visual === 'opacity') ? 'opa' : 'vis';
    var pm = (cfg.preload || 'all');
    if (pm.indexOf('window') === 0) {
      preloadMode = 'window';
      var c = pm.indexOf(':');
      preloadN = (c >= 0) ? (parseInt(pm.slice(c + 1)) || 1) : 1;
    } else {
      preloadMode = 'all';
    }

    root.innerHTML = '';
    frames = [];
    urls = (cfg.dashboards || []).map(function (d) { return d.url; });
    urls.forEach(function (url, i) {
      var f = document.createElement('iframe');
      f.className = 'frame';
      f.setAttribute('allow', 'fullscreen');
      // Preload-all sets every src immediately; window mode sets them lazily.
      if (preloadMode === 'all') f.src = safeUrl(url);
      root.appendChild(f);
      frames.push(f);
    });

    msg.style.display = frames.length ? 'none' : 'flex';
    if (!frames.length) { msg.textContent = 'no dashboards configured'; return; }
    setActive(typeof cfg.active === 'number' ? cfg.active : 0);
  }

  function loadConfig() {
    return fetch('/config', { cache: 'no-store' })
      .then(function (r) { return r.json(); })
      .then(build)
      .catch(function () {
        // Server not ready yet (e.g. Chromium started first); retry shortly.
        msg.style.display = 'flex';
        msg.textContent = 'waiting for kiosk service…';
        setTimeout(loadConfig, 500);
      });
  }

  function connect() {
    var es = new EventSource('/events');
    es.addEventListener('switch', function (e) { setActive(parseInt(e.data, 10)); });
    es.addEventListener('reload', function () { loadConfig(); });
    // EventSource auto-reconnects on error (e.g. when the service restarts).
  }

  loadConfig().then(connect);
})();
</script>
</body>
</html>
)HTML";

} // namespace kiosk
