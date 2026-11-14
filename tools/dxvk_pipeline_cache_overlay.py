"""Optional Android/Mali native pipeline cache; apply LAST to a full GPU tree.

The fingerprint hashes actual composed GPU inputs, before this overlay, plus
the pinned upstream revision and this cache implementation. CPU/runtime/APK
inputs are deliberately absent. Never applies to reference/dxvk in place.
"""
import hashlib
import json
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[1]
REVISION = '0cf05780abd7250c2cd713b7749cf32180157cf5'
INPUT_SHA256 = {
    'src/dxvk/dxvk_device.h': 'e11f1da7a71c232c0c88317e8fe7ab858a26d1d68f65091e36548836e54d0b70',
    'src/dxvk/dxvk_device.cpp': '4b49077b4d12d6c33678c6206195ea6e52e9f9f73e6ad127aa2502bac90381c9',
    'src/dxvk/meson.build': '906a913522f2c4228194c3b45c594b15dbcb92c0af68bde88aa86c75ccf64c65',
    'src/dxvk/dxvk_graphics.cpp': '7d23368eddd53c8cdf4c86bc7b5b90198898eefbcecc24abd83e913ba5423b89',
    'src/dxvk/dxvk_compute.cpp': '257baf583b3bc89ed19604f0f8160b0507e05654c312c3f926ec46773231a8ca',
    'src/dxvk/dxvk_shader.cpp': 'efee138e982d62b1a172f08faf585f4b3ce4ac3414e2354d92a8a4b779ec27f6',
    'src/dxvk/dxvk_meta_blit.h': 'd7ffdfddbe1b375b0edd15e8955d7202e155e15e2a76c6d6641a595ff3c8b101',
    'src/dxvk/dxvk_meta_blit.cpp': '8abe9dbc2dfafed181b9e90481e15ee5c773c399b9646f287c964bc3bb622dd2',
    'src/dxvk/dxvk_meta_clear.h': 'b92fc73739985bbb48879313d8d3554efcfb9744f404a76c027e39575ad22b27',
    'src/dxvk/dxvk_meta_clear.cpp': '183a43201438904783d29795bead7361bd2f90900c38b91c1983cc43ec4cc4f1',
    'src/dxvk/dxvk_meta_copy.h': '5f029b979a1b53fa0e9c0253337edc81d9a4bc36f6a7b9f598213fe8c949059d',
    'src/dxvk/dxvk_meta_copy.cpp': '723020e6399810faeffd58d1fca374f199daac2e3ae6112e2d4437d8aba7f30b',
    'src/dxvk/dxvk_meta_pack.h': 'a8693152ac1d17a576887a4b57c9dca28f05627d95d7850a6be037245463e357',
    'src/dxvk/dxvk_meta_pack.cpp': '9ecf63510106570482ed93d952f02b47b91b23166d93ad4dbbe3a9efb51216e5',
    'src/dxvk/dxvk_meta_resolve.h': '2af70bd6c519a879c6ff84153b4f86a2a076c9d7e80d65f029545c877bc9d00b',
    'src/dxvk/dxvk_meta_resolve.cpp': 'f1ce8743e4c87751977e402e39b399931e466d01f63c070e4d1105370b59cd58',
}
NEW_FILES = ('src/dxvk/dxvk_pipeline_cache.h', 'src/dxvk/dxvk_pipeline_cache.cpp',
             'src/dxvk/dxvk_pipeline_cache_identity.h')
