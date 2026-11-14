"""Isolated SC journal fast revalidation; never writes production or old proofs."""
from pathlib import Path
import difflib
import hashlib
import importlib.util
import json
import subprocess
import sys

from leaf_probe import ROOT, windows_toolchain

OUT=ROOT/'build/android-region-atomic-journal-candidate'
BASE_SHA='72726db44adea6107ac44b6536dc15873847626434fe22ab018c74db686672be'
LOCKED=ROOT/'build/android-region-journal-candidate/candidate/android/host/win64.c'


def sha(path):return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def candidate_source(base):
    # Reuse the reviewed locked-history producer but do not execute its main or
    # write any of its outputs. Require exact identity with the frozen C body.
    import test_android_region_journal as old
    source=old.candidate_source(base)
    if source!=LOCKED.read_text():raise ValueError('Frozen locked16 journal changed')
    def replace(before,after):
        nonlocal source
        if source.count(before)!=1:raise ValueError('Atomic journal preimage changed: '+before[:100])
        source=source.replace(before,after)
    replace('static region_change region_journal[REGION_JOURNAL_CAPACITY];', '''typedef struct {
    _Atomic uint64_t epoch,low,high;
    _Atomic unsigned known;
} region_change_slot;
static region_change_slot region_journal[REGION_JOURNAL_CAPACITY];
#ifdef WIN64_REGION_CACHE_TEST
static _Thread_local void (*region_journal_probe)(unsigned,uint64_t);
static _Thread_local uint64_t region_journal_fast_revalidations;
#define REGION_JOURNAL_PROBE(stage,generation) do{if(region_journal_probe)region_journal_probe(stage,generation);}while(0)
#else
#define REGION_JOURNAL_PROBE(stage,generation) ((void)0)
#endif
static int region_cache_try_revalidate(uint64_t current,uint64_t p,size_t n);

/* Every shared field is atomic. A unique nonzero generation on both sides of
 * payload loads rejects concurrent invalidation/overwrite without a C data race.
 * See the pre-implementation memory-model argument; epochs never wrap. */
static void region_journal_publish(region_change value) {
    region_change_slot *slot=&region_journal[(value.epoch>>1)&(REGION_JOURNAL_CAPACITY-1)];
    atomic_store_explicit(&slot->epoch,0,memory_order_seq_cst);
    REGION_JOURNAL_PROBE(10,value.epoch);
    atomic_store_explicit(&slot->low,value.low,memory_order_seq_cst);
    atomic_store_explicit(&slot->high,value.high,memory_order_seq_cst);
    atomic_store_explicit(&slot->known,value.known,memory_order_seq_cst);
    REGION_JOURNAL_PROBE(11,value.epoch);
    atomic_store_explicit(&slot->epoch,value.epoch,memory_order_seq_cst);
    REGION_JOURNAL_PROBE(12,value.epoch);
}
static int region_journal_read(uint64_t epoch,region_change *value) {
    region_change_slot *slot=&region_journal[(epoch>>1)&(REGION_JOURNAL_CAPACITY-1)];
    uint64_t first=atomic_load_explicit(&slot->epoch,memory_order_seq_cst);
    if(first!=epoch)return 0;
    REGION_JOURNAL_PROBE(1,epoch);
    value->low=atomic_load_explicit(&slot->low,memory_order_seq_cst);
    value->high=atomic_load_explicit(&slot->high,memory_order_seq_cst);
    value->known=atomic_load_explicit(&slot->known,memory_order_seq_cst);
    REGION_JOURNAL_PROBE(2,epoch);
    value->epoch=atomic_load_explicit(&slot->epoch,memory_order_seq_cst);
    return value->epoch==first;
}''')
    if source.count('atomic_store_explicit(&region_epoch,current+1,memory_order_release);')!=2:
        raise ValueError('Expected exactly two global publication stores')
    source=source.replace('atomic_store_explicit(&region_epoch,current+1,memory_order_release);',
                          'atomic_store_explicit(&region_epoch,current+1,memory_order_seq_cst);')
    replace('region_journal[((current+1)>>1)&(REGION_JOURNAL_CAPACITY-1)]=region_pending;',
            'region_journal_publish(region_pending);')
    replace('''        region_change change=region_journal[(epoch>>1)&(REGION_JOURNAL_CAPACITY-1)];
        if(change.epoch!=epoch || !change.known || change.low>=change.high)goto clear;''', '''        region_change change;
        if(!region_journal_read(epoch,&change) || !change.known || change.low>=change.high)goto clear;''')
    replace('''void win64_region_add(uint64_t address, size_t bytes, int writable) {''', '''/* No AVL/region pointer is read here. Only a request already covered by an
 * existing local snapshot can benefit. Every intervening complete changed
 * range must be disjoint from ALL snapshots; overlap/unknown/race declines to
 * the unchanged locked authoritative path. */
static int region_cache_try_revalidate(uint64_t current,uint64_t p,size_t n) {
    uint64_t previous=region_cache_epoch;
    if((previous|current)&1 || previous>=current || !region_cache_count ||
       region_cache_count>REGION_CACHE_CAPACITY || (current-previous)/2>REGION_JOURNAL_CAPACITY)return 0;
    if(atomic_load_explicit(&region_epoch,memory_order_seq_cst)!=current)return 0;
    int covered=0;
    for(size_t j=0;j<region_cache_count;++j) {
        region_snapshot s=region_cache[j];
        if(!s.size || s.base>UINT64_MAX-s.size)return 0;
        if(p>=s.base && p-s.base<s.size && n<=s.size-(size_t)(p-s.base))covered=1;
    }
    if(!covered)return 0;
    REGION_JOURNAL_PROBE(0,current);
    unsigned steps=(unsigned)((current-previous)/2);
    for(unsigned i=1;i<=steps;++i) {
        uint64_t epoch=previous+2*(uint64_t)i;region_change change;
        if(!region_journal_read(epoch,&change) || !change.known || change.low>=change.high)return 0;
        for(size_t j=0;j<region_cache_count;++j) {
            region_snapshot s=region_cache[j];
            if(change.low<s.base+s.size && s.base<change.high)return 0;
        }
    }
    REGION_JOURNAL_PROBE(3,current);
    if(atomic_load_explicit(&region_epoch,memory_order_seq_cst)!=current)return 0;
    region_cache_epoch=current;
#ifdef WIN64_REGION_CACHE_TEST
    ++region_journal_fast_revalidations;
#endif
    return 1;
}

void win64_region_add(uint64_t address, size_t bytes, int writable) {''')
    replace('''    uint64_t version=atomic_load_explicit(&region_epoch,memory_order_acquire);
    if(!(version&1) && region_cache_epoch==version''', '''    uint64_t version=atomic_load_explicit(&region_epoch,memory_order_acquire);
    if(region_cache_epoch!=version && region_cache_count)
        (void)region_cache_try_revalidate(version,p,n);
    if(!(version&1) && region_cache_epoch==version''')
    return source


