"use strict";

window.cardinalClassroomLoadPatch = async () => {
  const params = new URLSearchParams(location.search);
  const patchStorage = params.get("patchstorage");
  const patchUrl = params.get("patchurl");
  if (!patchStorage && !patchUrl) return;

  const api = "https://patchstorage.com/api/alpha/";
  const patchStorageHost = /^(www\.)?patchstorage\.com$/;
  async function fetchFile(source) {
    const url = new URL(source, location.href);
    if (url.protocol !== "https:" && url.origin !== location.origin) {
      throw new Error("Use an HTTPS patch URL.");
    }

    const response = await fetch(url, { signal: AbortSignal.timeout(20000) });
    if (!response.ok) throw new Error(`The patch server returned HTTP ${response.status}.`);
    return response;
  }

  let source = patchUrl;
  const url = new URL(patchStorage || patchUrl, location.href);
  if (patchStorage || (patchStorageHost.test(url.hostname) && !url.pathname.startsWith("/api/"))) {
    const slug = patchStorageHost.test(url.hostname)
      ? url.pathname.split("/").filter(Boolean).at(-1)
      : patchStorage;
    if (!slug || !/^[a-z0-9-]+$/i.test(slug)) {
      throw new Error("Use a PatchStorage Cardinal patch slug or page URL.");
    }

    const query = new URLSearchParams({ platform: "7834", slug });
    const results = await (await fetchFile(`${api}patches?${query}`)).json();
    const match = results.find(patch => patch.slug === slug);
    if (!match) throw new Error("That Cardinal patch was not found on PatchStorage.");

    const patch = await (await fetchFile(`${api}patches/${match.id}`)).json();
    source = patch.files.find(file => /\.vcv$/i.test(file.filename))?.url;
    if (!source) throw new Error("This PatchStorage entry has no .vcv file.");
  } else if (url.hostname === "github.com") {
    const [, owner, repo, view, ...path] = url.pathname.split("/");
    if (view === "blob" && path.length > 1) {
      source = `https://raw.githubusercontent.com/${owner}/${repo}/${path.join("/")}`;
    }
  }

  const response = await fetchFile(source);
  const bytes = new Uint8Array(await response.arrayBuffer());
  const compressed = bytes[0] === 0x28 && bytes[1] === 0xb5 && bytes[2] === 0x2f && bytes[3] === 0xfd;
  if (!compressed) {
    try {
      if (!Array.isArray(JSON.parse(new TextDecoder().decode(bytes)).modules)) {
        throw new Error("Invalid patch");
      }
    } catch {
      throw new Error("This URL did not return a Cardinal .vcv patch.");
    }
  }

  const chunks = [];
  for (let offset = 0; offset < bytes.length; offset += 8192) {
    chunks.push(String.fromCharCode(...bytes.subarray(offset, offset + 8192)));
  }
  window.cardinalClassroomPatch = btoa(chunks.join(""));
};
