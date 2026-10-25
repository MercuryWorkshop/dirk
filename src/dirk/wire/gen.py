#!/usr/bin/env python3
"""Generates dirk's D3D11/DXGI wire: guest proxies and the host's decoder.

    gen.py <wine-include-dir> <out-dir>

From the COM interfaces in Wine's IDL files it writes:

  dirk_wire_ops.h     the opcodes, the interface table (IIDs, vtable sizes)
  dirk_wire_guest.c   a proxy vtable for every interface: each method writes
                      a record into the ring, or asks and waits for a reply
  dirk_wire_host.cpp  the host's decoder: each record called on the real
                      object

Most methods follow from their signatures. The rest are named in OVERRIDES:
hand-written on both sides (dirk_guest.c, dirk_host.cpp), unsupported, or a
parameter whose size the IDL does not say.
"""
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import idl  # noqa: E402

FILES = ['dxgitype.idl', 'dxgicommon.idl', 'dxgiformat.idl', 'd3dcommon.idl',
         'dxgi.idl', 'dxgi1_2.idl', 'dxgi1_3.idl', 'dxgi1_4.idl', 'dxgi1_5.idl',
         'dxgi1_6.idl', 'd3d11.idl', 'd3d11_1.idl', 'd3d11_2.idl', 'd3d11_3.idl',
         'd3d11_4.idl']

# Interfaces the wire does not carry: video, content protection, and the
# DXGI corners no D3D11 game reaches. Their methods are never called on a
# proxy because no proxy of them is ever made.
SKIP = re.compile(r'Video|Authenticated|Crypto|Media|Decode|Duplication|'
                  r'DisplayControl|ID3D10Blob|Destruction|ID3DUserDefinedAnnotation|'
                  r'ID3D11Multithread|IDXGIKeyedMutex')

# Hand-written on both sides: the guest's in dirk_guest.c (named
# dirk_g_<Iface>_<Method>), any record they send decoded in dirk_host.cpp.
GUEST = {
    'IUnknown.QueryInterface', 'IUnknown.AddRef', 'IUnknown.Release',
    'ID3D11Device.CreateBuffer', 'ID3D11Device.CreateTexture1D',
    'ID3D11Device.CreateTexture2D', 'ID3D11Device.CreateTexture3D',
    'ID3D11Device3.CreateTexture2D1', 'ID3D11Device3.CreateTexture3D1',
    'ID3D11DeviceContext.Map', 'ID3D11DeviceContext.Unmap',
    'ID3D11DeviceContext.UpdateSubresource', 'ID3D11DeviceContext1.UpdateSubresource1',
    'IDXGIFactory.MakeWindowAssociation', 'IDXGIFactory.GetWindowAssociation',
    'IDXGISwapChain4.SetHDRMetaData', 'ID3D11Device.CheckCounterInfo',
    'IDXGIFactory5.CheckFeatureSupport', 'IDXGISwapChain.GetFrameStatistics',
    'IDXGISwapChain1.GetHwnd', 'IDXGISwapChain3.ResizeBuffers1',
    'IDXGIDevice2.OfferResources', 'IDXGIDevice2.ReclaimResources',
    'IDXGIDevice4.OfferResources1', 'IDXGIDevice4.ReclaimResources1',
    'ID3D11Device.CheckCounter', 'ID3D11Device.CreateCounter',
}

# Sends what the stream holds once the call is written: the frame's end, and
# the calls that ask the GPU to start.
# Methods naming a window: told to the transport first (dirk_window_seen).
WINDOW = {
    'IDXGIFactory.CreateSwapChain': 'desc ? (uint64_t)(uintptr_t)desc->OutputWindow : 0',
    'IDXGIFactory2.CreateSwapChainForHwnd': '(uint64_t)(uintptr_t)hWnd',
}
# Methods after which a window may have changed size: each window seen is
# looked at again first (dirk_windows_check).
WINDOW_CHECK = {'IDXGISwapChain.Present', 'IDXGISwapChain1.Present1', 'IDXGISwapChain.ResizeBuffers',
                'IDXGISwapChain3.ResizeBuffers1'}
# DIRK_WIRE_TRACE: the capability checks, logged with their answers after
# the call (a printf format and its arguments).
TRACE = {
    'ID3D11Device.CheckFeatureSupport': ('CheckFeatureSupport %u size %u -> 0x%08x first word 0x%x',
        '(unsigned)Feature, FeatureSupportDataSize, (unsigned)result, '
        '(pFeatureSupportData && FeatureSupportDataSize >= 4) ? *(const uint32_t *)pFeatureSupportData : 0u'),
    'ID3D11Device.CheckFormatSupport': ('CheckFormatSupport %u -> 0x%08x 0x%x',
        '(unsigned)Format, (unsigned)result, pFormatSupport ? *pFormatSupport : 0u'),
    'ID3D11Device.CheckMultisampleQualityLevels': ('CheckMultisampleQualityLevels %u x%u -> 0x%08x %u',
        '(unsigned)Format, SampleCount, (unsigned)result, pNumQualityLevels ? *pNumQualityLevels : 0u'),
    'ID3D11Device.GetFeatureLevel': ('GetFeatureLevel -> %x', '(unsigned)result'),
    'IDXGIFactory1.EnumAdapters1': ('EnumAdapters1 %u -> 0x%08x', 'Adapter, (unsigned)result'),
    'IDXGIFactory.EnumAdapters': ('EnumAdapters %u -> 0x%08x', 'adapter_idx, (unsigned)result'),
    'IDXGIAdapter.CheckInterfaceSupport': ('CheckInterfaceSupport -> 0x%08x', '(unsigned)result'),
}

