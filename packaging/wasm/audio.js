// Sound in the browser build: small changes to the Web Audio graph that
// Emscripten's OpenAL builds for the engine. Bundled into openjkdf2.js with
// --pre-js (cmake_modules/plat_wasm.cmake), so it is in place before the engine
// creates its AudioContext.
//
// The engine moves the listener and its 3D sounds as the game runs, and
// OpenAL makes each move by assigning AudioParam.value on the listener or on
// a sound's PannerNode. In Chrome, a panner whose position or orientation
// params, or the listener's, were written runs sample by sample for that
// audio block, its slow path: with 32 sounds playing the audio thread used
// about 11% of a core instead of about 3%. A 'k-rate' param takes one value
// per block of 128 samples (under 3 ms), which no one can hear in game
// sound, and keeps every panner on its fast path (about 3.5% measured).
//
// Everything is feature-tested and wrapped in try: a browser without
// automationRate, or one that refuses 'k-rate' for these params, keeps the
// graph exactly as OpenAL made it.
(function () {
  'use strict';
  var Ctx = typeof BaseAudioContext == 'function' ? BaseAudioContext
          : typeof AudioContext == 'function' ? AudioContext : null;
  if (!Ctx || typeof Ctx.prototype.createPanner != 'function') return;
  var proto = Ctx.prototype;
  if (proto.createPanner.jkAudio) return;  // already in place

  var PANNER = ['positionX', 'positionY', 'positionZ', 'orientationX', 'orientationY', 'orientationZ'];
  var LISTENER = ['positionX', 'positionY', 'positionZ', 'forwardX', 'forwardY', 'forwardZ', 'upX', 'upY', 'upZ'];

  function setKRate(param) {
    try {
      if (param && 'automationRate' in param && param.automationRate != 'k-rate')
        param.automationRate = 'k-rate';
    } catch (e) {}
  }

  // OpenAL sets the listener's position and its orientation in two separate
  // calls, and each call rewrites all nine listener params. Pass a write on
  // only when it changes the param's value, so each param is written once,
  // and only when it moved. (OpenAL already does this for panners.)
  var valueDesc = null;
  try {
    if (typeof AudioParam == 'function')
      valueDesc = Object.getOwnPropertyDescriptor(AudioParam.prototype, 'value');
  } catch (e) {}

  function writeOnlyChanges(param) {
    if (!valueDesc || !valueDesc.get || !valueDesc.set) return;
    var last;
    try {
      Object.defineProperty(param, 'value', {
        configurable: true,
        enumerable: true,
        get: function () { return valueDesc.get.call(this); },
        set: function (v) {
          var f = Math.fround(v);  // what the param stores
          if (f === last) return;
          valueDesc.set.call(this, v);
          last = f;
        }
      });
    } catch (e) {}
  }

  var listenersDone = typeof WeakSet == 'function' ? new WeakSet() : null;

  function setUpListener(ctx) {
    if (!listenersDone || !ctx || listenersDone.has(ctx)) return;
    listenersDone.add(ctx);
    var listener = ctx.listener;
    if (!listener) return;
    for (var i = 0; i < LISTENER.length; i++) {
      var param = listener[LISTENER[i]];
      if (!param) continue;  // older browsers only have setPosition()/setOrientation()
      setKRate(param);
      writeOnlyChanges(param);
    }
  }

  // OpenAL makes a panner for each 3D sound (AL.initSourcePanner calls
  // audioCtx.createPanner()).
  var createPanner = proto.createPanner;
  proto.createPanner = function () {
    var panner = createPanner.apply(this, arguments);
    try {
      for (var i = 0; i < PANNER.length; i++)
        setKRate(panner[PANNER[i]]);
      setUpListener(this);
    } catch (e) {}
    return panner;
  };
  proto.createPanner.jkAudio = true;

  // OpenAL makes its context's main GainNode straight after it creates the
  // AudioContext (alcCreateContext), so the listener is set up as the context
  // is created.
  var createGain = proto.createGain;
  if (typeof createGain == 'function') {
    proto.createGain = function () {
      var gain = createGain.apply(this, arguments);
      try { setUpListener(this); } catch (e) {}
      return gain;
    };
  }
})();
