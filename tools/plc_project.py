"""Project validation and LAD/STL/SCL front ends for the native program compiler."""
from __future__ import annotations
import copy
import hashlib
import json
import re
from dataclasses import dataclass

TYPES = ['BOOL', 'BYTE', 'INT', 'DINT', 'REAL', 'LREAL', 'TIME', 'STRING']
SIZES = [1, 1, 2, 4, 4, 8, 8, 256]
IDENT = re.compile(r'[A-Za-z_][A-Za-z0-9_]*\Z')
REF = re.compile(r'[A-Za-z_][A-Za-z0-9_]*(?:\.[A-Za-z_][A-Za-z0-9_]*)*\Z')

class ProjectError(ValueError):
    pass

def ident(name):
    if not isinstance(name, str) or not IDENT.fullmatch(name) or name.lower().startswith('__plc_'):
        raise ProjectError(f'Invalid or reserved name: {name!r}')
    return name

def integer(value, low, high, label):
    if isinstance(value, bool) or not isinstance(value, int) or not low <= value <= high:
        raise ProjectError(f'{label} must be an integer from {low} to {high}')
    return value

def init_literal(field):
    typ = field['type']
    value = field.get('initial', False if typ == 'BOOL' else '' if typ == 'STRING' else 0)
    if typ == 'BOOL':
        if not isinstance(value, bool): raise ProjectError('BOOL initial values must be true/false')
        return 'TRUE' if value else 'FALSE'
    if typ == 'STRING':
        if not isinstance(value, str) or "'" in value or len(value.encode('utf-8')) > 254 or '\x00' in value:
            raise ProjectError('STRING initial value must be at most 254 UTF-8 bytes without quotes or NUL')
        return "'" + value + "'"
    if typ == 'TIME':
        integer(value, 0, 2147483647, 'TIME initializer (ms)')
        return f'T#{value}ms'
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        raise ProjectError(f'{typ} initializer must be numeric')
    import math
    if not math.isfinite(value): raise ProjectError('Initial values must be finite')
    bounds = {'BYTE': (0, 255), 'INT': (-32768, 32767), 'DINT': (-2147483647, 2147483647)}
    if typ in bounds: integer(value, *bounds[typ], typ + ' initializer')
    if typ == 'REAL' and abs(value) > 3.402823466e38: raise ProjectError('REAL initializer out of range')
    # The existing lexer accepts fixed-point literals, not exponent syntax.
    return format(value, '.17f').rstrip('0').rstrip('.') if isinstance(value, float) else str(value)

def address(db, offset, bit, typ):
    size = SIZES[TYPES.index(typ)]
    suffix = 'X' if typ == 'BOOL' else {1:'B', 2:'W', 4:'D', 8:'L', 256:'B'}[size]
    return f'DB{db}.DB{suffix}{offset}' + (f'.{max(bit, 0)}' if typ == 'BOOL' else '')

@dataclass
class Prepared:
    project: dict
    source: str
    networks: list
    native: list
    db_fields: dict
    instances: dict
    build_id: str