FLUSH = {'IDXGISwapChain.Present', 'IDXGISwapChain1.Present1', 'ID3D11DeviceContext.Flush'}

# Returns E_NOTIMPL (or nothing) without asking the host.
UNSUPPORTED = {
    'IDXGIResource.GetSharedHandle', 'IDXGIResource1.CreateSharedHandle',
    'IDXGISurface1.GetDC', 'IDXGISurface1.ReleaseDC', 'IDXGISurface.Map', 'IDXGISurface.Unmap',
    'IDXGISwapChain1.GetCoreWindow', 'IDXGIFactory2.CreateSwapChainForCoreWindow',
    'IDXGIFactory2.CreateSwapChainForComposition', 'IDXGIFactory2.RegisterStereoStatusWindow',
    'IDXGIFactory2.RegisterStereoStatusEvent', 'IDXGIFactory2.RegisterOcclusionStatusWindow',
    'IDXGIFactory2.RegisterOcclusionStatusEvent', 'IDXGIFactory2.UnregisterStereoStatus',
    'IDXGIFactory2.UnregisterOcclusionStatus',
    'IDXGIFactory.CreateSoftwareAdapter',
    'IDXGIAdapter3.RegisterHardwareContentProtectionTeardownStatusEvent',
    'IDXGIAdapter3.UnregisterHardwareContentProtectionTeardownStatus',
    'IDXGIAdapter3.RegisterVideoMemoryBudgetChangeNotificationEvent',
    'IDXGIAdapter3.UnregisterVideoMemoryBudgetChangeNotification',
    'IDXGIFactory7.RegisterAdaptersChangedEvent', 'IDXGIFactory7.UnregisterAdaptersChangedEvent',
    'IDXGIDevice.CreateSurface', 'IDXGIDevice.QueryResourceResidency',
    'IDXGIDevice2.EnqueueSetEvent',
    'IDXGISwapChain1.GetRestrictToOutput', 'IDXGISwapChain2.GetFrameLatencyWaitableObject',
    'ID3D11Device.OpenSharedResource', 'ID3D11Device1.OpenSharedResource1',
    'ID3D11Device1.OpenSharedResourceByName', 'ID3D11Device1.CreateDeviceContextState',
    'ID3D11DeviceContext1.SwapDeviceContextState',
    'ID3D11Device.CreateClassLinkage', 'ID3D11Device2.GetResourceTiling',
    'ID3D11DeviceContext2.UpdateTileMappings', 'ID3D11DeviceContext2.CopyTileMappings',
    'ID3D11DeviceContext2.CopyTiles', 'ID3D11DeviceContext2.UpdateTiles',
    'ID3D11DeviceContext2.ResizeTilePool', 'ID3D11DeviceContext2.TiledResourceBarrier',
    'ID3D11DeviceContext2.SetMarkerInt', 'ID3D11DeviceContext2.BeginEventInt',
    'ID3D11DeviceContext2.EndEvent', 'ID3D11DeviceContext2.IsAnnotationEnabled',
    'ID3D11Fence.CreateSharedHandle', 'ID3D11Fence.SetEventOnCompletion',
    'ID3D11Device5.OpenSharedFence', 'ID3D11Device5.CreateFence',
    'ID3D11Device4.RegisterDeviceRemovedEvent', 'ID3D11Device4.UnregisterDeviceRemoved',
    'ID3D11Device3.WriteToSubresource', 'ID3D11Device3.ReadFromSubresource',
    'ID3D11ClassInstance.GetInstanceName', 'ID3D11ClassInstance.GetTypeName',
    'ID3D11DeviceContext3.GetHardwareProtectionState',
    'ID3D11DeviceContext3.SetHardwareProtectionState',
    'ID3D11Device.CreateGeometryShaderWithStreamOutput',
    'IDXGIFactory2.GetSharedResourceAdapterLuid',
}

# Sizes the IDL does not say. ('blob', expr): bytes; ('count', expr): elements.
SIZES = {
    ('ID3D11Device', 'CreateVertexShader', 'pShaderBytecode'): ('blob', 'BytecodeLength'),
    ('ID3D11Device', 'CreateHullShader', 'pShaderBytecode'): ('blob', 'BytecodeLength'),
    ('ID3D11Device', 'CreateDomainShader', 'pShaderBytecode'): ('blob', 'BytecodeLength'),
    ('ID3D11Device', 'CreateGeometryShader', 'pShaderBytecode'): ('blob', 'BytecodeLength'),
    ('ID3D11Device', 'CreatePixelShader', 'pShaderBytecode'): ('blob', 'BytecodeLength'),
    ('ID3D11Device', 'CreateComputeShader', 'pShaderBytecode'): ('blob', 'BytecodeLength'),
    ('ID3D11Device', 'CreateInputLayout', 'pShaderBytecodeWithInputSignature'): ('blob', 'BytecodeLength'),
    ('ID3D11Device', 'CheckFeatureSupport', 'pFeatureSupportData'): ('blob', 'FeatureSupportDataSize'),
    ('IDXGIFactory5', 'CheckFeatureSupport', 'feature_data'): ('blob', 'data_size'),
    ('ID3D11DeviceContext', 'GetData', 'pData'): ('blob', 'DataSize'),
    ('ID3D11DeviceContext', 'RSGetViewports', 'pViewports'): ('count', '(pNumViewports ? *pNumViewports : 0)'),
    ('ID3D11DeviceContext', 'RSGetScissorRects', 'pRects'): ('count', '(pNumRects ? *pNumRects : 0)'),
    ('IDXGIOutput', 'GetDisplayModeList', 'desc'): ('count', '(mode_count ? *mode_count : 0)'),
}

