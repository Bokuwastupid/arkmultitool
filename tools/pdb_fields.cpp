// Dumps field offsets out of ShooterGame.pdb.
//
// The offsets table in include/kopt/runtime.hpp cites a "PDB/DIA dump" that
// lives on the Linux side of this project and is not in the repository. The
// PDB itself ships with the game, so the dump is reproducible on any machine
// with the same build installed -- this is that reproduction, using DbgHelp
// (already linked by the payload) instead of the DIA SDK, which is not
// installed here.
//
// Usage:
//   pdb_fields.exe <ShooterGame.exe path> <TypeName> [TypeName ...]
//
// Prints one line per member: "<TypeName>::<Member> offset=0x... size=... kind".
// Matching is exact on the type name; a type that resolves to nothing is
// reported as such rather than silently skipped, so a typo does not read as
// "the field does not exist".

#include <windows.h>
#include <dbghelp.h>

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#pragma comment(lib, "dbghelp.lib")

namespace
{
    // DbgHelp hands back type names as BSTR-ish wide strings it allocated with
    // LocalAlloc; every successful TI_GET_SYMNAME has to be freed by the caller.
    std::wstring type_name(const DWORD64 base, const ULONG index)
    {
        WCHAR* raw = nullptr;
        if (!SymGetTypeInfo(GetCurrentProcess(), base, index, TI_GET_SYMNAME, &raw) || raw == nullptr)
            return {};
        std::wstring name(raw);
        LocalFree(raw);
        return name;
    }

    ULONG64 type_size(const DWORD64 base, const ULONG index)
    {
        ULONG64 size{};
        if (!SymGetTypeInfo(GetCurrentProcess(), base, index, TI_GET_LENGTH, &size)) return 0;
        return size;
    }

    void dump_type(const DWORD64 base, const ULONG type_index, const std::wstring& label)
    {
        DWORD child_count{};
        if (!SymGetTypeInfo(GetCurrentProcess(), base, type_index, TI_GET_CHILDRENCOUNT, &child_count) ||
            child_count == 0)
        {
            std::wprintf(L"%s: no members\n", label.c_str());
            return;
        }
        // TI_FINDCHILDREN_PARAMS is a variable-length struct: the fixed header
        // plus child_count trailing ULONGs, so it has to be allocated as raw
        // bytes rather than declared.
        std::vector<std::byte> storage(sizeof(TI_FINDCHILDREN_PARAMS) + child_count * sizeof(ULONG));
        auto* params = reinterpret_cast<TI_FINDCHILDREN_PARAMS*>(storage.data());
        params->Count = child_count;
        params->Start = 0;
        if (!SymGetTypeInfo(GetCurrentProcess(), base, type_index, TI_FINDCHILDREN, params))
        {
            std::wprintf(L"%s: TI_FINDCHILDREN failed (%lu)\n", label.c_str(), GetLastError());
            return;
        }
        for (DWORD i = 0; i < child_count; ++i)
        {
            const ULONG child = params->ChildId[i];
            DWORD offset{};
            // Members carry an offset; static members, methods and nested types
            // do not, and are skipped rather than printed with a bogus 0.
            if (!SymGetTypeInfo(GetCurrentProcess(), base, child, TI_GET_OFFSET, &offset)) continue;
            const std::wstring member = type_name(base, child);
            if (member.empty()) continue;
            ULONG member_type{};
            std::wstring member_type_name;
            ULONG64 member_size{};
            if (SymGetTypeInfo(GetCurrentProcess(), base, child, TI_GET_TYPEID, &member_type))
            {
                member_type_name = type_name(base, member_type);
                member_size = type_size(base, member_type);
            }
            std::wprintf(L"%s::%s offset=0x%X size=%llu type=%s\n", label.c_str(), member.c_str(),
                offset, member_size, member_type_name.empty() ? L"?" : member_type_name.c_str());
        }
    }

    struct EnumContext
    {
        DWORD64 base{};
        const std::vector<std::wstring>* wanted{};
        std::vector<std::wstring> matched;
    };

    BOOL CALLBACK on_type(PSYMBOL_INFOW symbol, ULONG, PVOID user)
    {
        auto* context = static_cast<EnumContext*>(user);
        const std::wstring name(symbol->Name);
        for (const std::wstring& wanted : *context->wanted)
        {
            if (name != wanted) continue;
            std::wprintf(L"\n=== %s (size=%llu) ===\n", name.c_str(), symbol->Size);
            dump_type(context->base, symbol->TypeIndex, name);
            context->matched.push_back(name);
        }
        return TRUE;
    }
}

int wmain(int argc, wchar_t** argv)
{
    if (argc < 3)
    {
        std::wprintf(L"usage: pdb_fields.exe <ShooterGame.exe> <TypeName> [TypeName ...]\n");
        return 2;
    }
    const HANDLE process = GetCurrentProcess();
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES);
    if (!SymInitializeW(process, nullptr, FALSE))
    {
        std::wprintf(L"SymInitialize failed (%lu)\n", GetLastError());
        return 1;
    }
    // A load address of 0 lets DbgHelp pick one; the module is never executed,
    // only its symbols are read, so the address is irrelevant here.
    const DWORD64 base = SymLoadModuleExW(process, nullptr, argv[1], nullptr, 0, 0, nullptr, 0);
    if (base == 0)
    {
        std::wprintf(L"SymLoadModuleEx failed (%lu) -- is ShooterGame.pdb next to the exe?\n",
            GetLastError());
        SymCleanup(process);
        return 1;
    }
    std::vector<std::wstring> wanted;
    for (int i = 2; i < argc; ++i) wanted.emplace_back(argv[i]);
    EnumContext context{base, &wanted, {}};
    // The symbol enumeration over a 300 MB PDB takes a while; there is no
    // narrower entry point that also resolves UE4's mangled type names.
    if (!SymEnumTypesW(process, base, on_type, &context))
        std::wprintf(L"SymEnumTypes failed (%lu)\n", GetLastError());
    for (const std::wstring& name : wanted)
    {
        bool found = false;
        for (const std::wstring& matched : context.matched) found = found || matched == name;
        if (!found) std::wprintf(L"\n=== %s: NOT FOUND in this PDB ===\n", name.c_str());
    }
    SymCleanup(process);
    return 0;
}
