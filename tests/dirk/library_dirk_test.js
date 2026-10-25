// SPDX-License-Identifier: AGPL-3.0-only
//
// Test-only: publish the raw wasm export of dirk_cube_main, for the page to
// enter through WebAssembly.promising.
var LibraryDirkTest = {
  dirk_test_publish__postset:
    "addOnPostRun(function () {" +
    "  var exports = typeof wasmExports != 'undefined' ? wasmExports : Module['asm'];" +
    "  Module['dirkCubeRaw'] = exports['dirk_cube_main'];" +
    "});",
  dirk_test_publish: function () {},
};
mergeInto(LibraryManager.library, LibraryDirkTest);
