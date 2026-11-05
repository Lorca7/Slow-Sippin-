#!/usr/bin/env node
/*
 * Build the macOS app: dist/Slow Sippin-darwin-<arch>/Slow Sippin.app
 * and a zip per architecture.
 *
 *   npm run dist:mac                 # arm64 (Apple Silicon) + x64 (Intel)
 *   npm run dist:mac -- arm64        # one architecture only
 *
 * The app is ad-hoc signed (required to launch on Apple Silicon). On a Mac
 * this uses `codesign`; on Linux it uses `rcodesign` (apple-codesign) if it
 * is on the PATH or given via RCODESIGN=/path/to/rcodesign.
 */
'use strict';

const path = require('path');
const fs = require('fs');
const { execFileSync } = require('child_process');
const { packager } = require('@electron/packager');

const ROOT = path.join(__dirname, '..');
const DIST = path.join(ROOT, 'dist');
const NAME = 'Slow Sippin';
const pkg = require(path.join(ROOT, 'package.json'));

const archs = process.argv.slice(2).filter((a) => /^(arm64|x64)$/.test(a));
if (!archs.length) archs.push('arm64', 'x64');

const AUDIO_UTIS = [
  'public.audio', 'com.microsoft.waveform-audio', 'public.aiff-audio', 'public.aifc-audio',
  'public.mp3', 'org.xiph.flac',
];

function which(cmd) {
  try { return execFileSync('which', [cmd]).toString().trim() || null; } catch (e) { return null; }
}

function sign(appPath) {
  if (process.platform === 'darwin') {
    execFileSync('codesign', ['--force', '--deep', '--sign', '-', appPath], { stdio: 'inherit' });
    execFileSync('codesign', ['--verify', '--deep', '--strict', appPath], { stdio: 'inherit' });
    return 'codesign (ad-hoc)';
  }
  const rc = process.env.RCODESIGN || which('rcodesign');
  if (!rc) {
    console.warn('! not signed here — sign once on the Mac (see README, "First launch").');
    return 'unsigned (sign on the Mac: see README)';
  }
  execFileSync(rc, ['sign', appPath], { stdio: 'inherit' });
  return 'rcodesign (ad-hoc)';
}

function zip(dir, appName, out) {
  if (fs.existsSync(out)) fs.unlinkSync(out);
  if (process.platform === 'darwin') {
    execFileSync('ditto', ['-c', '-k', '--sequesterRsrc', '--keepParent', appName, out], { cwd: dir, stdio: 'inherit' });
  } else {
    execFileSync('zip', ['-q', '-r', '-y', '-9', out, appName], { cwd: dir, stdio: 'inherit' });
  }
}

(async () => {
  fs.mkdirSync(DIST, { recursive: true });
  for (const arch of archs) {
    console.log(`\n▸ packaging ${arch}…`);
    const [outDir] = await packager({
      dir: ROOT,
      out: DIST,
      name: NAME,
      executableName: NAME,
      platform: 'darwin',
      arch,
      overwrite: true,
      asar: true,
      prune: false,          // no runtime dependencies
      icon: path.join(ROOT, 'build', 'icon.icns'),
      appBundleId: 'eu.slowsippin.app',
      appVersion: pkg.version,
      buildVersion: pkg.version,
      appCategoryType: 'public.app-category.music',
      darwinDarkModeSupport: false,
      ignore: [
        /^\/node_modules($|\/)/, /^\/dist($|\/)/, /^\/build($|\/)/, /^\/scripts($|\/)/, /^\/\.git($|\/)/,
        /^\/\.gitignore$/, /^\/package-lock\.json$/, /\.DS_Store$/,
      ],
      extendInfo: {
        NSHumanReadableCopyright: 'Slow Sippin',
        LSMinimumSystemVersion: '12.0',
        CFBundleDocumentTypes: [{
          CFBundleTypeName: 'Audio sample',
          CFBundleTypeRole: 'Viewer',
          LSHandlerRank: 'Alternate',
          LSItemContentTypes: AUDIO_UTIS,
        }],
      },
    });
    const appPath = path.join(outDir, `${NAME}.app`);
    const how = sign(appPath);
    const zipName = `Slow-Sippin-${pkg.version}-mac-${arch === 'arm64' ? 'apple-silicon' : 'intel'}.zip`;
    zip(outDir, `${NAME}.app`, path.join(DIST, zipName));
    const mb = (fs.statSync(path.join(DIST, zipName)).size / 1048576).toFixed(0);
    console.log(`✓ ${arch}: ${appPath}\n  signed: ${how}\n  zip: dist/${zipName} (${mb} MB)`);
  }
})().catch((e) => { console.error(e); process.exit(1); });