FILES = (*INPUT_SHA256, *NEW_FILES)


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def fingerprint(source, overrides=None):
    """Hash a complete, already-composed GPU source tree, never build outputs."""
    source = Path(source)
    inputs = {}
    overrides = overrides or {}
    for directory in ('src', 'include'):
        base = source / directory
        if not base.is_dir():
            raise ValueError('Native cache fingerprint requires the full composed GPU tree')
        for path in sorted(base.rglob('*')):
            if not path.is_file() or '.git' in path.parts:
                continue
            name = path.relative_to(source).as_posix()
            if name in NEW_FILES:
                continue
            inputs[name] = sha(path)
    for name, raw in overrides.items():
        if name not in NEW_FILES and (name.startswith('src/') or name.startswith('include/')):
            inputs[name] = hashlib.sha256(raw).hexdigest()
    if not all(name in inputs for name in INPUT_SHA256 if name != 'src/dxvk/meson.build'):
        raise ValueError('Incomplete native cache GPU source map')
    for name in ('meson.build', 'meson_options.txt'):
        inputs[name] = hashlib.sha256(overrides[name]).hexdigest() if name in overrides else sha(source / name)
    implementation = {str(path.relative_to(ROOT)).replace('\\', '/'): sha(path)
                      for path in (Path(__file__), *(ROOT / 'tools/dxvk_pipeline_cache' / name
                                                    for name in ('dxvk_pipeline_cache.h', 'dxvk_pipeline_cache.cpp')))}
    identity = hashlib.sha256(json.dumps({'revision': REVISION, 'gpu_sources': inputs,
                                         'cache_implementation': implementation},
                                        sort_keys=True, separators=(',', ':')).encode()).hexdigest()
    return identity, inputs, implementation


def verify_preimages(source):
    text = {}
    for name, expected in INPUT_SHA256.items():
        raw = (Path(source) / name).read_bytes()
        checked = raw
        if name == 'src/dxvk/meson.build':
            # Remove only the declared FSR additions, then verify every remaining upstream byte.
            checked = checked.replace(b'\r\n', b'\n')
            for addition in (b"  'shaders/dxvk_present_frag_fsr.frag',\n",b"  'shaders/dxvk_fsr3_prepare.comp',\n",b"  'dxvk_fsr3.cpp',\n"):
                checked = checked.replace(addition,b'',1)
            checked = checked.replace(b'\n', b'\r\n')
        if hashlib.sha256(checked).hexdigest() != expected:
            raise ValueError('Native cache exact preimage drift: ' + name)
        text[name] = raw.decode('utf-8').replace('\r\n', '\n')
    return text


