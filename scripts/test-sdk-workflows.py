#!/usr/bin/env python3
"""Executable release-tag guards plus checks of publication workflow boundaries."""
import importlib.util
from pathlib import Path
import unittest

ROOT=Path(__file__).resolve().parents[1]
spec=importlib.util.spec_from_file_location('tag_guard',ROOT/'scripts/check-sdk-release-tag.py')
guard=importlib.util.module_from_spec(spec);spec.loader.exec_module(guard)

class Publication(unittest.TestCase):
    def test_version_tag_and_clean_same_revision_manifest(self):
        good={'version':'1.0.1','git_revision':'abc','source_clean':True,'status':'PASS'}
        guard.validate('v1.0.1','1.0.1',good,'abc')
        for tag in ['main','v1.0.2','v1.0.1; echo unsafe','v01.0.1','1.0.1']:
            with self.assertRaises(ValueError):guard.validate(tag,'1.0.1',good,'abc')
        for field,value in [('git_revision','other'),('source_clean',False),('status','FAIL'),('version','2.0.0')]:
            with self.assertRaises(ValueError):guard.validate('v1.0.1','1.0.1',{**good,field:value},'abc')
    def test_release_jobs_require_guard_tests_environment_and_exact_artifact(self):
        for name,environment in [('python-publish.yml','pypi'),('java-publish.yml','maven-central')]:
            text=(ROOT/'.github/workflows'/name).read_text()
            for token in ['workflow_dispatch:','release_tag:','./.github/workflows/sdk-packages.yml','needs: build','environment: '+environment,'actions/download-artifact@v4','check-sdk-release-tag.py','--require-tag','--manifest','tested-sdk-packages','verify-sdk-packages.py','--hash-manifest']:
                self.assertIn(token,text,name)
            for unsafe in ['pull_request_target','secrets: inherit','--no-check-certificate','--insecure','set -x','autoPublish=true','publishingType=AUTOMATIC']:
                self.assertNotIn(unsafe,text,name)
            top=text.split('jobs:')[0];self.assertNotIn('id-token: write',top)
        python=(ROOT/'.github/workflows/python-publish.yml').read_text()
        self.assertIn('id-token: write',python);self.assertIn('ed0c53931b1dc9bd32cbe73a98c7f6766f8a527e',python);self.assertNotIn('PYPI_API_TOKEN',python)
        java=(ROOT/'.github/workflows/java-publish.yml').read_text()
        self.assertIn('pack-java-sdk.py',java);self.assertIn('--sign',java);self.assertIn('upload-java-validation.py',java)
        uploader=(ROOT/'scripts/upload-java-validation.py').read_text();self.assertIn('USER_MANAGED',uploader);self.assertNotIn('/deployment/',uploader)
    def test_build_has_supported_runtimes_and_installed_smoke(self):
        text=(ROOT/'.github/workflows/sdk-packages.yml').read_text()
        for token in ['workflow_call:','windows-latest','ubuntu-22.04',"'3.10'","'3.12'","'11'","'17'","'21'",'test-python-installed-client.sh','test-java-installed-client.sh','test-cross-sdk-e2e.mjs','test-sdk-capability-matrix.py','test-sdk-packages.py','test-sdk-workflows.py','twine check','verify-sdk-packages.py','pack-java-sdk.py','actions/upload-artifact@v4','tested-sdk-packages']:
            self.assertIn(token,text)
        self.assertNotIn('id-token: write',text)
        self.assertNotIn('central.skipPublishing=false',text)

if __name__=='__main__':unittest.main()
