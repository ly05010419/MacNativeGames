#!/usr/bin/env python3
import hashlib,json,subprocess
from pathlib import Path
root=Path(__file__).resolve().parents[1];exe=root/'build/totk-native'
positive=subprocess.run([str(exe),'rewritten/v0','probe-gfx'],cwd=root,capture_output=True,text=True,check=True)
report=json.loads(positive.stdout);assert report['texture_object_bytes']==128
negative=subprocess.run([str(exe),'rewritten/v0','probe-gfx-negative'],cwd=root,capture_output=True,text=True)
assert negative.returncode==2 and 'unimplemented NVN call nvnCommandBufferDrawTexture' in negative.stderr
pins=subprocess.run([str(exe),'rewritten/v0','probe-gfx'],cwd=root,capture_output=True,text=True,env={**__import__('os').environ,'TOTK_PROBE_NEGATIVE':'21'})
assert pins.returncode!=0 and 'copies a live recording refers to were freed' in pins.stderr, pins.stderr[-400:]
marks=subprocess.run([str(exe),'rewritten/v0','probe-gfx'],cwd=root,capture_output=True,text=True,env={**__import__('os').environ,'TOTK_PROBE_NEGATIVE':'22'})
assert marks.returncode!=0 and 'control memory was reused was not released' in marks.stderr, marks.stderr[-400:]
prediction={'24':'pipeline prediction selected a binding variant for the active draw','25':'pipeline prediction recycled an in-flight or recent pending slot','26':'pipeline prediction predicted a program with an untranslated stage','27':'pipeline prediction guessed a pass without depth for a depth-exporting fragment function','28':'pipeline prediction took a variant of another binding state','29':"pipeline prediction changed a program's variants",'30':'pipeline prediction created a program','31':'the draw did not wait for the library compiling for prediction'}
for number,message in prediction.items():
 run=subprocess.run([str(exe),'rewritten/v0','probe-gfx'],cwd=root,capture_output=True,text=True,env={**__import__('os').environ,'TOTK_PROBE_NEGATIVE':number})
 assert run.returncode!=0 and message in run.stderr, (number, run.stderr[-400:])
report['prediction_negatives']='active variant taken, in-flight slots freed, untranslated stages predicted, depth-less guesses for depth export, unseen variant looked up without the bound state, recorded in the program, program created, library compiling for prediction not waited for: each caught'
report['texture_copy_negative']='render targets of recordings not marked: caught';report['recording_copy_negative']='control-memory marks never checked: caught';report['unsupported_call']='stops with exit 2';report['executable_sha256']=hashlib.sha256(exe.read_bytes()).hexdigest()
(root/'analysis/native-gfx-probe.json').write_text(json.dumps(report,indent=2)+'\n');print(json.dumps(report))
