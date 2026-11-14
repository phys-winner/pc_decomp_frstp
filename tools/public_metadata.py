"""Small APK metadata views; full build provenance remains in private research receipts."""
import hashlib
import json


def game_contract(contract):
    result = dict(schema_version=contract.get('schema_version',1),kind='frostpunk-game-data',complete=contract['complete'],
                package=contract['package'],original_exe_sha256=contract['original_exe_sha256'],
                files={name:{key:row[key] for key in ('bytes','sha256','kind','exact_size_required') if key in row}
                       for name,row in sorted(contract['files'].items())})
    for name in ('optional_resources','storage','texture_conversion'):
        if name in contract: result[name] = contract[name]
    return result


def runtime_contract(receipt, *, binary_sha256=None, binary_bytes=None):
    return dict(schema_version=1,kind='frostpunk-static-runtime',target=receipt['target'],
                original_exe_sha256=receipt['original_exe_sha256'],
                binary='libfrostpunk_runtime.so',binary_sha256=binary_sha256 or receipt['binary_sha256'],
                binary_bytes=receipt['binary_bytes'] if binary_bytes is None else binary_bytes,
                source_manifest_sha256=hashlib.sha256(json.dumps(receipt['sources'],sort_keys=True).encode()).hexdigest(),
                runtime_dependencies={name:{key:row[key] for key in ('sha256','bytes') if key in row}
                                      for name,row in sorted(receipt['runtime_dependencies'].items())})


def fsr3_contract(receipt):
    result={key:receipt[key] for key in ('source_revision','archive_url','archive_sha256','android_api','frame_generation','sha256')}
    result.update(kind='frostpunk-fsr3-runtime',output='libfrostpunk_fsr3.so',
                  source_manifest_sha256=hashlib.sha256(json.dumps(receipt['inputs'],sort_keys=True).encode()).hexdigest())
    return result
