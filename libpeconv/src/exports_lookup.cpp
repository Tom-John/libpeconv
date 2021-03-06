#include "peconv/exports_lookup.h"

#include <iostream>

/*
typedef struct _IMAGE_EXPORT_DIRECTORY {
    DWORD   Characteristics;
    DWORD   TimeDateStamp;
    WORD    MajorVersion;
    WORD    MinorVersion;
    DWORD   Name;
    DWORD   Base;
    DWORD   NumberOfFunctions;
    DWORD   NumberOfNames;
    DWORD   AddressOfFunctions;     // RVA from base of image
    DWORD   AddressOfNames;         // RVA from base of image
    DWORD   AddressOfNameOrdinals;  // RVA from base of image
} IMAGE_EXPORT_DIRECTORY, *PIMAGE_EXPORT_DIRECTORY;
*/

#ifndef TO_LOWERCASE
#define TO_LOWERCASE(c1) c1 = (c1 <= 'Z' && c1 >= 'A') ? c1 = (c1 - 'A') + 'a': c1;
#endif

bool is_wanted_func(LPSTR curr_name, LPSTR wanted_name)
{
    if (curr_name == NULL || wanted_name == NULL) return false;

    size_t wanted_name_len = strlen(wanted_name);
    size_t curr_name_len = strlen(curr_name);

    if (curr_name_len != wanted_name_len) return false;

    for (size_t i = 0; i < wanted_name_len; i++) {
        char c1 = curr_name[i];
        char c2 = wanted_name[i];
        TO_LOWERCASE(c1);
        TO_LOWERCASE(c2);
        if (c1 != c2) return false;
    }
    return true;
}

size_t forwarder_name_len(BYTE* fPtr)
{
    size_t len = 0;
    while ((*fPtr >= 'a' && *fPtr <= 'z')
            || (*fPtr >= 'A' && *fPtr <= 'Z')
            || (*fPtr >= '0' && *fPtr <= '9')
            || (*fPtr == '.')
            || (*fPtr == '_') 
            || (*fPtr == '-'))
    {
        len++;
        fPtr++;
    }
    if (*fPtr == '\0') {
        return len;
    }
    return 0;
}

bool is_ordinal(IMAGE_EXPORT_DIRECTORY *exp, LPSTR func_name)
{
    ULONGLONG base = exp->Base;
    ULONGLONG max_ord = base + exp->NumberOfFunctions;
    ULONGLONG name_ptr_val = (ULONGLONG)func_name;
    if (name_ptr_val >= base && name_ptr_val < max_ord) {
        return true;
    }
    return false;
}

FARPROC get_export_by_ord(PVOID modulePtr, IMAGE_EXPORT_DIRECTORY* exp, DWORD wanted_ordinal)
{
    SIZE_T functCount = exp->NumberOfFunctions;
	DWORD funcsListRVA = exp->AddressOfFunctions;
	DWORD ordBase = exp->Base;

    //go through names:
    for (SIZE_T i = 0; i < functCount; i++) {
		DWORD* funcRVA = (DWORD*)(funcsListRVA + (BYTE*) modulePtr + i * sizeof(DWORD));
        BYTE* fPtr = (BYTE*) modulePtr + (*funcRVA); //pointer to the function
		DWORD ordinal = ordBase + i;
        if (ordinal == wanted_ordinal) {
            if (forwarder_name_len(fPtr) > 1) {
                std::cerr << "[!] Forwarded function: ["<< wanted_ordinal << " -> "<< fPtr << "] cannot be resolved!" << std::endl;
                return NULL; // this function is forwarded, cannot be resolved
            }
            return (FARPROC) fPtr; //return the pointer to the found function
        }
    }
    return NULL;
}

size_t peconv::get_exported_names(PVOID modulePtr, std::vector<std::string> &names_list)
{
    IMAGE_DATA_DIRECTORY *exportsDir = peconv::get_pe_directory((BYTE*) modulePtr, IMAGE_DIRECTORY_ENTRY_EXPORT);

    if (exportsDir == NULL) {
        std::cerr << "Function not found!" << std::endl;
        return 0;
    }
    DWORD expAddr = exportsDir->VirtualAddress;
    if (expAddr == 0) return 0;

    IMAGE_EXPORT_DIRECTORY* exp = (IMAGE_EXPORT_DIRECTORY*)(expAddr + (ULONG_PTR) modulePtr);
    SIZE_T namesCount = exp->NumberOfNames;
    DWORD funcNamesListRVA = exp->AddressOfNames;

    //go through names:
    SIZE_T i = 0;
    for (i = 0; i < namesCount; i++) {
        DWORD* nameRVA = (DWORD*)(funcNamesListRVA + (BYTE*) modulePtr + i * sizeof(DWORD));
       
        LPSTR name = (LPSTR)(*nameRVA + (BYTE*) modulePtr);
        if (IsBadReadPtr(name, 1)) break; // this shoudld not happen. maybe the PE file is corrupt?

        names_list.push_back(name);
    }
    return i;
}