def main():
    # Generation checkpoint only until the focused fixture is installed below.
    OUT.mkdir(parents=True,exist_ok=True)
    base=ROOT/'android/host/win64.c'
    if sha(base)!=BASE_SHA:raise ValueError('Exact72726 production changed')
    if not (OUT/'memory-model.md').exists():raise ValueError('Formal argument must precede C generation')
    text=base.read_text();candidate=candidate_source(text)
    start=text.index('    if(!(version&1) && region_cache_epoch==version')
    end=text.index('    metadata_lock();\n    uint64_t current=',start)
    if text[start:end] not in candidate:raise ValueError('Original double-epoch fast path changed')
    folder=OUT/'candidate';folder.mkdir(exist_ok=True)
    target=folder/'win64.c';target.write_text(candidate,newline='\n')
    (OUT/'candidate.patch').write_text(''.join(difflib.unified_diff(text.splitlines(True),candidate.splitlines(True),fromfile='android/host/win64.c72726',tofile='isolated/atomic-journal/win64.c')))
    (OUT/'locked-to-atomic.patch').write_text(''.join(difflib.unified_diff(LOCKED.read_text().splitlines(True),candidate.splitlines(True),fromfile='frozen-locked16/win64.c',tofile='isolated/atomic-journal/win64.c')))
    print(json.dumps(dict(candidate=str(target),sha256=sha(target),formal_argument_sha256=sha(OUT/'memory-model.md'),production_modified=False,proof_complete=False)))


if __name__=='__main__':main()
