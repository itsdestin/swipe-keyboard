"""Offline utilities against disposable roots only; no installed keyboard or session."""
import copy
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class OfflineLanguageCLI(unittest.TestCase):
    def test_queries_are_not_active_runtime_and_missing_packs_are_unavailable(self):
        missing = subprocess.run([os.environ['SWIPEKB_PROFILECHECK_EXE']], capture_output=True, text=True)
        self.assertEqual(missing.returncode, 2)
        with tempfile.TemporaryDirectory() as tmp:
            for locale, layout in [('en-US','qwerty'), ('es-ES','qwerty'), ('fr-FR','azerty'),
                                   ('de-DE','qwertz'), ('xx-XX','qwerty'), ('es-ES','unknown')]:
                p = subprocess.run([os.environ['SWIPEKB_PROFILECHECK_EXE'], '--model-root', tmp,
                                    '--locale',locale,'--layout',layout,'--mode','Latin'],
                                   capture_output=True, text=True)
                self.assertEqual(p.returncode, 1, p.stderr)
                value = json.loads(p.stdout)
                self.assertEqual(value['scope'], 'queried-installation-capability')
                self.assertEqual(value['engineReady'], 'unknown')
                self.assertEqual(value['activeFieldPolicy'], 'unknown')
                self.assertFalse(value['swipeCapable'])
                self.assertNotIn(tmp, p.stdout)
            self.assertEqual(list(Path(tmp).iterdir()), [])

    def evaluate(self, cases):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp)/'cases.json'
            path.write_text(json.dumps({'schema':1,'cases':cases}), encoding='utf-8')
            p = subprocess.run([os.environ['SWIPEKB_LANGUAGE_BENCH_EXE'], '--dataset', str(path),
                                '--model-root',tmp,'--packs-root',tmp], capture_output=True,text=True)
            self.assertEqual(p.returncode, 0, p.stderr)
            self.assertEqual(list(Path(tmp).iterdir()), [path])
            value = json.loads(p.stdout)
            import hashlib
            self.assertEqual(value['datasetSha256'], hashlib.sha256(path.read_bytes()).hexdigest())
            self.assertEqual(value['matchingPolicy']['display'], 'NFC-case-sensitive')
            self.assertEqual(value['matchingPolicy']['denominator'], 'all-input-cases')
            return value

    def seed(self):
        data = json.loads((ROOT/'bench/languages/synthetic-centered-v1.json').read_text())
        return copy.deepcopy(data['cases'][12])

    def test_malformed_and_wrong_layout_cases_remain_in_totals(self):
        valid = self.seed()
        wrong = copy.deepcopy(valid); wrong['id']='wrong'; wrong['layout']='azerty'
        nonfinite = copy.deepcopy(valid); nonfinite['id']='nonfinite'; nonfinite['keys'][0]['x']=None
        bad_key = copy.deepcopy(valid); bad_key['id']='size'; bad_key['keys'][0]['w']=0
        out = self.evaluate([valid, wrong, nonfinite, bad_key, {}])
        self.assertEqual(out['totals']['total'], 5)
        self.assertEqual(out['totals']['unavailable'], 1)
        self.assertEqual(out['totals']['invalid'], 4)
        self.assertEqual(out['totals']['evaluated'], 0)
        self.assertEqual(out['totals']['top1'], 0)
        self.assertEqual(len(out['cases']), 5)

    def test_explicit_human_points_accepted_without_recording_or_fabrication(self):
        case = self.seed(); case.pop('traceKeys')
        case.update(provenance='supplied-human',points=[{'x':50,'y':50,'t':0},{'x':150,'y':50,'t':20}])
        out = self.evaluate([case])
        self.assertEqual(out['totals']['geometryCovered'], 1)
        self.assertEqual(out['totals']['unavailable'], 1)
        self.assertEqual(out['cases'][0]['provenance'], 'supplied-human')
        case['points'][1]['t']=-1
        self.assertEqual(self.evaluate([case])['totals']['invalid'], 1)

    def test_synthetic_noisy_points_accepted_but_never_native_reviewed(self):
        case = self.seed(); case.pop('traceKeys')
        case.update(provenance='synthetic-noisy-v1',points=[{'x':50,'y':50,'t':0},{'x':150,'y':50,'t':20}])
        out = self.evaluate([case])
        self.assertEqual(out['totals']['geometryCovered'], 1)
        self.assertEqual(out['cases'][0]['provenance'], 'synthetic-noisy-v1')
        self.assertFalse(out['cases'][0]['nativeReviewed'])
        # WHY: a model-generated trace must not be able to claim a native speaker checked it.
        reviewed = copy.deepcopy(case); reviewed['nativeReviewed']=True
        self.assertEqual(self.evaluate([reviewed])['totals']['invalid'], 1)
        # and generated points still may not be mixed with traceKeys
        mixed = copy.deepcopy(case); mixed['traceKeys']='casa'
        self.assertEqual(self.evaluate([mixed])['totals']['invalid'], 1)

    def test_conflicting_matching_policy_is_rejected_not_used_to_inflate_accuracy(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp)/'cases.json'
            path.write_text(json.dumps({'schema':1,'cases':[self.seed()],
                                        'matchingPolicy':{'display':'strip-accents'}}))
            p = subprocess.run([os.environ['SWIPEKB_LANGUAGE_BENCH_EXE'], '--dataset',str(path),
                                '--model-root',tmp,'--packs-root',tmp],capture_output=True,text=True)
            self.assertEqual(p.returncode, 2)
            self.assertEqual(p.stdout, '')

    def test_candidate_inspection_is_bounded(self):
        for count in ('0','101','-1','not-a-number'):
            p = subprocess.run([os.environ['SWIPEKB_LANGUAGE_BENCH_EXE'], '--candidates', count],
                               capture_output=True, text=True)
            self.assertEqual(p.returncode, 2)
            self.assertEqual(p.stdout, '')

    def test_corrupt_manifest_fails_closed_without_paths_or_text(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp)/'fr.json'; path.write_text('{invalid /private/text')
            before = path.read_bytes()
            p = subprocess.run([os.environ['SWIPEKB_PROFILECHECK_EXE'], '--model-root',tmp,
                                '--packs-root',tmp,'--locale','fr-FR','--layout','azerty','--mode','Latin'],
                               capture_output=True,text=True)
            self.assertEqual(p.returncode, 1)
            self.assertEqual(json.loads(p.stdout)['unavailableReason'], 'Language pack manifest missing or invalid')
            self.assertNotIn('private', p.stdout)
            self.assertEqual(path.read_bytes(), before)


if __name__ == '__main__':
    unittest.main()