class Frontend:
    def __init__(self, project):
        self.p = copy.deepcopy(project)
        self.networks, self.native, self.markers = [], [], []
        self.fields, self.aliases, self.instances = {}, {}, {}
        self.db_ids = set()
        self.temporary = []

    def symbol(self, text):
        if not isinstance(text, str): raise ProjectError('Expected a tag/expression string')
        # Never substitute text inside ST comments or string literals.
        token = re.compile(r"'[^']*'|\(\*.*?\*\)|//[^\n]*|%?DB\d+\.DB[XBWDL]\d+(?:\.[0-7])?", re.I | re.S)
        def sub(m):
            s = m.group()
            if s.startswith(("'", '(*', '//')): return s
            key = s.lstrip('%').upper()
            if key not in self.aliases: raise ProjectError(f'{s}: declare a matching DB field before using this absolute address')
            return self.aliases[key]
        return token.sub(sub, text)

    def reference(self, text):
        result = self.symbol(text)
        if not REF.fullmatch(result): raise ProjectError(f'Invalid tag reference: {text}')
        return result

    def call(self, item):
        name = self.reference(item.get('block', ''))
        args = []
        for param, value in item.get('inputs', {}).items():
            args.append(f'{ident(param)} := {self.symbol(str(value))}')
        for param, value in item.get('outputs', {}).items():
            args.append(f'{ident(param)} => {self.reference(value)}')
        return name + '(' + ', '.join(args) + ');'

    def ladder(self, net):
        branches = net.get('branches', [[]])
        if not isinstance(branches, list) or not 1 <= len(branches) <= 32: raise ProjectError('LAD needs 1–32 parallel branches')
        expressions = []
        for row in branches:
            if not isinstance(row, list) or len(row) > 64: raise ProjectError('LAD branch supports at most 64 contacts')
            contacts = []
            for contact in row:
                tag = self.reference(contact['tag'])
                contacts.append(('NOT ' if contact.get('nc', False) else '') + tag)
            expressions.append('(' + ' AND '.join(contacts) + ')' if contacts else 'TRUE')
        condition = '(' + ' OR '.join(expressions) + ')'
        output = []
        for coil in net.get('coils', []):
            mode = coil.get('mode', 'coil')
            if mode not in ('coil', 'set', 'reset'): raise ProjectError(f'Invalid coil mode: {mode}')
            output.append(('' if mode == 'coil' else mode.upper() + ' ') + self.reference(coil['tag']))
        body = f'RUNG {condition} => ' + ', '.join(output) + ';\n' if output else ''
        calls = net.get('calls', [])
        if calls: body += f'IF {condition} THEN\n' + '\n'.join(self.call(c) for c in calls) + '\nEND_IF;\n'
        if not output and not calls: raise ProjectError('LAD network needs a coil or a block call')
        return condition, body

    def operand_type(self, operand, block):
        if re.fullmatch(r'-?\d+', operand): return 'DINT'
        if re.fullmatch(r'-?\d+\.\d+', operand): return 'LREAL'
        if operand in self.fields: return TYPES[self.fields[operand]['type']]
        for f in self.p.get('globals', []):
            if f['name'] == operand: return f['type']
        declarations = block.get('declarations', '')
        local = operand
        if '.' in operand:
            prefix, local = operand.split('.', 1)
            instance = next((i for i in self.p.get('instances', []) if i['name'] == prefix), None)
            if instance:
                definition = next((b for b in self.p.get('blocks', []) if b['name'] == instance['type']), {})
                declarations = definition.get('declarations', '')
        found = re.search(r'\b'+re.escape(local)+r'\s*:\s*(BOOL|BYTE|INT|DINT|REAL|LREAL|TIME|STRING)\b', declarations, re.I)
        if found: return found.group(1).upper()
        raise ProjectError('STL L requires a declared numeric tag or numeric literal: '+operand)

    def stl(self, text, nid, block):
        rlo, acc1, acc2 = None, None, None
        statements = []
        def snapshot(expr, typ):
            name = f'__plc_tmp_n{nid}_{len(self.temporary)}'
            self.temporary.append(f'{name} : {typ};')
            statements.append(f'{name} := {expr};')
            return name
        arithmetic = {'+I': '+', '-I': '-', '*I': '*', '/I': '/', '+R': '+', '-R': '-', '*R': '*', '/R': '/'}
        compares = {'==I': '=', '<>I': '<>', '>I': '>', '<I': '<', '>=I': '>=', '<=I': '<=', '==R': '=', '<>R': '<>', '>R': '>', '<R': '<', '>=R': '>=', '<=R': '<='}
        for lineno, raw in enumerate(text.splitlines(), 1):
            line = raw.split('//', 1)[0].strip().rstrip(';').strip()
            if not line: continue
            parts = line.split(None, 1); op = parts[0].upper(); operand = self.symbol(parts[1]) if len(parts) > 1 else ''
            def need(value, what):
                if value is None: raise ProjectError(f'STL line {lineno}: {what} is not initialized')
            if op in ('SET', 'CLR'):
                if operand: raise ProjectError(f'STL line {lineno}: {op} has no operand')
                rlo = 'TRUE' if op == 'SET' else 'FALSE'
            elif op in ('A', 'AN', 'O', 'ON', 'X', 'XN'):
                if not operand: raise ProjectError(f'STL line {lineno}: contact needs an operand')
                value = ('NOT (' + operand + ')') if op.endswith('N') else '(' + operand + ')'
                rlo = snapshot(value if rlo is None else f'({rlo} ' + {'A':'AND','O':'OR','X':'XOR'}[op[0]] + f' {value})', 'BOOL')
            elif op == 'NOT':
                need(rlo, 'RLO'); rlo = f'(NOT {rlo})'
            elif op in ('=', 'S', 'R'):
                need(rlo, 'RLO'); target = self.reference(operand)
                statements.append(f'RUNG {rlo} => ' + {'=':'', 'S':'SET ', 'R':'RESET '}[op] + target + ';')
                rlo = None
            elif op == 'L':
                if not operand: raise ProjectError(f'STL line {lineno}: L needs an operand')
                typ=self.operand_type(operand,block)
                if typ not in ('BYTE','INT','DINT','REAL','LREAL'):raise ProjectError('STL accumulator supports numeric values only')
                acc2, acc1 = acc1, snapshot(operand,typ)
            elif op == 'T':
                need(acc1, 'accumulator'); statements.append(f'{self.reference(operand)} := {acc1};')
            elif op in arithmetic or op in compares:
                need(acc1, 'ACCU1'); need(acc2, 'ACCU2')
                expr = f'({acc2} {(arithmetic | compares)[op]} {acc1})'
                if op in compares: rlo = snapshot(expr, 'BOOL')
                else: acc1, acc2 = snapshot(expr, 'LREAL' if op.endswith('R') else 'DINT'), None
            elif op == 'CALL':
                if not re.fullmatch(r'[A-Za-z_]\w*\s*\(.*\)', operand): raise ProjectError('STL CALL uses Instance(Input := value, Output => tag)')
                statements.append(operand + ';')
            else:
                raise ProjectError(f'STL line {lineno}: unsupported instruction {op}; jumps and indirect addressing are not implemented')
        return '\n'.join(statements)

    def networks_for(self, block):
        result = []
        self.temporary = []
        for index, net in enumerate(block.get('networks', []), 1):
            nid = len(self.networks) + 1
            lang = str(net.get('language', 'SCL')).upper()
            lang = {'LADDER':'LAD', 'C++':'CPP', 'ST':'SCL'}.get(lang, lang)
            if lang not in ('LAD','STL','SCL','C','CPP'): raise ProjectError(f'Unsupported language: {lang}')
            self.networks.append({'id':nid,'block':block['name'],'index':index,'title':str(net.get('title', f'Network {index}')),'language':lang})
            marker = f'__plc_network_{nid}'; self.markers.append(marker)
            power = 'TRUE'
            if lang == 'LAD': power, body = self.ladder(net)
            elif lang == 'STL': body = self.stl(net.get('source', ''), nid, block)
            elif lang == 'SCL': body = self.symbol(net.get('source', ''))
            else:
                native = f'__plc_native_{nid}';self.markers.append(native)
                self.native.append({'name':native,'language':lang,'source':str(net.get('source','')),'network':nid})
                body = native + ' := TRUE;'
            result.append(f'(* {block["name"]}: {net.get("title", index)} *)\n{marker} := {power};\n{body}')
        return '\n'.join(result)

    def prepare(self):
        p=self.p
        if not isinstance(p, dict) or p.get('schema')!=1: raise ProjectError('Project schema must be 1')
        if not isinstance(p.get('name'),str) or not p['name'].strip(): raise ProjectError('Project needs a name')
        if len(json.dumps(p))>1000000: raise ProjectError('Project exceeds 1 MB')
        db_source=[]
        for db in p.get('data_blocks',[]):
            number=integer(db['number'],1,65535,'DB number')
            if number in self.db_ids: raise ProjectError(f'Duplicate DB{number}')
            self.db_ids.add(number); used={}; cursor=0;declarations=[]
            for field in db.get('fields',[]):
                ident(field['name']);typ=field['type'].upper();field['type']=typ
                if typ not in TYPES: raise ProjectError(f'Unsupported type: {typ}')
                size=SIZES[TYPES.index(typ)];off=integer(field.get('offset',cursor),0,1048576-size,'DB offset');bit=integer(field.get('bit',0),0,7,'DB bit') if typ=='BOOL' else -1
                for byte in range(off,off+size):
                    mask=1<<bit if typ=='BOOL' else 255
                    if used.get(byte,0)&mask: raise ProjectError(f'Overlapping field in DB{number}: {field["name"]}')
                    used[byte]=used.get(byte,0)|mask
                cursor=max(cursor,off+size);name=f'DB{number}.{field["name"]}'
                if name in self.fields: raise ProjectError(f'Duplicate field {name}')
                addr=address(number,off,bit,typ)
                self.fields[name]={'area':4,'db':number,'offset':off,'bit':bit,'address':addr,'type':TYPES.index(typ)}
                self.aliases[addr.upper()]=name
                declarations.append(f'{field["name"]} : {typ} := {init_literal(field)};')
            db_source.append(f'DATA_BLOCK DB{number}\nVAR\n'+ '\n'.join(declarations)+'\nEND_VAR\nEND_DATA_BLOCK')
        block_names=set()
        for b in p.get('blocks',[]):
            ident(b['name'])
            if b['name'] in block_names or b['name'] in ('TON','TOF','CTU','CTD'):raise ProjectError('Duplicate/reserved block '+b['name'])
            if b.get('kind') not in ('FB','FC'):raise ProjectError('Block kind must be FB or FC')
            block_names.add(b['name'])
        instance_decls=[]
        for inst in p.get('instances',[]):
            name=ident(inst['name']);typ=ident(inst['type']);number=integer(inst['db'],1,65535,'Instance DB')
            if number in self.db_ids or name in self.instances:raise ProjectError('Duplicate instance name or DB number')
            if typ not in {b['name'] for b in p.get('blocks',[]) if b['kind']=='FB'}|{'TON','TOF','CTU','CTD'}:raise ProjectError('Unknown FB '+typ)
            self.db_ids.add(number);self.instances[name]=number;instance_decls.append(f'{name} : {typ};')
        tasks=p.get('tasks',[])
        if not 1<=len(tasks)<=128:raise ProjectError('Project needs 1–128 OBs')
        ids=set();main=0
        for task in tasks:
            ob=integer(task['ob'],1,65535,'OB number');task['name']=f'OB{ob}'
            if ob in ids:raise ProjectError('Duplicate OB number')
            ids.add(ob)
            if task.get('kind') not in ('main','cyclic','startup'):raise ProjectError('OB kind must be main, cyclic or startup')
            if task['kind']=='main':
                main+=1
                if ob!=1:raise ProjectError('The main OB must be OB1')
            elif ob==1:raise ProjectError('OB1 must be the main task')
            integer(task.get('period_ms',10),1,60000,'OB period')
            integer(task.get('priority',1 if task['kind']=='main' else 12),0,99,'OB priority')
            integer(task.get('watchdog_ms',1000),1,60000,'OB watchdog')
            if len(task.get('networks',[]))>1000:raise ProjectError('Too many networks')
        if main!=1:raise ProjectError('Exactly one main OB1 is required')
        globals=[]
        for field in p.get('globals',[]):
            ident(field['name']);typ=field['type'].upper();field['type']=typ
            if typ not in TYPES:raise ProjectError('Unsupported global type')
            at=''
            if field.get('address'):
                if not re.fullmatch(r'%[IQM][XBWDL]?\d+(?:\.[0-7])?',field['address'],re.I):raise ProjectError('Invalid I/Q/M address')
                at=' AT '+field['address']
            globals.append(f'{field["name"]}{at} : {typ} := {init_literal(field)};')
        pous=[]
        for block in p.get('blocks',[]):
            keyword='FUNCTION_BLOCK' if block['kind']=='FB' else 'FUNCTION'
            declaration=block.get('declarations','')
            if '__plc_' in declaration.lower():raise ProjectError('Reserved declaration prefix')
            body=self.networks_for(block)
            if self.temporary:declaration+='\nVAR_TEMP\n'+'\n'.join(self.temporary)+'\nEND_VAR'
            pous.append(f'{keyword} {block["name"]}\n{self.symbol(declaration)}\n{body}\nEND_{keyword}')
        for task in tasks:
            body=self.networks_for(task)
            decl=task.get('declarations','')
            if '__plc_' in decl.lower():raise ProjectError('Reserved declaration prefix')
            if self.temporary:decl+='\nVAR_TEMP\n'+'\n'.join(self.temporary)+'\nEND_VAR'
            pous.append(f'FUNCTION_BLOCK __plc_type_ob{task["ob"]}\n{self.symbol(decl)}\n{body}\nEND_FUNCTION_BLOCK')
            instance_decls.append(f'OB{task["ob"]} : __plc_type_ob{task["ob"]};')
        dispatch='\n'.join(('IF' if i==0 else 'ELSIF')+f' __plc_dispatch = {t["ob"]} THEN\nOB{t["ob"]}();' for i,t in enumerate(tasks))+'\nEND_IF;'
        declarations=['__plc_dispatch : DINT;']+globals+[f'{m} : BOOL;' for m in self.markers]+instance_decls
        source='\n\n'.join(db_source+pous)+ '\nPROGRAM CompiledProject\nVAR\n'+'\n'.join(declarations)+'\nEND_VAR\n'+dispatch+'\nEND_PROGRAM\n'
        build_id=hashlib.sha256(json.dumps(p,sort_keys=True,separators=(',',':')).encode()).hexdigest()[:16]
        return Prepared(p,source,self.networks,self.native,self.fields,self.instances,build_id)

