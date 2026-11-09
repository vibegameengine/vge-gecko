import { execFileSync, spawn, spawnSync } from "node:child_process";
import { createRequire } from "node:module";
import { readFileSync, writeFileSync } from "node:fs";
import os from "node:os";
import path from "node:path";

const require = createRequire("C:/GameEngien/hyperreal-starter-kit/");
const { chromium } = require("playwright-core");

const label = process.argv[2] ?? "chrome";
const seconds = Number(process.argv[3] ?? 80);
const results = "C:/Projects/gecko-build-logs/bench";
const tools = "C:/Projects/gecko/vge/bedrock/tools";
const chrome = "C:/Users/pavel/AppData/Local/ms-playwright/chromium-1234/chrome-win64/chrome.exe";
const slots = path.join(os.homedir(), ".claude/skills/browser-slots/browser-slots.mjs");
const name = `chromebench-${label}`;

function powershell(command) {
  return execFileSync("powershell", ["-NoProfile", "-Command", command], { encoding: "utf8" }).trim();
}

function chromePids() {
  return powershell("(Get-Process chrome | Where-Object { $_.Path -like '*chromium-1234*' }).Id")
    .split(/\s+/)
    .filter(Boolean);
}

function memory(file) {
  const pids = chromePids();
  const out = execFileSync("python", ["-I", `${tools}/regions.py`, `${results}/${label}.${file}.json`, ...pids], {
    encoding: "utf8",
  });
  const totals = { private_ws_mb: 0, shared_ws_mb: 0, commit_mb: 0, processes: 0 };
  for (const match of out.matchAll(/pws (\d+) MB sws (\d+) MB commit (\d+) MB/g)) {
    totals.private_ws_mb += Number(match[1]);
    totals.shared_ws_mb += Number(match[2]);
    totals.commit_mb += Number(match[3]);
    totals.processes++;
  }
  return totals;
}

function videoMemory() {
  const value = powershell(
    "$c = Get-Counter '\\GPU Process Memory(*)\\Dedicated Usage' -ErrorAction SilentlyContinue; " +
      `$ids = (Get-Process chrome | Where-Object { $_.Path -like '*chromium-1234*' }).Id; ` +
      "$sum = 0; foreach ($s in $c.CounterSamples) { if ($s.InstanceName -match 'pid_(\\d+)_' -and $ids -contains [int]$Matches[1]) { $sum += $s.CookedValue } }; " +
      "[int]($sum / 1MB)",
  );
  return Number(value);
}

function trimAll() {
  const source = `import ctypes,sys
k=ctypes.WinDLL('kernel32');p=ctypes.WinDLL('psapi')
k.OpenProcess.restype=ctypes.c_void_p
p.EmptyWorkingSet.argtypes=[ctypes.c_void_p]
for pid in sys.argv[1:]:
    h=k.OpenProcess(0x1F0FFF,False,int(pid))
    if h: p.EmptyWorkingSet(h)
`;
  spawnSync("python", ["-I", "-c", source, ...chromePids()]);
}

function parseFrames(lines) {
  const samples = lines
    .map((line) => line.match(/frameprobe fps (\d+) longest ([\d.]+) over25 (\d+) over50 (\d+)/))
    .filter(Boolean)
    .map((match) => match.slice(1).map(Number));
  const spikes = lines
    .map((line) => line.match(/spike at (\d+) ms length ([\d.]+)/))
    .filter(Boolean)
    .map((match) => [Number(match[1]), Number(match[2])]);
  const steady = samples.slice(1);
  const late = spikes.filter(([time]) => time > 15000).map(([, length]) => length);
  return {
    first_frame_ms: Math.max(0, ...samples.map((sample) => sample[1])),
    fps_min: Math.min(...steady.map((sample) => sample[0])),
    late_spikes: late.length,
    late_max_ms: Math.max(0, ...late),
  };
}

execFileSync("node", [slots, "acquire-perf", name, "--yes-timing", "Chromium baseline of the game"], { stdio: "ignore" });
let browser;
try {
  const probe = readFileSync(`${tools}/../test/frame-probe.js`, "utf8");
  browser = await chromium.launch({ headless: false, executablePath: chrome, args: ["--window-size=1280,800"] });
  const page = await browser.newPage({ viewport: { width: 1264, height: 761 } });
  const lines = [];
  page.on("console", (message) => {
    if (message.text().includes("frameprobe")) lines.push(message.text());
  });
  await page.goto("https://claybound-56949.web.app/");
  await page.evaluate(probe);
  const pid = powershell(
    "(Get-Process chrome | Where-Object { $_.Path -like '*chromium-1234*' -and $_.MainWindowHandle -ne 0 } | Select-Object -First 1).Id",
  );
  const watchFile = `${results}/${label}.screen.json`;
  const watch = spawn("python", [`${tools}/windowwatch.py`, pid, String(seconds), watchFile], { stdio: "inherit" });
  await new Promise((resolve) => watch.on("exit", resolve));
  const before = memory("before");
  const video = videoMemory();
  trimAll();
  await new Promise((resolve) => setTimeout(resolve, 15000));
  const after = memory("after");
  const result = {
    label,
    frames: parseFrames(lines),
    memory_before_trim: before,
    memory_15s_after_trim: after,
    video_mb: video,
    screen: JSON.parse(readFileSync(watchFile, "utf8")),
  };
  writeFileSync(`${results}/${label}.json`, JSON.stringify(result, null, 1));
  writeFileSync(`${results}/${label}.log`, lines.join("\n"));
  console.log(JSON.stringify(result));
} finally {
  await browser?.close();
  execFileSync("node", [slots, "release", name], { stdio: "ignore" });
}
