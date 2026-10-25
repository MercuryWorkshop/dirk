"""A reader for the COM interfaces of Wine's D3D11 and DXGI IDL files.

Only what the wire generator needs: interfaces (name, base, uuid, methods),
methods (return type, name, parameters with their [in]/[out] attributes),
and which names are enums, structs (with their fields) or interfaces.
"""
import re

class Param:
    def __init__(self, attrs, type_, name):
        self.attrs, self.type, self.name = attrs, type_, name
        self.fixed = None
    def __repr__(self):
        return f"[{','.join(self.attrs)}] {self.type} {self.name}"

class Method:
    def __init__(self, iface, ret, name, params):
        self.iface, self.ret, self.name, self.params = iface, ret, name, params

class Interface:
    def __init__(self, name, base, uuid, methods):
        self.name, self.base, self.uuid, self.methods = name, base, uuid, methods

def strip_comments(s):
    s = re.sub(r'/\*.*?\*/', '', s, flags=re.S)
    s = re.sub(r'//[^\n]*', '', s)
    s = re.sub(r'^\s*(cpp_quote|import|#)[^\n]*', '', s, flags=re.M)
    return s

def split_params(body):
    out, depth, cur = [], 0, ''
    for ch in body:
        if ch in '([': depth += 1
        if ch in ')]': depth -= 1
        if ch == ',' and depth == 0:
            out.append(cur); cur = ''
        else:
            cur += ch
    if cur.strip():
        out.append(cur)
    return [p.strip() for p in out if p.strip() and p.strip() != 'void']

def parse_param(text):
    attrs = []
    m = re.match(r'\[([^\]]*)\]\s*(.*)', text, re.S)
    if m:
        attrs = [a.strip() for a in split_params(m.group(1))]
        text = m.group(2)
    text = ' '.join(text.split())
    fixed = None
    arr = re.search(r'\[(\w+)\]$', text)
    if arr:
        fixed = arr.group(1)
        text = text[:arr.start()].strip()
    m = re.match(r'(.*?)(\w+)$', text)
    type_ = ' '.join(m.group(1).replace('*', ' * ').split())
    if fixed:
        type_ += ' *'
    p = Param(attrs, type_, m.group(2))
    p.fixed = fixed
    return p

class Idl:
    def __init__(self):
        self.interfaces = {}
        self.order = []
        self.enums = set()
        self.structs = {}      # name -> [(type, field name)]
        self.typedefs = {}     # alias -> type

    def read(self, path):
        s = strip_comments(open(path, encoding='utf-8', errors='replace').read())
        for m in re.finditer(r'typedef\s+enum\s+(\w+)?\s*\{[^}]*\}\s*([\w\s,*]+);', s):
            for n in [m.group(1)] + m.group(2).split(','):
                if n and n.strip() and '*' not in n:
                    self.enums.add(n.strip())
        for m in re.finditer(r'typedef\s+(?:struct|union)\s+(\w+)?\s*\{', s):
            # The body, nested braces included.
            depth, i = 1, m.end()
            while depth:
                if s[i] == '{': depth += 1
                elif s[i] == '}': depth -= 1
                i += 1
            body = s[m.end():i - 1]
            tail = re.match(r'\s*([\w\s,*]+);', s[i:])
            if not tail:
                continue
            names = [m.group(1)] + tail.group(1).split(',')
            if '{' in body or s[m.start():m.end()].find('union') >= 0:
                # Unions and nested aggregates are copied whole: they hold
                # plain numbers, or they are refused for having a pointer.
                pod = not re.search(r'\*|LPC?W?STR|HANDLE|HWND|SIZE_T|void', body)
                for n in names:
                    if n and n.strip() and '*' not in n:
                        self.structs[n.strip()] = 'raw' if pod else 'opaque'
                continue
            fields = []
            for f in body.split(";"):
                f = re.sub(r'\[[^\]]*\]', lambda a: a.group(0) if re.match(r'\[\w+\]$', a.group(0).strip()) else '', f)
                f = ' '.join(f.split())
                if not f:
                    continue
                fm = re.match(r'(.*?)(\w+)\s*((?:\[\w+\])*)$', f)
                if fm:
                    dims = re.findall(r'\[(\w+)\]', fm.group(3))
                    fields.append((' '.join(fm.group(1).replace('*', ' * ').split()), fm.group(2), dims))
            for n in names:
                if n and n.strip() and '*' not in n:
                    self.structs[n.strip()] = fields
        for m in re.finditer(r'typedef\s+([\w\s]+?)\s+(\w+)\s*;', s):
            self.typedefs[m.group(2)] = m.group(1).strip()
        for m in re.finditer(r'\[\s*([^\]]*?uuid\(([0-9a-fA-F-]+)\)[^\]]*)\]\s*interface\s+(\w+)\s*:\s*(\w+)\s*\{', s, re.S):
            start = m.end()
            depth, i = 1, start
            while depth:
                if s[i] == '{': depth += 1
                elif s[i] == '}': depth -= 1
                i += 1
            body = s[start:i - 1]
            methods = []
            for mm in re.finditer(r'([\w\s*]+?)\s+(\w+)\s*\(([^;]*?)\)\s*;', body, re.S):
                ret = ' '.join(mm.group(1).split())
                methods.append(Method(m.group(3), ret, mm.group(2),
                                      [parse_param(p) for p in split_params(mm.group(3))]))
            name = m.group(3)
            self.interfaces[name] = Interface(name, m.group(4), m.group(2), methods)
            self.order.append(name)

    def chain(self, name):
        """The interface and its bases, base first, IUnknown excluded."""
        out = []
        while name in self.interfaces:
            out.insert(0, self.interfaces[name])
            name = self.interfaces[name].base
        return out
