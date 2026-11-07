#!/usr/bin/env python3
"""Read and validate the source-controlled release identity; never publish."""
import argparse
import json
from pathlib import Path
import re


def load_release_identity(root=None):
    root = Path(root) if root is not None else Path(__file__).resolve().parents[1]
    value = json.loads((root/'release.json').read_text())
    required = {'schemaVersion','version','releaseVersion','name','channel','branch',
                'tag','npmVersion','pythonVersion','javaVersion'}
    if set(value) != required or value['schemaVersion'] != 1:
        raise ValueError('invalid release identity schema')
    base = value['version']
    number = r'(?:0|[1-9][0-9]*)'
    if not isinstance(base,str) or not re.fullmatch(fr'{number}\.{number}\.{number}',base):
        raise ValueError('invalid base version')
    release = value['releaseVersion']
    match = re.fullmatch(re.escape(base)+fr'-(alpha|beta|rc)\.({number})',release)
    prerelease = match is not None
    if release != base and not prerelease:
        raise ValueError('release version does not match base/candidate syntax')
    python = base
    if prerelease:
        label = {'alpha':'a','beta':'b','rc':'rc'}[match[1]]
        python += label+match[2]
    expected = {'tag':'v'+release,'npmVersion':release,'javaVersion':release,
                'pythonVersion':python,'channel':'prerelease' if prerelease else 'stable'}
    for key, expected_value in expected.items():
        if value[key] != expected_value:
            raise ValueError(f'{key} must be {expected_value}')
    if not all(isinstance(value[k],str) and value[k].strip() for k in ('name','branch')):
        raise ValueError('name and branch are required')
    return value


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--field')
    parser.add_argument('--root',type=Path)
    args = parser.parse_args()
    value = load_release_identity(args.root)
    if args.field:
        if args.field not in value:
            parser.error('unknown release field')
        print(value[args.field])
    else:
        print(json.dumps(value,indent=2))


if __name__ == '__main__':
    main()
