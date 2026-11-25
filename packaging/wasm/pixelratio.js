// How many canvas pixels the game draws for each of the page's (CSS) pixels,
// in place of Emscripten's own emscripten_get_device_pixel_ratio, which says
// devicePixelRatio. SDL asks when it makes the window (Window.c always asks
// for SDL_WINDOW_HIGH_PIXEL_DENSITY in the browser) and on every resize, and
// sizes the canvas at the page's size times this. The page picks it
// (index.html, Sharpness: Module.jkPixelRatio); without the page it's 1, one
// canvas pixel per page pixel, as the game drew before.
//
// A library given with --js-library comes after Emscripten's own, so this one
// is used (emscripten src/modules.mjs).
addToLibrary({
  emscripten_get_device_pixel_ratio: () => {
    var pick = Module['jkPixelRatio'];
    var r = typeof pick == 'function' ? +pick() : 1;
    return r >= 1 && r <= 8 ? r : 1;
  },
});
