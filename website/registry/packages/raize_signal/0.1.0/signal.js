// raize Signals: the browser half. Served at /rz/signal.js.
//
// Speaks Phoenix's v2 channel protocol on /signal/websocket, joins
// "signal:<name>" for #rz-root[data-signal], keeps the rendered tree the
// server sends, merges each diff into it (replies to events, and "diff"
// pushes for messages the server published), re-renders to HTML and morphs
// the DOM (keeping focus and the value of the input being typed in).
//
// Events: an element with rz-click / rz-input / rz-change / rz-submit holds a
// message as JSON, ["Variant"] or ["Variant", payload]. A one-element message
// on an input, change or submit takes the element's value as its payload
// (a form's first named field on submit).
(function () {
  "use strict";
  var root = document.getElementById("rz-root");
  if (!root) return;
  var name = root.getAttribute("data-signal");
  var topic = "signal:" + name;
  var ws = null, ref = 0, joinRef = null, tree = null, pending = {}, retry = 0, heartbeat = null;
  // The focused input keeps what the user is typing, except right after a
  // submit, when the server's values win (a form that clears itself).
  var takeValues = false;

  // ---- rendered tree ------------------------------------------------------
  function html(r) {
    if (r === undefined || r === null) return "";
    if (typeof r === "string") return r;
    if (r.d) {
      var out = "";
      for (var k = 0; k < r.d.length; k++) out += weave(r.s, function (i) { return html(r.d[k][i]); });
      return out;
    }
    return weave(r.s, function (i) { return html(r[i]); });
  }
  function weave(s, dyn) {
    var out = s[0];
    for (var i = 1; i < s.length; i++) out += dyn(i - 1) + s[i];
    return out;
  }
  function merge(old, diff) {
    for (var k in diff) {
      var v = diff[k];
      var cur = old[k];
      if (v && typeof v === "object" && !("s" in v) && cur && typeof cur === "object") {
        if ("d" in v) cur.d = v.d; else merge(cur, v);
      } else {
        old[k] = v;
      }
    }
  }

  // ---- DOM morph ----------------------------------------------------------
  function sameNode(a, b) {
    return a.nodeType === b.nodeType && (a.nodeType !== 1 || a.tagName === b.tagName);
  }
  function syncAttrs(from, to) {
    var i, a;
    for (i = from.attributes.length - 1; i >= 0; i--) {
      a = from.attributes[i].name;
      if (!to.hasAttribute(a)) from.removeAttribute(a);
    }
    for (i = 0; i < to.attributes.length; i++) {
      a = to.attributes[i];
      if (from.getAttribute(a.name) !== a.value) from.setAttribute(a.name, a.value);
    }
    var tag = from.tagName;
    if ((tag === "INPUT" || tag === "TEXTAREA" || tag === "SELECT") && (from !== document.activeElement || takeValues)) {
      if (from.type === "checkbox" || from.type === "radio") from.checked = to.hasAttribute("checked");
      else from.value = to.getAttribute("value") || (tag === "TEXTAREA" ? to.textContent : "");
    }
  }
  function morphChildren(from, to) {
    var f = from.firstChild, t = to.firstChild;
    while (t) {
      var next = t.nextSibling;
      if (!f) {
        from.appendChild(t);
      } else if (sameNode(f, t)) {
        if (f.nodeType === 1) { syncAttrs(f, t); morphChildren(f, t); }
        else if (f.nodeValue !== t.nodeValue) f.nodeValue = t.nodeValue;
        f = f.nextSibling;
      } else {
        var after = f.nextSibling;
        from.replaceChild(t, f);
        f = after;
      }
      t = next;
    }
    while (f) { var gone = f.nextSibling; from.removeChild(f); f = gone; }
  }
  function paint() {
    var next = document.createElement("div");
    next.innerHTML = html(tree);
    morphChildren(root, next);
  }

  // ---- socket -------------------------------------------------------------
  function push(t, event, payload, onReply) {
    var r = String(++ref);
    if (event === "phx_join") joinRef = r;
    if (onReply) pending[r] = onReply;
    ws.send(JSON.stringify([joinRef, r, t, event, payload]));
  }
  function connect() {
    var proto = location.protocol === "https:" ? "wss://" : "ws://";
    // The page path picks the guards the upgrade runs and the one Signal it may join.
    ws = new WebSocket(proto + location.host + "/signal/websocket?vsn=2.0.0&path=" + encodeURIComponent(location.pathname));
    ws.onopen = function () {
      retry = 0;
      push(topic, "phx_join", {}, function (reply) {
        if (reply.status !== "ok") { console.error("raize signal: join refused", reply.response); return; }
        tree = reply.response.rendered;
        paint();
        root.setAttribute("data-connected", "true");
      });
      heartbeat = setInterval(function () { push("phoenix", "heartbeat", {}); }, 30000);
    };
    ws.onmessage = function (e) {
      var m = JSON.parse(e.data);
      if (m[3] === "phx_reply" && pending[m[1]]) {
        var cb = pending[m[1]];
        delete pending[m[1]];
        cb(m[4]);
      } else if (m[3] === "diff" && m[2] === topic && tree) {
        // A push: the server ran update() for a published message.
        var d = m[4].diff;
        if (d && Object.keys(d).length) { merge(tree, d); paint(); }
      }
    };
    ws.onclose = function () {
      clearInterval(heartbeat);
      root.removeAttribute("data-connected");
      pending = {};
      setTimeout(connect, Math.min(5000, 200 * Math.pow(2, retry++)));
    };
  }

  // ---- events -------------------------------------------------------------
  function send(msg, submitted) {
    if (!ws || ws.readyState !== 1 || !tree) return;
    push(topic, "event", msg, function (reply) {
      if (reply.status !== "ok") { console.error("raize signal:", reply.response); return; }
      var d = reply.response.diff;
      if (d && Object.keys(d).length) {
        merge(tree, d);
        takeValues = !!submitted;
        paint();
        takeValues = false;
      }
    });
  }
  function handle(kind, e) {
    var el = e.target.closest ? e.target.closest("[rz-" + kind + "]") : null;
    if (!el || !root.contains(el)) return;
    if (kind === "submit") e.preventDefault();
    var msg = JSON.parse(el.getAttribute("rz-" + kind));
    if (msg.length === 1 && kind !== "click") {
      if (kind === "submit") {
        var field = el.querySelector("[name]");
        if (field) msg.push(field.value);
      } else {
        msg.push(el.type === "checkbox" ? el.checked : el.value);
      }
    }
    send(msg, kind === "submit");
  }
  ["click", "input", "change", "submit"].forEach(function (kind) {
    root.addEventListener(kind, function (e) { handle(kind, e); });
  });

  connect();
})();
