#!/usr/bin/env python3
"""A registry tag must agree with the exact published GitHub release identity."""
import importlib.util
from pathlib import Path
import unittest

spec = importlib.util.spec_from_file_location('npm_policy', Path(__file__).with_name('npm_publication_policy.py'))
policy = importlib.util.module_from_spec(spec)
spec.loader.exec_module(policy)


class PublicationPolicy(unittest.TestCase):
    def release(self, *, candidate=False):
        return dict(tag_name='v1.2.0-rc.2' if candidate else 'v1.2.0', draft=False,
                    prerelease=candidate, published_at='2026-11-07T00:00:00Z')

    def test_only_public_stable_release_can_use_latest(self):
        self.assertEqual(policy.dist_tag('v1.2.0', 'stable', self.release()), 'latest')

    def test_prerelease_uses_next(self):
        self.assertEqual(policy.dist_tag('v1.2.0-rc.2', 'prerelease', self.release(candidate=True)), 'next')

    def test_drafts_unpublished_other_tags_and_channel_mismatch_rejected(self):
        for changes in ({'draft': True}, {'draft': None}, {'published_at': None},
                        {'prerelease': True}, {'tag_name':'v1.1.2'}, {'prerelease': None}):
            with self.subTest(changes=changes), self.assertRaises(ValueError):
                policy.dist_tag('v1.2.0', 'stable', {**self.release(), **changes})
        for tag, channel in [('main','stable'), ('v1.2.0-rc.2','stable'),
                             ('v1.2.0','prerelease'), ('v1.2.0','unknown')]:
            with self.subTest(tag=tag,channel=channel), self.assertRaises(ValueError):
                policy.dist_tag(tag, channel, {**self.release(), 'tag_name':tag})

    def test_candidate_cannot_promote_to_latest(self):
        with self.assertRaises(ValueError):
            policy.dist_tag('v1.2.0-rc.2', 'prerelease', {**self.release(candidate=True), 'prerelease':False})


if __name__ == '__main__':
    unittest.main()
