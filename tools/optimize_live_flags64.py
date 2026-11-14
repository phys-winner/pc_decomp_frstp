"""Conservative flag liveness over lifted C CFG; guest memory and calls are observation barriers."""
import re
from collections import Counter

FLAGS = {'cf':1,'zf':2,'sf':4,'of':8,'pf':16,'af':32}
ALL = 63
CALL = re.compile(r'\b([A-Za-z_]\w*)\s*\(')
PURE = {'if','while','sizeof','uint64_t','uint32_t','int64_t','int32_t','mask_sz','signbit_sz',
        'SET64','SET32','SET16','SET8L','SET8H','R64','R32','R16','R8L','R8H',
        'flags_add','flags_sub','flags_logicz'}
LABEL = re.compile(r'^([A-Za-z_]\w*):')
TARGET = re.compile(r'\bgoto\s+([A-Za-z_]\w*)\s*;')
HELPER = re.compile(r'\bflags_(add|sub|logicz)\(')


def arguments(line, at):
    start=at;depth=1;parts=[]
    while at<len(line):
        char=line[at]
        if char=='(':depth+=1
        elif char==')':
            depth-=1
            if not depth:
                parts.append(line[start:at].strip());return parts,at+1
        elif char==',' and depth==1:
            parts.append(line[start:at].strip());start=at+1
        at+=1
    raise ValueError('Unbalanced flag helper')


def body(text):
    lines=text.splitlines(keepends=True)
    clean=[line.split('/*',1)[0].strip() for line in lines]
    loops={i for i,line in enumerate(clean) if re.search(r'\b(?:for|while)\s*\(|\bdo\s*\{',line)}
    # A complete one-line loop is an opaque barrier; multiline loop backedges need a real C CFG.
    if any(not (re.match(r'^(?:for|while)\s*\(',clean[i]) and clean[i].endswith('}')
                and clean[i].count('{')==clean[i].count('}')==1) for i in loops):
        return text,{'structured_functions_skipped':1}
    labels={m[1]:i for i,line in enumerate(clean) if (m:=LABEL.match(line))}
    successors=[];reads=[];writes=[];eligible={};barriers=[]
    for i,line in enumerate(clean):
        targets=TARGET.findall(line)
        # Indirect transfers, EH and external calls retain all architectural flags.
        barrier=i in loops or bool(set(CALL.findall(line))-PURE) or 'return' in line or 'switch' in line or '->rip' in line and not line.startswith('c->rip = 0x')
        if len(re.findall(r'\bgoto\b',line))!=len(targets):barrier=True
        matches=list(HELPER.finditer(line));written=0
        if matches and (not re.match(r'^(?:flags_(?:add|sub|logicz)\(|SET(?:64|32|16|8L|8H)\()',line)
                        or any(token in line for token in ('?','&&','||'))):barrier=True
        if len(matches)==1 and not barrier:
            m=matches[0];args,end=arguments(line,m.end())
            if args[0]!='c' or len(args)!=(3 if m[1]=='logicz' else 4):raise ValueError('Unexpected flag helper signature')
            written=31 if m[1]=='logicz' else ALL
            eligible[i]=(m,args,end)
        elif matches:
            barrier=True
        read=0
        for flag in re.findall(r'c->(cf|zf|sf|of|pf|af)\b',line):read|=FLAGS[flag]
        edge=[labels[target] for target in targets if target in labels]
        if len(edge)!=len(targets):barrier=True
        unconditional=bool(re.match(r'^goto\s',line)) or line.startswith('return')
        if i+1<len(lines) and not unconditional:edge.append(i+1)
        successors.append(edge);reads.append(read);writes.append(written);barriers.append(barrier)
    live=[ALL if barriers[i] else 0 for i in range(len(lines))]
    changed=True
    while changed:
        changed=False
        for i in range(len(lines)-1,-1,-1):
            outgoing=0
            for target in successors[i]:outgoing|=live[target]
            value=ALL if barriers[i] else reads[i]|(outgoing&~writes[i])
            if value!=live[i]:live[i]=value;changed=True
    counts=Counter()
    for i,(m,args,end) in eligible.items():
        outgoing=ALL if not successors[i] else 0
        for target in successors[i]:outgoing|=live[target]
        needed=outgoing&writes[i]
        if needed==writes[i]:continue
        # A flag read inside the helper's arguments is evaluated before its writes.
        replacement='fp_flags_'+m[1]+'('+', '.join(args)+', '+str(needed)+'u)'
        # Positions refer to stripped instruction text, so preserve only its leading whitespace and comment.
        instruction=clean[i];instruction=instruction[:m.start()]+replacement+instruction[end:]
        comment=' /*'+lines[i].split('/*',1)[1] if '/*' in lines[i] else '\n'
        lines[i]='    '+instruction+comment
        counts[m[1]]+=1;counts['flag_writes_removed']+=(writes[i]&~needed).bit_count()
    return ''.join(lines),dict(counts)


def optimize(text):
    counts=Counter();pieces=[];start=0
    # A function-local CFG must never connect labels from different lifted functions.
    entries=list(re.finditer(r'^void L_[0-9A-Fa-f]+\(CPU \*c\)\n\{',text,re.M))
    assert entries,'No lifted functions'
    for index,entry in enumerate(entries):
        end=entries[index+1].start() if index+1<len(entries) else len(text)
        pieces.append(text[start:entry.start()]);part,report=body(text[entry.start():end]);pieces.append(part)
        counts.update(report);start=end
    pieces.append(text[start:])
    return '#include "live_flags64.h"\n'+''.join(pieces),dict(counts)
