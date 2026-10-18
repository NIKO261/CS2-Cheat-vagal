"""Execute extracted rendering/input production C++ with deterministic COM/input stubs.

Only the small regression harness is compiled; this script never builds or runs the DLL.
Use --root to test an independent source/rollback checkout.
"""
from __future__ import annotations

import argparse
import hashlib
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile


def body(source: str, marker: str) -> str:
    """Extract one real C++ function/block, ignoring braces in comments/literals."""
    start = source.index("{", source.index(marker))
    token = re.compile(r'//[^\n]*|/\*[\s\S]*?\*/|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|[{}]')
    depth = 0
    for match in token.finditer(source, start):
        if match[0] == "{":
            depth += 1
        elif match[0] == "}":
            depth -= 1
            if depth == 0:
                return source[start:match.end()]
    raise ValueError(f"Unbalanced source after {marker!r}")


STUBS = r'''
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>
#define __fastcall
#define __uuidof(type) 0
#define SUCCEEDED(value) ((value) >= 0)
using HRESULT = long;
using UINT = unsigned;
using DXGI_FORMAT = int;
constexpr int VK_LEFT=1, VK_RIGHT=2, VK_HOME=3, VK_END=4, VK_BACK=5,
    VK_DELETE=6, VK_ESCAPE=7, VK_RETURN=8;
constexpr std::uintptr_t null_id=0;
struct D3D11_TEXTURE2D_DESC { unsigned Width{1280}, Height{720}; };
struct D3D11_VIEWPORT { float TopLeftX{},TopLeftY{},Width{},Height{},MinDepth{},MaxDepth{}; };
struct ID3D11Texture2D {
    void GetDesc(D3D11_TEXTURE2D_DESC* out) { *out={}; }
    void Release() {}
};
struct ID3D11RenderTargetView {
    int refs{1};
    void Release() { --refs; }
};
struct Device {
    std::vector<ID3D11RenderTargetView*> allocated;
    ~Device() { for(auto* target:allocated) delete target; }
    HRESULT CreateRenderTargetView(ID3D11Texture2D*, void*, ID3D11RenderTargetView** out) {
        *out=new ID3D11RenderTargetView;
        allocated.push_back(*out);
        return 0;
    }
};
struct DeviceContext {
    ID3D11RenderTargetView* bound{};
    void OMSetRenderTargets(unsigned count, ID3D11RenderTargetView* const* targets, void*) {
        if(bound) bound->Release();
        bound=count && targets ? targets[0] : nullptr;
        if(bound) ++bound->refs;
    }
    void RSSetViewports(unsigned, const D3D11_VIEWPORT*) {}
};
template<class T> struct checked_pointer {
    T* value{};
    checked_pointer& operator=(T* pointer) { value=pointer; return *this; }
    explicit operator bool() const { return value!=nullptr; }
    T* operator->() const {
        if(!value) throw std::runtime_error("null device/context dereference");
        return value;
    }
};
struct IDXGISwapChain {
    bool buffer_available{true};
    ID3D11Texture2D buffer;
    HRESULT GetBuffer(unsigned, int, void** out) {
        *out=buffer_available ? &buffer : nullptr;
        return buffer_available ? 0 : -1;
    }
};
namespace rendering {
struct context {
    Device device;
    DeviceContext device_context;
    checked_pointer<Device> m_device;
    checked_pointer<DeviceContext> m_context;
    ID3D11RenderTargetView* m_rtv{};
    bool m_initialized{};
    bool initialize(IDXGISwapChain* chain) {
        m_device=&device; m_context=&device_context;
        create_rtv(chain); m_initialized=true; return true;
    }
    void on_present(IDXGISwapChain*);
    void on_resize_buffers();
    void on_resize_buffers_post(IDXGISwapChain*);
    void create_rtv(IDXGISwapChain*);
};
context g_context;
}
struct OriginalResize {
    long result{-1};
    template<class T, class... Args> T call(Args...) { return static_cast<T>(result); }
};
struct cheat {
    OriginalResize m_resize_buffers;
    HRESULT resize_buffers(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
};
struct text_input_state {
    std::size_t cursor{},sel_start{},sel_end{};
    float blink_timer{},scroll_offset{};
    std::unordered_map<int,float> key_repeat_timers;
    std::unordered_map<int,bool> key_was_down;
};
struct Input {
    std::vector<int> presses;
    std::wstring characters;
    bool ctrl{},shift{};
    bool shift_held() const { return shift; }
    bool ctrl_held() const { return ctrl; }
    const auto& key_presses() const { return presses; }
    bool key_down(int) const { return false; }
    const auto& chars() const { return characters; }
};
namespace xdraw { float delta_time() { return 1.0f/60.0f; } }
struct UiContext { std::uintptr_t active_text_input{1}; };
'''

