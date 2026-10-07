#!/usr/bin/env python3
"""Negative artifact checks; never downloads or publishes a package."""
import importlib.util
import tempfile
import unittest
import zipfile
from pathlib import Path

spec=importlib.util.spec_from_file_location('verify',Path(__file__).with_name('verify-sdk-packages.py'))
verify=importlib.util.module_from_spec(spec);spec.loader.exec_module(verify)

class Artifacts(unittest.TestCase):
    def wheel(self,root,omit=None,metadata=None):
        entries={f'pacificdb/{module}.py':b'# module\n' for module in verify.PYTHON_MODULES}
        entries['pacificdb/__init__.py']=b'PacificDB = PacificDBClient\nfrom .errors import PacificDBError, MediaUploadError\n'
        entries['pacificdb-1.1.2.dist-info/METADATA']=(metadata or 'Metadata-Version: 2.4\nName: pacificdb\nVersion: 1.1.2\nRequires-Python: >=3.10\nLicense-Expression: Apache-2.0\nLicense-File: LICENSE\nDescription-Content-Type: text/markdown\nProject-URL: Source, https://github.com/hitesh-reddy-k/pacificdb-community\nAuthor: PacificDB contributors\n\n# PacificDB\nDatabase-first SDK\n').encode()
        entries['pacificdb-1.1.2.dist-info/licenses/LICENSE']=b'Apache License\nVersion 2.0\n'
        entries['pacificdb-1.1.2.dist-info/WHEEL']=b'Wheel-Version: 1.0\nRoot-Is-Purelib: true\nTag: py3-none-any\n'
        if omit:entries.pop(omit)
        path=root/'pacificdb-1.1.2-py3-none-any.whl'
        with zipfile.ZipFile(path,'w') as archive:
            import csv,io,hashlib,base64
            rows=[]
            for name,data in entries.items():
                archive.writestr(name,data);rows.append([name,'sha256='+base64.urlsafe_b64encode(hashlib.sha256(data).digest()).decode().rstrip('='),len(data)])
            record='pacificdb-1.1.2.dist-info/RECORD';rows.append([record,'','']);out=io.StringIO();csv.writer(out).writerows(rows);archive.writestr(record,out.getvalue())
        return path
    def test_good_wheel_and_missing_modules_license_readme(self):
        with tempfile.TemporaryDirectory() as d:
            root=Path(d);verify.python_wheel(self.wheel(root),'1.1.2')
            for omitted in ['pacificdb/files.py','pacificdb-1.1.2.dist-info/licenses/LICENSE']:
                with self.assertRaises(ValueError):verify.python_wheel(self.wheel(root,omit=omitted),'1.1.2')
            for replacement in ['Version: 2.0.0','Requires-Python: >=3.12','Requires-Dist: requests','License-Expression: MIT']:
                field=replacement.split(':')[0]
                import re
                metadata='Metadata-Version: 2.4\nName: pacificdb\nVersion: 1.1.2\nRequires-Python: >=3.10\nLicense-Expression: Apache-2.0\nDescription-Content-Type: text/markdown\nProject-URL: Source, https://github.com/hitesh-reddy-k/pacificdb-community\nAuthor: PacificDB contributors\n\n# SDK\n'
                metadata=re.sub('^'+field+':.*$',replacement,metadata,flags=re.M) if field in metadata else replacement+'\n'+metadata
                with self.assertRaises(ValueError):verify.python_wheel(self.wheel(root,metadata=metadata),'1.1.2')
            with self.assertRaises(ValueError):verify.python_wheel(self.wheel(root,metadata='Name: pacificdb\nVersion: 1.1.2\n'),'1.1.2')
    def test_tampering_and_secret_build_files_fail(self):
        with tempfile.TemporaryDirectory() as d:
            root=Path(d)
            for bad in ['pacificdb/.env','pacificdb/test.key','pacificdb/__pycache__/client.pyc','pacificdb/client.py']:
                path=self.wheel(root)
                with zipfile.ZipFile(path,'a') as archive:archive.writestr(bad,b'should not ship')
                with self.assertRaises(ValueError):verify.python_wheel(path,'1.1.2')
    def test_sdist_needs_license_readme_and_modules(self):
        import tarfile,io
        with tempfile.TemporaryDirectory() as d:
            path=Path(d)/'pacificdb-1.1.2.tar.gz'
            for omit in [None,'LICENSE','README.md','pacificdb/files.py']:
                with tarfile.open(path,'w:gz') as archive:
                    for name in ['LICENSE','README.md','pyproject.toml',*[f'pacificdb/{m}.py' for m in verify.PYTHON_MODULES]]:
                        if name==omit:continue
                        data=b'valid\n';info=tarfile.TarInfo('pacificdb-1.1.2/'+name);info.size=len(data);archive.addfile(info,io.BytesIO(data))
                if omit:
                    with self.assertRaises(ValueError):verify.python_sdist(path,'1.1.2')
                else:verify.python_sdist(path,'1.1.2')

class JavaArtifacts(unittest.TestCase):
    def test_pom_metadata_floor_runtime_dependency_and_no_publish_defaults(self):
        import xml.etree.ElementTree as ET
        root=Path(__file__).resolve().parents[1]
        verify.java_pom(root/'sdk/java/pom.xml','1.1.2')
        tree=ET.parse(root/'sdk/java/pom.xml');ns={'m':'http://maven.apache.org/POM/4.0.0'}
        for field in ['description','url','developers','scm']:
            good=ET.fromstring(ET.tostring(tree.getroot()));good.remove(good.find('m:'+field,ns))
            with tempfile.TemporaryDirectory() as d:
                file=Path(d)/'pom.xml';ET.ElementTree(good).write(file)
                with self.assertRaises(ValueError):verify.java_pom(file,'1.1.2')
    def test_java_bytecode_docs_sources_and_signatures_required(self):
        with tempfile.TemporaryDirectory() as d:
            root=Path(d)
            for missing in [None,'license','class','source','doc','bytecode']:
                binary=root/'client.jar';sources=root/'sources.jar';docs=root/'javadoc.jar'
                with zipfile.ZipFile(binary,'w') as archive:
                    if missing!='license':archive.writestr('META-INF/LICENSE','Apache License')
                    for cls in verify.JAVA_CLASSES:
                        if missing=='class' and cls=='PacificDB':continue
                        archive.writestr('io/pacificdb/'+cls+'.class',b'\xca\xfe\xba\xbe\x00\x00'+(61 if missing=='bytecode' else 55).to_bytes(2,'big'))
                with zipfile.ZipFile(sources,'w') as archive:
                    for cls in verify.JAVA_CLASSES:
                        if missing=='source' and cls=='PacificDB':continue
                        archive.writestr('io/pacificdb/'+cls+'.java','package io.pacificdb;')
                with zipfile.ZipFile(docs,'w') as archive:
                    for cls in verify.JAVA_CLASSES:
                        if missing=='doc' and cls=='PacificDB':continue
                        archive.writestr('io/pacificdb/'+cls+'.html','public API')
                if missing:
                    with self.assertRaises(ValueError):verify.java_jars(binary,sources,docs)
                else:verify.java_jars(binary,sources,docs)
            with self.assertRaises(ValueError):verify.java_artifacts(root,'1.1.2')

if __name__=='__main__':unittest.main()
