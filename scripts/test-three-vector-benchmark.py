#!/usr/bin/env python3
import importlib.util
from pathlib import Path
import unittest
import tempfile
from types import SimpleNamespace
import numpy as np

path=Path(__file__).with_name('benchmark-three-vector-databases.py')

class Contract(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not path.exists():
            raise AssertionError('Three-database validation/adapter is not implemented')
        spec=importlib.util.spec_from_file_location('three_bench',path)
        cls.b=importlib.util.module_from_spec(spec);spec.loader.exec_module(cls.b)

    def fixture(self, normalized=False):
        base=np.array([[2.,float(i)] for i in range(20)],np.float32)
        q=np.array([1.,0.],np.float32)
        truth={'ids':list(range(10)),'cutoff':2/np.linalg.norm(base[9].astype(float))}
        docs=[]
        for i in range(10):
            vector=base[i].astype(float)
            docs.append({'id':str(i),'vector':(vector/np.linalg.norm(vector) if normalized else vector).tolist(),
                         'score':float(vector[0]/np.linalg.norm(vector)),**self.b.payload(i)})
        return docs,q,base,truth

    def test_qdrant_normalized_cosine_is_verified_without_original_vector_claim(self):
        docs,q,base,truth=self.fixture(True)
        self.assertEqual(self.b.validate(docs,q,base,truth,'qdrant')['strict_recall'],1.)
        with self.assertRaises(AssertionError):self.b.validate(docs,q,base,truth,'pacific')

    def test_wrong_payload_vector_score_cardinality_or_tombstone_fails(self):
        import copy
        for key,value in [('title','wrong'),('vector',[99.,99.]),('score',-.5),('group_id',99)]:
            docs,q,base,truth=self.fixture(True);docs[0][key]=value
            with self.subTest(key=key),self.assertRaises(AssertionError):
                self.b.validate(docs,q,base,truth,'qdrant')
        docs,q,base,truth=self.fixture(True)
        for rows in [docs[:-1],docs[:-1]+[copy.deepcopy(docs[0])]]:
            with self.assertRaises(AssertionError):self.b.validate(rows,q,base,truth,'qdrant')
        with self.assertRaises(AssertionError):self.b.validate(docs,q,base,truth,'qdrant',deleted={0})

    def test_unready_or_failed_graph_cannot_be_ann_ready(self):
        self.assertTrue(self.b.qdrant_ready({'status':'green','optimizer_status':'ok','points_count':1000,'indexed_vectors_count':1000},1000))
        # Collection counters include tombstones during maintenance; exact live
        # count is checked separately with Qdrant's exact count API.
        self.assertTrue(self.b.qdrant_ready({'status':'green','optimizer_status':'ok','points_count':995,'indexed_vectors_count':1000},995))
        for field,value in [('status','yellow'),('optimizer_status',{'error':'broken'}),('indexed_vectors_count',0),('points_count',900)]:
            info={'status':'green','optimizer_status':'ok','points_count':1000,'indexed_vectors_count':1000};info[field]=value
            self.assertFalse(self.b.qdrant_ready(info,1000))

    def test_mixed_errors_or_incomplete_workload_never_pass(self):
        self.assertEqual(self.b.mixed_status(360,[],40),'PASS')
        for counts in [(359,[],40),(360,[{'error':'lag'}],40),(360,[],39)]:
            self.assertEqual(self.b.mixed_status(*counts),'FAIL')

    def test_qdrant_config_uses_a_supported_yaml_filename(self):
        self.assertEqual(self.b.qdrant_config_path(Path('/owned/root')).suffix,'.yaml')

    def test_qdrant_index_parameters_respect_server_validation(self):
        self.assertGreaterEqual(self.b.qdrant_hnsw_config()['full_scan_threshold'],10)
        self.assertEqual(self.b.qdrant_hnsw_config()['m'],16)
        self.assertEqual(self.b.qdrant_hnsw_config()['ef_construct'],200)

    def test_concurrent_recall_below_target_fails(self):
        with self.assertRaises(AssertionError):self.b.qualified_recall([{'tie_aware_recall':0.}],.95)
        self.assertEqual(self.b.qualified_recall([{'tie_aware_recall':1.}],.95),1.)

    def test_attempted_query_and_bad_response_are_retained(self):
        docs,q,base,truth=self.fixture()
        docs[0]['title']='wrong'
        with tempfile.TemporaryDirectory() as directory:
            server=SimpleNamespace(kind='pacific',out=Path(directory),
              query=lambda *a,**kw:(docs,1.,{}),plan=lambda *a:None,connection=None)
            queries=np.repeat(q[None,:],110,axis=0)
            with self.assertRaises(AssertionError):
                self.b.measured(server,base,queries,[truth]*110,'exact',100,10,1,0)
            import json
            rows=[json.loads(line) for line in (Path(directory)/'exact-attempts.jsonl').read_text().splitlines()]
            self.assertEqual(rows[0]['query'],100)
            self.assertEqual(rows[0]['status'],'FAIL')
            self.assertEqual(rows[0]['response'][0]['title'],'wrong')

if __name__=='__main__':unittest.main()
