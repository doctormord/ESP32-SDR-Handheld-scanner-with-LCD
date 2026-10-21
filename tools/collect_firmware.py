#!/usr/bin/env python3
"""Collect firmware CI artifacts into a static firmware/ folder with one manifest."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil


def collect(source, output, catalog=None):
    source, output = source.resolve(), output.resolve()
    if output.exists():
        raise ValueError('Use a fresh output folder; refusing to mix releases')
    manifests = sorted(source.rglob('manifest.json'))
    if not manifests:
        raise ValueError('No firmware manifests found')
    variants, files, metadata = {}, [], []
    for path in manifests:
        if not path.resolve().is_relative_to(source):
            raise ValueError('Manifest escapes input folder')
        manifest = json.loads(path.read_text())
        if manifest.get('schema_version') != 1 or not manifest.get('variants'):
            raise ValueError('Invalid firmware manifest')
        for profile, variant in manifest['variants'].items():
            if not re.fullmatch(r'[A-Za-z0-9][A-Za-z0-9_.-]*', profile):
                raise ValueError('Invalid profile ID')
            if profile in variants:
                raise ValueError('Duplicate firmware profile: ' + profile)
            if not all(isinstance(variant.get(key), str) and variant[key] for key in ('label', 'chip', 'target', 'version')):
                raise ValueError('Incomplete firmware profile')
            target = variant['target']
            if not re.fullmatch(r'esp32[a-z0-9]*', target) or variant['chip'] != 'ESP32' + ('-' + target[5:].upper() if target[5:] else ''):
                raise ValueError('Chip and target mismatch')
            size = re.fullmatch(r'(\d+)MB', variant['flash_size'])
            if not size or int(size[1]) <= 0 or variant.get('flash_size_policy', 'exact') not in ('exact', 'minimum'):
                raise ValueError('Invalid flash size or policy')
            if not variant.get('parts'):
                raise ValueError('No firmware images')
            end, names = 0, set()
            for part in sorted(variant['parts'], key=lambda p: p['offset']):
                name = part['name']
                if not re.fullmatch(r'[A-Za-z0-9][A-Za-z0-9_.-]*\.bin', name) or name in names:
                    raise ValueError('Invalid firmware filename')
                names.add(name)
                image = path.parent / profile / name
                if not image.resolve().is_relative_to(source):
                    raise ValueError('Firmware path escapes input folder')
                offset, length = part['offset'], part['size']
                if type(offset) is not int or type(length) is not int or length <= 0 or offset < end or offset + length > int(size[1]) * 1024 * 1024:
                    raise ValueError('Overlapping or out-of-flash image')
                end = offset + length
                with image.open('rb') as f:
                    sha = hashlib.file_digest(f, 'sha256').hexdigest()
                with image.open('rb') as f:
                    md5 = hashlib.file_digest(f, 'md5').hexdigest()
                if image.stat().st_size != length or sha != part['sha256'] or md5 != part['md5']:
                    raise ValueError('Firmware integrity check failed: ' + str(image))
                files.append((image, Path(profile) / name))
            variants[profile] = variant
            for name in ('build-info.json', 'flash_args', 'flash_command.txt'):
                extra = path.parent / name
                if extra.exists():
                    if not extra.resolve().is_relative_to(source):
                        raise ValueError('Metadata escapes input folder')
                    metadata.append((extra, Path(profile) / name))
    if catalog is not None:
        expected = {p['id'] for p in json.loads(catalog.read_text())['profiles']}
        if set(variants) != expected:
            raise ValueError('Artifact set does not match the complete build catalog')
    versions = {v['version'] for v in variants.values()}
    combined = dict(schema_version=1, version=next(iter(versions)) if len(versions) == 1 else 'multiple releases', variants=variants)
    # Validate the whole set before producing anything; copy images without buffering them.
    output.mkdir(parents=True)
    for image, relative in files:
        dest = output / relative
        dest.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(image, dest)
    for extra, relative in metadata:
        text = extra.read_text()
        if extra.name == 'flash_command.txt':
            text = text.replace('@flash_args', '@' + str(relative.parent / 'flash_args'))
        (output / relative).write_text(text)
    (output / 'manifest.json').write_text(json.dumps(combined, indent=2) + '\n')
    return combined


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--input', type=Path, required=True, help='Folder of extracted matrix artifacts')
    parser.add_argument('--output', type=Path, required=True, help='New firmware folder to publish unchanged')
    parser.add_argument('--catalog', type=Path, help='Require every profile from this build catalog')
    args = parser.parse_args()
    collect(args.input, args.output, args.catalog)
    print(args.output)
