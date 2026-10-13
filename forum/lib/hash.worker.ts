import { sha256 } from "@noble/hashes/sha2.js";
self.onmessage = async (event: MessageEvent<File>) => {
  try {
    const file = event.data;
    const hash = sha256.create();
    for (let offset = 0; offset < file.size; offset += 1024 * 1024) {
      hash.update(
        new Uint8Array(
          await file.slice(offset, offset + 1024 * 1024).arrayBuffer(),
        ),
      );
      self.postMessage({
        progress: Math.min(1, (offset + 1024 * 1024) / file.size),
      });
    }
    self.postMessage({
      hash: Array.from(hash.digest(), (b) =>
        b.toString(16).padStart(2, "0"),
      ).join(""),
    });
  } catch (e) {
    self.postMessage({ error: String(e) });
  }
};
