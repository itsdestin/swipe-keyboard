#!/usr/bin/env python3

# SPDX-FileCopyrightText: 2026 Destin Moss <https://youcoded.ai>
#
# SPDX-License-Identifier: MIT

"""Reproduce authored, NOT native-reviewed synthetic fixtures; no corpus/user input."""
import json
import sys

SEEDS = (
    ('en-US', 'qwerty', [('the','the'), ('color','color'), ('colour','colour'), ("can't",'cant'), ('walking','walking'), ('zzqnonword','zzqnonword')]),
    ('en-GB', 'qwerty', [('the','the'), ('colour','colour'), ('color','color'), ('organise','organise'), ('organize','organize'), ('zzqnonword','zzqnonword')]),
    ('es-ES', 'qwerty', [('casa','casa'), ('si','si'), ('sí','si'), ('niño','niño'), ('caminando','caminando'), ('zzqnonword','zzqnonword')]),
    ('fr-FR', 'azerty', [('école','ecole'), ('l’école','lecole'), ('cœur','cour'), ('français','francais'), ('mangeaient','mangeaient'), ('zzqnonword','zzqnonword')]),
    ('de-DE', 'qwertz', [('schön','schon'), ('schon','schon'), ('Straße','strase'), ('Häuser','hauser'), ('Krankenhaus','krankenhaus'), ('zzqnonword','zzqnonword')]),
)
cases = []
for locale, layout, words in SEEDS:
    rows = {'qwerty': ['qwertyuiop','asdfghjkl','zxcvbnm'],
            'azerty': ['azertyuiop','qsdfghjklm','wxcvbn'],
            'qwertz': ['qwertzuiop','asdfghjkl','yxcvbnm']}[layout]
    keys = [dict(letter=letter,x=i*100,y=r*100,w=100,h=100)
            for r,row in enumerate(rows) for i,letter in enumerate(row)]
    if locale.startswith('es'):
        keys.append(dict(letter='ñ',x=900,y=100,w=100,h=100))
    for i,(target,trace) in enumerate(words):
        cases.append(dict(id=f'{locale}-{i+1}',locale=locale,layout=layout,target=target,
                          provenance='synthetic-centered-v1',nativeReviewed=False,
                          traceKeys=trace,keys=keys))
json.dump(dict(schema=1,matchingPolicy=dict(display='NFC-case-sensitive',
              lookup='shared-lookup-lowercase-NFC-no-diacritic-folding',denominator='all-input-cases'),
              cases=cases),sys.stdout,ensure_ascii=False,indent=2)
print()
