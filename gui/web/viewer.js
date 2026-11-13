/* Minimal WebGL OBJ previewer: flat shaded triangles + optional wireframe.
   No dependencies, no network access. */
(function (global) {
  'use strict';

  function m4() { return new Float32Array(16); }

  function perspective(out, fovy, aspect, near, far) {
    var f = 1.0 / Math.tan(fovy / 2);
    var nf = 1 / (near - far);
    out[0] = f / aspect; out[1] = 0; out[2] = 0; out[3] = 0;
    out[4] = 0; out[5] = f; out[6] = 0; out[7] = 0;
    out[8] = 0; out[9] = 0; out[10] = (far + near) * nf; out[11] = -1;
    out[12] = 0; out[13] = 0; out[14] = 2 * far * near * nf; out[15] = 0;
    return out;
  }

  function sub3(a, b, out) {
    out[0] = a[0] - b[0]; out[1] = a[1] - b[1]; out[2] = a[2] - b[2];
    return out;
  }
  function cross3(a, b, out) {
    var x = a[1] * b[2] - a[2] * b[1];
    var y = a[2] * b[0] - a[0] * b[2];
    var z = a[0] * b[1] - a[1] * b[0];
    out[0] = x; out[1] = y; out[2] = z;
    return out;
  }
  function norm3(v) {
    var l = Math.sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (l > 1e-12) { v[0] /= l; v[1] /= l; v[2] /= l; }
    return v;
  }

  function lookAt(out, eye, center, up) {
    var z = norm3(sub3(eye, center, [0, 0, 0]));
    var x = norm3(cross3(up, z, [0, 0, 0]));
    var y = cross3(z, x, [0, 0, 0]);
    out[0] = x[0]; out[1] = y[0]; out[2] = z[0]; out[3] = 0;
    out[4] = x[1]; out[5] = y[1]; out[6] = z[1]; out[7] = 0;
    out[8] = x[2]; out[9] = y[2]; out[10] = z[2]; out[11] = 0;
    out[12] = -(x[0] * eye[0] + x[1] * eye[1] + x[2] * eye[2]);
    out[13] = -(y[0] * eye[0] + y[1] * eye[1] + y[2] * eye[2]);
    out[14] = -(z[0] * eye[0] + z[1] * eye[1] + z[2] * eye[2]);
    out[15] = 1;
    return out;
  }

  // ---------------------------------------------------------------- OBJ parse
  function pushTri(v, i0, i1, i2, pos, nrm) {
    var a = [v[i0 * 3], v[i0 * 3 + 1], v[i0 * 3 + 2]];
    var b = [v[i1 * 3], v[i1 * 3 + 1], v[i1 * 3 + 2]];
    var c = [v[i2 * 3], v[i2 * 3 + 1], v[i2 * 3 + 2]];
    var n = norm3(cross3(sub3(b, a, [0, 0, 0]), sub3(c, a, [0, 0, 0]), [0, 0, 0]));
    pos.push(a[0], a[1], a[2], b[0], b[1], b[2], c[0], c[1], c[2]);
    for (var k = 0; k < 3; k++) { nrm.push(n[0], n[1], n[2]); }
  }

  function parseOBJ(text) {
    var v = [];
    var tri = [];
    var triN = [];
    var lines = [];
    var faces = 0, quads = 0, nverts = 0;
    var rows = text.split(/\r?\n/);
    for (var i = 0; i < rows.length; i++) {
      var raw = rows[i];
      if (!raw || raw.length < 2) continue;
      var c0 = raw.charCodeAt(0);
      if (c0 === 118 && raw.charCodeAt(1) === 32) { // "v "
        var p = raw.trim().split(/\s+/);
        var x = +p[1], y = +p[2], z = +p[3];
        if (!isFinite(x) || !isFinite(y) || !isFinite(z)) continue;
        v.push(x, y, z);
        nverts++;
      } else if (c0 === 102 && raw.charCodeAt(1) === 32) { // "f "
        var parts = raw.trim().split(/\s+/);
        var idx = [];
        for (var k = 1; k < parts.length; k++) {
          var tok = parts[k].split('/')[0];
          var n = parseInt(tok, 10);
          if (!isFinite(n) || n === 0) continue;
          var vi = n < 0 ? nverts + n : n - 1;
          if (vi < 0 || vi * 3 + 2 >= v.length) continue;
          idx.push(vi);
        }
        if (idx.length < 3) continue;
        faces++;
        if (idx.length === 4) quads++;
        for (var t = 1; t + 1 < idx.length; t++) {
          pushTri(v, idx[0], idx[t], idx[t + 1], tri, triN);
        }
        for (var e = 0; e < idx.length; e++) {
          var a = idx[e] * 3;
          var b = idx[(e + 1) % idx.length] * 3;
          lines.push(v[a], v[a + 1], v[a + 2], v[b], v[b + 1], v[b + 2]);
        }
      }
    }
    return {
      positions: new Float32Array(tri),
      normals: new Float32Array(triN),
      lines: new Float32Array(lines),
      vertices: nverts,
      faces: faces,
      quads: quads,
      triangles: tri.length / 9
    };
  }

  var VS_TRI = `attribute vec3 aPos;
attribute vec3 aNormal;
uniform mat4 uProj;
uniform mat4 uView;
varying vec3 vN;
varying vec3 vP;
void main() {
  vN = aNormal;
  vP = aPos;
  gl_Position = uProj * uView * vec4(aPos, 1.0);
}`;

  var FS_TRI = `precision mediump float;
varying vec3 vN;
varying vec3 vP;
uniform vec3 uColor;
uniform vec3 uEye;
void main() {
  vec3 N = normalize(vN);
  vec3 V = normalize(uEye - vP);
  if (dot(N, V) < 0.0) { N = -N; }
  vec3 L1 = normalize(vec3(0.55, 0.75, 0.62));
  vec3 L2 = normalize(vec3(-0.65, 0.15, -0.55));
  float d = 0.70 * max(dot(N, L1), 0.0) + 0.28 * max(dot(N, L2), 0.0);
  vec3 H = normalize(L1 + V);
  float spec = pow(max(dot(N, H), 0.0), 22.0);
  vec3 col = uColor * (0.20 + d) + vec3(0.35) * spec;
  gl_FragColor = vec4(col, 1.0);
}`;

  var VS_LINE = `attribute vec3 aPos;
uniform mat4 uProj;
uniform mat4 uView;
void main() { gl_Position = uProj * uView * vec4(aPos, 1.0); }`;

  var FS_LINE = `precision mediump float;
uniform vec3 uColor;
void main() { gl_FragColor = vec4(uColor, 1.0); }`;

  function compile(gl, type, src) {
    var sh = gl.createShader(type);
    gl.shaderSource(sh, src);
    gl.compileShader(sh);
    if (!gl.getShaderParameter(sh, gl.COMPILE_STATUS)) {
      throw new Error('shader: ' + gl.getShaderInfoLog(sh));
    }
    return sh;
  }

  function program(gl, vs, fs, attrs, uniforms) {
    var p = gl.createProgram();
    gl.attachShader(p, compile(gl, gl.VERTEX_SHADER, vs));
    gl.attachShader(p, compile(gl, gl.FRAGMENT_SHADER, fs));
    gl.linkProgram(p);
    if (!gl.getProgramParameter(p, gl.LINK_STATUS)) {
      throw new Error('link: ' + gl.getProgramInfoLog(p));
    }
    var out = { program: p, attr: {}, uniform: {} };
    for (var i = 0; i < attrs.length; i++) { out.attr[attrs[i]] = gl.getAttribLocation(p, attrs[i]); }
    for (var j = 0; j < uniforms.length; j++) { out.uniform[uniforms[j]] = gl.getUniformLocation(p, uniforms[j]); }
    return out;
  }

  // ------------------------------------------------------------------ Viewer
  function Viewer(canvas) {
    this.canvas = canvas;
    this.gl = null;
    this.mode = 'shaded';
    this.yaw = -0.55;
    this.pitch = 0.40;
    this.dist = 3;
    this.center = [0, 0, 0];
    this.radius = 1;
    this.mesh = null;
    this.pending = false;
    this.ready = false;
    this.dragging = false;
    this.lastX = 0;
    this.lastY = 0;

    try {
      this.gl = canvas.getContext('webgl', { antialias: true, alpha: false })
        || canvas.getContext('experimental-webgl');
    } catch (e) {
      this.gl = null;
    }
    if (!this.gl) return;

    this.tri = program(this.gl, VS_TRI, FS_TRI, ['aPos', 'aNormal'], ['uProj', 'uView', 'uColor', 'uEye']);
    this.line = program(this.gl, VS_LINE, FS_LINE, ['aPos'], ['uProj', 'uView', 'uColor']);

    var gl = this.gl;
    this.triBuf = gl.createBuffer();
    this.lineBuf = gl.createBuffer();
    this.triCount = 0;
    this.lineCount = 0;
    this.ready = true;
    this.bindEvents();
    var self = this;
    window.addEventListener('resize', function () { self.requestRender(); });
    this.requestRender();
  }

  Viewer.prototype.bindEvents = function () {
    var self = this;
    var c = this.canvas;

    c.addEventListener('pointerdown', function (e) {
      self.dragging = true;
      self.lastX = e.clientX;
      self.lastY = e.clientY;
      if (c.setPointerCapture) { try { c.setPointerCapture(e.pointerId); } catch (err) { } }
    });
    c.addEventListener('pointermove', function (e) {
      if (!self.dragging) return;
      var dx = e.clientX - self.lastX;
      var dy = e.clientY - self.lastY;
      self.lastX = e.clientX;
      self.lastY = e.clientY;
      self.yaw -= dx * 0.008;
      self.pitch += dy * 0.008;
      var lim = Math.PI / 2 - 0.05;
      if (self.pitch > lim) self.pitch = lim;
      if (self.pitch < -lim) self.pitch = -lim;
      self.requestRender();
    });
    function stop() { self.dragging = false; }
    c.addEventListener('pointerup', stop);
    c.addEventListener('pointercancel', stop);
    c.addEventListener('pointerleave', stop);

    c.addEventListener('wheel', function (e) {
      e.preventDefault();
      var f = Math.exp(e.deltaY * 0.0012);
      var minD = Math.max(self.radius * 1.02, 0.001);
      var maxD = Math.max(self.radius * 60, 10);
      self.dist = Math.min(maxD, Math.max(minD, self.dist * f));
      self.requestRender();
    }, { passive: false });

    c.addEventListener('dblclick', function () { self.reset(); });
  };

  Viewer.prototype.eye = function () {
    var cp = Math.cos(this.pitch);
    return [
      this.center[0] + this.dist * cp * Math.sin(this.yaw),
      this.center[1] + this.dist * Math.sin(this.pitch),
      this.center[2] + this.dist * cp * Math.cos(this.yaw)
    ];
  };

  Viewer.prototype.requestRender = function () {
    if (this.pending) return;
    var self = this;
    this.pending = true;
    window.requestAnimationFrame(function () {
      self.pending = false;
      self.render();
    });
  };

  Viewer.prototype.render = function () {
    var gl = this.gl;
    if (!gl) return;
    var dpr = Math.min(window.devicePixelRatio || 1, 2);
    var w = Math.max(1, Math.floor((this.canvas.clientWidth || 640) * dpr));
    var h = Math.max(1, Math.floor((this.canvas.clientHeight || 480) * dpr));
    if (this.canvas.width !== w || this.canvas.height !== h) {
      this.canvas.width = w;
      this.canvas.height = h;
    }
    gl.viewport(0, 0, w, h);
    gl.clearColor(0.043, 0.051, 0.066, 1.0);
    gl.clear(gl.COLOR_BUFFER_BIT | gl.DEPTH_BUFFER_BIT);
    gl.enable(gl.DEPTH_TEST);
    gl.disable(gl.CULL_FACE);
    if (!this.mesh) return;

    var aspect = w / h;
    var near = Math.max(this.dist - this.radius * 3.0, this.radius * 0.01);
    var far = this.dist + this.radius * 8.0;
    var proj = perspective(m4(), 45 * Math.PI / 180, aspect, near, far);
    var eye = this.eye();
    var view = lookAt(m4(), eye, this.center, [0, 1, 0]);

    if (this.mode !== 'wire' && this.triCount > 0) {
      var tp = this.tri;
      gl.useProgram(tp.program);
      gl.uniformMatrix4fv(tp.uniform.uProj, false, proj);
      gl.uniformMatrix4fv(tp.uniform.uView, false, view);
      gl.uniform3fv(tp.uniform.uEye, new Float32Array(eye));
      gl.uniform3f(tp.uniform.uColor, 0.70, 0.78, 0.88);
      gl.bindBuffer(gl.ARRAY_BUFFER, this.triBuf);
      gl.enableVertexAttribArray(tp.attr.aPos);
      gl.vertexAttribPointer(tp.attr.aPos, 3, gl.FLOAT, false, 24, 0);
      gl.enableVertexAttribArray(tp.attr.aNormal);
      gl.vertexAttribPointer(tp.attr.aNormal, 3, gl.FLOAT, false, 24, 12);
      gl.enable(gl.POLYGON_OFFSET_FILL);
      gl.polygonOffset(1.0, 1.0);
      gl.drawArrays(gl.TRIANGLES, 0, this.triCount);
      gl.disable(gl.POLYGON_OFFSET_FILL);
      gl.disableVertexAttribArray(tp.attr.aNormal);
    }

    if (this.mode !== 'shaded' && this.lineCount > 0) {
      var lp = this.line;
      gl.useProgram(lp.program);
      gl.uniformMatrix4fv(lp.uniform.uProj, false, proj);
      gl.uniformMatrix4fv(lp.uniform.uView, false, view);
      gl.uniform3f(lp.uniform.uColor, 0.30, 0.85, 0.68);
      gl.bindBuffer(gl.ARRAY_BUFFER, this.lineBuf);
      gl.enableVertexAttribArray(lp.attr.aPos);
      gl.vertexAttribPointer(lp.attr.aPos, 3, gl.FLOAT, false, 12, 0);
      gl.drawArrays(gl.LINES, 0, this.lineCount);
    }
  };

  Viewer.prototype.fit = function () {
    if (!this.mesh) return;
    var p = this.mesh.positions;
    if (!p.length) return;
    var min = [Infinity, Infinity, Infinity];
    var max = [-Infinity, -Infinity, -Infinity];
    for (var i = 0; i < p.length; i += 3) {
      for (var k = 0; k < 3; k++) {
        if (p[i + k] < min[k]) min[k] = p[i + k];
        if (p[i + k] > max[k]) max[k] = p[i + k];
      }
    }
    var c = [(min[0] + max[0]) / 2, (min[1] + max[1]) / 2, (min[2] + max[2]) / 2];
    var ext = Math.max(max[0] - min[0], max[1] - min[1], max[2] - min[2]);
    var radius = Math.max(ext * 0.5, 1e-6);
    this.center = c;
    this.radius = radius;
    this.dist = radius / Math.tan(22.5 * Math.PI / 180) * 1.25;
    this.yaw = -0.55;
    this.pitch = 0.40;
  };

  Viewer.prototype.loadText = function (text) {
    var mesh = parseOBJ(text);
    this.loadMesh(mesh);
    return mesh;
  };

  Viewer.prototype.loadMesh = function (mesh) {
    this.mesh = mesh;
    if (!this.ready) return;
    var gl = this.gl;
    var n = mesh.triangles * 3;
    var inter = new Float32Array(n * 6);
    var pos = mesh.positions;
    var nrm = mesh.normals;
    for (var i = 0; i < n; i++) {
      inter[i * 6] = pos[i * 3];
      inter[i * 6 + 1] = pos[i * 3 + 1];
      inter[i * 6 + 2] = pos[i * 3 + 2];
      inter[i * 6 + 3] = nrm[i * 3];
      inter[i * 6 + 4] = nrm[i * 3 + 1];
      inter[i * 6 + 5] = nrm[i * 3 + 2];
    }
    gl.bindBuffer(gl.ARRAY_BUFFER, this.triBuf);
    gl.bufferData(gl.ARRAY_BUFFER, inter, gl.STATIC_DRAW);
    this.triCount = n;
    gl.bindBuffer(gl.ARRAY_BUFFER, this.lineBuf);
    gl.bufferData(gl.ARRAY_BUFFER, mesh.lines, gl.STATIC_DRAW);
    this.lineCount = mesh.lines.length / 3;
    this.fit();
    this.requestRender();
  };

  Viewer.prototype.setMode = function (mode) {
    this.mode = mode;
    this.requestRender();
  };

  Viewer.prototype.reset = function () {
    if (!this.mesh) return;
    this.fit();
    this.requestRender();
  };

  Viewer.prototype.clear = function () {
    this.mesh = null;
    this.triCount = 0;
    this.lineCount = 0;
    this.requestRender();
  };

  global.Viewer = Viewer;
  global.parseOBJ = parseOBJ;
})(window);
