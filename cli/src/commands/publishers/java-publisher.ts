import {
  BASE_DIR,
  ensureEmptyDir,
  getRootedPath,
  listFiles,
} from '@/utils/file_utils.js';
import { Log } from '@/utils/terminal_utils.js';
import { getRootVersion } from '@/utils/toml_utils.js';
import { execSync } from 'node:child_process';
import fs from 'node:fs';
import path from 'node:path';

import { PublisherOptions } from './publisher-options.js';

const CENTRAL_PORTAL_UPLOAD = 'https://central.sonatype.com/api/v1/publisher/upload';
const CENTRAL_PORTAL_STATUS = 'https://central.sonatype.com/api/v1/publisher/status';

const TARGETS = [
  'aarch64-apple-darwin',
  'x86_64-apple-darwin',
  'aarch64-unknown-linux-gnu',
  'x86_64-unknown-linux-gnu',
  'x86_64-unknown-linux-musl',
  'aarch64-unknown-linux-musl',
  'x86_64-pc-windows-msvc',
  'i686-pc-windows-msvc',
];

const JAVA_NATIVE_DIR = path.resolve(
  BASE_DIR,
  'statsig-java/src/main/resources/native',
);

export async function javaPublish(options: PublisherOptions) {
  const libFiles = [
    ...listFiles(options.workingDir, '**/target/**/release/*.dylib'),
    ...listFiles(options.workingDir, '**/target/**/release/*.so'),
    ...listFiles(options.workingDir, '**/target/**/release/*.dll'),
  ].filter(isMappedTarget);

  Log.stepBegin('Clearing Java Native Directory');
  ensureEmptyDir(JAVA_NATIVE_DIR);
  Log.stepEnd(`Cleared ${JAVA_NATIVE_DIR}`);

  moveJavaLibraries(libFiles);
  await publishJavaPackages();
}

function isMappedTarget(file: string): boolean {
  return TARGETS.some((target) => file.includes(target));
}

function getDestination(file: string, destKeys: string[]): string | null {
  const found = destKeys.findIndex((key) => file.includes(key));

  if (found !== -1) {
    const value = destKeys[found];
    return value;
  }

  return null;
}

function moveJavaLibraries(libFiles: string[]) {
  Log.stepBegin('Moving Java Libraries');

  let allFilesMoved = true;
  let movedFiles = 0;
  libFiles.forEach((file) => {
    const destination = getDestination(file, TARGETS);
    if (!destination) {
      Log.stepProgress(`No mapping found for: ${file}`, 'failure');
      allFilesMoved = false;
      return;
    }

    const filename = path.basename(file);
    const destDir = path.resolve(JAVA_NATIVE_DIR, destination);
    ensureEmptyDir(destDir);

    const destinationPath = path.resolve(destDir, filename);
    execSync(`cp ${file} ${destinationPath}`);
    ++movedFiles;
    Log.stepProgress(`Copied lib to ${destinationPath}`);
  });

  if (!allFilesMoved) {
    Log.stepEnd('Failed to move all files', 'failure');
    throw new Error('Failed to move all files');
  }

  if (movedFiles < TARGETS.length) {
    Log.stepEnd(
      `Moved only ${movedFiles} of ${TARGETS.length} expected files`,
      'failure',
    );
    throw new Error('Failed to move all files');
  }

  Log.stepEnd('Successfully moved Java Libraries');
}

async function publishJavaPackages() {
  Log.stepBegin('Staging Java Packages');

  // Publish the signed publication (jars + POM + .asc + checksums) into a local
  // maven2 tree at statsig-java/build/central-staging.
  execSync('./gradlew publishMavenJavaPublicationToLocalStagingRepository', {
    cwd: getRootedPath('statsig-java'),
    stdio: 'inherit',
  });

  Log.stepEnd('Successfully staged Java Packages');

  await uploadBundleToCentralPortal();
}

