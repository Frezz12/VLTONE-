import { api, type ForumAttachment, type ForumUpload } from "@vlt/api-client";
async function hashFile(
  file: File,
  signal: AbortSignal,
  progress: (value: number) => void,
): Promise<string> {
  return new Promise((resolve, reject) => {
    if (signal.aborted) {
      reject(new DOMException("Cancelled", "AbortError"));
      return;
    }
    const worker = new Worker(new URL("./hash.worker.ts", import.meta.url));
    const stop = () => {
      worker.terminate();
      reject(new DOMException("Cancelled", "AbortError"));
    };
    signal.addEventListener("abort", stop, { once: true });
    const done = () => {
      worker.terminate();
      signal.removeEventListener("abort", stop);
    };
    worker.onerror = (e) => {
      done();
      reject(Error(e.message));
    };
    worker.onmessage = (e) => {
      if (e.data.hash) {
        done();
        resolve(e.data.hash);
      } else if (e.data.error) {
        done();
        reject(Error(e.data.error));
      } else progress(e.data.progress);
    };
    worker.postMessage(file);
  });
}
function put(
  url: string,
  headers: Record<string, string>,
  body: Blob,
  signal: AbortSignal,
  onProgress: (n: number) => void,
) {
  return new Promise<void>((resolve, reject) => {
    const xhr = new XMLHttpRequest();
    const abort = () => xhr.abort();
    signal.addEventListener("abort", abort, { once: true });
    const finish = () => signal.removeEventListener("abort", abort);
    xhr.open("PUT", url);
    xhr.timeout = 5 * 60 * 1000;
    Object.entries(headers).forEach(([k, v]) => {
      if (k.toLowerCase() !== "content-length") xhr.setRequestHeader(k, v);
    });
    xhr.upload.onprogress = (e) => onProgress(e.loaded);
    xhr.onerror = () => {
      finish();
      reject(Error("Upload connection failed"));
    };
    xhr.ontimeout = () => {
      finish();
      reject(Error("Upload timed out"));
    };
    xhr.onabort = () => {
      finish();
      reject(new DOMException("Cancelled", "AbortError"));
    };
    xhr.onload = () => {
      finish();
      if (xhr.status >= 200 && xhr.status < 300) resolve();
      else reject(Error(`Upload failed (${xhr.status})`));
    };
    xhr.send(body);
  });
}
export async function uploadFile(
  file: File,
  csrf: string,
  signal: AbortSignal,
  progress: (value: number) => void,
  allocated: (id: string) => void,
  existing?: string,
  metadata?: { resource_type?: string; minimum_version?: string },
): Promise<ForumAttachment> {
  let upload: ForumUpload;
  if (existing)
    upload = await api.json<ForumUpload>(
      `/v1/forum/uploads/${existing}/resume`,
      "POST",
      {},
      csrf,
    );
  else {
    const sha256 = await hashFile(file, signal, (v) => progress(v * 10));
    if (signal.aborted) throw new DOMException("Cancelled", "AbortError");
    upload = await api.json<ForumUpload>(
      "/v1/forum/uploads",
      "POST",
      { name: file.name, bytes: file.size, sha256, ...metadata },
      csrf,
    );
    allocated(upload.attachment.id);
  }
  const id = upload.attachment.id;
  let offset = 0;
  for (const part of upload.parts) {
    if (signal.aborted) throw new DOMException("Cancelled", "AbortError");
    if (!part.done && part.request) {
      const before = offset;
      await put(
        part.request.url,
        part.request.headers,
        file.slice(offset, offset + part.bytes),
        signal,
        (n) => progress(10 + (85 * (before + n)) / file.size),
      );
    }
    offset += part.bytes;
    progress(10 + (85 * offset) / file.size);
  }
  const result = await api.json<{ attachment: ForumAttachment }>(
    `/v1/forum/uploads/${id}/complete`,
    "POST",
    {},
    csrf,
  );
  progress(100);
  return result.attachment;
}