# Parameters whose direction the IDL gets wrong or leaves out.
DIRECTION = {
    ('ID3D11DeviceContext', 'GetData', 'pData'): 'out',
    ('ID3D11Device', 'CheckFeatureSupport', 'pFeatureSupportData'): 'inout',
}

# HRESULT methods whose result the caller needs: they wait for the host
# although they have nothing else to answer.
FORCE_SYNC = {
    'IDXGIDevice.GetGPUThreadPriority', 'ID3D11Device.GetDeviceRemovedReason',
    'IDXGISwapChain.SetFullscreenState', 'IDXGISwapChain.ResizeTarget',
}

WIRE32 = {'UINT', 'INT', 'BOOL', 'FLOAT', 'DWORD', 'ULONG', 'LONG', 'HRESULT',
          'UINT32', 'INT32', 'DXGI_USAGE', 'D3D11_RECT_LONG'}
WIRE16 = {'WCHAR', 'USHORT', 'UINT16', 'SHORT'}
WIRE8 = {'BYTE', 'UINT8', 'BOOLEAN', 'CHAR', 'char'}
WIRE64 = {'UINT64', 'INT64', 'ULONGLONG', 'LONGLONG', 'SIZE_T', 'LARGE_INTEGER',
          'HWND', 'HANDLE', 'HMONITOR', 'HDC', 'HMODULE', 'D3D11_RECT_PTR', 'UINT_PTR',
          'ULONG_PTR', 'D3DCOLORVALUE_'}
POINTERISH = {'HWND', 'HANDLE', 'HMONITOR', 'HDC', 'HMODULE', 'SIZE_T', 'UINT_PTR', 'ULONG_PTR'}
# Structs whose pointer fields the wire leaves out (the host sees NULL):
# Present1's dirty and scroll rectangles, hints DXVK does not use.
POINTER_FIELDS_DROPPED = {'DXGI_PRESENT_PARAMETERS'}
BUILTIN_STRUCTS = {
    'RECT': [('LONG', 'left', []), ('LONG', 'top', []), ('LONG', 'right', []), ('LONG', 'bottom', [])],
    'POINT': [('LONG', 'x', []), ('LONG', 'y', [])],
    'GUID': 'raw', 'IID': 'raw',
}


