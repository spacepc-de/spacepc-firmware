#!/usr/bin/env node

import { createHash } from "node:crypto";
import { copyFile, mkdir, readFile, writeFile } from "node:fs/promises";
import { basename, resolve } from "node:path";

function argument(name) {
  const index = process.argv.indexOf(`--${name}`);
  return index === -1 ? undefined : process.argv[index + 1];
}

function argumentsFor(name) {
  return process.argv.flatMap((value, index) =>
    value === `--${name}` && process.argv[index + 1]
      ? [process.argv[index + 1]]
      : []
  );
}

const project = argument("project");
const version = argument("version");
const outputDirectory = resolve(argument("out") ?? "firmware-dist");
const hardware = argumentsFor("hardware");
const stability = argument("stability") ?? (version?.includes("-") ? "beta" : "stable");
const buildDate = argument("build-date") ?? new Date().toISOString();
const sourceCommit = argument("source-commit");
const releaseNotes = argumentsFor("release-note");
const requestedTargets = argumentsFor("target");

if (!project || !version) {
  throw new Error("Usage: prepare-release.mjs --project <id> --version <semver> --target <chip-family:prefix:build-directory> [--target ...] [--hardware <revision>]");
}

if (!/^\d+\.\d+\.\d+(?:-[0-9A-Za-z.-]+)?$/.test(version)) {
  throw new Error("Version must use semantic versioning.");
}

if (!["beta", "stable"].includes(stability)) {
  throw new Error("Stability must be either beta or stable.");
}

await mkdir(outputDirectory, { recursive: true });
const checksumLines = [];
const legacyTargets = argument("esp32-build-dir") && argument("esp32-s3-build-dir")
  ? [
      `ESP32:esp32:${argument("esp32-build-dir")}`,
      `ESP32-S3:esp32-s3:${argument("esp32-s3-build-dir")}`
    ]
  : [];
const targetArguments = requestedTargets.length > 0 ? requestedTargets : legacyTargets;
if (targetArguments.length === 0) {
  throw new Error("At least one --target is required.");
}

const allowedChipFamilies = new Set([
  "ESP32",
  "ESP32-S2",
  "ESP32-S3",
  "ESP32-C3",
  "ESP32-C6"
]);
const targets = targetArguments.map((target) => {
  const [chipFamily, prefix, ...directoryParts] = target.split(":");
  const buildDirectory = directoryParts.join(":");
  if (
    !allowedChipFamilies.has(chipFamily) ||
    !/^[a-z0-9-]+$/.test(prefix) ||
    !buildDirectory
  ) {
    throw new Error(`Invalid target "${target}". Expected CHIP-FAMILY:file-prefix:build-directory.`);
  }
  return {
    chipFamily,
    prefix,
    buildDirectory: resolve(buildDirectory)
  };
});
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
    hardware: hardware.length > 0 ? hardware : ["development-target-unverified"],
    stability,
    buildDate,
    sourceCommit,
    releaseNotes: releaseNotes.length > 0
      ? releaseNotes
      : ["Firmware release produced by the SpacePC build pipeline."],
    eraseSettings: true
  }
};

await writeFile(resolve(outputDirectory, "manifest.json"), `${JSON.stringify(manifest, null, 2)}\n`);
await writeFile(resolve(outputDirectory, "SHA256SUMS"), `${checksumLines.join("\n")}\n`);
console.log(`Prepared ${builds.length} chip-family builds in ${outputDirectory}`);
