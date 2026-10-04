#!/usr/bin/env python3
"""Validate every actual Community dispatch, named SDK method and wire alias."""
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'sdk/python'))


def dispatched_actions(source):
    # Main request chain has this indentation; ignore permission lists and nested tests.
    conditions = re.findall(r'^        else if \(action == (.*?)\) \{', source, re.M | re.S)
    return {action for condition in conditions for action in re.findall(r'action == "([^"]+)"', 'action == ' + condition)}


def validate(matrix, implementations=True):
    rows = matrix['actions']
    names = [row['action'] for row in rows]
    if len(names) != len(set(names)): raise ValueError('Duplicate capability action')
    dispatched = dispatched_actions(
        (ROOT / 'engine/src/server.cpp').read_text(encoding='utf-8')
    )
    if set(names) != dispatched:
        raise ValueError(f'Dispatch mismatch: missing {sorted(dispatched-set(names))}; extra {sorted(set(names)-dispatched)}')
    by_action = {row['action']: row for row in rows}
    for row in rows:
        if row['classification'] not in ('beginner','advanced','alias','internal'): raise ValueError('Invalid classification')
        if row['scope'] not in ('principal','database','collection','explicit_database','global'): raise ValueError('Invalid scope')
        if row['classification'] == 'internal':
            if not row.get('reason'): raise ValueError('Internal action lacks source reason')
            continue
        if not row.get('python') or not row.get('java'): raise ValueError('Missing named API')
        if row['classification'] == 'alias':
            canonical = by_action.get(row.get('alias_of'))
            if not canonical or canonical['classification'] in ('alias','internal'): raise ValueError('Invalid wire alias')
            if any(row[key] != canonical[key] for key in ('python','java','scope','required_fields')): raise ValueError('Alias API mismatch')
        if not isinstance(row['required_fields'],list) or not set(row['required_fields']) <= row['sample'].keys(): raise ValueError('Missing field sample')
    if implementations:
        from pacificdb import PacificDB
        with PacificDB(database='app') as db:
            for row in rows:
                if row['classification'] == 'internal': continue
                obj = db
                for part in row['python'].split('.'): obj = getattr(obj,part)
                if not callable(obj): raise ValueError('Python API is not callable')
        java = (
            ROOT / 'sdk/java/src/main/java/io/pacificdb/PacificDBClient.java'
        ).read_text(encoding='utf-8') + (
            ROOT / 'sdk/java/src/main/java/io/pacificdb/Operations.java'
        ).read_text(encoding='utf-8')
        for row in rows:
            if row['classification'] != 'internal' and not re.search(r'\b'+re.escape(row['java'].split('.')[-1])+r'\s*\(',java): raise ValueError('Missing Java API')
    return len(rows)


if __name__ == '__main__':
    matrix = json.loads(
        (ROOT / 'sdk/contracts/community-capabilities.json').read_text(
            encoding='utf-8'
        )
    )
    print(f'PASS: {validate(matrix)} Community dispatches have checked named SDK bindings')
