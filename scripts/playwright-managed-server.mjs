import { spawn } from "node:child_process";
import { setTimeout as delay } from "node:timers/promises";
import process from "node:process";

async function isAvailable(url) {
  try {
    const response = await fetch(url, { signal: AbortSignal.timeout(2_000) });
    return response.status >= 200 && response.status < 500;
  } catch {
    return false;
  }
}

async function stopChild(child, exitPromise) {
  if (child.exitCode !== null || child.signalCode !== null) return;
  child.kill();
  if (await Promise.race([exitPromise.then(() => true), delay(2_000).then(() => false)])) return;
  child.kill("SIGKILL");
  await Promise.race([exitPromise, delay(2_000)]);
}

export async function startManagedNodeServer({ name, cwd, args, url, env = {} }) {
  if (await isAvailable(url)) return async () => {};

  const child = spawn(process.execPath, args, {
    cwd,
    env: { ...process.env, ...env },
    windowsHide: true,
    stdio: ["ignore", "inherit", "inherit"],
  });
  let exitResult;
  const exitPromise = new Promise((resolve) => {
    child.once("exit", (code, signal) => {
      exitResult = { code, signal };
      resolve(exitResult);
    });
  });

  const deadline = Date.now() + 120_000;
  while (Date.now() < deadline) {
    if (await isAvailable(url)) {
      return () => stopChild(child, exitPromise);
    }
    if (exitResult) {
      throw new Error(`${name} exited before becoming ready (code=${exitResult.code}, signal=${exitResult.signal})`);
    }
    await delay(100);
  }

  await stopChild(child, exitPromise);
  throw new Error(`${name} did not become ready at ${url} within 120 seconds`);
}
