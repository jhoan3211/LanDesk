#include "lancast/platform/signature.hpp"

#include "win.hpp"

#include <softpub.h>
#include <wincrypt.h>
#include <wintrust.h>

#include <format>

#pragma comment(lib, "wintrust.lib")
#pragma comment(lib, "crypt32.lib")

namespace lancast::sig {

Info verify(const std::filesystem::path& file) {
    Info out;
    const std::wstring path = file.wstring();
    WINTRUST_FILE_INFO fi{};
    fi.cbStruct = sizeof(fi);
    fi.pcwszFilePath = path.c_str();

    WINTRUST_DATA wd{};
    wd.cbStruct = sizeof(wd);
    wd.dwUIChoice = WTD_UI_NONE;
    // Sin consultar revocaciones por red: la verificación funciona sin Internet (la descarga ya usa HTTPS).
    wd.fdwRevocationChecks = WTD_REVOKE_NONE;
    wd.dwUnionChoice = WTD_CHOICE_FILE;
    wd.pFile = &fi;
    wd.dwStateAction = WTD_STATEACTION_VERIFY;
    wd.dwProvFlags = WTD_CACHE_ONLY_URL_RETRIEVAL;

    GUID action = WINTRUST_ACTION_GENERIC_VERIFY_V2;
    const LONG status = WinVerifyTrust(static_cast<HWND>(INVALID_HANDLE_VALUE), &action, &wd);
    out.valid = status == ERROR_SUCCESS;
    if (!out.valid) {
        out.error = status == TRUST_E_NOSIGNATURE ? "sin firma" : std::format("firma no válida (0x{:08X})", static_cast<unsigned>(status));
    }

    // Editor: certificado del firmante (aunque la firma no sea de confianza, para informarlo).
    if (CRYPT_PROVIDER_DATA* prov = WTHelperProvDataFromStateData(wd.hWVTStateData); prov != nullptr) {
        if (CRYPT_PROVIDER_SGNR* signer = WTHelperGetProvSignerFromChain(prov, 0, FALSE, 0);
            signer != nullptr && signer->csCertChain > 0 && signer->pasCertChain[0].pCert != nullptr) {
            wchar_t name[256]{};
            CertGetNameStringW(signer->pasCertChain[0].pCert, CERT_NAME_SIMPLE_DISPLAY_TYPE, 0, nullptr, name, 256);
            out.subject = platform::detail::to_utf8(name);
        }
    }
    wd.dwStateAction = WTD_STATEACTION_CLOSE;
    WinVerifyTrust(static_cast<HWND>(INVALID_HANDLE_VALUE), &action, &wd);
    return out;
}

}  // namespace lancast::sig
