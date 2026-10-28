"""Guard retained measurements against identity drift or candidate retries."""
import copy
import os
import unittest
if os.name!='nt':raise unittest.SkipTest('Windows measurement tooling')
from measure_render_ab import validate_deferred_completion

class CompletionContract(unittest.TestCase):
    def setUp(self):
        self.current=dict(package='P10',host='test-host',platform='Windows',scope='CPU',
            features={'video_driver':'dummy','rewind_depth':32},frames=10,
            identities={'baseline':{'sha256':'original'},'candidate':{'sha256':'candidate'}},
            dependency_sha256={'route_converter':'same'})
        work=dict(frames=10,cycles=100,pc=17,wram_sha256='ram',native_instructions=80,
            interpreted_instructions=0,fallback_instructions=0,presents=9,synthesized_audio_frames=25)
        self.previous=copy.deepcopy(self.current)
        self.previous.update(passed=False,deferred=['CPU headroom'],affinity_mask='0x303',
            runs=[dict(label=n,status='complete',headroom_warnings=[],metric=copy.deepcopy(work))
                  for n in ('warm-A','warm-B','A1','A2','A3','B1','B2','A4','A5')])
    def test_keeps_original_affinity_and_work(self):
        before=copy.deepcopy(self.previous)
        mask,work=validate_deferred_completion(self.previous,self.current)
        self.assertEqual(mask,0x303);self.assertEqual(work['cycles'],100)
        self.assertEqual(self.previous,before)
    def test_rejects_changed_host_features_binary_and_dependencies(self):
        for key in ('package','host','platform','scope','features','frames','identities','dependency_sha256'):
            with self.subTest(key=key):
                changed=copy.deepcopy(self.current);changed[key]='changed'
                with self.assertRaises(ValueError):validate_deferred_completion(self.previous,changed)
    def test_never_repeats_a_candidate_outcome(self):
        row=copy.deepcopy(self.previous['runs'][-1]);row['label']='B3'
        self.previous['runs'].append(row)
        with self.assertRaises(ValueError):validate_deferred_completion(self.previous,self.current)
    def test_rejects_failed_dirty_or_different_work_samples(self):
        for modification in ('failure','warning','work','not-deferred'):
            with self.subTest(modification=modification):
                previous=copy.deepcopy(self.previous)
                if modification=='failure':previous['runs'][-1]['status']='invalid_result'
                elif modification=='warning':previous['runs'][-1]['headroom_warnings']=['busy']
                elif modification=='work':previous['runs'][-1]['metric']['cycles']+=1
                else:previous['deferred']=[]
                with self.assertRaises(ValueError):validate_deferred_completion(previous,self.current)

if __name__=='__main__':unittest.main()