//WARNING: doesn't work for the forwarded functions.
FARPROC peconv::get_exported_func(PVOID modulePtr, LPSTR wanted_name)
{
    IMAGE_DATA_DIRECTORY *exportsDir = peconv::get_pe_directory((BYTE*) modulePtr, IMAGE_DIRECTORY_ENTRY_EXPORT);

    if (exportsDir == NULL) {
        std::cerr << "Function not found!" << std::endl;
        return NULL;
    }
    DWORD expAddr = exportsDir->VirtualAddress;
    if (expAddr == 0) return NULL;

    IMAGE_EXPORT_DIRECTORY* exp = (IMAGE_EXPORT_DIRECTORY*)(expAddr + (ULONG_PTR) modulePtr);
    SIZE_T namesCount = exp->NumberOfNames;

    DWORD funcsListRVA = exp->AddressOfFunctions;
    DWORD funcNamesListRVA = exp->AddressOfNames;
    DWORD namesOrdsListRVA = exp->AddressOfNameOrdinals;

    if (is_ordinal(exp, wanted_name)) {
#ifdef _DEBUG
        std::cerr << "[*] Getting function by ordinal" << std::endl;
#endif
        return get_export_by_ord(modulePtr, exp, (DWORD)wanted_name);
    }
    if (IsBadReadPtr(wanted_name, 1)) {
        std::cerr << "[-] Invalid pointer to the name" << std::endl;
        return NULL;
    }

    //go through names:
    for (SIZE_T i = 0; i < namesCount; i++) {
        DWORD* nameRVA = (DWORD*)(funcNamesListRVA + (BYTE*) modulePtr + i * sizeof(DWORD));
        WORD* nameIndex = (WORD*)(namesOrdsListRVA + (BYTE*) modulePtr + i * sizeof(WORD));
        DWORD* funcRVA = (DWORD*)(funcsListRVA + (BYTE*) modulePtr + (*nameIndex) * sizeof(DWORD));
       
        LPSTR name = (LPSTR)(*nameRVA + (BYTE*) modulePtr);
        BYTE* fPtr = (BYTE*) modulePtr + (*funcRVA); //pointer to the function
        
        if (!is_wanted_func(name, wanted_name)) {
            continue; //this is not the function we are looking for
        }
        if (forwarder_name_len(fPtr) > 1) {
            std::cerr << "[!] Forwarded function: ["<< name << " -> "<< fPtr << "] cannot be resolved!" << std::endl;
            return NULL; // this function is forwarded, cannot be resolved
        }
        return (FARPROC) fPtr; //return the pointer to the found function
    }
    //function not found
    std::cerr << "Function not found!" << std::endl;
    return NULL;
}

FARPROC peconv::export_based_resolver::resolve_func(LPSTR lib_name, LPSTR func_name)
{
    HMODULE libBasePtr = LoadLibraryA(lib_name);
    if (libBasePtr == NULL) {
        std::cerr << "Could not load the library!" << std::endl;
        return NULL;
    }

    FARPROC hProc = get_exported_func(libBasePtr, func_name);

    if (hProc == NULL) {
        if (!IsBadReadPtr(func_name, 1)) {
            std::cerr << "[!] Cound not get the function: "<< func_name <<" from exports!" << std::endl;
        } else {
            std::cerr << "[!] Cound not get the function: "<< (DWORD)func_name <<" from exports!" << std::endl;
        }
        std::cerr << "[!] Falling back to the default resolver..." <<std::endl;
        hProc = default_func_resolver::resolve_func(lib_name, func_name);
        if (hProc == NULL) {
            std::cerr << "[-] Loading function from " << lib_name << " failed!" << std::endl;
        }
    }
#ifdef _DEBUG
    FARPROC defaultProc = default_func_resolver::resolve_func(lib_name, func_name);
    if (hProc != defaultProc) {
        std::cerr << "[-] Loaded proc is not matching the default one!" << std::endl;
    }
#endif
    return hProc;
}