#!/usr/bin/env python3
"""Select an npm distribution tag from a matching public GitHub release."""
import argparse
import json
import re
from pathlib import Path
from release_identity import load_release_identity


def dist_tag(tag, channel, published):
    number = r'(?:0|[1-9][0-9]*)'
    base = fr'v{number}\.{number}\.{number}'
    candidate = re.fullmatch(base + fr'-(alpha|beta|rc)\.{number}', tag) is not None
    if channel not in ('stable', 'prerelease') or not re.fullmatch(
            base + (fr'-(alpha|beta|rc)\.{number}' if channel == 'prerelease' else ''), tag):
        raise ValueError('release tag does not match publication channel')
    if (published.get('tag_name') != tag or published.get('draft') is not False
            or published.get('prerelease') is not candidate
            or not isinstance(published.get('published_at'), str)
            or not published['published_at'].strip()):
        raise ValueError('a matching public GitHub release is required before npm publication')
    return 'next' if candidate else 'latest'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--github-release', required=True, type=Path)
    parser.add_argument('--root', type=Path, required=True)
    args = parser.parse_args()
    identity = load_release_identity(args.root)
    print(dist_tag(identity['tag'], identity['channel'], json.loads(args.github_release.read_text())))


if __name__ == '__main__':
    main()