def prepare(project):
    return Frontend(project).prepare()

def map_symbols(prepared, symbols):
    cursor={}; occupied={}
    for symbol in symbols:
        name=symbol['name'];typ=TYPES[symbol['type']]
        symbol.setdefault('area',0);symbol.setdefault('offset',0);symbol.setdefault('bit',-1);symbol['db']=0;symbol['address']=''
        if name in prepared.db_fields:
            symbol.update(prepared.db_fields[name])
        elif not symbol['temporary'] and name.split('.')[0] in prepared.instances:
            db=prepared.instances[name.split('.')[0]];off=cursor.get(db,0);bit=0 if typ=='BOOL' else -1
            symbol.update(area=4,db=db,offset=off,bit=bit,address=address(db,off,bit,typ));cursor[db]=off+SIZES[symbol['type']]
        elif symbol['area']:
            area='IQM'[symbol['area']-1]
            symbol['address']=f'%{area}{symbol["offset"]}'+(f'.{symbol["bit"]}' if symbol['bit']>=0 else '')
        if symbol['area']:
            if symbol['bit']>=0 and typ!='BOOL':raise ProjectError('Bit addresses require BOOL: '+name)
            for byte in range(symbol['offset'],symbol['offset']+SIZES[symbol['type']]):
                key=(symbol['area'],symbol['db'],byte);mask=1<<max(symbol['bit'],0) if typ=='BOOL' else 255
                if occupied.get(key,0)&mask:raise ProjectError('Overlapping address: '+name)
                occupied[key]=occupied.get(key,0)|mask
    return symbols
