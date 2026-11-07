#!/usr/bin/env python3
"""Candidate identity and exact staged-consumer regression contracts."""
import importlib.util
import json
from pathlib import Path
import unittest
ROOT=Path(__file__).resolve().parents[1]
def module(name):
    spec=importlib.util.spec_from_file_location(name,ROOT/'scripts'/f'{name}.py')
    obj=importlib.util.module_from_spec(spec);spec.loader.exec_module(obj);return obj
class Candidate(unittest.TestCase):
    def test_candidate_and_stable_tag_mapping(self):
        guard=module('check-sdk-release-tag')
        guard.validate('v1.2.0-rc.2','1.2.0rc2',java_version='1.2.0-rc.2')
        guard.validate('v1.1.2','1.1.2',java_version='1.1.2')
        for tag,python,java in [('v1.2.0-rc.2','1.2.0','1.2.0-rc.2'),('v1.2.0','1.2.0rc2','1.2.0-rc.2'),('v1.2.0-rc.01','1.2.0rc2','1.2.0-rc.01')]:
            with self.assertRaises(ValueError):guard.validate(tag,python,java_version=java)
    def test_registry_workflows_are_explicit_and_prerelease_channel_is_next(self):
        for file in ['npm-publish.yml','python-publish.yml','java-publish.yml']:
            text=(ROOT/'.github/workflows'/file).read_text()
            self.assertIn('workflow_dispatch:',text)
            self.assertNotIn('  push:',text)
            self.assertIn('environment:',text)
        npm=(ROOT/'.github/workflows/npm-publish.yml').read_text()
        self.assertIn('next',npm);self.assertNotIn('--tag latest',npm)
    def test_staged_consumers_do_not_build_source_artifacts(self):
        java=(ROOT/'scripts/test-java-installed-client.sh').read_text()
        self.assertIn('install-file',java);self.assertNotIn('"$REPOSITORY/sdk/java/pom.xml"',java)
        cross=(ROOT/'scripts/test-cross-sdk-e2e.mjs').read_text()
        self.assertIn('PACIFICDB_TEST_NODE_MODULE',cross)
        self.assertIn('PACIFICDB_TEST_SKIP_JAVA_BUILD',cross)
        node=(ROOT/'scripts/test-node-installed-client.sh').read_text()
        self.assertIn('npm install --ignore-scripts',node)
        self.assertIn('node --test test/*.test.js',node)
    def test_publication_rejects_dirty_or_other_revision_candidate(self):
        guard=module('check-sdk-release-tag')
        manifest={'status':'PASS','version':'1.2.0-rc.2','git_revision':'abc','source_clean':True}
        guard.validate('v1.2.0-rc.2','1.2.0rc2',manifest,'abc','1.2.0-rc.2')
        for field,value in [('version','1.2.0'),('git_revision','other'),('source_clean',False),('status','FAIL')]:
            with self.assertRaises(ValueError):guard.validate('v1.2.0-rc.2','1.2.0rc2',{**manifest,field:value},'abc','1.2.0-rc.2')
    def test_runtime_candidate_metadata(self):
        identity=json.loads((ROOT/'release.json').read_text())
        self.assertEqual(json.loads((ROOT/'sdk/node/package.json').read_text())['version'],identity['npmVersion'])
        self.assertIn('version = "'+identity['pythonVersion']+'"',(ROOT/'sdk/python/pyproject.toml').read_text())
        self.assertIn('<version>'+identity['javaVersion']+'</version>',(ROOT/'sdk/java/pom.xml').read_text())
if __name__=='__main__':unittest.main()
