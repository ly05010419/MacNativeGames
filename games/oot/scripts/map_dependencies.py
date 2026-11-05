#!/usr/bin/env python3
"""Join the original NSO import/export inventories; never infer runtime usage."""
import argparse
import collections
import json
import subprocess
from pathlib import Path


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('directory', type=Path)
    ap.add_argument('--output', required=True, type=Path)
    args = ap.parse_args()
    modules = {name: json.loads((args.directory / f'{name}-inventory.json').read_text())
               for name in ['main', 'rtld', 'sdk', 'subsdk0']}
    definitions = collections.defaultdict(list)
    for module, inventory in modules.items():
        for symbol in inventory['exports']:
            definitions[symbol['name']].append({'module': module, 'offset': symbol['value'],
                                                 'binding': symbol['binding'], 'type': symbol['type']})
    output = {}
    for module, inventory in modules.items():
        imports = inventory['imports']
        names = [s['name'] for s in imports]
        process = subprocess.run(['/usr/bin/c++filt', '-n'], input='\n'.join(names) + '\n',
                                 text=True, capture_output=True, check=True)
        readable = process.stdout.splitlines()
        assert len(readable) == len(names)
        rows = []
        for symbol, demangled in zip(imports, readable):
            group = ('nn::' + demangled.split('::')[1]) if demangled.startswith('nn::') else (
                'nv/NVN' if demangled.startswith(('nv::', 'nvn')) else (
                    'glslc' if demangled.startswith('glslc') else 'C/C++ runtime or linker'))
            rows.append({**symbol, 'demangled': demangled, 'group': group,
                         'candidate_definitions': definitions.get(symbol['name'], [])})
        output[module] = {'needed': inventory['needed'], 'imports': rows,
                          'group_counts': dict(collections.Counter(r['group'] for r in rows)),
                          'resolved_by_name': sum(bool(r['candidate_definitions']) for r in rows),
                          'unresolved_by_name': [r for r in rows if not r['candidate_definitions']]}
    args.output.write_text(json.dumps({'modules': output,
                                      'limitations': ['Name matches are link candidates, not proof of ABI compatibility.',
                                                      'Imports and NPDM permissions do not prove runtime usage.',
                                                      'NVN obtains additional API entry points dynamically.']}, indent=2) + '\n')
    print(json.dumps({m: {'imports': len(d['imports']), 'resolved': d['resolved_by_name'],
                         'unresolved': len(d['unresolved_by_name']), 'groups': d['group_counts']}
                      for m, d in output.items()}, indent=2))


if __name__ == '__main__':
    main()
