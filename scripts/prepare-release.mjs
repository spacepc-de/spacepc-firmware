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
const buildDirectory = resolve(argument("build-dir") ?? "");
const outputDirectory = resolve(argument("out") ?? "firmware-dist");
const hardware = argument("hardware") ?? "development-target-unverified";

if (!project || !version || !argument("build-dir")) {
  throw new Error("Usage: prepare-release.mjs --project <id> --version <semver> --build-dir <path> [--out <path>] [--hardware <revision>]");
}

if (!/^\d+\.\d+\.\d+(?:-[0-9A-Za-z.-]+)?$/.test(version)) {
  throw new Error("Version must use semantic versioning.");
}

const flashLayoutPath = resolve(buildDirectory, "flash-layout.json");
const flashLayout = JSON.parse(await readFile(flashLayoutPath, "utf8"));
const parts = flashLayout.parts
  .filter((part) => Number.isInteger(part.offset) && typeof part.source === "string" && part.source.endsWith(".bin"))
  .map((part) => ({ ...part, path: basename(part.source) }));

if (parts.length === 0) {
  throw new Error(`No binary offsets found in ${flashLayoutPath}; refusing to guess flash offsets.`);
}

await mkdir(outputDirectory, { recursive: true });
const checksumLines = [];

for (const part of parts) {
  const target = resolve(outputDirectory, part.path);
  await copyFile(part.source, target);
  const digest = createHash("sha256").update(await readFile(target)).digest("hex");
  checksumLines.push(`${digest}  ${part.path}`);
}

const manifest = {
  name: `SpacePC ${project}`,
  version,
  new_install_prompt_erase: true,
  builds: [{
    chipFamily: "ESP32-S3",
    parts: parts.map(({ path, offset }) => ({ path, offset }))
  }],
  spacepc: {
    projectId: project,
    hardware: [hardware],
    stability: version.includes("-") ? "beta" : "stable"
  }
};

await writeFile(resolve(outputDirectory, "manifest.json"), `${JSON.stringify(manifest, null, 2)}\n`);
await writeFile(resolve(outputDirectory, "SHA256SUMS"), `${checksumLines.join("\n")}\n`);
console.log(`Prepared ${parts.length} binary parts in ${outputDirectory}`);
