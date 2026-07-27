#!/usr/bin/env node

import { createHash } from "node:crypto";
import { copyFile, mkdir, readFile, writeFile } from "node:fs/promises";
import { basename, resolve } from "node:path";

function argument(name) {
  const index = process.argv.indexOf(`--${name}`);
  return index === -1 ? undefined : process.argv[index + 1];
}

const project = argument("project");
const version = argument("version");
const esp32BuildDirectory = resolve(argument("esp32-build-dir") ?? "");
const esp32S3BuildDirectory = resolve(argument("esp32-s3-build-dir") ?? "");
const outputDirectory = resolve(argument("out") ?? "firmware-dist");
const hardware = argument("hardware") ?? "development-target-unverified";
const stability = argument("stability") ?? (version?.includes("-") ? "beta" : "stable");
const buildDate = argument("build-date") ?? new Date().toISOString();
const sourceCommit = argument("source-commit");

if (!project || !version || !argument("esp32-build-dir") || !argument("esp32-s3-build-dir")) {
  throw new Error("Usage: prepare-release.mjs --project <id> --version <semver> --esp32-build-dir <path> --esp32-s3-build-dir <path> [--out <path>] [--hardware <revision>]");
}

if (!/^\d+\.\d+\.\d+(?:-[0-9A-Za-z.-]+)?$/.test(version)) {
  throw new Error("Version must use semantic versioning.");
}

if (!["beta", "stable"].includes(stability)) {
  throw new Error("Stability must be either beta or stable.");
}

await mkdir(outputDirectory, { recursive: true });
const checksumLines = [];
const targets = [
  { chipFamily: "ESP32", buildDirectory: esp32BuildDirectory, prefix: "esp32" },
  { chipFamily: "ESP32-S3", buildDirectory: esp32S3BuildDirectory, prefix: "esp32-s3" }
];
const builds = [];

for (const target of targets) {
  const flashLayoutPath = resolve(target.buildDirectory, "flash-layout.json");
  const flashLayout = JSON.parse(await readFile(flashLayoutPath, "utf8"));
  const parts = flashLayout.parts
    .filter((part) => Number.isInteger(part.offset) && typeof part.source === "string" && part.source.endsWith(".bin"))
    .map((part) => ({ ...part, path: `${target.prefix}-${basename(part.source)}` }));

  if (parts.length === 0) {
    throw new Error(`No binary offsets found in ${flashLayoutPath}; refusing to guess flash offsets.`);
  }

  for (const part of parts) {
    const destination = resolve(outputDirectory, part.path);
    await copyFile(part.source, destination);
    const digest = createHash("sha256").update(await readFile(destination)).digest("hex");
    checksumLines.push(`${digest}  ${part.path}`);
  }

  builds.push({
    chipFamily: target.chipFamily,
    parts: parts.map(({ path, offset }) => ({ path, offset }))
  });
}

const manifest = {
  name: `SpacePC ${project}`,
  version,
  new_install_prompt_erase: true,
  builds,
  spacepc: {
    projectId: project,
    hardware: [hardware],
    stability,
    buildDate,
    sourceCommit,
    releaseNotes: ["Automated dummy firmware pipeline test. No device functionality is included."],
    eraseSettings: true
  }
};

await writeFile(resolve(outputDirectory, "manifest.json"), `${JSON.stringify(manifest, null, 2)}\n`);
await writeFile(resolve(outputDirectory, "SHA256SUMS"), `${checksumLines.join("\n")}\n`);
console.log(`Prepared ${builds.length} chip-family builds in ${outputDirectory}`);
