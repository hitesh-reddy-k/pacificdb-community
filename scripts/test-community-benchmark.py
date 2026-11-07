#!/usr/bin/env python3
import importlib.util,unittest
from types import SimpleNamespace
from pathlib import Path
import numpy as np
spec=importlib.util.spec_from_file_location('bench',Path(__file__).with_name('benchmark-community-v12.py'))
b=importlib.util.module_from_spec(spec);spec.loader.exec_module(b)
report_spec=importlib.util.spec_from_file_location('report',Path(__file__).with_name('report-community-v12-benchmark.py'))
report=importlib.util.module_from_spec(report_spec);report_spec.loader.exec_module(report)
class Measurement(unittest.TestCase):
 def test_cache_reuse_report_is_weighted_and_requires_complete_metadata(self):
  rows=[{'execution':{'payload_fetches':10,'payload_cache_hits':1}},
        {'execution':{'payload_fetches':40,'payload_cache_hits':39}}]
  self.assertEqual(report.cache_reuse(rows),{'payload_fetches':50,'payload_cache_hits':40,'hit_fraction':.8})
  self.assertIsNone(report.cache_reuse([{'execution':{'executed_mode':'exact'}}]))
  self.assertIsNone(report.cache_reuse(rows+[{'execution':{}}]))
  with self.assertRaises(AssertionError):report.cache_reuse([{'execution':{'payload_fetches':10,'payload_cache_hits':11}}])
 def test_tail_sample_thresholds_and_serial_throughput(self):
  tiny=b.stats([1.,2.,3.]);self.assertIsNone(tiny['p95_ms']);self.assertIsNone(tiny['p99_ms']);self.assertEqual(tiny['serial_qps'],500.)
  self.assertIsNotNone(b.stats(list(range(1,101)))['p95_ms']);self.assertIsNone(b.stats(list(range(1,101)))['p99_ms'])
  self.assertIsNotNone(b.stats(list(range(1,1001)))['p99_ms'])
 def test_independent_simple_metric_ground_truth(self):
  base=np.array([[float(i),1.] for i in range(20)],np.float32);q=np.array([[1.,0.]],np.float32)
  for metric in ['cosine','l2','dot']:
   truth=b.reference(base,q,metric=metric)[0]
   if metric=='l2':self.assertEqual(truth['ids'][0],1)
   else:self.assertEqual(truth['ids'][0],19)
 def test_payload_score_cardinality_and_deletion_verification(self):
  base=np.array([[1.,float(i)] for i in range(20)],np.float32);q=np.array([1.,0.],np.float32)
  truth=b.reference(base,q[None,:])[0];ids=truth['ids']
  docs=[{'id':str(i),'vector':base[i].tolist(),'score':float(1/np.linalg.norm(base[i].astype(np.float64)))} for i in ids]
  self.assertEqual(b.validate(docs,q,base,truth)['strict_recall'],1.)
  bad=[dict(d) for d in docs];bad[0]['vector']=[999.,999.]
  with self.assertRaises(AssertionError):b.validate(bad,q,base,truth)
  with self.assertRaises(AssertionError):b.validate(docs[:-1],q,base,truth)
  with self.assertRaises(AssertionError):b.validate(docs,q,base,truth,deleted={ids[0]})
  bad=[dict(d) for d in docs];bad[0]['score']=0.
  with self.assertRaises(AssertionError):b.validate(bad,q,base,truth)
 def test_mutation_reference_uses_overwrites_and_tombstones(self):
  base=np.array([[1.,float(i)] for i in range(20)],np.float32);q=np.array([1.,0.],np.float32)
  result=b.mutable_reference(base,q,{19:np.array([100.,0.],np.float32)},{0})
  self.assertEqual(result['ids'][0],19);self.assertNotIn(0,result['ids'])
 def test_update_rejects_a_not_found_response(self):
  class Connection:
   def update_one(self,*args):return {'status':'not_found'}
  server=SimpleNamespace(kind='pacific',connection=Connection())
  with self.assertRaisesRegex(AssertionError,'not_found'):
   b.update(server,1,np.array([1.,0.],np.float32))
 def test_changed_record_verification_checks_values_and_deletions(self):
  class Connection:
   rows={'1':{'id':'1','vector':[1.,0.]}}
   def find_one(self,collection,filter):return self.rows.get(filter['id'])
  server=SimpleNamespace(kind='pacific',connection=Connection())
  expected={1:np.array([1.,0.],np.float32)}
  self.assertEqual(b.verify_mutations(server,expected,{2})['changed_records_verified'],1)
  server.connection.rows={'1':{'id':'1','vector':[9.,9.]}}
  with self.assertRaises(AssertionError):b.verify_mutations(server,expected,{2})
  server.connection.rows={'1':{'id':'1','vector':[1.,0.]},'2':{'id':'2','vector':[2.,0.]}}
  with self.assertRaises(AssertionError):b.verify_mutations(server,expected,{2})
if __name__=='__main__':unittest.main()