function getCentralPortalToken(): string {
  const username = process.env.ORG_GRADLE_PROJECT_MAVEN_USERNAME;
  const password = process.env.ORG_GRADLE_PROJECT_MAVEN_PASSWORD;
  if (!username || !password) {
    throw new Error(
      'Missing ORG_GRADLE_PROJECT_MAVEN_USERNAME/ORG_GRADLE_PROJECT_MAVEN_PASSWORD for Central Portal upload',
    );
  }
  return Buffer.from(`${username}:${password}`).toString('base64');
}

async function uploadBundleToCentralPortal() {
  const version = getRootVersion().toString();
  const token = getCentralPortalToken();
  const stagingDir = getRootedPath('statsig-java/build/central-staging');
  const bundlePath = getRootedPath('statsig-java/build/central-bundle.zip');

  Log.stepBegin('Zipping Central Portal bundle');
  // Zip from the staging root so entries start at com/statsig/javacore/...
  execSync(`rm -f "${bundlePath}"`, { stdio: 'inherit' });
  execSync(`zip -r -q "${bundlePath}" .`, { cwd: stagingDir, stdio: 'inherit' });
  Log.stepEnd(`Created ${bundlePath}`);

  Log.stepBegin('Uploading bundle to Central Portal');
  const form = new FormData();
  form.append(
    'bundle',
    new Blob([fs.readFileSync(bundlePath)]),
    'central-bundle.zip',
  );

  // AUTOMATIC releases once validation passes; USER_MANAGED stops at VALIDATED
  // so a human publishes it from the Portal UI (used for manual recovery).
  const publishingType = process.env.CENTRAL_PORTAL_PUBLISHING_TYPE ?? 'AUTOMATIC';
  const uploadUrl =
    `${CENTRAL_PORTAL_UPLOAD}` +
    `?name=${encodeURIComponent(`javacore-${version}`)}` +
    `&publishingType=${publishingType}`;

  const res = await fetch(uploadUrl, {
    method: 'POST',
    headers: { Authorization: `Bearer ${token}` },
    body: form,
  });

  if (!res.ok) {
    throw new Error(
      `Central Portal upload failed (${res.status}): ${await res.text()}`,
    );
  }

  const deploymentId = (await res.text()).trim();
  Log.stepEnd(`Uploaded deployment ${deploymentId}`);

  await waitForDeployment(deploymentId, token);
}

async function waitForDeployment(deploymentId: string, token: string) {
  Log.stepBegin('Waiting for Central Portal to accept the deployment');

  const intervalMs = 15_000;
  const deadline = Date.now() + 30 * 60 * 1000;
  // AUTOMATIC deployments progress PENDING -> VALIDATING -> VALIDATED ->
  // PUBLISHING -> PUBLISHED; once VALIDATED the release is committed.
  const done = new Set(['VALIDATED', 'PUBLISHING', 'PUBLISHED']);

  while (Date.now() < deadline) {
    const res = await fetch(`${CENTRAL_PORTAL_STATUS}?id=${deploymentId}`, {
      method: 'POST',
      headers: { Authorization: `Bearer ${token}` },
    });

    if (!res.ok) {
      throw new Error(
        `Central Portal status check failed (${res.status}): ${await res.text()}`,
      );
    }

    const data = (await res.json()) as {
      deploymentState?: string;
      errors?: unknown;
    };
    const state = data.deploymentState ?? 'UNKNOWN';
    Log.stepProgress(`deployment ${deploymentId}: ${state}`);

    if (done.has(state)) {
      Log.stepEnd(`Central Portal deployment ${state}`);
      return;
    }

    if (state === 'FAILED') {
      throw new Error(
        `Central Portal deployment ${deploymentId} FAILED: ${JSON.stringify(
          data.errors ?? data,
        )}`,
      );
    }

    await new Promise((resolve) => setTimeout(resolve, intervalMs));
  }

  throw new Error(
    `Central Portal deployment ${deploymentId} did not reach a terminal state before timeout`,
  );
}