def apply(work, source=None, *, gpu_source=None):
    work, source = Path(work).resolve(), Path(source or work).resolve()
    if work == (ROOT / 'reference/dxvk').resolve():
        raise ValueError('Refuse to modify sealed DXVK reference')
    text = verify_preimages(source)
    gpu_source = Path(gpu_source).resolve() if gpu_source is not None else source
    # Existing builders compose a small candidate containing only changed
    # files. Merge those real pre-cache contents over the full already-copied
    # GPU tree without copying it or changing final source mtimes twice.
    overrides = {}
    if source != gpu_source:
        for path in source.rglob('*'):
            if path.is_file() and '.git' not in path.parts:
                name = path.relative_to(source).as_posix()
                if name not in NEW_FILES:
                    overrides[name] = path.read_bytes()
    identity, inputs, implementation = fingerprint(gpu_source, overrides)

    def replace(name, before, after, count=1):
        if text[name].count(before) != count:
            raise ValueError('Native cache exact anchor drift: ' + name)
        text[name] = text[name].replace(before, after)

    name = 'src/dxvk/dxvk_device.h'
    replace(name, '#include "dxvk_pipemanager.h"', '#include "dxvk_pipemanager.h"\n#include "dxvk_pipeline_cache.h"')
    replace(name, '    Rc<vk::DeviceFn> vkd() const {',
            '    DxvkPipelineCache& pipelineCache() const { return m_pipelineCache; }\n\n    Rc<vk::DeviceFn> vkd() const {')
    replace(name, '    DxvkObjects                 m_objects;',
            '    mutable DxvkPipelineCache    m_pipelineCache;\n    DxvkObjects                 m_objects;')
    name = 'src/dxvk/dxvk_device.cpp'
    replace(name, '#include "dxvk_instance.h"', '#include "dxvk_instance.h"\n#include "../util/util_env.h"')
    replace(name, '    m_objects           (this),', '''    m_pipelineCache     (vkd->device(), vkd->vkCreatePipelineCache, vkd->vkDestroyPipelineCache,
                         vkd->vkGetPipelineCacheData, vkd->vkCreateGraphicsPipelines,
                         vkd->vkCreateComputePipelines, m_properties.core.properties,
                         env::getEnvVar("DXVK_STATE_CACHE_PATH"), true),
    m_objects           (this),''')
    replace('src/dxvk/meson.build', "  'dxvk_pipemanager.cpp',", "  'dxvk_pipemanager.cpp',\n  'dxvk_pipeline_cache.cpp',")

    core_counts = {'graphics': 4, 'compute': 1, 'shader': 3}
    routes = []
    for stem, count in core_counts.items():
        name = 'src/dxvk/dxvk_' + stem + '.cpp'
        pattern = r'vk->vkCreate(Graphics|Compute)Pipelines\(vk->device\(\),\s*VK_NULL_HANDLE,\s*'
        text[name], found = re.subn(pattern, lambda m: 'm_device->pipelineCache().create' + m[1] + 'Pipelines(', text[name])
        if found != count:
            raise ValueError('Native cache core call count drift: ' + name)
        routes.append({'path': name, 'calls': found})
    for stem, count in {'blit': 1, 'clear': 1, 'copy': 2, 'pack': 1, 'resolve': 1}.items():
        header = 'src/dxvk/dxvk_meta_' + stem + '.h'
        cpp = 'src/dxvk/dxvk_meta_' + stem + '.cpp'
        replace(header, 'namespace dxvk {', 'namespace dxvk {\n\n  class DxvkPipelineCache;')
        classname = 'DxvkMeta' + stem.title() + 'Objects'
        start = text[header].index('  class ' + classname)
        prefix, section = text[header][:start], text[header][start:]
        # Some headers also contain view objects with a separate m_vkd.
        match = re.search(r'    Rc<vk::DeviceFn>\s+m_vkd;', section)
        if not match:
            raise ValueError('Native cache meta member drift: ' + header)
        section = section[:match.start()] + '    DxvkPipelineCache* m_pipelineCache;\n' + section[match.start():]
        text[header] = prefix + section
        constructor = '  ' + classname + '::' + classname + '(const DxvkDevice* device)\n  : m_vkd'
        replace(cpp, constructor, '  ' + classname + '::' + classname + '(const DxvkDevice* device)\n  : m_pipelineCache(&device->pipelineCache()),\n    m_vkd')
        pattern = r'm_vkd->vkCreate(Graphics|Compute)Pipelines\(\s*m_vkd->device\(\),\s*VK_NULL_HANDLE,\s*'
        text[cpp], found = re.subn(pattern, lambda m: 'm_pipelineCache->create' + m[1] + 'Pipelines(', text[cpp])
        if found != count:
            raise ValueError('Native cache meta call count drift: ' + cpp)
        routes.append({'path': cpp, 'calls': found})

    for name in NEW_FILES[:2]:
        text[name] = (ROOT / 'tools/dxvk_pipeline_cache' / Path(name).name).read_text(encoding='utf-8')
    raw_identity = bytes.fromhex(identity)
    text[NEW_FILES[2]] = ('#pragma once\n#include <cstdint>\n'
                         'static constexpr uint8_t FrostpunkPipelineCacheIdentity[32] = {'
                         + ','.join(str(x) for x in raw_identity) + '};\n'
                         'static constexpr const char* FrostpunkPipelineCacheIdentityHex = "' + identity + '";\n')
    if source != gpu_source:
        after_overrides = {path.relative_to(source).as_posix(): path.read_bytes()
                           for path in source.rglob('*') if path.is_file() and '.git' not in path.parts
                           and path.relative_to(source).as_posix() not in NEW_FILES}
        if overrides != after_overrides:
            raise ValueError('GPU source overrides changed while preparing native cache overlay')
    if identity != fingerprint(gpu_source, overrides)[0]:
        raise ValueError('GPU source changed while preparing native cache overlay')
    for name, value in text.items():
        path = work / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(value, encoding='utf-8', newline='\n')
    return {'schema_version': 1, 'success': True, 'revision': REVISION,
            'fingerprint_sha256': identity, 'gpu_source_hashes': inputs,
            'implementation_hashes': implementation, 'routes': routes, 'calls': sum(x['calls'] for x in routes),
            'outputs': {name: sha(work / name) for name in FILES},
            'native_cache_flags': 0, 'payload_max_bytes': 32 * 1024 * 1024,
            'periodic_seconds': 2, 'first_dirty_immediate': True}
