/*
 * media-stubs.h - page script that defines placeholder media element
 * interfaces while WebKit is built with ENABLE_VIDEO=OFF (Milestone 1).
 *
 * Without video there is no HTMLMediaElement, HTMLVideoElement or
 * HTMLAudioElement global, and sites that merely name them throw at load:
 * YouTube's main bundle (kevlar_base) evaluates TypeScript decorator
 * metadata `design:type HTMLVideoElement` while defining its components, so
 * `ReferenceError: Can't find variable: HTMLVideoElement` aborted the whole
 * module and the page stayed on its loading skeleton.
 *
 * The placeholders are constructors that throw (as the real ones do without
 * `new`), with HTMLElement as their prototype chain, so references,
 * `instanceof` and prototype lookups work. <video> elements remain
 * HTMLUnknownElement (no canPlayType), so feature checks still see no video.
 * Injected at document start in every frame. Remove with Milestone 2.
 */
#pragma once

static const char ps5wkMediaStubsScript[] =
    "(function () {\n"
    "  if (typeof HTMLMediaElement !== 'undefined' || typeof HTMLElement === 'undefined') return;\n"
    "  function stub(name, base) {\n"
    "    var C = function () { throw new TypeError('Illegal constructor'); };\n"
    "    C.prototype = Object.create(base.prototype, { constructor: { value: C, writable: true, configurable: true } });\n"
    "    Object.defineProperty(C, 'name', { value: name });\n"
    "    Object.setPrototypeOf(C, base);\n"
    "    Object.defineProperty(window, name, { value: C, writable: true, configurable: true });\n"
    "    return C;\n"
    "  }\n"
    "  var media = stub('HTMLMediaElement', HTMLElement);\n"
    "  var constants = { NETWORK_EMPTY: 0, NETWORK_IDLE: 1, NETWORK_LOADING: 2, NETWORK_NO_SOURCE: 3,\n"
    "    HAVE_NOTHING: 0, HAVE_METADATA: 1, HAVE_CURRENT_DATA: 2, HAVE_FUTURE_DATA: 3, HAVE_ENOUGH_DATA: 4 };\n"
    "  for (var k in constants) { media[k] = constants[k]; media.prototype[k] = constants[k]; }\n"
    "  stub('HTMLVideoElement', media);\n"
    "  stub('HTMLAudioElement', media);\n"
    "  if (typeof HTMLSourceElement === 'undefined') stub('HTMLSourceElement', HTMLElement);\n"
    "  if (typeof HTMLTrackElement === 'undefined') stub('HTMLTrackElement', HTMLElement);\n"
    "  if (typeof MediaError === 'undefined') stub('MediaError', Object);\n"
    "  if (typeof TimeRanges === 'undefined') stub('TimeRanges', Object);\n"
    // new Audio(): YouTube's voice-search feedback preloads sounds with it.
    // An <audio> element (HTMLUnknownElement) whose load/pause do nothing and
    // whose play() rejects, as a browser without media support would.
    "  if (typeof Audio === 'undefined') {\n"
    "    var audio = function Audio(src) {\n"
    "      var e = document.createElement('audio');\n"
    "      e.load = function () {}; e.pause = function () {};\n"
    "      e.play = function () { return Promise.reject(new DOMException('Media playback is not supported', 'NotSupportedError')); };\n"
    "      e.canPlayType = function () { return ''; };\n"
    "      if (src !== undefined) e.src = src;\n"
    "      return e;\n"
    "    };\n"
    "    Object.defineProperty(window, 'Audio', { value: audio, writable: true, configurable: true });\n"
    "  }\n"
    "})();\n";