class Gen:
    def __init__(self, inc):
        self.idl = idl.Idl()
        for f in FILES:
            self.idl.read(os.path.join(inc, f))
        for k, v in BUILTIN_STRUCTS.items():
            self.idl.structs.setdefault(k, v)
        self.ifaces = [n for n in self.idl.order if not SKIP.search(n)]
        self.problems = []
        self.structs_used = []
        for n in ('D3D11_BUFFER_DESC', 'D3D11_TEXTURE1D_DESC', 'D3D11_TEXTURE2D_DESC',
                  'D3D11_TEXTURE3D_DESC', 'D3D11_TEXTURE2D_DESC1', 'D3D11_TEXTURE3D_DESC1',
                  'D3D11_BOX'):
            self.use_struct(n)

    # ----------------------------------------------------------- types

    def resolve(self, t):
        seen = 0
        while t in self.idl.typedefs and seen < 8 and t not in WIRE32 | WIRE64 | WIRE16 | WIRE8:
            t = self.idl.typedefs[t]
            seen += 1
        return t

    def width(self, t):
        """Wire bytes of a plain value, or None."""
        r = self.resolve(t)
        if t in WIRE64 or r in WIRE64:
            return 8
        if t in WIRE16 or r in WIRE16:
            return 2
        if t in WIRE8 or r in WIRE8:
            return 1
        if t in WIRE32 or r in WIRE32 or t in self.idl.enums or r in self.idl.enums:
            return 4
        if r in ('unsigned int', 'int', 'long', 'unsigned long', 'float'):
            return 4
        return None

    def is_iface(self, t):
        return t == 'IUnknown' or t in self.idl.interfaces

    def struct(self, t):
        r = self.resolve(t)
        return self.idl.structs.get(t, self.idl.structs.get(r))

    def struct_name(self, t):
        return t if t in self.idl.structs else self.resolve(t)

    def use_struct(self, t):
        n = self.struct_name(t)
        if n in self.structs_used:
            return n
        s = self.idl.structs[n]
        if s == 'opaque':
            self.problems.append(f'struct {n} has pointers in a union')
        elif s != 'raw':
            for ft, fn, dims in s:
                base = ft.replace('const', '').replace('*', '').strip()
                if '*' in ft:
                    if base in ('char', 'CHAR') or ft in ('LPCSTR',) or n in POINTER_FIELDS_DROPPED:
                        continue
                    self.problems.append(f'struct {n}.{fn} is a pointer ({ft})')
                elif ft in ('LPCSTR', 'LPSTR'):
                    continue
                elif self.width(ft) is None:
                    if self.struct(ft) is None:
                        self.problems.append(f'struct {n}.{fn}: unknown type {ft}')
                    else:
                        self.use_struct(ft)
        self.structs_used.append(n)
        return n

    # ----------------------------------------------------------- params

    def classify(self, m, i, p):
        """(kind, info) for parameter i of method m."""
        t = p.type
        key = (m.iface, m.name, p.name)
        base = t.replace('const', '').replace('*', '').strip()
        stars = t.count('*')
        attrs = p.attrs
        dirn = DIRECTION.get(key)
        if not dirn:
            if 'out' in attrs and 'in' in attrs:
                dirn = 'inout'
            elif 'out' in attrs:
                dirn = 'out'
            elif 'in' in attrs:
                dirn = 'in'
            else:
                dirn = None
        if t == 'REFIID' or t == 'REFGUID':
            return ('guid', None)
        if t in ('LPCSTR', 'const char *', 'LPCWSTR', 'const WCHAR *'):
            return ('string16' if 'W' in t else 'string8', None)
        if stars == 0:
            w = self.width(t)
            if w:
                return ('val', w)
            if self.struct(t) is not None:
                return ('structval', self.use_struct(t))
            return ('?', f'value of type {t}')
        if self.is_iface(base) and base != 'IUnknown' and base not in self.ifaces:
            return ('skip', base)
        if self.is_iface(base):
            if stars == 1:
                return ('handle', base)
            if 'const' in t and stars == 2:
                return ('handles_in', (base, self.count(m, i, p)))
            if stars == 2:
                # Only a Create makes a new object, whose handle the guest
                # picks. Every other out returns an object the host may
                # already have a handle for, and answers with that handle.
                if m.name.startswith('Create') or m.name == 'FinishCommandList':
                    return ('new', base)
                count = self.count(m, i, p, quiet=True) if p.name.endswith('s') else None
                return ('handles_out', (base, count or '1'))
        if base == 'void' and stars == 2:
            return ('new_riid', None)
        if base == 'void' and stars == 1:
            size = SIZES.get(key)
            if not size:
                # The size is the parameter that says so: DataSize, data_size.
                for q in m.params:
                    if 'size' in q.name.lower() and q is not p:
                        size = ('blob', f'({q.name} ? *{q.name} : 0)' if '*' in q.type else q.name)
                        break
            if not size:
                return ('?', f'void * {p.name} of unknown size')
            if dirn is None:
                dirn = 'in' if 'const' in t else 'out'
            return ('blob_' + dirn, size[1])
        if stars == 1:
            if dirn is None:
                dirn = 'in' if t.startswith('const') else 'out'
            count = None
            if key in SIZES:
                count = SIZES[key][1]
            elif p.fixed:
                count = p.fixed
            elif p.name.endswith('s') or p.name in ('pViewports', 'pRects', 'Values', 'Color'):
                count = self.count(m, i, p, quiet=True)
            w = self.width(base)
            elem = ('val', w) if w else None
            if not elem and self.struct(base) is not None:
                elem = ('struct', self.use_struct(base))
            if not elem:
                return ('?', f'pointer to {base}')
            return ('ptr_' + dirn, (elem, count, base))
        return ('?', f'type {t}')

    def count(self, m, i, p, quiet=False):
        if p.fixed:
            return p.fixed
        # The nearest count before the parameter, else the first after it.
        for q in list(reversed(m.params[:i])) + m.params[i + 1:]:
            if '*' not in q.type and (q.name.startswith('Num') or q.name.lower().endswith('count')
                                      or q.name.startswith('num')):
                return q.name
            if q.type in ('UINT *',) and (q.name.startswith('pNum') or q.name.endswith('count')):
                return f'({q.name} ? *{q.name} : 0)'
        if not quiet:
            self.problems.append(f'{m.iface}.{m.name}.{p.name}: no count')
        return None

    # ----------------------------------------------------------- methods

    def methods(self):
        """Every method of every carried interface, once, by declaring interface."""
        out = []
        for n in self.ifaces:
            for m in self.idl.interfaces[n].methods:
                out.append(m)
        return out

    def plan(self, m):
        key = f'{m.iface}.{m.name}'
        if key in GUEST:
            return 'guest', None
        if key in UNSUPPORTED:
            return 'unsupported', None
        params = []
        for i, p in enumerate(m.params):
            k, info = self.classify(m, i, p)
            if k == 'skip':
                return 'unsupported', None
            if k == '?':
                self.problems.append(f'{key}: {p.name}: {info}')
                return 'unsupported', None
            params.append((p, k, info))
        outs = [k for _, k, _ in params if k in ('ptr_out', 'ptr_inout', 'blob_out', 'blob_inout', 'handles_out', 'new_riid')]
        news = [k for _, k, _ in params if k == 'new']
        sync = bool(outs) or m.ret not in ('void', 'HRESULT') or key in FORCE_SYNC or len(news) > 1
        return ('sync' if sync else 'async'), params


# --------------------------------------------------------------- writers

def c_params(m):
    return ', '.join([f'{m.iface} *This'] + [f'{p.type} {p.name}' for p in m.params])


def put_size(kind, info, name):
    """C expression for the bytes a parameter takes on the wire."""
    if kind == 'val':
        return str(info)
    if kind in ('handle', 'new', 'new_riid'):
        return '4'
    if kind == 'guid':
        return '16'
    if kind in ('string8', 'string16'):
        return f'dirk_wire_string_size({name}, {1 if kind == "string8" else 2})'
    if kind == 'structval':
        return f'dirk_size_{info}(&{name})'
    if kind == 'handles_in':
        return f'(4 + 4 * (size_t)({info[1]}))'
    if kind == 'handles_out':
        return '4'
    if kind in ('blob_in', 'blob_inout'):
        return f'(8 + (size_t)({info}))'
    if kind == 'blob_out':
        return '8'
    if kind.startswith('ptr_'):
        elem, count, base = info
        n = f'(size_t)({count})' if count else '1'
        if kind == 'ptr_out':
            return '4'
        if elem[0] == 'val':
            return f'(4 + {elem[1]} * {n})'
        return f'(4 + dirk_size_array_{elem[1]}({name}, {n}))'
    raise ValueError(kind)