TESTS = r'''
int total{},passed{};
void check(const char* name, bool success, const std::string& detail="") {
    ++total; passed+=success;
    std::cout<<(success ? "PASS " : "FAIL ")<<name;
    if(!detail.empty()) std::cout<<" | "<<detail;
    std::cout<<'\n';
}
void input_case(const char* name, std::string value, std::size_t start, std::size_t end,
                std::wstring chars, std::size_t limit, const std::string& expected,
                std::vector<int> keys={}, bool ctrl=false) {
    text_input_state state; state.cursor=end; state.sel_start=start; state.sel_end=end;
    Input input{keys,chars,ctrl};
    try {
        edit(value,state,input,limit);
        check(name,value==expected,"actual='"+value+"'; expected='"+expected+"'");
    } catch(const std::exception& error) { check(name,false,error.what()); }
}
int main() {
    IDXGISwapChain chain;
    {
        rendering::context context;
        try {
            context.on_resize_buffers_post(&chain);
            check("resize_before_first_present", !context.m_rtv && !context.m_initialized);
        } catch(const std::exception& error) { check("resize_before_first_present",false,error.what()); }
    }
    {
        rendering::context context; context.initialize(&chain);
        context.m_context->OMSetRenderTargets(1,&context.m_rtv,nullptr);
        context.on_resize_buffers();
        check("resize_releases_bound_backbuffer",!context.device_context.bound && !context.m_rtv);
    }
    {
        auto& context=rendering::g_context; context.initialize(&chain);
        cheat hook; hook.m_resize_buffers.result=-1;
        const auto result=hook.resize_buffers(&chain,0,800,600,0,0);
        context.on_present(&chain);
        check("failed_resize_recovers_next_present",result==-1 && context.m_rtv);
        context.on_resize_buffers();
    }
    {
        rendering::context context; context.initialize(&chain);
        context.on_resize_buffers(); chain.buffer_available=false;
        context.on_resize_buffers_post(&chain);
        check("missing_buffer_keeps_rtv_null",!context.m_rtv);
        chain.buffer_available=true;
        context.on_present(&chain);
        check("transient_buffer_failure_retries",context.m_rtv!=nullptr);
        context.on_resize_buffers();
        context.on_resize_buffers_post(&chain);
        check("successful_resize_recreates_rtv",context.m_rtv!=nullptr);
    }
    input_case("selection_multi_character_batch","abcdef",4,6,L"XY",64,"abcdXY");
    input_case("selection_replace_at_capacity","abcd",0,4,L"X",4,"X");
    input_case("ctrl_a_and_text_same_frame","abcd",4,4,L"XY",64,"XY",{'A'},true);
    input_case("selection_delete_then_text_same_frame","abcdef",4,6,L"XY",64,"abcdXY",{VK_BACK});
    input_case("external_buffer_shrink_clamps_cursor","a",6,6,L"X",64,"aX");
    input_case("ordinary_insert_preserved","abc",3,3,L"XY",64,"abcXY");
    input_case("ordinary_capacity_preserved","abcd",4,4,L"X",4,"abcd");
    input_case("selection_backspace_preserved","abcdef",2,4,L"",64,"abef",{VK_BACK});
    input_case("selection_delete_preserved","abcdef",2,4,L"",64,"abef",{VK_DELETE});
    input_case("ordinary_left_insert_preserved","abcd",4,4,L"X",64,"abcXd",{VK_LEFT});
    std::cout<<"RENDERING: "<<passed<<'/'<<total<<"; EXTRACTED C++ + DETERMINISTIC STUBS; NO DLL/GAME RUN\n";
    return passed==total ? 0 : 1;
}
'''


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[1])
    args = parser.parse_args()
    paths = ["firgune/project/core/rendering/impl/context.cpp",
             "firgune/project/core/hooks/impl/cheat.cpp",
             "firgune/project/external/xdraw/xui/xui.cpp"]
    sources = [(args.root / path).read_text(encoding="utf-8-sig") for path in paths]
    print("Source root:", args.root.resolve(), flush=True)
    for path in paths:
        print("SHA256", path, hashlib.sha256((args.root / path).read_bytes()).hexdigest(), flush=True)
    context, hooks, ui = sources
    present = body(context, "void context::on_present(")
    # Keep the exact initialization/retry control flow; omit unrelated feature drawing.
    present = present[:present.index("this->try_bind_ui_assets(")] + "\n}"
    methods = "\nnamespace rendering {\nvoid context::on_present(IDXGISwapChain* swap_chain) " + present
    for name, params in [("on_resize_buffers", ""),
                         ("on_resize_buffers_post", "IDXGISwapChain* swap_chain"),
                         ("create_rtv", "IDXGISwapChain* swap_chain")]:
        methods += f"\nvoid context::{name}({params}) " + body(context, f"void context::{name}(")
    methods += "\n}\nHRESULT cheat::resize_buffers(IDXGISwapChain* thisptr, UINT buffer_count, UINT width, UINT height, DXGI_FORMAT new_format, UINT swap_chain_flags) "
    methods += body(hooks, "HRESULT __fastcall cheat::resize_buffers(")
    edit_source = body(ui, "bool text_input(")
    edit_source = body(edit_source, "\n\t\tif ( is_active )\n")
    methods += "\nvoid edit(std::string& buf,text_input_state& st,const Input& input,std::size_t max_len) { bool is_active=true,changed=false; UiContext c; if(is_active) " + edit_source + "\n}\n"
    source = STUBS + methods + TESTS
    vswhere = Path(os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)")) / "Microsoft Visual Studio/Installer/vswhere.exe"
    result = subprocess.run([str(vswhere), "-latest", "-products", "*", "-requires",
                             "Microsoft.VisualStudio.Component.VC.Tools.x86.x64", "-property", "installationPath"],
                            capture_output=True, text=True, check=True)
    vcvars = Path(result.stdout.strip()) / "VC/Auxiliary/Build/vcvars64.bat"
    if not vcvars.is_file():
        raise RuntimeError("MSVC vcvars64.bat was not found")
    with tempfile.TemporaryDirectory(prefix="vagal-rendering-regression-") as temp:
        temp = Path(temp)
        cpp, exe = temp / "rendering.cpp", temp / "rendering.exe"
        cpp.write_text(source, encoding="utf-8")
        command = f'call "{vcvars}" >nul && cl.exe /nologo /std:c++20 /EHsc /Od /W3 /Fe:"{exe}" /Fo:"{temp / "rendering.obj"}" "{cpp}"'
        batch = temp / "compile.bat"
        batch.write_text("@echo off\n" + command + "\n", encoding="utf-8")
        compiled = subprocess.run(["cmd.exe", "/d", "/c", str(batch)], cwd=temp,
                                  capture_output=True, text=True)
        if compiled.returncode:
            print(compiled.stdout, end="")
            print(compiled.stderr, end="", file=sys.stderr)
            return compiled.returncode
        print("HARNESS COMPILE: exit=0; production function bodies extracted unchanged", flush=True)
        return subprocess.run([str(exe)], cwd=temp).returncode


if __name__ == "__main__":
    raise SystemExit(main())
