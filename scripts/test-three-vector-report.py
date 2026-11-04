#!/usr/bin/env python3
import importlib.util
from pathlib import Path
import tempfile
import unittest

class Report(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        path=Path(__file__).with_name('report-three-vector-benchmark.py')
        if not path.exists():raise AssertionError('Three-database report generator missing')
        spec=importlib.util.spec_from_file_location('report_three',path)
        cls.r=importlib.util.module_from_spec(spec);spec.loader.exec_module(cls.r)
    def sample(self):
        return {'status':'PASS','rows':1000000,'queries':1000,'rounds':3,'systems':{
          'pacific':{'status':'FAIL','load':{'rows_per_second':123},'ann':[{'p50_ms':20,'p95_ms':30,'p99_ms':40,'recall_at_10':.98},{'p50_ms':10,'p95_ms':20,'p99_ms':30,'recall_at_10':.99},{'p50_ms':30,'p95_ms':40,'p99_ms':50,'recall_at_10':.97}],
                     'lifecycle':{'mixed':{'read_successes':0,'read_attempts':360,'read_errors':[{'error':'lag'}]},'recovery':{}}},
          'postgres':{'status':'PASS'},'qdrant':{'status':'PASS'}}}
    def test_round_medians_not_pooled_or_best_round(self):
        result=self.r.metrics(self.sample()['systems']['pacific'])
        self.assertEqual(result['ANN p50 ms (median of round medians)'],20)
        self.assertEqual(result['ANN Recall@10 (mean across rounds)'],.98)
    def test_failed_availability_and_missing_cells_are_explicit(self):
        with tempfile.TemporaryDirectory() as d:
            result=self.r.render(self.sample(),Path(d))
            text=(Path(d)/'REPORT.txt').read_text()
            self.assertIn('FAIL',text);self.assertIn('0/360',text);self.assertIn('NOT MEASURED',text)
            self.assertFalse(result['all_systems_pass'])
    def test_derived_disk_cannot_be_hidden_or_inferred(self):
        self.assertIsNone(self.r.metrics({})['Derived graph checkpoint MiB'])
        result=self.r.metrics({'storage':{'bytes':2*1024**2},'derived_checkpoint_storage':{'bytes':1024**2},'total_storage_bytes':3*1024**2,
          'lifecycle':{'recovery':{'index_status':{'checkpoint_restored':False}}}})
        self.assertEqual(result['Derived graph checkpoint MiB'],1.)
        self.assertEqual(result['Total live + derived disk MiB'],3.)
        self.assertFalse(result['Recovery reused graph checkpoint'])
    def test_qdrant_recovery_never_silently_claims_zero_reconstruction(self):
        metrics=self.r.metrics({'lifecycle':{'recovery':{'ann_reconstruction_seconds':None,'ann_readiness_wait_seconds':1.5}}})
        self.assertIsNone(metrics['ANN reconstruction seconds'])
        self.assertEqual(metrics['Post-restart index readiness wait seconds'],1.5)
if __name__=='__main__':unittest.main()