class Writer:
    def __init__(self, g):
        self.g = g
        self.ops = []

    def op(self, m):
        return f'DIRK_OP_{m.iface}_{m.name}'

    # ---- structs

    def struct_funcs(self, guest):
        out = []
        for n in self.g.structs_used:
            s = self.g.idl.structs[n]
            if s in ('raw', 'opaque'):
                if guest:
                    out.append(f'size_t dirk_size_{n}(const {n} *v) {{ (void)v; return sizeof({n}); }}')
                if guest:
                    out.append(f'uint8_t *dirk_put_{n}(uint8_t *w, const {n} *v) {{ memcpy(w, v, sizeof(*v)); return w + sizeof(*v); }}')
                    out.append(f'const uint8_t *dirk_get_{n}(const uint8_t *r, {n} *v) {{ memcpy(v, r, sizeof(*v)); return r + sizeof(*v); }}')
                else:
                    out.append(f'void dirk_get_{n}(DirkReader &r, {n} *v) {{ r.bytes(v, sizeof(*v)); }}')
                    out.append(f'void dirk_put_{n}(DirkWriter &w, const {n} *v) {{ w.bytes(v, sizeof(*v)); }}')
            else:
                size = ['size_t n = 0;']
                put, get = [], []
                for ft, fn, dims in s:
                    loops = ''
                    idx = ''
                    for d, dim in enumerate(dims):
                        loops += f'for (unsigned i{d} = 0; i{d} < {dim}; i{d}++) '
                        idx += f'[i{d}]'
                    if '*' in ft and 'char' not in ft and n in POINTER_FIELDS_DROPPED:
                        get.append(f'v->{fn} = NULL;')
                        continue
                    if ft in ('LPCSTR', 'const char *', 'LPSTR'):
                        size.append(f'{loops}n += dirk_wire_string_size(v->{fn}{idx}, 1);')
                        put.append(f'{loops}w = dirk_put_string(w, v->{fn}{idx}, 1);')
                        get.append(f'{loops}v->{fn}{idx} = r.string();')
                        continue
                    wdt = self.g.width(ft)
                    if wdt:
                        size.append(f'{loops}n += {wdt};')
                        put.append(f'{loops}w = dirk_put_val(w, &v->{fn}{idx}, sizeof(v->{fn}{idx}), {wdt});')
                        get.append(f'{loops}r.val(&v->{fn}{idx}, sizeof(v->{fn}{idx}), {wdt});')
                    else:
                        sn = self.g.struct_name(ft)
                        size.append(f'{loops}n += dirk_size_{sn}(&v->{fn}{idx});')
                        put.append(f'{loops}w = dirk_put_{sn}(w, &v->{fn}{idx});')
                        get.append(f'{loops}dirk_get_{sn}(r, &v->{fn}{idx});')
                if guest:
                    out.append(f'size_t dirk_size_{n}(const {n} *v) {{ {" ".join(size)} return n; }}')
                    out.append(f'uint8_t *dirk_put_{n}(uint8_t *w, const {n} *v) {{ {" ".join(put)} return w; }}')
                    gget = [x.replace('r.val(', 'r = dirk_get_val(r, ').replace('dirk_get_', 'r = dirk_get_', 1) if False else x for x in get]
                    out.append(self.guest_get_struct(n, s))
                else:
                    out.append(f'void dirk_get_{n}(DirkReader &r, {n} *v) {{ {" ".join(get)} }}')
                    hput = [x.replace('w = dirk_put_val(w, ', 'w.val(').replace('w = dirk_put_string(w, ', 'w.string(')
                            for x in put]
                    hput = [re.sub(r'w = dirk_put_(\w+)\(w, ', r'dirk_put_\1(w, ', x) for x in hput]
                    out.append(f'void dirk_put_{n}(DirkWriter &w, const {n} *v) {{ {" ".join(hput)} }}')
            if guest:
                out.append(f'static size_t dirk_size_array_{n}(const {n} *v, size_t count) {{ size_t n = 0; '
                           f'for (size_t i = 0; v && i < count; i++) n += dirk_size_{n}(&v[i]); return n; }}')
        return '\n'.join(out)

    def guest_get_struct(self, n, s):
        get = []
        for ft, fn, dims in s:
            loops = ''
            idx = ''
            for d, dim in enumerate(dims):
                loops += f'for (unsigned i{d} = 0; i{d} < {dim}; i{d}++) '
                idx += f'[i{d}]'
            if '*' in ft and 'char' not in ft and n in POINTER_FIELDS_DROPPED:
                get.append(f'v->{fn} = NULL;')
                continue
            if ft in ('LPCSTR', 'const char *', 'LPSTR'):
                get.append(f'{loops}{{ v->{fn}{idx} = NULL; r = dirk_skip_string(r); }}')
                continue
            wdt = self.g.width(ft)
            if wdt:
                get.append(f'{loops}r = dirk_get_val(r, &v->{fn}{idx}, sizeof(v->{fn}{idx}), {wdt});')
            else:
                get.append(f'{loops}r = dirk_get_{self.g.struct_name(ft)}(r, &v->{fn}{idx});')
        return f'const uint8_t *dirk_get_{n}(const uint8_t *r, {n} *v) {{ {" ".join(get)} return r; }}'

    # ---- guest

    def guest_method(self, m, mode, params):
        key = f'{m.iface}.{m.name}'
        fn = f'dirk_w_{m.iface}_{m.name}'
        L = []
        L.append(f'static {m.ret} STDMETHODCALLTYPE {fn}({c_params(m)})')
        L.append('{')
        if mode == 'unsupported':
            for p in m.params:
                L.append(f'    (void){p.name};')
            L.append(f'    {{ static int seen_; if (!seen_) {{ seen_ = 1; dirk_unsupported(This, "{key}"); }} }}')
            if m.ret == 'HRESULT':
                L.append('    return E_NOTIMPL;')
            elif m.ret != 'void':
                L.append(f'    return ({m.ret})0;')
            L.append('}')
            return L
        self.ops.append(m)
        if key in WINDOW:
            L.append(f'    dirk_window_seen({WINDOW[key]});')
        if key in WINDOW_CHECK:
            L.append('    dirk_windows_check();')
        L.append('    struct dirk_proxy *self = dirk_proxy_of(This);')
        L.append(f'    {{ static int seen_; if (dirk_trace_on && !seen_) {{ seen_ = 1; dirk_trace_first("{key}"); }} }}')
        size = ' + '.join(['DIRK_REC_HEADER'] + [put_size(k, info, p.name) for p, k, info in params])
        L.append(f'    size_t size = {size};')
        news = [(p, info) for p, k, info in params if k == 'new']
        sync = mode == 'sync'
        if news and not sync:
            p, base = news[0]
            # A NULL out pointer asks only whether the call would succeed.
            L.append(f'    int wait = {p.name} == NULL;')
        else:
            L.append(f'    int wait = {1 if sync else 0};')
        L.append(f'    uint8_t *w = dirk_begin(self, {self.op(m)}, size, wait);')
        L.append('    if (!w)')
        L.append('        return ' + ({'HRESULT': 'DXGI_ERROR_DEVICE_REMOVED', 'void': ''}.get(m.ret, f'({m.ret})0')) + ';')
        created = []
        for p, k, info in params:
            n = p.name
            if k == 'val':
                L.append(f'    w = dirk_put_val(w, &{n}, sizeof({n}), {info});')
            elif k == 'guid':
                L.append(f'    memcpy(w, {n}, 16); w += 16;')
            elif k in ('string8', 'string16'):
                L.append(f'    w = dirk_put_string(w, {n}, {1 if k == "string8" else 2});')
            elif k == 'structval':
                L.append(f'    w = dirk_put_{info}(w, &{n});')
            elif k == 'handle':
                L.append(f'    w = dirk_put_u32(w, dirk_handle_of((void *){n}));')
            elif k == 'handles_in':
                L.append(f'    w = dirk_put_handles(w, (void *const *){n}, {info[1]});')
            elif k == 'handles_out':
                L.append(f'    w = dirk_put_u32(w, {n} ? (uint32_t)({info[1]}) : ~0u);')
            elif k == 'new':
                L.append(f'    struct dirk_proxy *new_{n} = {n} ? dirk_proxy_new(self, DIRK_IFACE_{info}) : NULL;')
                L.append(f'    w = dirk_put_u32(w, new_{n} ? new_{n}->handle : 0);')
                created.append((n, info))
            elif k == 'new_riid':
                L.append(f'    w = dirk_put_u32(w, {n} != NULL);')
            elif k in ('blob_in', 'blob_inout'):
                L.append(f'    w = dirk_put_blob(w, {n}, (size_t)({info}));')
            elif k == 'blob_out':
                L.append(f'    w = dirk_put_u64(w, {n} ? (uint64_t)({info}) : ~(uint64_t)0);')
            elif k.startswith('ptr_'):
                elem, count, base = info
                cnt = f'(size_t)({count})' if count else '1'
                if k == 'ptr_out':
                    L.append(f'    w = dirk_put_u32(w, {n} ? (uint32_t){cnt} : ~0u);')
                elif elem[0] == 'val':
                    L.append(f'    w = dirk_put_vals(w, {n}, sizeof(*{n}), {elem[1]}, {cnt});')
                else:
                    L.append(f'    w = dirk_put_u32(w, {n} ? (uint32_t){cnt} : ~0u);')
                    L.append(f'    for (size_t i = 0; {n} && i < {cnt}; i++) w = dirk_put_{elem[1]}(w, &{n}[i]);')
        L.append('    const uint8_t *r = dirk_end(self, w);')
        ret = m.ret
        if key in FLUSH:
            L.append('    dirk_flush(self);')
        if not sync and not news:
            L.append('    (void)r;')
            if ret == 'HRESULT':
                L.append('    return S_OK;')
            L.append('}')
            return L
        # The reply: the result, then each out parameter in order.
        if not sync:
            # A create that does not wait: the proxy is the object.
            p, base = news[0]
            L.append('    if (!wait) {')
            L.append(f'        *{p.name} = ({base} *)new_{p.name};')
            L.append('        return S_OK;')
            L.append('    }')
        L.append('    if (!r) {')
        for n, base in created:
            L.append(f'        if (new_{n}) dirk_proxy_drop(new_{n});')
            L.append(f'        if ({n}) *{n} = NULL;')
        L.append('        return ' + ({'HRESULT': 'DXGI_ERROR_DEVICE_REMOVED', 'void': ''}.get(ret, f'({ret})0')) + ';')
        L.append('    }')
        if ret != 'void':
            L.append(f'    {ret} result;')
            L.append(f'    r = dirk_get_val(r, &result, sizeof(result), 8);')
        else:
            L.append('    r = dirk_skip(r, 8);')
        riid = None
        for p, k, info in params:
            n = p.name
            if k == 'guid':
                riid = n
            if k == 'new' and sync:
                L.append(f'    if (new_{n}) {{ uint32_t ok; r = dirk_get_val(r, &ok, 4, 4);'
                         f' if (!ok) {{ dirk_proxy_drop(new_{n}); new_{n} = NULL; }} }}')
                L.append(f'    if ({n}) *{n} = ({info} *)new_{n};')
            elif k in ('ptr_out', 'ptr_inout'):
                elem, count, base = info
                cnt = f'(size_t)({count})' if count else '1'
                if elem[0] == 'val':
                    L.append(f'    r = dirk_get_vals(r, {n}, sizeof(*{n}), {elem[1]}, {cnt});')
                else:
                    L.append(f'    {{ uint32_t c_; r = dirk_get_val(r, &c_, 4, 4);'
                             f' for (uint32_t i = 0; i < c_; i++) {{ if ({n}) r = dirk_get_{elem[1]}(r, &{n}[i]);'
                             f' else {{ {base} t_; r = dirk_get_{elem[1]}(r, &t_); }} }} }}')
            elif k in ('blob_out', 'blob_inout'):
                L.append(f'    r = dirk_get_blob(r, {n}, (size_t)({info}));')
            elif k == 'handles_out':
                L.append(f'    r = dirk_get_handles(r, self, (void **){n}, DIRK_IFACE_{info[0]});')
            elif k == 'new_riid':
                L.append(f'    r = dirk_get_riid_handle(r, self, {riid}, {n});')
        L.append('    dirk_reply_done(self);')
        if key in TRACE:
            L.append(f'    if (dirk_trace_on) dirk_trace("{TRACE[key][0]}", {TRACE[key][1]});')
        if ret != 'void':
            L.append('    return result;')
        L.append('}')
        return L

    def guest(self):
        g = self.g
        L = ['/* Generated by src/dirk/wire/gen.py from Wine\'s D3D11 and DXGI IDL. */',
             '#include "dirk_guest.h"', '']
        L.append(self.struct_funcs(True))
        L.append('')
        bodies = []
        for m in g.methods():
            mode, params = g.plan(m)
            if mode == 'guest':
                bodies.append(f'{m.ret} STDMETHODCALLTYPE dirk_g_{m.iface}_{m.name}({c_params(m)});')
                continue
            bodies.extend(self.guest_method(m, mode, params))
            bodies.append('')
        L.extend(bodies)
        # vtables: IUnknown's three, then each interface of the chain in order.
        for n in g.ifaces:
            slots = ['(void *)dirk_g_IUnknown_QueryInterface', '(void *)dirk_g_IUnknown_AddRef',
                     '(void *)dirk_g_IUnknown_Release']
            for itf in g.idl.chain(n):
                for m in itf.methods:
                    key = f'{m.iface}.{m.name}'
                    slots.append(f'(void *)dirk_{"g" if key in GUEST else "w"}_{m.iface}_{m.name}')
            L.append(f'static void *const dirk_vtbl_{n}[] = {{')
            L.extend(f'    {s},' for s in slots)
            L.append('};')
            L.append(f'_Static_assert(sizeof(dirk_vtbl_{n}) == sizeof({n}Vtbl), "{n}\'s vtable");')
        L.append('')
        L.append('const struct dirk_iface dirk_ifaces[DIRK_IFACE_COUNT] = {')
        for n in g.ifaces:
            itf = g.idl.interfaces[n]
            u = itf.uuid.split('-')
            d4 = u[3] + u[4]
            guid = (f'{{0x{u[0]}, 0x{u[1]}, 0x{u[2]}, {{' +
                    ', '.join(f'0x{d4[i:i + 2]}' for i in range(0, 16, 2)) + '}}')
            base = g.idl.interfaces[n].base
            bidx = f'DIRK_IFACE_{base}' if base in g.ifaces else '-1'
            L.append(f'    {{ "{n}", {guid}, dirk_vtbl_{n}, {bidx} }},')
        L.append('};')
        return '\n'.join(L) + '\n'

    # ---- host

    def host_method(self, m, mode, params):
        L = [f'  case {self.op(m)}: {{']
        L.append(f'    auto *self = c.obj<{m.iface}>(rec.handle);')
        args = []
        post = []
        riid = None
        for p, k, info in params:
            n = p.name
            if k == 'val':
                L.append(f'    {p.type} {n}; r.val(&{n}, sizeof({n}), {info});')
                args.append(n)
            elif k == 'guid':
                L.append(f'    GUID {n}_; r.bytes(&{n}_, 16);')
                args.append(f'{n}_')
                riid = f'{n}_'
            elif k in ('string8', 'string16'):
                L.append(f'    auto {n} = r.{"string" if k == "string8" else "wstring"}();')
                args.append(n)
            elif k == 'structval':
                L.append(f'    {info} {n}; dirk_get_{info}(r, &{n});')
                args.append(n)
            elif k == 'handle':
                L.append(f'    auto *{n} = c.obj<{info}>(r.u32());')
                args.append(n)
            elif k == 'handles_in':
                L.append(f'    auto {n} = c.objs<{info[0]}>(r);')
                args.append(n)
            elif k == 'handles_out':
                L.append(f'    uint32_t {n}_n = r.u32(); {info[0]} **{n} = c.out_objs<{info[0]}>({n}_n);')
                args.append(n)
                post.append(f'    w.out_objs(c, {n}, {n}_n, DIRK_IFACE_{info[0]});')
            elif k == 'new':
                L.append(f'    uint32_t {n}_h = r.u32(); {info} *{n}_p = nullptr;')
                args.append(f'{n}_h ? &{n}_p : nullptr')
                post.append(f'    if ({n}_h) c.bind({n}_h, {n}_p, DIRK_IFACE_{info});')
                if mode == 'sync':
                    post.append(f'    if ({n}_h) w.u32({n}_p != nullptr);')
            elif k == 'new_riid':
                L.append(f'    bool {n}_want = r.u32() != 0; void *{n}_p = nullptr;')
                args.append(f'{n}_want ? &{n}_p : nullptr')
                post.append(f'    w.new_riid(c, {riid}, {n}_p);')
            elif k == 'blob_in':
                L.append(f'    const void *{n} = r.blob();')
                args.append(n)
            elif k == 'blob_inout':
                L.append(f'    uint64_t {n}_n; const void *{n}_in = r.blob(&{n}_n); void *{n} = c.copy({n}_in, {n}_n);')
                args.append(n)
                post.append(f'    w.blob({n}, {n}_n);')
            elif k == 'blob_out':
                L.append(f'    uint64_t {n}_n = r.u64(); void *{n} = c.scratch({n}_n);')
                args.append(n)
                post.append(f'    w.blob({n}, {n}_n);')
            elif k.startswith('ptr_'):
                elem, count, base = info
                if k == 'ptr_out':
                    L.append(f'    uint32_t {n}_n = r.u32(); {base} *{n} = c.out_array<{base}>({n}_n);')
                    args.append(n)
                    if elem[0] == 'val':
                        post.append(f'    w.vals({n}, sizeof({base}), {elem[1]}, {n} ? {n}_n : 0);')
                    else:
                        post.append(f'    w.u32({n} ? {n}_n : 0); for (uint32_t i = 0; {n} && i < {n}_n; i++) dirk_put_{elem[1]}(w, &{n}[i]);')
                elif elem[0] == 'val':
                    L.append(f'    auto *{n} = r.vals<{base}>(c, {elem[1]});')
                    args.append(n)
                    if k == 'ptr_inout':
                        post.append(f'    w.vals({n}, sizeof({base}), {elem[1]}, {n} ? 1 : 0);')
                else:
                    L.append(f'    uint32_t {n}_n = r.u32(); {base} *{n} = c.out_array<{base}>({n}_n);')
                    L.append(f'    for (uint32_t i = 0; {n} && i < {n}_n; i++) dirk_get_{elem[1]}(r, &{n}[i]);')
                    args.append(n)
        L.append('    if (!self) { c.missing(rec); break; }')
        call = f'self->{m.name}({", ".join(args)})'
        if mode == 'sync':
            if m.ret == 'void':
                L.append(f'    {call};')
                L.append('    w.u64(0);')
            else:
                L.append(f'    {m.ret} result = {call};')
                L.append('    w.val(&result, sizeof(result), 8);')
            L.extend(post)
        else:
            if m.ret == 'HRESULT':
                L.append(f'    HRESULT result = {call};')
                L.append(f'    if (FAILED(result)) c.failed(rec, result);')
                L.append(f'    if (rec.wait) w.val(&result, sizeof(result), 8);')
            else:
                L.append(f'    {call};')
            L.extend(p for p in post if 'w.' not in p)
        L.append('    break;')
        L.append('  }')
        return L

    def host(self):
        g = self.g
        L = ['// Generated by src/dirk/wire/gen.py from Wine\'s D3D11 and DXGI IDL.',
             '#include "dirk_host.h"', '', 'namespace dirk {', '']
        L.append(self.struct_funcs(False))
        L.append('')
        L.append('bool decode_generated(Client &c, const Record &rec, DirkReader &r, DirkWriter &w) {')
        L.append('  switch (rec.op) {')
        for m in g.methods():
            mode, params = g.plan(m)
            if mode in ('guest', 'unsupported'):
                continue
            L.extend(self.host_method(m, mode, params))
        L.append('  default:')
        L.append('    return false;')
        L.append('  }')
        L.append('  return true;')
        L.append('}')
        L.append('')
        L.append('}')
        return '\n'.join(L) + '\n'

    def ops_h(self):
        g = self.g
        L = ['/* Generated by src/dirk/wire/gen.py from Wine\'s D3D11 and DXGI IDL. */',
             '#ifndef DIRK_WIRE_OPS_H', '#define DIRK_WIRE_OPS_H', '',
             'enum dirk_iface_id {']
        L.extend(f'    DIRK_IFACE_{n},' for n in g.ifaces)
        L.append('    DIRK_IFACE_COUNT')
        L.append('};')
        L.append('')
        L.append('enum dirk_op {')
        L.append('    DIRK_OP_GENERATED = 0x100,')
        seen = set()
        for m in self.ops:
            if self.op(m) not in seen:
                seen.add(self.op(m))
                L.append(f'    {self.op(m)},')
        L.append('    DIRK_OP_GENERATED_END')
        L.append('};')
        L.append('')
        L.append('#endif')
        return '\n'.join(L) + '\n'


def main():
    inc, out = sys.argv[1], sys.argv[2]
    g = Gen(inc)
    for m in g.methods():
        g.plan(m)
    w = Writer(g)
    guest = w.guest()
    host = w.host()
    ops = w.ops_h()
    os.makedirs(out, exist_ok=True)
    for name, text in (('dirk_wire_guest.c', guest), ('dirk_wire_host.cpp', host), ('dirk_wire_ops.h', ops)):
        with open(os.path.join(out, name), 'w') as f:
            f.write(text)
    for p in sorted(set(g.problems)):
        print('gen.py: ' + p, file=sys.stderr)


if __name__ == '__main__':
    main()
