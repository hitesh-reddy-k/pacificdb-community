#!/usr/bin/env python3
"""Recreate the frozen 1M GloVe25 benchmark inputs; never overwrite a dataset.

The original preparation used NumPy 2.5.4, h5py 3.16.0 and threadpoolctl 3.7.0.
This is a portable transcription of the recorded preparation, not a new dataset.
Expected hashes are checked before an input can be called the published workload.
"""
import argparse
import hashlib
import json
import shutil
import time
from pathlib import Path

EXPECTED = {
    'glove-25-angular.hdf5': '51004cb0ae962159f0db507a51fec2b395de14b166f55976c89f16bd2f8b6391',
    'query-source-indices.npy': 'cb5f44845b7fab1cb12fb87581f7e4d54f8531ff485a2647e5efd54e716deb6d',
    'queries.npy': '87645a44b1078cdde409625a7679debda1c4cdf3b8c37a3a9cddc47d61d5e413',
    'base.npy': '97bc0b7684c7a49edb7403392e6daa100310374a382e6aa69d7d26236800c1f6',
    'truth.npz': '649702e2d359ec550596262170285443a57aaf22cffbc47078f69b410e8e82e9',
}


def digest(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def verify(data):
    hashes = {name: digest(data / name) for name in EXPECTED}
    mismatches = [name for name, value in hashes.items() if value != EXPECTED[name]]
    if mismatches:
        raise RuntimeError('Frozen workload checksum mismatch: ' + ', '.join(mismatches))
    return hashes


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, help='Downloaded, unmodified GloVe HDF5 file')
    parser.add_argument('--publication', type=Path, required=True, help='Parent of a new data/ directory')
    parser.add_argument('--verify-only', action='store_true', help='Check an existing dataset without writing')
    args = parser.parse_args()
    data = args.publication.resolve() / 'data'
    if args.verify_only:
        print(json.dumps({'status': 'PASS', 'hashes': verify(data)}, indent=2))
        return
    if args.source is None:
        parser.error('--source is required unless --verify-only is used')
    source = args.source.resolve(strict=True)
    if digest(source) != EXPECTED['glove-25-angular.hdf5']:
        raise RuntimeError('Source checksum differs from the recorded benchmark input')
    # A failed attempt remains available for diagnosis. Never replace its files.
    data.mkdir(parents=True, exist_ok=False)
    import h5py
    import numpy as np
    from threadpoolctl import threadpool_limits
    started = time.perf_counter()
    shutil.copyfile(source, data / source.name)
    if source.name != 'glove-25-angular.hdf5':
        (data / source.name).rename(data / 'glove-25-angular.hdf5')
    with h5py.File(source, 'r') as handle:
        base = handle['train'][:1_000_000].astype(np.float64)
        selected = np.random.default_rng(20261029).permutation(handle['test'].shape[0])[:1100]
        queries = handle['test'][:][selected].astype(np.float64)
        shapes = {key: list(handle[key].shape) for key in handle.keys()}
    if base.shape != (1_000_000, 25) or queries.shape != (1100, 25):
        raise RuntimeError('Unexpected source dimensions')
    if not np.isfinite(base).all() or not np.isfinite(queries).all():
        raise RuntimeError('Non-finite vectors')
    if (np.linalg.norm(base, axis=1) == 0).any() or (np.linalg.norm(queries, axis=1) == 0).any():
        raise RuntimeError('Zero vector in cosine dataset')
    base = (base / np.linalg.norm(base, axis=1, keepdims=True)).astype(np.float32)
    queries = (queries / np.linalg.norm(queries, axis=1, keepdims=True)).astype(np.float32)
    np.save(data / 'base.npy', base)
    np.save(data / 'queries.npy', queries)
    np.save(data / 'query-source-indices.npy', selected)
    base64 = base.astype(np.float64)
    base64 /= np.linalg.norm(base64, axis=1, keepdims=True)
    query64 = queries.astype(np.float64)
    query64 /= np.linalg.norm(query64, axis=1, keepdims=True)
    truth_ids, truth_scores = [], []
    with threadpool_limits(limits=4):
        for offset in range(0, len(queries), 25):
            scores = query64[offset:offset + 25] @ base64.T
            ids = np.argpartition(-scores, 10, axis=1)[:, :10]
            values = np.take_along_axis(scores, ids, axis=1)
            order = np.argsort(-values, axis=1)
            truth_ids.extend(np.take_along_axis(ids, order, axis=1))
            truth_scores.extend(np.take_along_axis(values, order, axis=1))
            print('truth', min(offset + 25, len(queries)), flush=True)
    np.savez(data / 'truth.npz', ids=np.array(truth_ids), scores=np.array(truth_scores))
    hashes = verify(data)
    manifest = {
        'dataset': 'ANN-Benchmarks GloVe-25 angular',
        'source_url': 'https://ann-benchmarks.com/glove-25-angular.hdf5',
        'source_shapes': shapes, 'base_count': len(base), 'dimensions': base.shape[1],
        'query_count': len(queries), 'tuning_queries': 100, 'measured_queries': 1000,
        'seed': 20261029, 'hashes': hashes,
        'preparation_seconds': time.perf_counter() - started,
    }
    (data / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    print(json.dumps({'status': 'PASS', **manifest}, indent=2))


if __name__ == '__main__':
    main()
