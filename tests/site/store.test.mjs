// Tests symlink target resolution, which decides whether a package can
// find its own libraries in the guest. A relative NAR target has to
// become an absolute path before it reaches emscripten's filesystem:
// FS.readlink returns an absolute path but stat keeps the relative
// target's length, and the guest's 9p client trusts the length.
import { test } from "node:test";
import assert from "node:assert/strict";
import { readFile } from "node:fs/promises";

import { absoluteTarget } from "../../site/js/store.js";

const LIB = "/share/nix/store/abc-zlib-1.3.2/lib";

test("a sibling target resolves against the link's own directory", () => {
  assert.equal(
    absoluteTarget(`${LIB}/libz.so.1`, "libz.so.1.3.2"),
    `${LIB}/libz.so.1.3.2`,
  );
});

test("a target that walks up resolves the .. segments", () => {
  assert.equal(
    absoluteTarget(`${LIB}/pkgconfig/z.pc`, "../libz.so"),
    `${LIB}/libz.so`,
  );
  assert.equal(absoluteTarget(`${LIB}/a/b/c`, "./d"), `${LIB}/a/b/d`);
});

test("an absolute target is left alone: it names a store path", () => {
  const target = "/nix/store/xyz-hello-2.12.2/bin/hello";
  assert.equal(absoluteTarget(`${LIB}/hello`, target), target);
});

// Tests which entries of a NAR count as programs: the files under bin/
// that a PATH lookup would run. They become the symlinks in the
// guest's /share/bin, so a data file or a nested directory must not
// be offered as a command.
import { programsOf } from "../../site/js/store.js";

test("executables and symlinks directly under bin/ are programs", () => {
  const entries = [
    { path: "", type: "directory" },
    { path: "bin", type: "directory" },
    {
      path: "bin/rg",
      type: "regular",
      executable: true,
      data: new Uint8Array(),
    },
    { path: "bin/python", type: "symlink", target: "python3.10" },
    {
      path: "bin/README",
      type: "regular",
      executable: false,
      data: new Uint8Array(),
    },
    { path: "bin/sub", type: "directory" },
    {
      path: "bin/sub/tool",
      type: "regular",
      executable: true,
      data: new Uint8Array(),
    },
    {
      path: "lib/libz.so",
      type: "regular",
      executable: true,
      data: new Uint8Array(),
    },
  ];
  assert.deepEqual(programsOf(entries), ["rg", "python"]);
});

test("a package without a bin directory has no programs", () => {
  assert.deepEqual(programsOf([{ path: "", type: "directory" }]), []);
});

// Tests which of a narinfo's promises may fail a boot.
//
// A narinfo describes the same path twice. FileSize and FileHash
// describe the compressed file the cache happens to be serving; NarSize
// and NarHash describe the archive inside it. Only the second pair is in
// the fingerprint a cache signs, and a cache that recompresses a NAR
// changes the first pair without touching the bytes anyone vouched for:
// cache.nixos.org did exactly that to glibc-2.40-224, whose narinfo
// still says 9096823 bytes for a file it serves 9099653 of, while its
// NarHash matches to the byte. Refusing that path rejects content that
// is provably genuine, on the one field no signature covers.
//
// These drive fetchNar against a stubbed fetch, serving the NAR fixture
// uncompressed, because the claim is about what does and does not abort
// a download rather than about either check in isolation.
import { createHash } from "node:crypto";
import { fetchNar } from "../../site/js/store.js";

// No Cache API in node: every call in cache.js degrades to no caching.
globalThis.caches = {
  open: async () => {
    throw new Error("no storage here");
  },
};

const narFixture = new URL("../fixtures/sample.nar", import.meta.url);

async function servedNar() {
  const bytes = new Uint8Array(await readFile(narFixture));
  let served = 0;
  globalThis.fetch = async () => {
    served += 1;
    return new Response(bytes);
  };
  return {
    bytes,
    sha256: `sha256:${createHash("sha256").update(bytes).digest("hex")}`,
    fetches: () => served,
  };
}

function narinfo(bytes, overrides) {
  return {
    storePath: "/nix/store/abc-sample",
    url: "nar/sample.nar",
    compression: "none",
    substituter: "https://cache.example.org",
    narSize: bytes.byteLength,
    ...overrides,
  };
}

test("a recompressed NAR still boots: FileSize and FileHash are unsigned", async () => {
  const nar = await servedNar();
  const info = narinfo(nar.bytes, {
    // What cache.nixos.org says about glibc: both compressed fields
    // stale, both signed fields exact.
    fileSize: nar.bytes.byteLength + 2830,
    fileHash: `sha256:${"a".repeat(64)}`,
    narHash: nar.sha256,
  });

  const entries = await fetchNar(info, () => {});
  assert.equal(entries.length > 0, true);
  assert.equal(nar.fetches(), 1);
});

test("a NAR whose signed hash is wrong is refused", async () => {
  const nar = await servedNar();
  const info = narinfo(nar.bytes, { narHash: `sha256:${"b".repeat(64)}` });

  await assert.rejects(() => fetchNar(info, () => {}), /sha256 does not match/);
});

test("a NAR whose signed size is wrong is refused", async () => {
  const nar = await servedNar();
  const info = narinfo(nar.bytes, {
    narSize: nar.bytes.byteLength + 1,
    narHash: nar.sha256,
  });

  await assert.rejects(() => fetchNar(info, () => {}), /narinfo says/);
});
