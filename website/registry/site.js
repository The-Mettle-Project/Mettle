(function () {
  var HALFTONE = [24, 10, 12, 26, 35, 47, 49, 37, 8, 0, 2, 14, 45, 59, 61, 51, 22, 6, 4, 16, 43, 57, 63, 53, 30, 20, 18, 28, 33, 41, 55, 39, 34, 46, 48, 36, 25, 11, 13, 27, 44, 58, 60, 50, 9, 1, 3, 15, 42, 56, 62, 52, 23, 7, 5, 17, 32, 40, 54, 38, 31, 21, 19, 29];
  var VERTEX = "attribute vec2 position; void main() { gl_Position = vec4(position, 0.0, 1.0); }";
  var FRAGMENT = [
    "precision highp float;",
    "uniform sampler2D image;",
    "uniform sampler2D map;",
    "uniform vec2 res;",
    "uniform vec2 scale;",
    "uniform vec2 offset;",
    "uniform vec4 fade;",
    "uniform vec2 rise;",
    "void main() {",
    "  vec2 cell = floor(gl_FragCoord.xy);",
    "  vec2 uv = (cell + 0.5) / res;",
    "  vec3 picture = texture2D(image, scale * uv + offset).rgb;",
    "  float lum = dot(picture, vec3(0.299, 0.587, 0.114));",
    "  vec3 ink = clamp((vec3(lum) + (picture - lum) * 1.25) * 0.95 * pow(max(lum, 0.05), -0.38), 0.0, 1.0);",
    "  float shaped = pow(lum, 0.62);",
    "  shaped *= smoothstep(rise.x, rise.y, uv.y) * (1.0 - smoothstep(1.0 - fade.y, 1.0, uv.y)) * smoothstep(fade.z, fade.w, uv.x);",
    "  float limit = texture2D(map, (mod(cell, 8.0) + 0.5) / 8.0).r;",
    "  vec3 ground = vec3(23.0, 24.0, 26.0) / 255.0;",
    "  gl_FragColor = vec4(mix(ground, ink, step(limit + 0.034, shaped)), 1.0);",
    "}"
  ].join("\n");

  function halftone(host) {
    var source = host.getAttribute("data-image");
    var fade = host.getAttribute("data-fade").split(" ").map(Number);
    var focus = parseFloat(host.getAttribute("data-focus") || "0.5");
    var canvas = document.createElement("canvas");
    var gl = canvas.getContext("webgl", { alpha: false, antialias: false });
    function raw() {
      canvas.remove();
      host.classList.add("raw");
      host.style.backgroundImage = "url(" + source + ")";
    }
    if (!gl) return raw();
    var program = gl.createProgram();
    [[gl.VERTEX_SHADER, VERTEX], [gl.FRAGMENT_SHADER, FRAGMENT]].forEach(function (part) {
      var shader = gl.createShader(part[0]);
      gl.shaderSource(shader, part[1]);
      gl.compileShader(shader);
      gl.attachShader(program, shader);
    });
    gl.linkProgram(program);
    if (!gl.getProgramParameter(program, gl.LINK_STATUS)) return raw();
    gl.useProgram(program);
    gl.bindBuffer(gl.ARRAY_BUFFER, gl.createBuffer());
    gl.bufferData(gl.ARRAY_BUFFER, new Float32Array([-1, -1, 3, -1, -1, 3]), gl.STATIC_DRAW);
    gl.enableVertexAttribArray(gl.getAttribLocation(program, "position"));
    gl.vertexAttribPointer(gl.getAttribLocation(program, "position"), 2, gl.FLOAT, false, 0, 0);
    function uni(name) {
      return gl.getUniformLocation(program, name);
    }
    var bytes = new Uint8Array(HALFTONE.length * 4);
    HALFTONE.forEach(function (value, i) {
      bytes[i * 4] = bytes[i * 4 + 1] = bytes[i * 4 + 2] = Math.round((value / 64) * 255);
      bytes[i * 4 + 3] = 255;
    });
    gl.activeTexture(gl.TEXTURE1);
    gl.bindTexture(gl.TEXTURE_2D, gl.createTexture());
    gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA, 8, 8, 0, gl.RGBA, gl.UNSIGNED_BYTE, bytes);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.NEAREST);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.NEAREST);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.REPEAT);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.REPEAT);
    gl.uniform1i(uni("map"), 1);
    gl.activeTexture(gl.TEXTURE0);
    gl.bindTexture(gl.TEXTURE_2D, gl.createTexture());
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.LINEAR);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.LINEAR);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);
    gl.pixelStorei(gl.UNPACK_FLIP_Y_WEBGL, true);
    gl.uniform1i(uni("image"), 0);
    gl.uniform4f(uni("fade"), fade[0], fade[1], fade[2], fade[3]);
    host.insertBefore(canvas, host.firstChild);

    var picture = 16 / 9;
    var ready = false;
    var words = host.classList.contains("sky") ? [].slice.call(host.querySelectorAll(".wrap > *")) : [];
    function textTop(box) {
      var top = box.bottom;
      words.forEach(function (word) {
        var r = word.getBoundingClientRect();
        if (r.height > 2 && r.top < top) top = r.top;
      });
      return top;
    }
    function settle() {
      var box = canvas.getBoundingClientRect();
      if (!box.height) return;
      var base = words.length ? Math.max(0, (box.bottom - textTop(box) + 28) / box.height) : 0;
      gl.uniform2f(uni("rise"), base, base + fade[0]);
    }
    function fit() {
      var density = Math.min(window.devicePixelRatio || 1, 2);
      var w = Math.max(1, Math.round(canvas.clientWidth * density));
      var h = Math.max(1, Math.round(canvas.clientHeight * density));
      if (canvas.width !== w || canvas.height !== h) {
        canvas.width = w;
        canvas.height = h;
        gl.viewport(0, 0, w, h);
      }
      var sx = Math.min(1, w / h / picture);
      var sy = Math.min(1, picture / (w / h));
      gl.uniform2f(uni("res"), w, h);
      gl.uniform2f(uni("scale"), sx, sy);
      gl.uniform2f(uni("offset"), (1 - sx) / 2, (1 - sy) * focus);
      settle();
    }
    function draw() {
      if (ready) gl.drawArrays(gl.TRIANGLES, 0, 3);
    }
    var image = new Image();
    image.onload = function () {
      gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGB, gl.RGB, gl.UNSIGNED_BYTE, image);
      picture = image.naturalWidth / image.naturalHeight;
      ready = true;
      fit();
      draw();
    };
    image.src = source;
    if (document.fonts && document.fonts.ready) {
      document.fonts.ready.then(function () {
        settle();
        draw();
      });
    }
    if (window.ResizeObserver) {
      new ResizeObserver(function () {
        fit();
        draw();
      }).observe(host);
    }
  }

  var sky = document.querySelector(".sky");
  if (sky) halftone(sky);
  var end = document.querySelector(".end");
  if (end) halftone(end);

  var bar = document.querySelector(".top");
  if (sky && "IntersectionObserver" in window) {
    new IntersectionObserver(function (entries) {
      bar.classList.toggle("past", !entries[0].isIntersecting);
    }, { rootMargin: "-" + bar.offsetHeight + "px 0px 0px 0px" }).observe(sky);
  }

  var tocLinks = [].slice.call(document.querySelectorAll(".toc ol a"));
  if (tocLinks.length && "IntersectionObserver" in window) {
    var reading = null;
    var mark = function (id) {
      if (id === reading) return;
      reading = id;
      tocLinks.forEach(function (link) {
        if (link.getAttribute("href") === "#" + id) link.setAttribute("aria-current", "location");
        else link.removeAttribute("aria-current");
      });
    };
    var visible = {};
    var watched = tocLinks.map(function (link) { return document.getElementById(link.getAttribute("href").slice(1)); }).filter(Boolean);
    var spy = new IntersectionObserver(function (entries) {
      entries.forEach(function (entry) { visible[entry.target.id] = entry.isIntersecting; });
      var first = watched.filter(function (section) { return visible[section.id]; })[0];
      if (first) mark(first.id);
    }, { rootMargin: "-20% 0px -55% 0px" });
    watched.forEach(function (section) { spy.observe(section); });
  }

  var filter = document.getElementById("filter");
  if (filter) {
    var rows = document.querySelectorAll("#ledger > li");
    var tally = document.getElementById("tally");
    var none = document.getElementById("none");
    var noneQuery = document.getElementById("none-query");
    var total = tally.textContent;
    var apply = function () {
      var words = filter.value.toLowerCase().split(/\s+/).filter(Boolean);
      var shown = 0;
      rows.forEach(function (row) {
        var text = row.getAttribute("data-search");
        var hit = words.every(function (word) { return text.indexOf(word) >= 0; });
        row.hidden = !hit;
        if (hit) shown++;
      });
      tally.textContent = words.length ? shown + " of " + total : total;
      noneQuery.textContent = filter.value.trim();
      none.hidden = shown !== 0;
    };
    filter.addEventListener("input", apply);
    document.getElementById("clear").addEventListener("click", function () {
      filter.value = "";
      apply();
      filter.focus();
    });
    var asked = new URLSearchParams(window.location.search).get("q");
    if (asked) {
      filter.value = asked;
      apply();
    }
    if (window.matchMedia && matchMedia("(pointer: fine)").matches) {
      filter.focus({ preventScroll: true });
    }
  }

  document.addEventListener("click", function (event) {
    var button = event.target.closest("button[data-copy]");
    if (!button || !navigator.clipboard) return;
    var label = button.querySelector("span") || button;
    navigator.clipboard.writeText(button.getAttribute("data-copy")).then(function () {
      label.textContent = "Copied";
      setTimeout(function () { label.textContent = "Copy"; }, 1400);
    }, function () {});
  });
})();
