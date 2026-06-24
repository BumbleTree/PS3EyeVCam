#include "Autostart.h"

#include <windows.h>
#define SECURITY_WIN32
#include <security.h>     // GetUserNameExW
#include <taskschd.h>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

namespace
{

constexpr wchar_t kTaskName[] = L"PSCam4Win";

// Tiny BSTR RAII so we don't need comdef.h/comsupp.lib.
class Bstr
{
public:
    explicit Bstr(const wchar_t* s) : _b(SysAllocString(s)) {}
    ~Bstr() { SysFreeString(_b); }
    Bstr(const Bstr&) = delete;
    Bstr& operator=(const Bstr&) = delete;
    operator BSTR() const { return _b; }
private:
    BSTR _b;
};

HRESULT ConnectTaskService(ComPtr<ITaskService>& service, ComPtr<ITaskFolder>& root)
{
    HRESULT hr = CoCreateInstance(CLSID_TaskScheduler, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&service));
    if (FAILED(hr)) return hr;
    VARIANT empty;
    VariantInit(&empty);
    hr = service->Connect(empty, empty, empty, empty);
    if (FAILED(hr)) return hr;
    return service->GetFolder(Bstr(L"\\"), &root);
}

bool CurrentUserId(wchar_t* buf, ULONG chars)
{
    return GetUserNameExW(NameSamCompatible, buf, &chars) != 0;  // DOMAIN\user
}

} // namespace

namespace autostart
{

bool IsEnabled()
{
    ComPtr<ITaskService> service;
    ComPtr<ITaskFolder> root;
    if (FAILED(ConnectTaskService(service, root)))
        return false;
    ComPtr<IRegisteredTask> task;
    return SUCCEEDED(root->GetTask(Bstr(kTaskName), &task)) && task;
}

bool Disable()
{
    ComPtr<ITaskService> service;
    ComPtr<ITaskFolder> root;
    if (FAILED(ConnectTaskService(service, root)))
        return false;
    const HRESULT hr = root->DeleteTask(Bstr(kTaskName), 0);
    return SUCCEEDED(hr) || hr == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
}

bool Enable()
{
    wchar_t exePath[MAX_PATH];
    if (!GetModuleFileNameW(nullptr, exePath, MAX_PATH))
        return false;
    wchar_t userId[256];
    if (!CurrentUserId(userId, 256))
        return false;

    ComPtr<ITaskService> service;
    ComPtr<ITaskFolder> root;
    if (FAILED(ConnectTaskService(service, root)))
        return false;

    ComPtr<ITaskDefinition> def;
    if (FAILED(service->NewTask(0, &def)))
        return false;

    ComPtr<IRegistrationInfo> regInfo;
    if (SUCCEEDED(def->get_RegistrationInfo(&regInfo)))
        regInfo->put_Description(Bstr(L"Starts the PSCam4Win virtual cameras in the background at logon."));

    // Elevated at logon without UAC: interactive token at highest run level.
    ComPtr<IPrincipal> principal;
    if (FAILED(def->get_Principal(&principal)) ||
        FAILED(principal->put_UserId(Bstr(userId))) ||
        FAILED(principal->put_LogonType(TASK_LOGON_INTERACTIVE_TOKEN)) ||
        FAILED(principal->put_RunLevel(TASK_RUNLEVEL_HIGHEST)))
        return false;

    // The defaults schtasks can't change: allow battery, never time-limit.
    ComPtr<ITaskSettings> taskSettings;
    if (FAILED(def->get_Settings(&taskSettings)))
        return false;
    taskSettings->put_DisallowStartIfOnBatteries(VARIANT_FALSE);
    taskSettings->put_StopIfGoingOnBatteries(VARIANT_FALSE);
    taskSettings->put_ExecutionTimeLimit(Bstr(L"PT0S"));
    taskSettings->put_MultipleInstances(TASK_INSTANCES_IGNORE_NEW);
    taskSettings->put_StartWhenAvailable(VARIANT_TRUE);

    ComPtr<ITriggerCollection> triggers;
    ComPtr<ITrigger> trigger;
    ComPtr<ILogonTrigger> logon;
    if (FAILED(def->get_Triggers(&triggers)) ||
        FAILED(triggers->Create(TASK_TRIGGER_LOGON, &trigger)) ||
        FAILED(trigger.As(&logon)) ||
        FAILED(logon->put_UserId(Bstr(userId))))
        return false;
    logon->put_Delay(Bstr(L"PT3S"));  // let the shell/Frame Server settle

    ComPtr<IActionCollection> actions;
    ComPtr<IAction> action;
    ComPtr<IExecAction> exec;
    if (FAILED(def->get_Actions(&actions)) ||
        FAILED(actions->Create(TASK_ACTION_EXEC, &action)) ||
        FAILED(action.As(&exec)) ||
        FAILED(exec->put_Path(Bstr(exePath))))
        return false;
    exec->put_Arguments(Bstr(L"--autostart"));

    VARIANT empty;
    VariantInit(&empty);
    ComPtr<IRegisteredTask> registered;
    const HRESULT hr = root->RegisterTaskDefinition(
        Bstr(kTaskName), def.Get(), TASK_CREATE_OR_UPDATE,
        empty, empty, TASK_LOGON_INTERACTIVE_TOKEN, empty, &registered);
    return SUCCEEDED(hr);
}

} // namespace autostart
